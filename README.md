# FryPi · OV_Watch 按键驱动改造

基于开源工程 [FryPi](https://github.com/No-Chicken/OV-Watch)（GPL-3.0）的 OV_Watch 智能手表子工程，对其按键驱动做二次改造。完整改动见 [按键改造.diff](按键改造.diff)，+87 / −18 行。

## 改了什么

1. **EXTI 中断 + FreeRTOS 任务通知** 替代 1ms 轮询 + `osDelay(3)` 阻塞消抖：按键任务由事件唤醒（替代原来的 1kHz 轮询，50ms 超时兜底），消抖改为任务侧时间戳比对（20ms 窗口）
2. **EXTI 改为双边沿触发**：仅下降沿时释放要等 50ms 超时才被感知，短于消抖窗口的毛刺仍会被误报为短按（仿真复现，见下）；改为双边沿后释放实时可观测，该判断才真正生效
3. **EXTI0 优先级 0 → 5**：满足 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 约束，否则新 ISR 中调用 `vTaskNotifyGiveFromISR` 会触发 `configASSERT`
4. **补齐永不触发的长按分支**：原 `KeyScan()` 只返回 1，长按是死代码；现按住 ≥1s 上报长按（只报一次），未达阈值则报短按

## 涉及文件

- `OV_Watch/BSP/KEY/key.c`
- `OV_Watch/User/Tasks/Src/user_KeyTask.c`

## 如何验证 diff

diff 路径与仓库结构一致，clone 本仓库后可直接操作：

```bash
# 还原为改造前的原版代码（diff 的"改前"侧）
git apply -R 按键改造.diff

# 在原版基础上重新应用改造（diff 的"改后"侧）
git apply 按键改造.diff
```

## 仿真验证

按键状态机是纯逻辑（时间戳 + 电平），用 Python 复刻 `KeyScan()` 的状态机与任务唤醒模型（双边沿中断 + 50ms 超时兜底），以 1ms 步长喂 6 类边界场景：干净短按、长按、按下侧抖动、4ms 毛刺、快速双击、真实机械按键两侧抖动。6/6 通过，其中"4ms 毛刺"场景复现了仅下降沿触发时的误报问题，直接推动了第 2 条改动。

```bash
python simulation/key_sim.py
```

仿真脚本：[simulation/key_sim.py](simulation/key_sim.py)，每个场景的预期事件均有注释，可对照 `key.c` 阅读。
