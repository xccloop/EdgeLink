#include "gd25.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"

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

void gd25_init()
{
    rcu_periph_clock_enable(RCU_GPIOB);

    gpio_init(GD25_CS_PORT,GPIO_MODE_OUT_PP, GPIO_OSPEED_50MHZ, GD25_CS_PIN);
    //只用设置CS

    //接下来就是外置flash的基础配置验证

}