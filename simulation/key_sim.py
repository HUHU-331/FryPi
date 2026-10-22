# -*- coding: utf-8 -*-
"""
按键状态机仿真：复刻 OV_Watch 改造后 key.c 里的纯逻辑，用 Python 验证边界行为。

与 C 代码的对应关系：
  KeyScan() 状态机      -> class KeyScan（逐行对应 key.c 里的静态变量与分支）
  HAL_GetTick()         -> 主循环的 t（1ms 一步推进）
  KEY1 电平             -> 波形表（1 = 松开，0 = 按下；按键低电平有效）
  EXTI 双边沿中断       -> 主循环检测到电平跳变时立即 scan 一次（模拟 ISR 唤醒任务）
  50ms 任务超时兜底     -> 每 50ms 无跳变时也 scan 一次（等价 ulTaskNotifyTake 超时）
  事件上报              -> events 列表，记录 (时刻, 1=短按 / 2=长按)

运行：python key_sim.py，全部 PASS 说明 C 逻辑在 6 类边界场景下行为正确。
"""

DEBOUNCE_MS = 20      # key.c: KEY_DEBOUNCE_MS
LONGPRESS_MS = 1000   # key.c: KEY_LONGPRESS_MS
SCAN_TIMEOUT_MS = 50  # user_KeyTask.c: pdMS_TO_TICKS(50)


class KeyScan:
    """对应 key.c 中改造后的 KeyScan()，static 变量 -> 实例属性。"""

    def __init__(self):
        # C 里 static 初值为 0，上电 20ms 内的首次按压会被吞掉；
        # 仿真假设上电已久，故初始化为 -DEBOUNCE_MS
        self.accept_tick = -DEBOUNCE_MS  # static uint32_t accept_tick
        self.press_tick = 0              # static uint32_t press_tick
        self.key_down = 0                # static uint8_t key_down
        self.long_sent = 0               # static uint8_t long_sent

    def scan(self, tick, level):
        """level: 0=按下(KEY1==GPIO_PIN_RESET), 1=松开(GPIO_PIN_SET)。返回 0/1/2。"""
        keyvalue = 0
        if level == 0:
            if self.key_down == 0:
                if tick - self.accept_tick < DEBOUNCE_MS:
                    return 0                # 距上次有效边沿不足 20ms，视为抖动
                self.accept_tick = tick
                self.press_tick = tick
                self.key_down = 1
            elif self.long_sent == 0 and tick - self.press_tick >= LONGPRESS_MS:
                self.long_sent = 1
                keyvalue = 2                # 长按：按住期间只报一次
        else:
            if self.key_down != 0:
                self.key_down = 0
                # 接触时长短于消抖窗口 = 抖动/毛刺，不报短按
                if self.long_sent == 0 and tick - self.press_tick >= DEBOUNCE_MS:
                    keyvalue = 1            # 短按：释放时上报
        return keyvalue


def pressed(a, b):
    """构造 [a, b) 区间按下的波形片段：这些时刻电平为 0，其余默认为 1。"""
    return {t: 0 for t in range(a, b)}


def run_scenario(name, waveform, duration, expect):
    ks = KeyScan()
    events = []
    last_level = 1
    for t in range(duration + 1):
        level = waveform.get(t, 1)
        # EXTI 双边沿：电平跳变立即 scan（模拟 ISR 唤醒任务）
        if level != last_level:
            ev = ks.scan(t, level)
            if ev:
                events.append((t, ev))
        # 50ms 超时兜底
        elif t % SCAN_TIMEOUT_MS == 0:
            ev = ks.scan(t, level)
            if ev:
                events.append((t, ev))
        last_level = level

    ok = events == expect
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    print(f"       事件: {events}")
    if not ok:
        print(f"       期望: {expect}")
    return ok


def main():
    results = []

    # 1. 干净短按 100ms：释放时上报短按（双边沿下释放即被感知，无需等超时）
    results.append(run_scenario(
        "短按 100ms",
        pressed(10, 110), 200,
        [(110, 1)],
    ))

    # 2. 长按 1500ms：按满 1000ms 报一次长按，释放后不再补报短按
    #    （上报时刻取决于 50ms 超时相位，真机上在 1000~1050ms 之间均正常）
    results.append(run_scenario(
        "长按 1500ms",
        pressed(10, 1510), 1600,
        [(1050, 2)],
    ))

    # 3. 按下侧抖动：多次弹跳后被稳定接受，全程只报一次短按
    wf = {}
    wf.update(pressed(10, 14))    # 弹起
    wf.update(pressed(18, 22))    # 再按下
    wf.update(pressed(26, 200))   # 稳定按住
    results.append(run_scenario(
        "按下抖动（两次弹跳后稳定按住）",
        wf, 250,
        [(200, 1)],
    ))

    # 4. 4ms 毛刺：短于 20ms 消抖窗口的接触必须被吞掉
    #    （改造前：按下即上报，毛刺会被误报为短按）
    results.append(run_scenario(
        "4ms 毛刺（应为零事件）",
        pressed(10, 14), 100,
        [],
    ))

    # 5. 快速双击：两次独立短按各自上报，互不吞并
    wf = {}
    wf.update(pressed(10, 60))
    wf.update(pressed(140, 190))
    results.append(run_scenario(
        "快速双击（间隔 80ms）",
        wf, 250,
        [(60, 1), (190, 1)],
    ))

    # 6. 真实机械按键：按下侧和释放侧都有典型抖动
    wf = {}
    wf.update(pressed(100, 104))  # 按下侧抖动
    wf.update(pressed(107, 112))
    wf.update(pressed(115, 400))  # 稳定按住
    wf.update({400: 1})           # 释放
    wf.update(pressed(403, 406))  # 释放侧抖动（406~410 保持松开由默认值覆盖）
    results.append(run_scenario(
        "真实机械按键波形（两侧抖动）",
        wf, 450,
        [(400, 1)],
    ))

    print()
    print(f"合计: {sum(results)}/{len(results)} 通过")
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
