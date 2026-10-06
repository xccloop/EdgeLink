#include "gd25.h"
#include "board_time.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "gd32f10x_spi.h"
#include <stdint.h>

/* CS 在 PB12 —— 和应用侧同一个引脚。 */
#define GD25_CS_PORT  GPIOB
#define GD25_CS_PIN   GPIO_PIN_12

/* 0x9F 按顺序返回厂商 / 类型 / 容量，拼成 24 位再比。 */
#define GD25_CHIP_ID  0xC84016UL

#define GD25_STATUS_WIP 0x01U
#define GD25_STATUS_WEL 0x02U

#define GD25_CMD_WRITE_ENABLE 0x06U
#define GD25_CMD_READ_STATUS  0x05U
#define GD25_CMD_READ_DATA    0x03U
#define GD25_CMD_PAGE_PROGRAM 0x02U
#define GD25_CMD_READ_ID      0x9FU

#define GD25_SPI_TIMEOUT_MS  10U
#define GD25_SPI_MAX_POLLS   100000U
#define GD25_BUSY_TIMEOUT_MS 500U
#define GD25_BUSY_MAX_POLLS  500000U

static uint8_t gd25_ready;

/* ------------------------------------------------------------------ */
/* SPI0 收发（Bootloader 里这条总线上只有 GD25，所以没有锁这回事）      */
/* ------------------------------------------------------------------ */

/*
    两处超时都保留【轮询次数兜底】。Bootloader 默认不开 SysTick
    （只在 boot_halt() 点屏前才开），board_systick_ms 根本不前进，
    只靠毫秒计时会死等。所以这个兜底是常用路径，不是保险。
*/
static uint8_t gd25_wait_flag(uint32_t flag, FlagStatus expected)
{
    uint32_t start = board_systick_ms;
    uint32_t polls = GD25_SPI_MAX_POLLS;

    while(spi_i2s_flag_get(SPI0, flag) != expected)
    {
        if((board_systick_ms - start) >= GD25_SPI_TIMEOUT_MS)
        {
            return 0U;
        }
        if(polls == 0U)
        {
            return 0U;
        }
        polls--;
    }
    return 1U;
}

/* 上一次超时后迟到的字节会留在接收寄存器，不清理会错位到下一笔传输。 */
static void gd25_rx_cleanup(void)
{
    while(spi_i2s_flag_get(SPI0, SPI_FLAG_RBNE) == SET)
    {
        (void)spi_i2s_data_receive(SPI0);
    }
    (void)SPI_STAT(SPI0);
}

static uint8_t gd25_spi_byte(uint8_t send_data, uint8_t *receive_data)
{
    if(receive_data == 0)
    {
        return 0U;
    }

    if(gd25_wait_flag(SPI_FLAG_TBE, SET) == 0U)
    {
        return 0U;
    }
    spi_i2s_data_transmit(SPI0, send_data);

    if(gd25_wait_flag(SPI_FLAG_RBNE, SET) == 0U)
    {
        gd25_rx_cleanup();
        return 0U;
    }
    *receive_data = (uint8_t)spi_i2s_data_receive(SPI0);
    return 1U;
}

/* CS 拉高之前确认最后一拍时钟真的发完了（RBNE 只说明收寄存器有数据）。 */
static uint8_t gd25_wait_idle(void)
{
    return gd25_wait_flag(SPI_FLAG_TRANS, RESET);
}

static void gd25_cs_select(void)
{
    gpio_bit_reset(GD25_CS_PORT, GD25_CS_PIN);
}

static void gd25_cs_release(void)
{
    gpio_bit_set(GD25_CS_PORT, GD25_CS_PIN);
}

static uint8_t gd25_send_address(uint32_t address)
{
    uint8_t dummy;

    if(gd25_spi_byte((uint8_t)(address >> 16), &dummy) == 0U)
    {
        return 0U;
    }
    if(gd25_spi_byte((uint8_t)(address >> 8), &dummy) == 0U)
    {
        return 0U;
    }
    return gd25_spi_byte((uint8_t)address, &dummy);
}

/* ------------------------------------------------------------------ */
/* 状态寄存器                                                          */
/* ------------------------------------------------------------------ */

static uint8_t gd25_read_status(uint8_t *status)
{
    uint8_t dummy;
    uint8_t ok;

    gd25_cs_select();
    ok = gd25_spi_byte(GD25_CMD_READ_STATUS, &dummy);
    if(ok != 0U)
    {
        ok = gd25_spi_byte(0xFFU, status);
    }
    (void)gd25_wait_idle();
    gd25_cs_release();

    return ok;
}

/* WIP 为 1 时芯片内部还在写/擦，此时不能开始下一笔事务。 */
static uint8_t gd25_wait_busy(void)
{
    uint8_t status;
    uint32_t start = board_systick_ms;
    uint32_t polls = GD25_BUSY_MAX_POLLS;

    while(1)
    {
        if(gd25_read_status(&status) == 0U)
        {
            return 0U;
        }
        if((status & GD25_STATUS_WIP) == 0U)
        {
            return 1U;
        }
        if((board_systick_ms - start) >= GD25_BUSY_TIMEOUT_MS)
        {
            return 0U;
        }
        if(polls == 0U)
        {
            return 0U;
        }
        polls--;
    }
}

static uint8_t gd25_write_enable(void)
{
    uint8_t dummy;
    uint8_t status;
    uint8_t ok;

    gd25_cs_select();
    ok = gd25_spi_byte(GD25_CMD_WRITE_ENABLE, &dummy);
    (void)gd25_wait_idle();
    gd25_cs_release();

    if((ok == 0U) || (gd25_read_status(&status) == 0U))
    {
        return 0U;
    }
    /* WEL 没置起来说明命令没被接受，接下来的页写一定失败。 */
    return ((status & GD25_STATUS_WEL) != 0U) ? 1U : 0U;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

uint8_t gd25_init(void)
{
    spi_parameter_struct spi_init_handler;
    uint8_t id[3];
    uint8_t dummy;
    uint8_t ok = 1U;
    uint32_t chip_id;

    gd25_ready = 0U;

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_SPI0);

    /* CS 先写高再切成输出，避免切换成输出的那一瞬间 Flash 被意外选中。 */
    gpio_bit_set(GD25_CS_PORT, GD25_CS_PIN);
    gpio_init(GD25_CS_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GD25_CS_PIN);

    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_5);
    gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_6);
    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_7);

    /* 模式0 / 主机 / 全双工 / MSB / 8bit / 软件 NSS / 16 分频 —— 和 GD25Q32 的要求一致。 */
    spi_struct_para_init(&spi_init_handler);
    spi_init_handler.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;
    spi_init_handler.device_mode = SPI_MASTER;
    spi_init_handler.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    spi_init_handler.endian = SPI_ENDIAN_MSB;
    spi_init_handler.frame_size = SPI_FRAMESIZE_8BIT;
    spi_init_handler.nss = SPI_NSS_SOFT;
    spi_init_handler.prescale = SPI_PSC_16;
    spi_init(SPI0, &spi_init_handler);
    spi_enable(SPI0);

    /* 读 0x9F 认芯片：认不出来就当这板子上没有外部 Flash，后面所有操作直接失败。 */
    gd25_cs_select();
    if(gd25_spi_byte(GD25_CMD_READ_ID, &dummy) == 0U)
    {
        ok = 0U;
    }
    if(ok != 0U)
    {
        uint8_t i;
        for(i = 0U; i < 3U; i++)
        {
            if(gd25_spi_byte(0xFFU, &id[i]) == 0U)
            {
                ok = 0U;
                break;
            }
        }
    }
    (void)gd25_wait_idle();
    gd25_cs_release();

    if(ok == 0U)
    {
        return 0U;
    }

    chip_id = ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | (uint32_t)id[2];
    if(chip_id != GD25_CHIP_ID)
    {
        return 0U;
    }

    /* 上电时前一次擦除可能还没结束，等它退出再开放读。 */
    if(gd25_wait_busy() == 0U)
    {
        return 0U;
    }

    gd25_ready = 1U;
    return 1U;
}

uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t dummy;
    uint32_t i;
    uint8_t ok = 1U;

    if((gd25_ready == 0U) || (data == 0) || (length == 0U) ||
       (address >= BOOT_GD25_CAPACITY_BYTES) ||
       (length > (BOOT_GD25_CAPACITY_BYTES - address)))
    {
        return 0U;
    }

    gd25_cs_select();
    if(gd25_spi_byte(GD25_CMD_READ_DATA, &dummy) == 0U)
    {
        ok = 0U;
    }
    if((ok != 0U) && (gd25_send_address(address) == 0U))
    {
        ok = 0U;
    }
    for(i = 0U; (ok != 0U) && (i < length); i++)
    {
        if(gd25_spi_byte(0xFFU, &data[i]) == 0U)
        {
            ok = 0U;
        }
    }
    if(gd25_wait_idle() == 0U)
    {
        ok = 0U;
    }
    gd25_cs_release();

    return ok;
}

uint8_t gd25_invalidate(uint32_t address, uint16_t length)
{
    uint8_t dummy;
    uint16_t i;
    uint8_t ok = 1U;

    if((gd25_ready == 0U) || (length == 0U) ||
       (address >= BOOT_GD25_CAPACITY_BYTES) ||
       ((uint32_t)length > (BOOT_GD25_CAPACITY_BYTES - address)))
    {
        return 0U;
    }

    /* 一次页写不能跨 256 边界。 */
    if(((address % BOOT_GD25_PAGE_SIZE) + (uint32_t)length) > BOOT_GD25_PAGE_SIZE)
    {
        return 0U;
    }

    if(gd25_write_enable() == 0U)
    {
        return 0U;
    }

    gd25_cs_select();
    if(gd25_spi_byte(GD25_CMD_PAGE_PROGRAM, &dummy) == 0U)
    {
        ok = 0U;
    }
    if((ok != 0U) && (gd25_send_address(address) == 0U))
    {
        ok = 0U;
    }
    /* 写 0：Flash 只能把 1 写成 0，所以不管原来是什么，这一步都盖得掉。 */
    for(i = 0U; (ok != 0U) && (i < length); i++)
    {
        if(gd25_spi_byte(0x00U, &dummy) == 0U)
        {
            ok = 0U;
        }
    }
    if(gd25_wait_idle() == 0U)
    {
        ok = 0U;
    }
    gd25_cs_release();

    if(ok == 0U)
    {
        return 0U;
    }

    /* 页写命令送完不等于写完了，要等 WIP 落下去。 */
    return gd25_wait_busy();
}
