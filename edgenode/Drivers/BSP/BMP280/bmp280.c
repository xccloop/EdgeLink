#include "bmp280.h"
#include "board_time.h"
#include "gd32f10x_gpio.h"
#include "spi0_bus.h"
#include <stdint.h>

/*
    这个文件我们来实现BMP280，硬件连接为，PA4,5,6,7,值得注意的是后续我们编写的外置FLASH也是基于PA5,6,7的SPI进行通讯的，这里会涉及到相关的冲突
    | PA4  | BMP280_CS 
    | PA5  | SCK       
    | PA6  | MISO / SO 
    | PA7  | MOSI / SI 

    PA4 20 I/O  
    Default: PA4 
    Alternate: SPI0_NSS, USART1_CK, ADC01_IN4, 
    DAC0_OUT0(4) 
    Remap:SPI2_NSS(4), I2S2_WS(4) 
    PA5 21 I/O  Default: PA5 
    Alternate: SPI0_SCK, ADC01_IN5, DAC0_OUT1(4) 
    PA6 22 I/O  
    Default: PA6 
    Alternate: SPI0_MISO, ADC01_IN6, TIMER2_CH0, 
    TIMER7_BRKIN(4), TIMER12_CH0(3) 
    Remap: TIMER0_BRKIN 
    PA7 23 I/O  
    Default: PA7 
    Alternate: SPI0_MOSI, ADC01_IN7, TIMER2_CH1, 
    TIMER7_CH0_ON(4), TIMER13_CH0(3) 
    Remap: TIMER0_CH0_ON 
*/

#define BMP280_SPI_PORT GPIOA
#define BMP280_CS GPIO_PIN_4
#define BMP280_STATUS_IM_UPDATE 0x01U
#define BMP280_STATUS_MEASURING 0x08U
#define BMP280_CTRL_MEAS_TEMP_X1_NORMAL 0x23U
#define BMP280_STATUS_TIMEOUT_MS 20U

typedef struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    int32_t  t_fine;
} bmp280_calib_t;

/* 修改：校准参数只在初始化时读取一次，后续每次读取温度直接复用。 */
static bmp280_calib_t bmp280_calib;
/* 修改：初始化失败时禁止把旧校准值用于新的温度计算。 */
static uint8_t bmp280_is_initialized;

static void bmp280_cs_select(void);
static uint8_t bmp280_cs_release(void);
static uint8_t bmp280_data_get(uint8_t reg, uint8_t *data);
static uint8_t bmp280_reg_data_get(uint8_t reg, uint8_t *data, uint8_t length);
static uint8_t bmp280_register_write(uint8_t reg, uint8_t data);
static uint8_t bmp280_status_wait_clear(uint8_t mask, uint32_t timeout_ms);
static uint16_t bmp280_u16_from_le(const uint8_t *data);
static uint8_t bmp280_calibration_read(void);
static float bmp280_temperature_calculate(const bmp280_calib_t *calib, uint32_t temperature_raw);

uint8_t bmp280_init(void)
{
    uint8_t chip_id;
    /*
        在此，我们来学习什么是SPI
        SPI有四根线，分别是CS,SCK,MISO,MOSI，SPI通信的时候我们称一端位主设备，一端为从设备，一个主设备能够拥有多个从设备，
        主设备是定义SCK也就是统一时钟的设备，在我们的项目中，自然就是MCU，从设备连接主设备在SCK,MOSI,MISO采用总线制连接，唯一需要单独连接的为CS片选线
        当主设备需要与某个从设备进行通信的时候，会将对应从设备的CS线拉低选中从设备进行通信，MOSI是主设备向从设备发送数据的线，MISO是从设备向主设备发送数据的线
        由此我们可以看出，双方通信互不打扰设计为推挽输出也是可以的这点和IIC不同
        关于SCK，他引出SPI的四种工作模式，我们知道，SCK发送的时钟是方波自然有上升沿和下降沿，那如何规定数据是在上升沿的时候发送一位还是下降沿的时候发送呢？
        我们举模式0为例子:时钟空闲时为低电平，数据在时钟的上升沿采样，在下降沿变化。
        空闲为低电平那也就是说时钟有信号的部分是先上升沿到1然后平稳然后下降沿到0
        这句话的意思就是，如果要发送 1，那么发送方会在采样沿到来之前就把数据线置为 1；到上升沿时，接收方读取这个 1；随后在下降沿，发送方再把数据线切换成下一位的数据。
        再比如模式1：时钟空闲时为低电平，数据在时钟的下降沿采样，在上升沿变化。
        你会发现对于这种模式的第一次时钟，数据会先发送而不会先进行采样，这是这种模式的特点，这同时也要求我们在数据发送之前就先进行采样确认
        这也是为什么 SPI 要有半个周期的间隔：给线路上的电压足够时间稳定。

    */
    gpio_bit_set(BMP280_SPI_PORT, BMP280_CS);
    gpio_init(BMP280_SPI_PORT,GPIO_MODE_OUT_PP,GPIO_OSPEED_50MHZ,BMP280_CS);
    //SPI0的PA5、PA6、PA7由spi0_bus_init()统一配置，BMP280只配置自己的CS。
    //因此SPI配置移动到了spi0_bus中

    //CS拉低进行通讯，我们在初始化拉高防止一些特殊情况将BMP选中
    /* 修改：每次初始化先清除上一次成功状态，失败时不会保留旧校准参数。 */
    bmp280_is_initialized = 0U;
    gpio_bit_set(BMP280_SPI_PORT, BMP280_CS);

    /* 修改：chip_id是本驱动的第一层运行证据；不是0x58就不继续读校准或温度。 */
    if ((bmp280_data_get(BMP280_REG_ID, &chip_id) == 0U) || (chip_id != BMP280_CHIP_ID)) {
        return 0U;
    }

    /* 修改：上电时NVM可能仍在复制校准参数，完成前不能读取0x88开始的校准数据。 */
    if (bmp280_status_wait_clear(BMP280_STATUS_IM_UPDATE, BMP280_STATUS_TIMEOUT_MS) == 0U) {
        return 0U;
    }
    if (bmp280_calibration_read() == 0U) {
        return 0U;
    }

    /*
        修改：0x23 = 温度过采样x1 + 压力跳过 + normal模式。
        复位后的ctrl_meas为0，温度测量被跳过；这里配置后0xFA~0xFC才会持续更新。
    */
    if (bmp280_register_write(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_TEMP_X1_NORMAL) == 0U) {
        return 0U;
    }

    /* 修改：等待首个x1温度转换完成，避免把复位/旧数据作为第一笔有效温度。 */
    delay_ms(5U);
    if (bmp280_status_wait_clear(BMP280_STATUS_MEASURING, BMP280_STATUS_TIMEOUT_MS) == 0U) {
        return 0U;
    }

    bmp280_is_initialized = 1U;
    return 1U;
}

static void bmp280_cs_select()
{
    gpio_bit_reset(BMP280_SPI_PORT, BMP280_CS);
}

static uint8_t bmp280_cs_release(void)
{
    uint8_t idle;

    /* 修改：最后一个SCK结束后再释放CS，保证BMP280完整接收最后一位数据。 */
    idle = spi0_bus_wait_idle();
    gpio_bit_set(BMP280_SPI_PORT, BMP280_CS);
    return idle;
}

/*
    单寄存器读取：CS拉低后，第一字节是“读reg”的控制字节；第二个0x00只用来
    产生时钟，返回值才是reg中的数据。
*/
static uint8_t bmp280_data_get(uint8_t reg, uint8_t *data)
{
    uint8_t ignored_data;
    uint8_t success = 1U;

    bmp280_cs_select();
    if (spi0_tansfer_data(reg | 0x80U, &ignored_data) == 0U) {
        success = 0U;
    } else if (spi0_tansfer_data(0x00U, data) == 0U) {
        success = 0U;
    }

    /* 修改：旧代码即使SPI卡住也会把某个字节当寄存器值继续计算；现在失败会向上返回。 */
    if (bmp280_cs_release() == 0U) {
        success = 0U;
    }
    return success;
}

/*
    修改：连续读只能有一对CS边界。BMP280在读命令后会自动递增地址，
    因而一个命令加length个dummy即可依次读回reg、reg+1……的数据。
*/
static uint8_t bmp280_reg_data_get(uint8_t reg, uint8_t *data, uint8_t length)
{
    uint8_t i;
    uint8_t ignored_data;
    uint8_t success = 1U;

    bmp280_cs_select();
    if (spi0_tansfer_data(reg | 0x80U, &ignored_data) == 0U) {
        success = 0U;
    }
    for (i = 0U; (i < length) && (success != 0U); i++) {
        if (spi0_tansfer_data(0x00U, &data[i]) == 0U) {
            success = 0U;
        }
    }
    if (bmp280_cs_release() == 0U) {
        success = 0U;
    }
    return success;
}

static uint8_t bmp280_register_write(uint8_t reg, uint8_t data)
{
    uint8_t ignored_data;
    uint8_t success = 1U;

    /* 修改：写操作的bit7必须为0，控制字节后紧接着发送写入的数据。 */
    bmp280_cs_select();
    if (spi0_tansfer_data(reg & 0x7FU, &ignored_data) == 0U) {
        success = 0U;
    } else if (spi0_tansfer_data(data, &ignored_data) == 0U) {
        success = 0U;
    }
    if (bmp280_cs_release() == 0U) {
        success = 0U;
    }
    return success;
}

static uint8_t bmp280_status_wait_clear(uint8_t mask, uint32_t timeout_ms)
{
    uint8_t status;

    /*
        修改：状态轮询有明确超时；传感器异常或接线故障时bmp280_init()会失败，
        而不是让CPU永远停在while循环里。
    */
    while (1) {
        if (bmp280_data_get(BMP280_REG_STATUS, &status) == 0U) {
            return 0U;
        }
        if ((status & mask) == 0U) {
            return 1U;
        }
        if (timeout_ms == 0U) {
            return 0U;
        }
        delay_ms(1U);
        timeout_ms--;
    }
}

/* 这个函数是辅助calib数据移动到bmp280_calib中的。 */
static uint16_t bmp280_u16_from_le(const uint8_t *data)
{
    return ((uint16_t)data[1] << 8) | data[0];
}

static uint8_t bmp280_calibration_read(void)
{
    uint8_t calib[BMP280_CALIB_LENGTH];

    if (bmp280_reg_data_get(BMP280_REG_CALIB_START, calib, BMP280_CALIB_LENGTH) == 0U) {
        return 0U;
    }
    bmp280_calib.dig_T1 = bmp280_u16_from_le(&calib[0]);
    bmp280_calib.dig_T2 = (int16_t)bmp280_u16_from_le(&calib[2]);
    bmp280_calib.dig_T3 = (int16_t)bmp280_u16_from_le(&calib[4]);
    bmp280_calib.dig_P1 = bmp280_u16_from_le(&calib[6]);
    bmp280_calib.dig_P2 = (int16_t)bmp280_u16_from_le(&calib[8]);
    bmp280_calib.dig_P3 = (int16_t)bmp280_u16_from_le(&calib[10]);
    bmp280_calib.dig_P4 = (int16_t)bmp280_u16_from_le(&calib[12]);
    bmp280_calib.dig_P5 = (int16_t)bmp280_u16_from_le(&calib[14]);
    bmp280_calib.dig_P6 = (int16_t)bmp280_u16_from_le(&calib[16]);
    bmp280_calib.dig_P7 = (int16_t)bmp280_u16_from_le(&calib[18]);
    bmp280_calib.dig_P8 = (int16_t)bmp280_u16_from_le(&calib[20]);
    bmp280_calib.dig_P9 = (int16_t)bmp280_u16_from_le(&calib[22]);
    return 1U;
}

/* 根据BMP280数据手册的温度补偿公式，返回单位：℃。 */
static float bmp280_temperature_calculate(const bmp280_calib_t *calib, uint32_t temperature_raw)
{
    float var1;
    float var2;

    var1 = ((float)temperature_raw / 16384.0f - (float)calib->dig_T1 / 1024.0f)
         * (float)calib->dig_T2;
    var2 = ((float)temperature_raw / 131072.0f - (float)calib->dig_T1 / 8192.0f);
    var2 = var2 * var2 * (float)calib->dig_T3;
    return (var1 + var2) / 5120.0f;
}

/* 这个函数是我们使用BMP280的主要功能用于读取温度。 */
float bmp280_temperature_get(void)
{
    uint8_t temperature_data[3];
    uint32_t temperature_raw;

    /* 修改：未通过bmp280_init()时不使用未验证的校准参数。 */
    if (bmp280_is_initialized == 0U) {
        return BMP280_TEMPERATURE_ERROR;
    }

    /* 修改：0xFA、0xFB、0xFC必须作为一笔连续读取，避免在CS低时重复发读命令。 */
    if (bmp280_reg_data_get(BMP280_REG_TEMP_MSB, temperature_data, 3U) == 0U) {
        /* 修改：旧代码在传输失败后仍会拼接残缺数据并返回伪温度。 */
        return BMP280_TEMPERATURE_ERROR;
    }

    /* 修改：XLSB只有bit7..4有效，右移4位后才是adc_T的bit3..0。 */
    temperature_raw = ((uint32_t)temperature_data[0] << 12)
                    | ((uint32_t)temperature_data[1] << 4)
                    | ((uint32_t)temperature_data[2] >> 4);

    return bmp280_temperature_calculate(&bmp280_calib, temperature_raw);
}
