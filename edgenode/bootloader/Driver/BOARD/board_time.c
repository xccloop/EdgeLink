#include "board_time.h"
#include "gd32f10x.h"

volatile uint32_t board_systick_ms = 0;

/*
    SysTick 中断：只做一件事 —— 把毫秒计数加一。

    应用那份在这里还要判断 FreeRTOS 调度器起没起来，再决定是自己计时
    还是转交给 FreeRTOS 的 tick 处理。Bootloader 没有 RTOS，没有这个分叉。

    这个中断不会带到应用里去：boot_jump_to_vector() 跳转前会关掉 SysTick
    并清空 NVIC，应用的 SystemInit() 之后会重新配置属于自己的计时。
*/
void SysTick_Handler(void)
{
    board_systick_ms++;
}

void board_systick_init(void)
{
    /* SystemCoreClock / 1000 = 每 1ms 中断一次。 */
    (void)SysTick_Config(SystemCoreClock / 1000U);
}

void delay_ms(uint32_t ms)
{
    uint32_t start = board_systick_ms;

    /*
        无符号减法，所以即使 board_systick_ms 溢出回绕，
        (当前 - 起点) 仍然等于真正过去的时间。
    */
    while ((board_systick_ms - start) < ms)
    {
    }
}
