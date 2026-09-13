#include "init.h"
#include "board_config.h"
#include "CH340/ch340.h"
#include "LED/led.h"
#include "KEY/key.h"
#include "ADC/adc.h"
#include "CAN/can.h"
#include "BMP280/bmp280.h"
#include "IPS/ips.h"
#include "Presentation/Buffer/display_buffer.h"
#include "Output/Storage/storage.h"

/*
    这个文件服务于裸机
    用于一次性将所有初始化完成
*/

/*
    这个函数是APP层唯一的初始化入口，main()只需要调用一次init_all()。

    首先调用board_config_init()，因为它负责公共时钟、NVIC、SysTick、调试端口
    和SPI0共享总线；后续按键、串口、BMP280、GD25Q32等BSP都依赖这些公共资源。
    然后依次完成各自的外设初始化。对于返回状态的初始化函数，一旦失败就立即返回，
    这样APP不会在传感器、显示或存储尚未准备好时误进入主循环。

    tcp_init()需要由调用者传入服务器地址、端口等tcp_config_struct，并且它会负责
    esp12s_init()；所以TCP和ESP12S都不放在这个无参数总入口中，避免USART1被重复初始化。
*/
uint8_t init_all(void)
{
    /* BOARD必须最先执行：它建立所有BSP共用的时钟、中断和SPI0。 */
    board_config_init();

    ch340_init();
    led_init();
    key_init();
    adc_init();
    if(Can_init() == 0U)
    {
        return INIT_ALL_FAIL;
    }

    /* BMP280和Storage都复用SPI0，因此它们必须在board_config_init()之后初始化。 */
    if(bmp280_init() == 0U)
    {
        return INIT_ALL_FAIL;
    }

    /* display_buffer只管理APP双行缓冲，前提是IPS的SPI2和DMA已经成功初始化。 */
    if(ips_init() == IPS_FAIL)
    {
        return INIT_ALL_FAIL;
    }
    if(display_buffer_init() == DISPLAY_BUFFER_FAIL)
    {
        return INIT_ALL_FAIL;
    }

    /* Storage会验证GD25Q32并寻找新日志扇区，失败时不能继续追加历史数据。 */
    if(storage_init() == STORAGE_FAIL)
    {
        return INIT_ALL_FAIL;
    }

    return INIT_ALL_SUCCESS;
}
