#ifndef BOARD_DEBUG_H_
#define BOARD_DEBUG_H_

/*
    释放被 JTAG 占用的引脚。

    PB3 复位后默认是 JTDO，而 IPS 用的 SPI2_SCK 正好也是 PB3。
    不关掉 JTAG，SPI2 的时钟就永远出不来 —— 屏幕全黑，而且
    ips_init() 反而不一定报错，因为 SPI 硬件本身配置是"成功"的。

    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE) 关的是 JTAG-DP，
    SW-DP（也就是 SWD 下载调试）保留 —— 所以烧录和调试都不受影响。

    应用里这件事由 Drivers/BOARD/board_config.c 的 board_debug_config() 完成，
    它是 board_config_init() 的一部分。Bootloader 不调 board_config_init()
    （那里还有 NVIC、ADC 时钟、SPI0 总线，都用不上），
    但这一条是 IPS 能工作的前提，必须自己补上。

    调用时机：ips_init() 之前。
*/
void board_debug_init(void);

#endif
