# IPS SPI2 单向发送回退设计

## 目标

把 IPS BSP 的 SPI2 从「全双工 + DMA1_CH0 排空回读字节」改为「主模式单向发送」，删掉整条 RX 通路。借此判定 2026-09-25 那次修复中，RX 排空究竟是因果性的还是防御性的。

## 依据

GD32F10x 用户手册 §18.3.5 表 18-4：MTB（主模式双向发送）配置为 `MSTMOD=1, RO=0, BDEN=1, BDOEN=1`，数据引脚为 **MOSI：发送、MISO：不使用**。对应标准库宏 `SPI_TRANSMODE_BDTRANSMIT`（`SPI_CTL0_BDEN | SPI_CTL0_BDOEN`）。主模式下单向数据线就是 MOSI，PB5 接线不变。

该模式是 `cb81e1f` 中 IPS BSP 最初使用的模式，因此本次改动是一次回退。

原先 TX 的 DMA 请求由 `TBE` 驱动，与 RX 通路无关；FULLDUPLEX 下不排空 RX 只会让 `OVR` 置位，不会停止发送。因此 RX 排空很可能只是防御性代码，真正修复黑屏的是同期加入的完整 ST7789 面板初始化。

## 边界

- **单变量**：SPI 时钟（`SPI_PSC_2`，18MHz）、ST7789 面板初始化命令、复位时序、`dbg_trace_pin_disable()` 一律不动。
- 只修改 `edgenode/Drivers/BSP/IPS/ips.c`。
- `ips.h` 对外契约不变，`display_buffer` 及以上各层不受影响。
- 不改启动花屏问题（`ips_init()` 打开背光时首帧尚未刷完）。

## 改动清单

1. `trans_mode` 由 `SPI_TRANSMODE_FULLDUPLEX` 改为 `SPI_TRANSMODE_BDTRANSMIT`。
2. 删除静态变量 `ips_spi_rx_sink`。
3. `ips_dma_stop()` 只停 DMA1_CH1 与 `SPI_DMA_TRANSMIT`。
4. 删除 `ips_spi_clear_rx_state()`。
5. `ips_dma_abort()` 去掉 RX 清理与 DMA1_CH0 的 `dma_flag_clear`。
6. 删除 `ips_spi_wait_rbne()` 与 `ips_spi_write_byte()`；`ips_write_command_data()` 退回 `ips_spi_wait_tbe()` + `spi_i2s_data_transmit()` 直写。
7. 删除 DMA1_CH0 的初始化块。
8. `ips_dma_start()` 删除 CH0 的地址/长度配置、清标志、`SPI_DMA_RECEIVE` 使能与 `dma_channel_enable(DMA1, DMA_CH0)`。
9. `ips_dma_poll()` 删除 CH0 的 `DMA_FLAG_ERR`、`SPI_FLAG_RXORERR`、CH0 的 `DMA_FLAG_FTF` 条件与 CH0 的 `dma_flag_clear`。
10. 修正文件头注释中 `ips_init()` 的 SPI 描述：`9MHz` 改为 `18MHz`。

    查证结果：文件头注释（40-73 行）**从未跟着全双工那次改动更新过**，它描述的仍是旧的「只发送」设计，全文没有出现 RX、DMA1_CH0、回读等内容。因此本次改动不需要删除注释里的 RX 描述，唯一与代码不符的是 `9MHz`（当前 `SPI_PSC_2` + APB1 36MHz = 18MHz，本次保留）。

第 6 条是必需项而非清理：BDTRANSMIT 下 `RBNE` 永不置位，`ips_spi_wait_rbne()` 会每字节空转 `IPS_SPI_TIMEOUT`（100000）次后返回失败，导致 `ips_init()` 整体失败。

## 错误处理

DMA 异常保护由「CH0+CH1 双 `FTF`、CH0 `ERR`、`RXORERR`、20ms 超时」收紧为「CH1 `FTF` + 20ms 超时」。`IPS_DMA_TIMEOUT_MS` 这道兜底仍在，DMA 卡死仍可恢复并释放行缓冲区。

## 风险

2026-09-25 笔记的结论段倾向于「RX 排空是必要的」。若 BDTRANSMIT 确为当初黑屏的根因，屏幕会再次全黑。此风险已知且可接受：失败即得到确定结论，成功则删掉一整条无用通路。

回退手段：`docs/develop/2026-09-25/known-good-ips-full-duplex.patch` 保存了改动前已验证可显示版本的完整 diff，可用 `git apply` 还原。

## 验证

宿主测试 `edgenode/tests/hmi/run.ps1` 覆盖不到本次改动：它 mock 了 `ips_flush_line`，不编译 `ips.c`。因此它只用于确认上层未被连带破坏。

1. `mingw32-make` 编译通过，无新增 warning。
2. `edgenode/flash.ps1` 烧录，OpenOCD 报 `Verified OK`。
3. 板上确认三项：上电不黑屏、三个页面均可显示、按键切页正常。
4. 运行 `edgenode/tests/hmi/run.ps1` 回归上层。
