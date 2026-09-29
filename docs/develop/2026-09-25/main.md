# 2026-09-25 IPS 无显示排查

今天把 EdgeNode 的 IPS 无显示问题排查了一遍。开始时串口已经打印到：

```text
IPS init success
display buffer init success
Edgenode start
```

但是屏幕没有内容。这里不能因为 `ips_init()` 返回成功，就认为屏幕已经正常工作；这个返回值只说明初始化命令在 MCU 这一侧没有等到 SPI 超时，不说明 ST7789 已经正确接收并显示了像素。

## 先把 HMI 和 FreeRTOS 从问题里拿掉

一开始我怀疑是 HMI 的双行缓冲、Render 状态机，或者 FreeRTOS 任务没有真正运行。为了缩小范围，先把 `main()` 改成最小裸机程序：

```text
board_config_init()
CH340 初始化
IPS 初始化
CPU 直接循环填充 RED / GREEN / BLUE / WHITE / BLACK
```

这个版本不创建队列、不启动调度器，也不调用 HMI、Render 和 `display_render_span()`。颜色数据由 CPU 直接写 SPI2，不经过行缓冲 DMA。

结果仍然没有显示。

这一步排除了一个范围：问题不在 HMI 页面、按键、FreeRTOS 调度，单纯的“IPS 初始化 + SPI 像素发送”已经无法工作。

## 先看物理信号，但不把万用表读数当成结论

测量时 BLK 已经存在，RST、CS、DC 等静态脚为 3.3 V；SCL、SDA 在发送时会变化。说明背光和 MCU 到屏幕的基础连线没有明显断开。

不过万用表只能看到平均电压，不能证明 SPI 的时序、模式、初始化命令和像素字节都正确，所以这一步只能说明“不是一个明显的断线或背光未上电”，不能说明软件驱动没有问题。

## 用 D:\Epoll\gd32 做已知正常对照

之后烧录 `D:\Epoll\gd32` 工程，同一块 IPS 可以正常显示。OpenOCD 的烧录和校验都成功，屏幕也确实亮起并显示内容。

这条证据很重要：

```text
同一块板子 + 同一块 IPS + D:\Epoll\gd32 能显示
```

因此不用再把重点放在 IPS 硬件、供电、背光和 PB3/PB5 接线，故障范围收敛到了 EdgeLink 的 IPS 初始化和传输配置。

对照两个工程后，发现 EdgeLink 原来的路径只有很少的 ST7789 初始化命令，SPI2 使用单线发送、9 MHz，DMA 只配置了 DMA1_CH1 的 TX。能显示的工程使用了完整的面板初始化、SPI2 全双工方式，并同时处理接收方向的数据。

## 先让 EdgeLink 裸机色块显示

为了先验证这组差异，在 EdgeLink 的裸机色块程序中补了下面的内容：

```text
SPI2: Mode 0, FULLDUPLEX, 8 bit, PCLK1 / 2
命令发送: 每写一个字节后读取一次 RX，清掉无意义回读数据
ST7789: RGB565、VCOM、PORCTRL、Gamma 等完整初始化参数
复位: RST 低 100 ms，再高 100 ms
```

ST7789 屏幕没有接 MISO，但 GD32 的全双工 SPI 每发送一个字节仍会接收一个无意义字节。如果只发送、不读接收寄存器，`RBNE` / overrun 会积累，后续发送可能异常。

这版裸机程序重新烧录后，红、绿、蓝、白、黑纯色可以正常轮换显示。

这里的结论是：EdgeLink 原来的 IPS 初始化/传输配置确实不足。因为这次同时补了 SPI 模式、RX 清理、完整面板参数和复位时间，当前还不能严格说“只改某一个寄存器就能修复”；能确定的是原始的简化初始化加单向 DMA 路径不能驱动这块屏，而对齐后的配置可以。

## 恢复 HMI 时不能继续只开 TX DMA

裸机色块能显示后，还需要把 CPU 填色临时路径删除，恢复原来的 HMI 双行缓冲和逐行 DMA 刷新。

这时不能只把 SPI 改成全双工，却继续保留原来的 DMA1_CH1 TX：全双工每发一个像素字节都会产生一个 RX 字节，RX 不被读取会溢出。

正式 IPS 路径现在是：

```text
DMA1_CH1: RAM -> SPI2_DATA，发送一行 RGB565 像素
DMA1_CH0: SPI2_DATA -> ips_spi_rx_sink，接收无意义回读字节
```

每次 `ips_flush_line()` 启动一行时，CH0 和 CH1 使用相同字节数并同时启动；完成时必须等两个通道都完成，再等 SPI2 的 `TRANS` 清零。超时或 DMA 错误时，先停两个 DMA 请求，等待正在移位的字节结束，再读取 RX 和状态寄存器清理接收状态，最后释放这行缓冲区。

这样没有改变 HMI 的边界：Page/Widget 仍只描述画面，Render 仍通过双行缓冲提交，IPS BSP 才拥有 SPI2、DMA、CS/DC 和 ST7789 命令。

## 正式 HMI 验证

恢复 `init_all()`、队列和 HMI 任务后，固件可以完整编译：

```text
text 37692
bss  19744
```

HMI 宿主测试通过：

```text
PASS: real Buffer + simulated DMA; navigation, dirty regions, snapshots,
wrapping, colors, clipping, BUSY, errors, tail drain.
```

随后通过 `edgenode/flash.ps1` 烧录。OpenOCD 输出：

```text
** Programming Finished **
** Verified OK **
** Resetting Target **
```

这证明当前 ELF 已经写入并校验通过；屏幕实际显示由板上观察确认。

## 上下颠倒

恢复正式 HMI 后，屏幕可以显示，但画面上下颠倒。这个问题不是 Page 坐标计算错误，而是 ST7789 的 `MADCTL (0x36)` 扫描方向设置不同。

原来的 EdgeLink 值是：

```c
const uint8_t memory_access_control = 0xA0U;
```

已显示正常的对照工程使用 `0x60`，因此改为：

```c
const uint8_t memory_access_control = 0x60U;
```

重新编译并烧录后，用实机继续确认方向。这个改动只影响控制器把逻辑 X/Y 坐标映射到面板扫描方向的方式，不修改 HMI 的 320×240 横屏坐标体系。

## 最后的原因

这次 IPS 无显示不是 HMI 页面没有创建，也不是屏幕、背光或 SPI2 引脚本身坏了。

根因在于 EdgeLink 原来的 IPS 底层初始化过于简化：它没有采用这块面板可工作的完整 ST7789 参数，并且正式像素路径只配置了 SPI2 TX DMA。对齐已验证工程后，SPI2 使用全双工，CPU 命令路径会读走 RX，DMA 像素路径用 CH0 同步消耗 RX、CH1 发送像素，屏幕才可以稳定显示。

现在暂时保留的边界是：上电时仍可能先看到未定义 GRAM 的花屏，因为 `ips_init()` 完成后会打开背光，HMI 首帧还没有刷完。之后可以单独做启动画面：背光保持关闭，从 GD25Q32 的专用图片区逐行读取 RGB565 数据，刷完整图后再打开背光。图片区不能和 StorageTask 的日志区混用。

TCP 发送失败后的运行期重连已经删除。现在 TCP 一次发送失败会标记为离线并立即 CAN 回退；两条链路都失败的记录继续保留 Flash pending，只在下一次上电恢复时补发。



## 追加实验：SPI2 单向发送（BDTRANSMIT）回退 — 失败

在完整面板初始化的基础上，把 SPI2 从全双工改为 `SPI_TRANSMODE_BDTRANSMIT`（GD32F10x 用户手册 §18.3.5 表 18-4 的 MTB 模式：MSTMOD=1、BDEN=1、BDOEN=1，数据线为 MOSI、MISO 不使用），并删除 DMA1_CH0、`ips_spi_rx_sink`、`ips_spi_wait_rbne()` / `ips_spi_write_byte()` 以及全部 RX 排空代码。其余变量——SPI 时钟、ST7789 面板初始化命令、复位时序——一律保持不变。

结果：**屏幕不显示。**

当时把结论写成"回读字节的排空是必需项"，**这个归因是错的**，见下面的实验 A。

原因是这次改动并不是单变量：`SPI_TRANSMODE_BDTRANSMIT` 下 `RBNE` 永不置位，所以必须删掉 `ips_spi_wait_rbne()`；而删掉它的同时，命令字节的 **DC 抬高时机**也被一起改掉了。两个变量绑在一起，无法区分。

### 当时的回退状态（后被实验 C 取代）

已回退到全双工，并把 SPI 时钟从 18MHz 降到 9MHz：

```c
spi_init_handler.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
spi_init_handler.prescale   = SPI_PSC_4;   /* SPI2 时钟 36MHz / 4 = 9MHz */
```

降频原因：ST7789V 手册 Table 6（4-line serial）中 `TSCYCW` 写周期最小 66ns，即写时钟上限约 15.15MHz。原先的 `SPI_PSC_2` 是 18MHz、周期 55.6ns，超出该规格约 21%——能跑，但没有余量。9MHz 留出了余量。

回退与核对方式：`git checkout edgenode/Drivers/BSP/IPS/ips.c` 后套用 `known-good-ips-full-duplex.patch`，再单独改 prescale。已用编译器生成的汇编逐指令比对确认：相对已验证可工作版本，唯一差异就是 prescale 这一个常量（text 段 37692 → 37696，正是 `movs r3, #8` 多出的那条指令）。

**已确认**：9MHz 这一版烧录后正常显示。9MHz + 完整面板初始化这个组合在真机上可用。

## 实验 A：DC 时序单变量 — 不显示

为了把上面绑在一起的两个变量分开，做了实验 A。这次**只动一件事**：

| 项 | 状态 |
|---|---|
| SPI2 模式 | `SPI_TRANSMODE_FULLDUPLEX`，**不变** |
| DMA1_CH0 / CH1、RX 排空 | **不变**（OVR 不会积累） |
| 参数字节路径 | 仍走 `ips_spi_write_byte`，**不变** |
| 像素路径 | **不变** |
| **命令字节的 DC 抬高时机** | **唯一改动** —— 写进 DR 后立刻抬，不等它移完 |

实现上保留了 `ips_spi_wait_rbne()` 与 `spi_i2s_data_receive()`，只是把 `gpio_bit_set(IPS_DC_PORT, ...)` 移到等待之前，所以排空行为完全没变。

结果：**屏幕不显示。**

### 根因：D/CX 只在第 8 个 SCL 上升沿被采样

ST7789V 手册第 56 页 §8.4.2（4-line serial 接口说明）原文：

> "SDA is sampled at the rising edge of SCL. D/CX indicates whether the byte is command (D/CX='0') or parameter/RAM data (D/CX='1'). **D/CX is sampled when first rising edge of SCL (3-line serial interface) or 8th rising edge of SCL (4-line serial interface).**"

代入两种写法：

| 版本 | 命令字节移完需要 | DC 何时抬高 | 第 8 个上升沿时 DC | 结果 |
|---|---|---|---|---|
| 原版（经 `ips_spi_write_byte`） | 8 个 SCL ≈ 830ns @9MHz | 等 `RBNE` 之后 | **低** | ✅ 显示 |
| 实验 A | 同上 | 写 DR 后约 100ns | **高** | ❌ 不显示 |

`gpio_bit_set()` 是几十纳秒的操作，而 9MHz 下一个字节要 889ns，所以 DC 在第 8 个上升沿之前就抬高了。控制器把**命令字节当成参数**收下，`ips_init()` 的 17 条命令全部失效，面板停在复位/睡眠状态——表现不是花屏，是彻底不显示。

### `ips_spi_wait_rbne()` 的真实身份

它不是"等到收到数据"，而是 **"等 8 个 SCL 走完"**。

PB4 是 SPI2_MISO，屏幕那头没有接线，`ips.c` 也从未 `gpio_init` 过它，它停在复位默认的浮空输入。但 SPI 的收发共用同一个移位寄存器：每个 SCL 上升沿必然把一位从 MOSI 移出、同时从 PB4 移入。8 个沿走完，硬件就把移位寄存器整个复制到接收缓冲并置 `RBNE` —— **它不检查 MISO 上有没有东西**。

所以 `RBNE` 只是个伪装的延时信号，而它恰好提供了"这个字节真的发完了"这一信息。**全双工在这里是"碰巧"带来了正确性，并不是它本身被需要。**

### 实验 B：等待原语换成 `TRANS` — 显示

在**已知可工作的 `FULLDUPLEX` 配置**里，只把 `ips_spi_write_byte()` 的等待从 `ips_spi_wait_rbne()` 换成 `ips_spi_wait_idle()`（轮询 `SPI_FLAG_TRANS`），读 DR 保留。结果：**正常显示**。

这证明 `TRANS` 清零与 `RBNE` 置位在"发生在第 8 个 SCL 上升沿之后"这点上等价，而 `TRANS` **不依赖全双工**。也就是说，"等这一字节发完"存在与传输模式无关的实现。

### 实验 C：BDTRANSMIT + 删除整条 RX 通路 — 显示

把 SPI2 切回 `SPI_TRANSMODE_BDTRANSMIT`，并删除 `DMA1_CH0`、`ips_spi_rx_sink`、`ips_spi_wait_rbne()`、`ips_spi_clear_rx_state()`、逐字节读 DR，以及 `ips_dma_poll()` 里的 CH0-`ERR`、`RXORERR`、CH0-`FTF` 检查。DC 时机由实验 B 验证过的 `ips_spi_wait_idle()` 保证。结果：**正常显示**。

`text 37696 → 37408`（−288 字节），bss 不变。全工程再无第二处引用 `DMA1_CH0`，通道确实空出来了。

错误处理没有被削弱：`ips_dma_poll()` 仍保留 20ms 超时、CH1 `DMA_FLAG_ERR`、以及 DMA 完成后的 `SPI_FLAG_TRANS` 检查。被删掉的 CH0-`ERR` 与 `RXORERR` 在单向发送下本来就不可能置位。

## 最终结论

| 变量 | 是否黑屏根因 |
|---|---|
| ST7789 面板初始化命令（3 条 → 17 条） | **是** |
| **命令字节的 DC 抬高时机** | **是** |
| SPI2 传输模式（BDTRANSMIT ↔ FULLDUPLEX） | **否** |
| SPI2 时钟（9MHz / 18MHz） | 否 |

最初写下的"必须全双工、RX 排空是必需项"**是错的**。真正必需的条件是：**命令字节完全移完之后才能抬高 DC**。`RBNE` 只是碰巧提供了这个时机——因为全双工下不读走回读字节还会积累 `OVR`，所以它看起来像"必须排空"，实际上排空从来不是重点。

换成一个与传输模式无关的等待原语（`SPI_FLAG_TRANS`）之后，单向发送完全可用，整条 RX 通路可以删除。

## 为什么最终选择 9MHz

不是因为 9MHz "更正确"，而是余量：ST7789V 手册 Table 6（4-line serial）规定 `TSCYCW` 写周期最小 66ns，即写时钟上限约 15.15MHz。18MHz 周期 55.6ns，超出规格约 21%——实测能跑，但没有余量；9MHz 有足够余量。

**降频在现有架构下不产生刷新代价**：`hmi_task` 周期 1ms，而 `hmi_render_service()` 每次调用最多提交一行，一帧 240 行本来就至少要 240ms。9MHz 下一行 640 字节耗时 640×8/9MHz ≈ 0.57ms，仍在一拍之内；18MHz 是 0.28ms。真正的瓶颈是任务的 1ms 节流，不是 SPI 速度。


## 最终配置

```c
spi_init_handler.trans_mode = SPI_TRANSMODE_BDTRANSMIT;  /* MTB：主模式单向发送，数据线MOSI，MISO不使用 */
spi_init_handler.prescale   = SPI_PSC_4;                 /* SPI2 时钟 36MHz / 4 = 9MHz */
```

命令路径的 DC 时机由 `ips_spi_write_byte()` 里的 `ips_spi_wait_idle()`（轮询 `SPI_FLAG_TRANS`）保证，不再是 `RBNE`。

与 2026-09-25 那个"能显示"的中间版本相比：删除 `DMA1_CH0`、`ips_spi_rx_sink`、`ips_spi_wait_rbne()`、`ips_spi_clear_rx_state()`、逐字节读 DR，`text` 段 37696 → 37408。

### 三轮单变量的完整矩阵

| # | SPI 模式 | 命令字节 DC 时机 | RX 排空 | 结果 |
|---|---|---|---|---|
| — | FULLDUPLEX | 等 `RBNE` 后（晚） | 有 | ✅ 显示 |
| A | FULLDUPLEX | 写 DR 后立刻（早） | 有 | ❌ 不显示 |
| B | FULLDUPLEX | 等 `TRANS` 后（晚） | 有 | ✅ 显示 |
| C | **BDTRANSMIT** | 等 `TRANS` 后（晚） | **无** | ✅ 显示 |
| 旧版 | BDTRANSMIT | 写 DR 后立刻（早） | 无 | ❌ 不显示 |

看"A 与 B 只差等待原语、结果相反"，以及"C 在 A 的失败配置上只补回正确的 DC 时机就成功"，可以确定决定性变量是 **DC 时机**，与传输模式和 RX 排空都无关。
