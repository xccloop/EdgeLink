# IPS SPI2 单向发送回退 Implementation Plan

> **状态：先失败，后以修正版成功（2026-09-25）。** 本计划第一次执行的结果是**屏幕不显示**，当时误判为"BDTRANSMIT 模式本身不可用"，已回退。后续单变量实验查出真正根因是**命令字节的 DC 抬高时机**（ST7789 只在第 8 个 SCL 上升沿采样 D/C/X），与传输模式无关。
>
> 把命令路径的等待原语从 `RBNE` 换成 `SPI_FLAG_TRANS`（即本计划 Task 1 Step 3 里那段 `ips_spi_wait_tbe()` + 立刻抬 DC 的写法，改成 `ips_spi_wait_idle()` 之后才抬 DC）后，本计划的方案**已重新实施并在真机验证通过**。差别只在那一处。
>
> 下面的「完成标准」描述的是第一次执行时的判据；最终配置与完整实验矩阵见 `docs/develop/2026-09-25/main.md`。保留本文档是为了记录这条弯路。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 IPS BSP 的 SPI2 从「全双工 + DMA1_CH0 排空回读字节」改为「主模式单向发送」，删掉整条 RX 通路。

**Architecture:** 只修改一个文件 `edgenode/Drivers/BSP/IPS/ips.c`。SPI2 配置为 `SPI_TRANSMODE_BDTRANSMIT`（手册 MTB 模式：MSTMOD=1、BDEN=1、BDOEN=1，数据线为 MOSI、MISO 不使用），并删除 DMA1_CH0、`ips_spi_rx_sink` 以及全部 RX 排空代码。SPI 时钟（`SPI_PSC_2` = 18MHz）、ST7789 面板初始化命令、复位时序一律不动，保持单变量。

**Tech Stack:** GD32F103RCT6、GD32F10x 标准外设库、arm-none-eabi-gcc 5.4、FreeRTOS、minGW32-make、OpenOCD。

**关于测试：** 本次改动**无法做宿主单元测试**。`edgenode/tests/hmi/run.ps1` 会 mock 掉 `ips_flush_line`，根本不编译 `ips.c`，因此它只能用于回归上层、不能验证本次改动。本计划用「编译通过 + 符号清点 + 板上肉眼确认」替代 TDD，这是该改动唯一可行的验证方式。

**开工前确认检查点存在：**

```bash
ls -l docs/develop/2026-09-25/known-good-ips-full-duplex.patch
```

期望：文件存在，约 293 行。这是改动前已验证可显示版本的完整 diff。若本次改动失败需要回退，执行 `git apply docs/develop/2026-09-25/known-good-ips-full-duplex.patch`（注意：只有当 `ips.c` 已 `git checkout` 回 HEAD 时才能干净套用）。

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `edgenode/Drivers/BSP/IPS/ips.c` | 修改 | 唯一的改动文件。SPI2 配置、命令路径、DMA 路径 |
| `edgenode/Drivers/BSP/IPS/ips.h` | 不动 | 对外契约（`ips_init` / `ips_flush_line`）无变化 |

---

### Task 1: 命令路径切到单向发送

**Files:**
- Modify: `edgenode/Drivers/BSP/IPS/ips.c:291`（SPI 配置）、`edgenode/Drivers/BSP/IPS/ips.c:167-198`（删除）、`edgenode/Drivers/BSP/IPS/ips.c:200-233`（命令收发）

**为什么这三处必须一起改：** BDTRANSMIT 下 `RBNE` 永不置位，`ips_spi_wait_rbne()` 会每个字节空转 `IPS_SPI_TIMEOUT`（100000）次后返回失败，导致 `ips_init()` 整体失败并关闭背光。只改模式不删 `write_byte` 会让屏幕彻底不亮。

- [x] **Step 1: 把 SPI2 切到主模式单向发送**

在 `edgenode/Drivers/BSP/IPS/ips.c` 中找到：

```c
    spi_init_handler.trans_mode           = SPI_TRANSMODE_FULLDUPLEX;
```

替换为：

```c
    spi_init_handler.trans_mode           = SPI_TRANSMODE_BDTRANSMIT; /* MTB：主模式单向发送，数据线为 MOSI，MISO 不使用 */
```

- [x] **Step 2: 删除 `ips_spi_wait_rbne()` 与 `ips_spi_write_byte()`**

删除 `ips_spi_wait_idle()` 之后、`ips_write_command_data()` 之前的这两个完整函数（当前 167-198 行）：

```c
static uint8_t ips_spi_wait_rbne(void)
{
    uint32_t timeout = IPS_SPI_TIMEOUT;

    while(spi_i2s_flag_get(SPI2, SPI_FLAG_RBNE) == RESET)
    {
        if(timeout-- == 0U)
        {
            return IPS_FAIL;
        }
    }
    return IPS_SUCCESS;
}

static uint8_t ips_spi_write_byte(uint8_t value)
{
    if(ips_spi_wait_tbe() == IPS_FAIL)
    {
        return IPS_FAIL;
    }

    spi_i2s_data_transmit(SPI2, value);

    /* 屏幕没有MISO；全双工模式的回读字节必须及时取走，避免RBNE/OVR阻塞发送。 */
    if(ips_spi_wait_rbne() == IPS_FAIL)
    {
        return IPS_FAIL;
    }
    (void)spi_i2s_data_receive(SPI2);

    return IPS_SUCCESS;
}

```

删除后，`ips_spi_wait_idle()` 的结束花括号应直接与 `ips_write_command_data()` 的开头相邻。

- [x] **Step 3: `ips_write_command_data()` 退回 `wait_tbe` + 直写**

把整个 `ips_write_command_data()` 函数体替换为：

```c
static uint8_t ips_write_command_data(uint8_t command, const uint8_t *data, uint8_t length)
{
    uint8_t i;
    uint8_t success = IPS_SUCCESS;

    /* 一条命令和它的参数必须在同一次CS有效期间连续发送，DC低表示命令、DC高表示参数。 */
    gpio_bit_reset(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_reset(IPS_DC_PORT, IPS_DC_PIN);

    if(ips_spi_wait_tbe() == IPS_FAIL)
    {
        success = IPS_FAIL;
    }
    else
    {
        spi_i2s_data_transmit(SPI2, command);
        gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);

        for(i = 0U; i < length; i++)
        {
            if(ips_spi_wait_tbe() == IPS_FAIL)
            {
                success = IPS_FAIL;
                break;
            }
            spi_i2s_data_transmit(SPI2, data[i]);
        }
    }

    if(ips_spi_wait_idle() == IPS_FAIL)
    {
        success = IPS_FAIL;
    }
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
    return success;
}
```

- [x] **Step 4: 编译验证**

Run:
```bash
cd edgenode && mingw32-make
```

Expected: 编译通过，无新增 warning，无 `undefined reference`。

- [x] **Step 5: 确认命令路径已无 RX 残留**

Run:
```bash
grep -n "wait_rbne\|write_byte" edgenode/Drivers/BSP/IPS/ips.c
```

Expected: **无输出**。

（不要在这里 grep `SPI_FLAG_RBNE`：此时它还存在于 `ips_spi_clear_rx_state()` 内，那是 Task 2 的处理对象。全量清点在 Task 2 Step 9。）

---

### Task 2: 删除整条 RX DMA 通路

**Files:**
- Modify: `edgenode/Drivers/BSP/IPS/ips.c:95`、`108-137`、`367-379`、`488-503`、`530-559`

- [x] **Step 1: 删除 RX 静态变量与 RX 清理函数**

删除这一行（当前 95 行）：

```c
static volatile uint8_t ips_spi_rx_sink;
```

删除整个 `ips_spi_clear_rx_state()` 函数（当前 117-125 行）：

```c
static void ips_spi_clear_rx_state(void)
{
    /* 清RBNE后再读状态寄存器，符合SPI接收溢出的硬件清除顺序。 */
    if(spi_i2s_flag_get(SPI2, SPI_FLAG_RBNE) != RESET)
    {
        (void)spi_i2s_data_receive(SPI2);
    }
    (void)SPI_STAT(SPI2);
}

```

- [x] **Step 2: 收窄 `ips_dma_stop()`**

替换为：

```c
static void ips_dma_stop(void)
{
    /* 先关闭DMA通道，再关闭SPI2的DMA发送请求，避免DMA在配置过程中继续搬运数据。 */
    dma_channel_disable(DMA1, DMA_CH1);
    spi_dma_disable(SPI2, SPI_DMA_TRANSMIT);
}
```

- [x] **Step 3: 收窄 `ips_dma_abort()`**

替换为：

```c
static void ips_dma_abort(void)
{
    /* DMA或SPI异常时统一收尾，确保APP不会继续把这块缓冲区当作DMA占用。 */
    ips_dma_stop();
    (void)ips_spi_wait_idle();
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    ips_dma_active = 0U;
    gpio_bit_set(IPS_CS_PORT, IPS_CS_PIN);
}
```

- [x] **Step 4: 删除 DMA1_CH0 初始化块**

删除 `dma_init(DMA1, DMA_CH1, &dma_init_handler);` 之后、ST7789 复位注释之前的整段（当前 367-379 行，含注释）：

```c
    /* SPI2全双工时，DMA1_CH0同步把每个无意义的接收字节写入固定sink。 */
    dma_deinit(DMA1, DMA_CH0);
    dma_struct_para_init(&dma_init_handler);
    dma_init_handler.direction = DMA_PERIPHERAL_TO_MEMORY;
    dma_init_handler.memory_addr = (uint32_t)&ips_spi_rx_sink;
    dma_init_handler.memory_inc = DMA_MEMORY_INCREASE_DISABLE;
    dma_init_handler.memory_width = DMA_MEMORY_WIDTH_8BIT;
    dma_init_handler.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    dma_init_handler.periph_addr = (uint32_t)&SPI_DATA(SPI2);
    dma_init_handler.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_init_handler.number = 0U;
    dma_init_handler.priority = DMA_PRIORITY_HIGH;
    dma_init(DMA1, DMA_CH0, &dma_init_handler);

```

删除后，DMA1_CH1 的 `dma_init(...)` 应直接跟着 `/* ST7789上电后还不能直接写像素...` 注释块。

- [x] **Step 5: 收窄 `ips_dma_start()`**

找到这一整段（当前 488-503 行）：

```c
    dma_memory_address_config(DMA1, DMA_CH1, (uint32_t)data);
    dma_transfer_number_config(DMA1, DMA_CH1, byte_count);
    dma_memory_address_config(DMA1, DMA_CH0, (uint32_t)&ips_spi_rx_sink);
    dma_transfer_number_config(DMA1, DMA_CH0, byte_count);

    /* 清除上一笔传输遗留的完成、半完成和错误状态，避免把旧状态误当成本次已经完成。 */
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    dma_flag_clear(DMA1, DMA_CH0, DMA_FLAG_G);

    /* TX发送像素，RX同步消耗全双工回读数据，两个通道的字节数必须一致。 */
    gpio_bit_reset(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);
    spi_dma_enable(SPI2, SPI_DMA_RECEIVE);
    spi_dma_enable(SPI2, SPI_DMA_TRANSMIT);
    dma_channel_enable(DMA1, DMA_CH0);
    dma_channel_enable(DMA1, DMA_CH1);
```

替换为：

```c
    dma_memory_address_config(DMA1, DMA_CH1, (uint32_t)data);
    dma_transfer_number_config(DMA1, DMA_CH1, byte_count);

    /* 清除上一笔传输遗留的完成、半完成和错误状态，避免把旧状态误当成本次已经完成。 */
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);

    /* SPI2产生发送请求后，DMA才会把内存中的下一个字节自动写入SPI2数据寄存器。 */
    gpio_bit_reset(IPS_CS_PORT, IPS_CS_PIN);
    gpio_bit_set(IPS_DC_PORT, IPS_DC_PIN);
    spi_dma_enable(SPI2, SPI_DMA_TRANSMIT);
    dma_channel_enable(DMA1, DMA_CH1);
```

- [x] **Step 6: 收窄 `ips_dma_poll()` 的检查**

删除 CH0 的 `DMA_FLAG_ERR` 检查与 `SPI_FLAG_RXORERR` 检查（当前 530-540 行），并把完成条件改回单通道：

```c
    if(dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_FTF) == RESET)
    {
        return IPS_DMA_STATE_BUSY;
    }
```

- [x] **Step 7: 收窄 `ips_dma_poll()` 的清标志**

把：

```c
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    dma_flag_clear(DMA1, DMA_CH0, DMA_FLAG_G);
    ips_dma_active = 0U;
```

改为：

```c
    dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_G);
    ips_dma_active = 0U;
```

- [x] **Step 8: 编译验证**

Run:
```bash
cd edgenode && mingw32-make
```

Expected: 编译通过，无 warning。

- [x] **Step 9: 全文件清点，确认 RX 通路已彻底移除**

Run:
```bash
grep -n "CH0\|rx_sink\|RBNE\|RXORERR\|DMA_RECEIVE\|data_receive\|clear_rx\|FULLDUPLEX\|wait_rbne\|write_byte\|全双工\|回读" edgenode/Drivers/BSP/IPS/ips.c
```

Expected: **无输出**。若有残留，回到对应 Step 补删。

---

### Task 3: 修正文件头注释

**Files:**
- Modify: `edgenode/Drivers/BSP/IPS/ips.c:48`

**背景：** 该文件头注释从未跟着全双工那次改动更新，描述的仍是旧的「只发送」设计，全文无 RX 相关内容，因此不需要删除 RX 描述。唯一与代码不符的是时钟：注释写 `9MHz`，而 `SPI_PSC_2` + APB1 36MHz 实为 18MHz（本次保留 18MHz）。

- [x] **Step 1: 修正时钟描述**

找到：

```c
    ，配置SPI2为Mode0、主机、只发送、8bit、9MHz、软件NSS，配置DMA1_CH1为内存到外设、内存地址递增、8bit宽度、外设地址固定为SPI_DATA(SPI2)、高优先级，
```

改为：

```c
    ，配置SPI2为Mode0、主机、单向发送（只接MOSI）、8bit、18MHz、软件NSS，配置DMA1_CH1为内存到外设、内存地址递增、8bit宽度、外设地址固定为SPI_DATA(SPI2)、高优先级，
```

- [x] **Step 2: 编译验证**

Run:
```bash
cd edgenode && mingw32-make
```

Expected: 编译通过。

---

### Task 4: 上层回归

**Files:**
- 无改动

- [x] **Step 1: 跑 HMI 宿主测试**

Run:
```bash
cd edgenode && powershell -NoProfile -File tests/hmi/run.ps1
```

Expected: 输出 `PASS: real Buffer + simulated DMA; navigation, dirty regions, snapshots, wrapping, colors, clipping, BUSY, errors, tail drain.`，退出码 0。

注意：该测试**不覆盖**本次改动（它 mock 了 `ips_flush_line`），只用于确认 `display_buffer` 及以上各层未被连带破坏。若此步失败，说明改动越界了。

---

### Task 5: 烧录与板上验证

**Files:**
- 无改动

- [x] **Step 1: 烧录**

Run:
```bash
cd edgenode && powershell -NoProfile -File flash.ps1
```

Expected: OpenOCD 输出 `** Programming Finished **`、`** Verified OK **`、`** Resetting Target **`。

- [x] **Step 2: 板上确认三项**

1. **上电不黑屏** —— 背光点亮后能看到 HMI 画面（允许先出现未定义 GRAM 花屏，这是已知待办，不算失败）。
2. **三个页面均可显示** —— HOME 显示 `EDGE / NODE 03`、`TEMPERATURE --.-`、`SSID: --`；LINKS 显示 `TCP UNKNOWN` / `CAN UNKNOWN`；LOG 显示 `No log received.`。
3. **按键切页正常** —— KEY2 移动选中框、KEY3 确认切页。

- [x] **Step 3: 记录结论**

把结果追加到 `docs/develop/2026-09-25/main.md`，明确写清二选一：

- **成功** → RX 排空是防御性的，不是黑屏根因。可删除 `known-good-ips-full-duplex.patch`，并在笔记里更正「最后的原因」一节对 RX 的归因。
- **失败（黑屏）** → RX 排空是必需的，BDTRANSMIT 就是根因。回退：`git checkout edgenode/Drivers/BSP/IPS/ips.c && git apply docs/develop/2026-09-25/known-good-ips-full-duplex.patch`。

---

## 完成标准

- [ ] `grep -n "CH0\|rx_sink\|RBNE\|RXORERR\|DMA_RECEIVE\|FULLDUPLEX\|wait_rbne\|write_byte" edgenode/Drivers/BSP/IPS/ips.c` 无输出
- [ ] `mingw32-make` 编译通过且无新增 warning
- [ ] `tests/hmi/run.ps1` PASS
- [ ] 板上三项确认通过，结论已记入 `docs/develop/2026-09-25/main.md`
