#ifndef BOARD_TIME_H_
#define BOARD_TIME_H_

#include <stdint.h>

/*
    毫秒时间基准 —— IPS 驱动要用（delay_ms 和 DMA 超时判断）。

    和应用里的 Drivers/BOARD/board_time.c 是同一套接口，但实现简单得多：

        应用：调度器未启动 → 忙等；调度器已启动 → vTaskDelay 让出 CPU
        Bootloader：没有 RTOS，只有忙等这一种

    为什么 Bootloader 只在"要卡住"时才初始化 IPS：
        ips_init() 里有约 490ms 的固定延时（屏幕复位 + 初始化序列）。
        每次上电都点屏 = 每次启动多花半秒，还会闪一下。
        只在 boot_halt() 里点屏，正常启动完全不受影响。
*/
extern volatile uint32_t board_systick_ms;

/* 启动 SysTick（每 1ms 中断一次）。IPS 初始化之前必须调用。 */
void board_systick_init(void);

/* 忙等 ms 毫秒。 */
void delay_ms(uint32_t ms);

#endif
