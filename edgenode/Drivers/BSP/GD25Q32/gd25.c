#include "gd25.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"
#include "board_time.h"
#include "spi0_bus.h"
#include <stdint.h>

/*
    这个文件是外置flash存储芯片的BSP，要求有负责片选、读 JEDEC ID、读状态寄存器、等待忙结束、写使能、读、页写入、扇区擦除。
    硬件为PA4,5,6,7,PB12,SPI0初始化已经初始化一次了，不再赘述
    | PA5  | SCK       
    | PA6  | MISO / SO 
    | PA7  | MOSI / SI 
    | PB12 | W25Q32_CS 
*/

#define GD25_CS_PORT GPIOB
#define GD25_CS_PIN GPIO_PIN_12
/* GD25Q32一共4 MiB；一页256字节；最小擦除单位是4 KiB。 */
#define GD25Q32_CAPACITY_BYTES 0x00400000UL
#define GD25Q32_PAGE_SIZE 256U
#define GD25Q32_SECTOR_SIZE 4096U
/* 擦除时Flash会忙一段时间，两个上限防止接线异常时程序永远卡住。 */
#define GD25_BUSY_TIMEOUT_MS 500U
#define GD25_BUSY_MAX_POLLS 500000U
/* 状态寄存器中，WIP表示正在写/擦，WEL表示已经允许本次写/擦。 */
#define GD25_STATUS_WIP 0x01U
#define GD25_STATUS_WEL 0x02U

/* 只有JEDEC ID确认正确后才允许读写擦，避免接错芯片时误发擦除命令。 */
static uint8_t gd25_ready;

static void gd25_cs_select(void);
static void gd25_cs_release(void);
static uint8_t gd25_cmd_transfer(uint8_t cmd, uint8_t *data, uint32_t data_len);
static uint8_t gd25_send_byte(uint8_t send_data);
static uint8_t gd25_send_address(uint32_t address);
static uint8_t gd25_write_enable(void);
static uint8_t gd25_wait_busy(void);

uint8_t gd25_init(void)
{
    /* 重新初始化时先视为不可用，只有最后验证成功才重新开放读写擦。 */
    gd25_ready = 0U;
    rcu_periph_clock_enable(RCU_GPIOB);

    /* 先把输出数据写成高电平，再把PB12切换为输出，避免刚切换时Flash被意外选中。 */
    gpio_bit_set(GD25_CS_PORT, GD25_CS_PIN);
    gpio_init(GD25_CS_PORT,GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GD25_CS_PIN);
    //只用设置CS

    //接下来就是外置flash的基础配置验证
    /*
        然后我们开始简单介绍一下GD25Q32CSIG
        32M-bit Serial Flash 4096K-byte 256 bytes per programmable page
        我们要使用这个外置FLASH，他与BMP280不同的是，我们要获取BMP280采集数据是要读取BMP280对应寄存器的值，涉及到软件就是发送一个对应的寄存器编号
        但是外置flash更类似于一个命令返回，他相当于我们发送对应的命令然后返回一些想要的值
        具体的命令可以查看gd25q32csig_datasheet Page13-14，注意0x06H的H不是字节而是标识16进制
    */
    uint8_t chip_id_raw[3];
    if((gd25_cmd_transfer(GD25Q32_CMD_READ_ID,chip_id_raw,3) == 0U))
    {
       return 0U; 
    }
    /* 0x9F按顺序返回厂商、类型、容量三个字节，要拼成一个24位ID再比较。 */
    uint32_t chip_id = ((uint32_t)chip_id_raw[0] << 16U) |
                       ((uint32_t)chip_id_raw[1] << 8U)  |
                       (uint32_t)chip_id_raw[2];
    if(chip_id != 0xC84016)
    {
        return 0U;
    }

    /* 到这里说明SPI通信正常且芯片型号正确，后续公开读写擦函数才允许执行。 */
    gd25_ready = 1U;
    return 1U;
}

static void gd25_cs_select(void)
{
    gpio_bit_reset(GD25_CS_PORT, GD25_CS_PIN);
}

static void gd25_cs_release(void)
{
    gpio_bit_set(GD25_CS_PORT, GD25_CS_PIN);
}

static uint8_t gd25_send_byte(uint8_t send_data)
{
    uint8_t ignored_data;

    /* 写命令、地址和待写数据时，Flash返回的字节没有意义，所以只关心SPI发送是否成功。 */
    return spi0_tansfer_data(send_data, &ignored_data);
}

static uint8_t gd25_send_address(uint32_t address)
{
    /* GD25Q32使用24位地址，必须从高字节到低字节连续发送三个字节。 */
    if(gd25_send_byte((uint8_t)(address >> 16U)) == 0U)
    {
        return 0U;
    }
    if(gd25_send_byte((uint8_t)(address >> 8U)) == 0U)
    {
        return 0U;
    }
    return gd25_send_byte((uint8_t)address);
}

static uint8_t gd25_write_enable(void)
{
    uint8_t status;

    /* Flash为了防止误写，0x02和0x20之前都必须先单独发送一次0x06。 */
    if(gd25_cmd_transfer(GD25Q32_CMD_WRITE_ENABLE, 0, 0U) == 0U)
    {
        return 0U;
    }
    if(gd25_cmd_transfer(GD25Q32_CMD_READ_STATUS, &status, 1U) == 0U)
    {
        return 0U;
    }
    /* 再读一次WEL，确认芯片确实接受了写使能，而不是只相信命令已经发出。 */
    return ((status & GD25_STATUS_WEL) != 0U) ? 1U : 0U;
}

static uint8_t gd25_wait_busy(void)
{
    uint8_t status;
    uint32_t start = board_systick_ms;
    uint32_t polls = GD25_BUSY_MAX_POLLS;

    while(1)
    {
        /* WIP为1时，Flash内部还在写或擦，此时不能开始下一笔事务。 */
        if(gd25_cmd_transfer(GD25Q32_CMD_READ_STATUS, &status, 1U) == 0U)
        {
            return 0U;
        }
        if((status & GD25_STATUS_WIP) == 0U)
        {
            return 1U;
        }
        /* 正常情况下用毫秒计时退出；若SysTick没有运行，下面的轮询上限仍会生效。 */
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

/*
    这个函数用于发送命令，并读取命令返回的数据
*/
static uint8_t gd25_cmd_transfer(uint8_t cmd, uint8_t *data, uint32_t data_len)
{
    uint8_t ignored_data;
    uint8_t success = 1U;
    gd25_cs_select();
    // 先发送命令
    if(spi0_tansfer_data(cmd, &ignored_data) == 0U)
    {
        success = 0U;
    }
    else
    {
        /*
            SPI是全双工通信，因此想让GD25Q32继续发送数据，
            MCU也必须继续发送数据来产生SCLK。

            这里发送的0x00只是dummy data，
            我们真正关心的是GD25Q32返回的数据。
        */
        for(uint32_t i = 0; i < data_len; i++)
        {
            if(spi0_tansfer_data(0x00, &data[i]) == 0U)
            {
                success = 0U;
                break;
            }
        }
    }
    /* 收到最后一个字节不代表最后8个时钟已经结束，CS拉高前必须确认SPI真正空闲。 */
    if(spi0_bus_wait_idle() == 0U)
    {
        success = 0U;
    }
    gd25_cs_release();
    return success;
}

uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t success = 1U;
    uint32_t i;

    /* 不允许未初始化、越过4 MiB末尾，或没有接收数组却要求读取数据。 */
    if((gd25_ready == 0U) || (address >= GD25Q32_CAPACITY_BYTES) || (length > (GD25Q32_CAPACITY_BYTES - address)) || ((data == 0) && (length != 0U)))
    {
        return 0U;
    }
    if(length == 0U)
    {
        return 1U;
    }
    /* 读取前先确认上一笔写擦结束，避免读到Flash内部操作期间的不确定数据。 */
    if(gd25_wait_busy() == 0U)
    {
        return 0U;
    }

    /* 读数据的线序固定为：0x03 -> 3字节地址 -> 发送dummy并接收数据。 */
    gd25_cs_select();
    if((gd25_send_byte(GD25Q32_CMD_READ_DATA) == 0U) || (gd25_send_address(address) == 0U))
    {
        success = 0U;
    }
    else
    {
        for(i = 0U; i < length; i++)
        {
            if(spi0_tansfer_data(0x00U, &data[i]) == 0U)
            {
                success = 0U;
                break;
            }
        }
    }
    if(spi0_bus_wait_idle() == 0U)
    {
        success = 0U;
    }
    gd25_cs_release();
    return success;
}

uint8_t gd25_write(uint32_t address, const uint8_t *data, uint16_t length)
{
    uint8_t success = 1U;
    uint16_t i;
    uint32_t page_remaining = GD25Q32_PAGE_SIZE - (address % GD25Q32_PAGE_SIZE);

    /* 一次页写不能跨256字节页；跨页时芯片会从本页开头覆盖，所以这里直接拒绝。 */
    if((gd25_ready == 0U) || (address >= GD25Q32_CAPACITY_BYTES) || (data == 0) || (length == 0U) || (length > page_remaining) || (length > (GD25Q32_CAPACITY_BYTES - address)))
    {
        return 0U;
    }
    /* 等待旧操作完成并确认WEL后，才允许发送页写命令。 */
    if((gd25_wait_busy() == 0U) || (gd25_write_enable() == 0U))
    {
        return 0U;
    }

    /* 页写的线序固定为：0x02 -> 3字节地址 -> 本页数据；CS拉高后Flash才开始内部写入。 */
    gd25_cs_select();
    if((gd25_send_byte(GD25Q32_CMD_PAGE_PROGRAM) == 0U) || (gd25_send_address(address) == 0U))
    {
        success = 0U;
    }
    else
    {
        for(i = 0U; i < length; i++)
        {
            if(gd25_send_byte(data[i]) == 0U)
            {
                success = 0U;
                break;
            }
        }
    }
    if(spi0_bus_wait_idle() == 0U)
    {
        success = 0U;
    }
    gd25_cs_release();
    if(success == 0U)
    {
        /* SPI中途失败时，Flash可能已经收到前面部分数据；先等它停下来，不能马上重试。 */
        (void)gd25_wait_busy();
        return 0U;
    }
    /* 页写命令送完后还不能立刻继续访问，要等Flash把数据真正写进存储单元。 */
    return gd25_wait_busy();
}

uint8_t gd25_clear(uint32_t address)
{
    uint8_t success = 1U;

    /* 4 KiB擦除按扇区工作，强制地址对齐可以让调用者明确自己要擦哪一块。 */
    if((gd25_ready == 0U) || (address >= GD25Q32_CAPACITY_BYTES) || ((address % GD25Q32_SECTOR_SIZE) != 0U))
    {
        return 0U;
    }
    /* 擦除同样必须先等待不忙，再发送写使能。 */
    if((gd25_wait_busy() == 0U) || (gd25_write_enable() == 0U))
    {
        return 0U;
    }

    /* 扇区擦除只发送：0x20 -> 3字节扇区地址，不需要发送数据。 */
    gd25_cs_select();
    if((gd25_send_byte(GD25Q32_CMD_SECTOR_ERASE) == 0U) || (gd25_send_address(address) == 0U))
    {
        success = 0U;
    }
    if(spi0_bus_wait_idle() == 0U)
    {
        success = 0U;
    }
    gd25_cs_release();
    if(success == 0U)
    {
        /* 命令或地址可能已经被Flash接收，先等待可能开始的擦除结束，再把失败交给上层。 */
        (void)gd25_wait_busy();
        return 0U;
    }
    /* CS拉高后擦除才真正开始，必须等待WIP清零才算这一笔结束。 */
    return gd25_wait_busy();
}

