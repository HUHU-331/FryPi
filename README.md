# FryPi · OV_Watch 二次改造

基于开源工程 [FryPi](https://github.com/No-Chicken/OV-Watch)（GPL-3.0）的 OV_Watch 智能手表子工程，做两处互相独立的改造：

| 改动 | diff | 规模 |
| --- | --- | --- |
| 一、按键驱动改造（轮询 → EXTI + 任务通知） | [`按键驱动改造.diff`](按键驱动改造.diff) | +87 / −18 |
| 二、LCD 刷新链路修复（DMA 忙检测缺失） | [`LCD刷新链路修复.diff`](LCD刷新链路修复.diff) | +58 / −14 |

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
   - Keil Debug 的 Watch 窗口实测：约 2 分钟操作内共发出 **513 帧**，其中 **33 次**遇到"上一帧 DMA 未传完"并被 `LCD_Wait_Idle()` 正确等待——这 33 次在改造前每一个都是静默丢帧/花屏候选；超时 0 次、DMA 启动失败 0 次、兜底 ack 0 次

已知边界：若 DMA **已经启动**却永不完成（硬件级故障），两版都会停摆——本次修复只覆盖"发不出去"这一类，超时复位不在范围内。


---

## 如何验证 diff

两份 diff 的路径均与仓库结构一致，clone 本仓库后可直接操作：

```bash
git apply -R 按键驱动改造.diff      # 还原为改造前
git apply    按键驱动改造.diff      # 重新应用

git apply -R LCD刷新链路修复.diff   # 同理，可对比改造前后
```

## 仿真验证（按键状态机）

按键状态机是纯逻辑（时间戳 + 电平），用 Python 复刻 `KeyScan()` 的状态机与任务唤醒模型（双边沿中断 + 50ms 超时兜底），以 1ms 步长喂 6 类边界场景：干净短按、长按、按下侧抖动、4ms 毛刺、快速双击、真实机械按键两侧抖动。6/6 通过，其中"4ms 毛刺"场景复现了仅下降沿触发时的误报问题，直接推动了改动 2。

```bash
python simulation/key_sim.py
```

仿真脚本：[simulation/key_sim.py](simulation/key_sim.py)，每个场景的预期事件均有注释，可对照 `key.c` 阅读。
