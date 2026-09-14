//这个项目位采集节点的设计，还是一样，先做硬件基础然后有了基础才可以构建应用层内容
//我们先在BSP中实现我们要实现的外设，包括LED,KEY,ESP-12S,GD25Q32,ADC,CAN,IPS,BMP280

#include "CH340/ch340.h"
#include "board_config.h"
#include "board_time.h"
#include "GD25Q32/gd25.h"
#include "gd32f10x_gpio.h"

#include <stdint.h>
#include <stdio.h>

/*
    现在让我们尝试完整的数据链路，不加入HMI,RS485,FAN
    我们来梳理一下
    数据链路从BMP280原始温度获取，进入message统一内部模型
    此时分三路一路用TCP通讯，一路用CAN通讯，一路给flash进行存储
    由于上发的数据都是给edgehub，我们从节点侧注意先用TCP，再用CAN
    两路都用怕引起数据重复
*/

int main()
{
    
    setvbuf(stdout, NULL, _IONBF, 0);   /* 关掉缓冲：每个字节立刻经 _write 发出 */
    printf("\nEdgenode start\n");

    /* BOARD先建立SysTick与SPI0共享总线；BMP280随后才能开始SPI事务。 */
    board_config_init();
    ch340_init();
    
    gpio_bit_set(GPIOA, GPIO_PIN_4);

    gd25_init();
    
    uint32_t address = 0x00000000UL;
    uint8_t write_data[16] =
    {
        0x12, 0x34, 0x56, 0x78,
        0xA5, 0x5A, 0x00, 0xFF,
        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88
    };
    uint8_t read_data[16];
    uint8_t i;

    if (gd25_clear(address) == 0U) {
    printf("erase fail\r\n");
    return 0;
    }

        /* 擦除验证：擦后应全为 0xFF。 */
        if (gd25_read(address, read_data, sizeof(read_data)) == 0U) {
            printf("erase read fail\r\n");
            return 0;
        }
        for (i = 0U; i < sizeof(read_data); i++) {
            if (read_data[i] != 0xFFU) {
                printf("erase verify fail: index=%u data=%02X\r\n", i, read_data[i]);
                return 0;
            }
        }

        if (gd25_write(address, write_data, sizeof(write_data)) == 0U) {
        printf("write fail\r\n");
        return 0;
    }

    if (gd25_read(address, read_data, sizeof(read_data)) == 0U) {
        printf("read fail\r\n");
        return 0;
    }

    for (i = 0U; i < sizeof(write_data); i++) {
        if (read_data[i] != write_data[i]) {
            printf("compare fail: index=%u write=%02X read=%02X\r\n",
                i, write_data[i], read_data[i]);
            return 0;
        }
    }

    printf("GD25 erase/write/read PASS\r\n");


    while(1)
    {

        delay_ms(200);
    }
}
