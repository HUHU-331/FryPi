# FryPi · OV_Watch 二次改造

基于开源工程 [FryPi](https://github.com/No-Chicken/OV-Watch)（GPL-3.0）的 OV_Watch 智能手表子工程，做三处互相独立的改造：

| 改动 | diff | 规模 |
| --- | --- | --- |
| 一、按键驱动改造（轮询 → EXTI + 任务通知） | [`按键驱动改造.diff`](按键驱动改造.diff) | +87 / −18 |
| 二、LCD 刷新链路修复（DMA 忙检测缺失） | [`LCD刷新链路修复.diff`](LCD刷新链路修复.diff) | +58 / −14 |
| 三、页面切换竞态修复（LVGL 单上下文收敛） | [`页面切换竞态修复.diff`](页面切换竞态修复.diff) | +55 / −30 |

---

## 一、按键驱动改造

1. **EXTI 中断 + FreeRTOS 任务通知** 替代 1ms 轮询 + `osDelay(3)` 阻塞消抖：按键任务由事件唤醒（替代原来的 1kHz 轮询，50ms 超时兜底），消抖改为任务侧时间戳比对（20ms 窗口）
2. **EXTI 改为双边沿触发**：仅下降沿时释放要等 50ms 超时才被感知，短于消抖窗口的毛刺仍会被误报为短按（仿真复现，见下）；改为双边沿后释放实时可观测，该判断才真正生效
3. **EXTI0 优先级 0 → 5**：满足 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 约束，否则新 ISR 中调用 `vTaskNotifyGiveFromISR` 会触发 `configASSERT`
4. **补齐永不触发的长按分支**：原 `KeyScan()` 只返回 1，长按是死代码；现按住 ≥1s 上报长按（只报一次），未达阈值则报短按

涉及文件：`OV_Watch/BSP/KEY/key.c`、`OV_Watch/User/Tasks/Src/user_KeyTask.c`

---

## 二、LCD 刷新链路修复

**现象**：偶发局部画面不刷新、花屏，严重时整个界面卡住不再更新。

**根因**（读码定位，行号指改造前版本）：

1. `BSP/LCD/lcd.c` 的 `LCD_Color_Fill()` 直接调用 `HAL_SPI_Transmit_DMA()`，**不检查返回值**。上一帧 DMA 未完成时 HAL 返回 `HAL_BUSY`，这一帧一个字节都没发出去，且无人补偿 → 该区域保持旧内容（局部不刷新/花屏）。
2. 同一个函数里的 `LCD_Address_Set()` 走阻塞式 `HAL_SPI_Transmit()`，DMA 忙时同样返回 `HAL_BUSY`，**地址命令丢失**，后续像素会写到错误位置。
3. `Middlewares/LVGL/GUI/porting/lv_port_disp.c` 的 `disp_flush()` 在 `g_current_disp_drv != NULL`（上一次 flush 未完成）时，**又发起一次 `LCD_Color_Fill` 然后直接 `return`**：既制造了上面两次 `HAL_BUSY`，又**永远不调用 `lv_disp_flush_ready()`**。一旦进入这个分支一次，LVGL 就再也收不到该区域的完成信号，后续所有 flush 全部堵死 → 界面静止，直到复位。
4. 同一文件中的 `s_pending_disp_drv` 只被赋值和清零，**从未被读取**——原作者"挂起一帧、等空闲再发"的机制只写了一半。

**修复**：

- `lcd.c` 新增 `LCD_Wait_Idle()`：发起新一帧前先等 `HAL_SPI_GetState() == HAL_SPI_STATE_READY`（`osDelay(1)` 让出 CPU，带 20ms 超时），然后才设地址、启动 DMA
- `LCD_Color_Fill()` 改为返回 `HAL_StatusTypeDef`，把"这一帧到底发出去没有"暴露给调用方
- `disp_flush()` 删掉错误的重入分支与死变量，只保留一条路径：发出去就等 DMA 完成中断 ack；发不出去（超时/硬件异常）就**立即 ack**，宁可丢一帧也不让渲染停摆

涉及文件：`OV_Watch/BSP/LCD/lcd.c`、`OV_Watch/BSP/LCD/lcd.h`、`OV_Watch/Middlewares/LVGL/GUI/porting/lv_port_disp.c`

**验证**：

1. 逻辑层：`simulation/lcd_flush_sim.py` 按 LVGL 真实协议（ack 之后才发下一帧）复刻改造前/后的两条路径，1ms 步长跑三种场景：
   - 无干扰连续 5 帧：**两版都正常** → 说明缺陷是条件触发，不是天天花屏
   - 首帧时 SPI 被别的路径占用 5ms：改造前第一帧 `HAL_BUSY` 丢掉后 `g_current` 永不清空，**卡死在第一帧直到复位**；改造后等 5ms 空闲后 5 帧全部正常发出
   - SPI 被长期占用：改造前同样冻结；改造后每帧 20ms 超时后 ack，**画面刷不出来但任务不卡死**
2. 编译层：Keil MDK 全工程编译通过（0 Error；190 Warning 均为原工程遗留）；GNU Arm 工具链对改动文件 `-fsyntax-only` 亦无告警
3. 真机验证（扩展板占用 IO 无法接逻辑分析仪，改用 ST-Link 调试器 + 临时计数器，验证后计数器已移除）：
   - 烧录后正常使用（翻页 / 游戏 / 休眠唤醒）无花屏、无局部不刷新、无卡死
   - Keil Debug 的 Watch 窗口实测：约 2 分钟操作共发出 **513 帧**，超时 0 次、DMA 启动失败 0 次、兜底 ack 0 次，无花屏、无卡死。该次操作中"上一帧 DMA 未传完"的竞争没有出现（等待计数为 0），因此真机证实的是**稳态刷新无丢帧**；竞争与失败路径的行为由上面的逻辑模型覆盖，真机压测仍待补

已知边界：若 DMA **已经启动**却永不完成（硬件级故障），两版都会停摆——本次修复只覆盖"发不出去"这一类，超时复位不在范围内。


---

## 三、页面切换竞态修复

**性质**：代码审计发现的**并发隐患**（非已复现故障）。LVGL v8 内部没有任何锁，官方要求所有 `lv_*` 调用发生在同一上下文；违反时症状是时序相关的偶发 HardFault，难复现、难定位，因此按"隐患"处理而不是等它炸。

**根因**（读码定位）：

1. `User/Tasks/Src/user_ScrRenewTask.c` 消费 `Key_MessageQueue` 后，**在自己的任务上下文里**直接调用 `Page_Back()` / `Page_Back_Bottom()`，即由 ScrRenewTask 创建/删除 LVGL 对象；
2. 与此同时 `LvHandlerTask`（`user_TasksInit.c`）在跑 `lv_task_handler()`，**沿同一棵对象树遍历渲染**；
3. 两个任务同时操作一棵无锁的对象树：渲染正在读某个子节点时，该节点被另一任务 `lv_obj_del()` 释放 → 渲染侧解引用已释放内存（use-after-free），时序相关、偶发 HardFault。全工程无任何互斥量，队列只传了按键事件，**没有解决"谁可以碰对象树"这个问题**。

**修复（单上下文收敛，零锁零开销）**：

- 新增 `PageReq_MessageQueue`（深度 2，容纳快速双击）：ScrRenewTask 降级为**纯转发**——把按键事件翻译成"切页请求"投进队列，文件内不再有任何 `lv_*` 调用；
- 切页动作与随附的传感器休眠（`EM7028_hrs_DisEnable()` / `LSM303DLH_Sleep()`）迁移到 `LvHandlerTask` 循环内、`lv_task_handler()` 之前执行；
- 结果：全工程 `lv_*` 调用收敛到**单一任务上下文**，"读树"与"剪树"由同一个任务按顺序执行，结构上不可能并发 → 竞态根除，且不需要任何锁。

**为什么不是"加个互斥量"**：加锁只是让冲突变慢，仍需在每个 `lv_*` 调用点覆盖、易漏、并引入持锁时长与优先级反转风险；单一写者把问题从"调度正确性"降级为"结构正确性"。同理，队列在这里的作用只是**传事件**，不是并发保护——保护来自"只剩一个写者"。

涉及文件：`OV_Watch/User/Tasks/Src/user_TasksInit.c`、`OV_Watch/User/Tasks/Inc/user_TasksInit.h`、`OV_Watch/User/Tasks/Src/user_ScrRenewTask.c`

**验证**：

1. 编译层：GNU Arm 工具链对两个改动源文件 `-fsyntax-only` 通过（仅剩未使用参数的既有风格告警）；
2. 行为等价性：短按仍走 `Page_Back()`，长按仍走 `Page_Back_Bottom()` 并在离开菜单页时休眠传感器，与改造前路径一一对应；
3. **待补**：Keil 全工程编译与真机翻页回归（本改动仅做代码层与语法层验证）。

已知边界：该修复消除的是"两个上下文并发访问对象树"，不改变"单次 `lv_obj_del` 期间渲染被阻塞"的既有行为——切页瞬间可能出现一次可感知的刷新停顿，属原有特性，不在本次范围内。


---

## 如何验证 diff

三份 diff 的路径均与仓库结构一致，clone 本仓库后可直接操作：

```bash
git apply -R 按键驱动改造.diff      # 还原为改造前
git apply    按键驱动改造.diff      # 重新应用

git apply -R LCD刷新链路修复.diff   # 同理，可对比改造前后
git apply -R 页面切换竞态修复.diff  # 页面切换竞态修复
```

## 仿真验证（按键状态机）

按键状态机是纯逻辑（时间戳 + 电平），用 Python 复刻 `KeyScan()` 的状态机与任务唤醒模型（双边沿中断 + 50ms 超时兜底），以 1ms 步长喂 6 类边界场景：干净短按、长按、按下侧抖动、4ms 毛刺、快速双击、真实机械按键两侧抖动。6/6 通过，其中"4ms 毛刺"场景复现了仅下降沿触发时的误报问题，直接推动了改动 2。

```bash
python simulation/key_sim.py
```

仿真脚本：[simulation/key_sim.py](simulation/key_sim.py)，每个场景的预期事件均有注释，可对照 `key.c` 阅读。
