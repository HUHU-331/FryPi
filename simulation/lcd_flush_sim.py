# -*- coding: utf-8 -*-
"""
LCD 刷新链路：改造前 / 改造后行为对比模型（1ms 步长）。

这是对 C 代码的逐行结构复刻（不是跑真机），用来讲清两件事：
  1. 为什么"正常刷新"时改造前的代码也能跑 —— LVGL 本身是串行的，ack 之后才发下一帧
  2. 为什么一旦有别的路径同时碰 SPI（或 DMA 卡住），改造前会永久停摆、改造后只是丢一帧

对应关系：
  HAL_SPI_Transmit_DMA()     -> SpiBus.transmit_dma()（忙时返回 HAL_BUSY，一个字节都不发）
  HAL_SPI_Transmit()（地址） -> SpiBus.transmit_blocking()
  HAL_SPI_GetState()         -> SpiBus.state
  LCD_Color_Fill()           -> lcd_color_fill_old / lcd_color_fill_new
  disp_flush()               -> FlushOld.flush / FlushNew.flush
  lv_disp_flush_ready()      -> acks

运行：python lcd_flush_sim.py
"""

FRAME_DMA_MS = 8      # 一帧 10 行 buffer 走 SPI-DMA 的大致耗时
IDLE_TIMEOUT_MS = 20  # lcd.c: LCD_SPI_TIMEOUT_MS


class SpiBus:
    def __init__(self, stuck=False):
        self.state = "READY"
        self.ready_at = 0
        self.stuck = stuck
        self.tx_count = 0      # 成功发起的 DMA 次数
        self.busy_hits = 0     # 撞上 HAL_BUSY 的次数
        self.dma_in_flight = False   # 真的有一笔 DMA 在跑（才会产生 TxCplt 中断）

    def is_ready(self, now):
        if self.state == "BUSY" and not self.stuck and now >= self.ready_at:
            self.state = "READY"
        return self.state == "READY"

    def occupy(self, now, ms):
        """模拟别的路径占用 SPI（例如 BSP 里其它模块直接调 HAL_SPI_Transmit）"""
        self.state = "BUSY"
        self.ready_at = now + ms

    def transmit_blocking(self, now):
        """LCD_Address_Set 走 HAL_SPI_Transmit：忙时返回 HAL_BUSY，地址命令丢失"""
        if not self.is_ready(now):
            self.busy_hits += 1
            return False
        return True

    def transmit_dma(self, now):
        if not self.is_ready(now):
            self.busy_hits += 1
            return "HAL_BUSY"
        self.state = "BUSY"
        self.ready_at = now + FRAME_DMA_MS
        self.tx_count += 1
        self.dma_in_flight = True
        return "HAL_OK"


# ---------------------------------------------------------------- 改造前
def lcd_color_fill_old(bus, now):
    """不检查返回值：地址命令和整帧数据都可能在忙时被静默丢掉"""
    bus.transmit_blocking(now)
    return bus.transmit_dma(now)


class FlushOld:
    def __init__(self, bus):
        self.bus = bus
        self.g_current = None
        self.acks = 0
        self.dropped = 0
        self.reentry = 0

    def flush(self, now, area):
        if self.g_current is not None:
            self.reentry += 1
            st = lcd_color_fill_old(self.bus, now)
            if st != "HAL_OK":
                self.dropped += 1
            return              # 不调 flush_ready，LVGL 永远等这一块
        self.g_current = area
        if lcd_color_fill_old(self.bus, now) != "HAL_OK":
            self.dropped += 1   # 丢了也没人知道，且 g_current 不会清

    def dma_complete(self, now):
        """等价于 HAL_SPI_TxCpltCallback -> lcd_flush_ready_callback。
        注意：只有真的发起过一笔 DMA 才会有完成中断；没发出去就没有回调。"""
        if self.bus.dma_in_flight and self.bus.state == "BUSY" and self.bus.is_ready(now) \
                and self.g_current is not None:
            self.bus.dma_in_flight = False
            self.acks += 1
            self.g_current = None


# ---------------------------------------------------------------- 改造后
def lcd_color_fill_new(bus, now):
    """等 SPI 空闲（20ms 超时）→ 设地址 → 启动 DMA，结果上抛"""
    deadline = now + IDLE_TIMEOUT_MS
    while not bus.is_ready(now):
        if now > deadline:
            return "HAL_TIMEOUT"
        now += 1
    bus.transmit_blocking(now)
    return bus.transmit_dma(now)


class FlushNew:
    def __init__(self, bus):
        self.bus = bus
        self.g_current = None
        self.acks = 0
        self.dropped = 0
        self.reentry = 0

    def flush(self, now, area):
        self.g_current = area
        if lcd_color_fill_new(self.bus, now) != "HAL_OK":
            self.g_current = None
            self.acks += 1      # 发不出去也立刻 ack：宁可丢一帧，不让渲染停摆
            self.dropped += 1

    def dma_complete(self, now):
        if self.bus.dma_in_flight and self.bus.state == "BUSY" and self.bus.is_ready(now) \
                and self.g_current is not None:
            self.bus.dma_in_flight = False
            self.acks += 1
            self.g_current = None


def simulate(impl_cls, n_frames, occupy_at=None, occupy_ms=5, stuck=False, horizon=400):
    """按 LVGL 真实协议驱动：拿到 ack 之后才发下一帧。"""
    bus = SpiBus(stuck)
    impl = impl_cls(bus)
    sent_next = True        # 第一帧立刻发
    k = 0
    for t in range(horizon):
        if occupy_at is not None and t == occupy_at:
            bus.occupy(t, occupy_ms)
        if sent_next and k < n_frames:
            impl.flush(t, f"frame{k}")
            k += 1
            sent_next = False
        impl.dma_complete(t)
        if not sent_next and impl.g_current is None:
            sent_next = True    # 收到 ack，LVGL 发下一块
    return bus, impl, k


def report(tag, n_frames, bus, impl, attempted, expect_stuck):
    frozen = impl.g_current is not None
    print(f"  {tag}: 尝试 {attempted}/{n_frames} 帧, 真正发出 {bus.tx_count}, 丢帧 {impl.dropped}, "
          f"ack {impl.acks}, 撞 HAL_BUSY {bus.busy_hits} 次, 重入分支 {impl.reentry} 次, "
          f"渲染是否卡死: {'是' if frozen else '否'}")
    assert frozen == expect_stuck, f"{tag} 的卡死状态与预期不符"
    return bus, impl


def main():
    n = 5
    print("场景 A：无外部干扰，连续 5 帧（LVGL 串行：ack 后才发下一帧）")
    bus, impl, k = simulate(FlushOld, n)
    report("改造前", n, bus, impl, k, expect_stuck=False)
    bus, impl, k = simulate(FlushNew, n)
    report("改造后", n, bus, impl, k, expect_stuck=False)
    print("  -> 都正常：说明缺陷不是天天出现，而是条件触发\n")

    print("场景 B：第一帧到来时 SPI 正被别的路径占用 5ms（启动阶段极易发生）")
    bus, impl, k = simulate(FlushOld, n, occupy_at=0)
    report("改造前", n, bus, impl, k, expect_stuck=True)
    bus, impl, k = simulate(FlushNew, n, occupy_at=0)
    report("改造后", n, bus, impl, k, expect_stuck=False)
    print("  -> 改造前：第一帧 HAL_BUSY 丢掉，TxCplt 永远不来，g_current 再也不清，"
          "LVGL 卡在第一帧直到复位\n"
          "  -> 改造后：等 5ms 空闲后正常发出，5 帧全部完成\n")

    print("场景 C：SPI 长期被别的路径占用（一直不释放），考察会不会拖死整个系统")
    bus, impl, k = simulate(FlushOld, n, occupy_at=0, occupy_ms=100000)
    report("改造前", n, bus, impl, k, expect_stuck=True)
    bus, impl, k = simulate(FlushNew, n, occupy_at=0, occupy_ms=100000, horizon=200)
    report("改造后", n, bus, impl, k, expect_stuck=False)
    print("  -> 改造前：第一帧就永久冻结；改造后：每帧 20ms 超时后 ack，"
          "画面刷不出来但任务不卡死，其它功能照常\n")

    print("结论：改造前把\"发不出去\"当成了\"发出去\"；改造后两条失败路径都会 ack，"
          "最坏情况从\"冻结\"降级为\"丢一帧\"。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
