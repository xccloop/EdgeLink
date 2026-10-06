#include "board_debug.h"
#include "gd32f10x.h"
#include "gd32f10x_gpio.h"
#include "gd32f10x_rcu.h"

/*
    只做一件事：把 JTAG 关掉、SWD 留着。

    这样 PB3（JTDO）被释放出来，才能当 SPI2_SCK 驱动 IPS。
*/
void board_debug_init(void)
{
    rcu_periph_clock_enable(RCU_AF);

    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
}
