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

    TCP 初始化需要等待 ESP-AT 和 WiFi 回应，放在 TransmitTask 内执行；这样启动时
    StorageTask 能先恢复 sequence 和历史 pending，不被网络入网时间阻塞。
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

    return INIT_ALL_SUCCESS;
}
