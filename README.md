# FryPi · OV_Watch 按键驱动改造

基于开源工程 [FryPi](https://gitee.com/kingham/FryPi)（GPL-3.0）的 OV_Watch 智能手表子工程，
对其按键驱动做二次改造。完整改动见 [`按键改造.diff`](按键改造.diff)，+81 / −16 行。

## 改了什么
1. **EXTI 中断 + FreeRTOS 任务通知** 替代 1ms 轮询 + `osDelay(3)` 阻塞消抖：
   按键任务唤醒频率 1kHz → 20Hz（50ms 超时兜底），消抖改为任务侧时间戳比对（20ms 窗口）
2. **EXTI0 优先级 0 → 5**：满足 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 约束，
   否则新 ISR 中调用 `vTaskNotifyGiveFromISR` 会触发 `configASSERT`
3. **补齐永不触发的长按分支**：原 `KeyScan()` 只返回 1，长按是死代码；
   现按住 ≥1s 上报长按（只报一次），未达阈值则报短按

## 涉及文件
- `OV_Watch/BSP/KEY/key.c`
- `OV_Watch/User/Tasks/Src/user_KeyTask.c`

## 验证
状态机为纯逻辑（时间戳 + 电平），用 Python 以 1ms 步进仿真 6 类场景
（短按 / 长按 / 按下抖动 / 4ms 毛刺 / 快速双击 / 50ms 实际采样）全部通过；
仿真中发现并修复"短于消抖窗口的释放被误报为短按"的边界缺陷。真机烧录验证进行中。
