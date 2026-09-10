#include "board_time.h"

/*
    这个文件我们来配置一些关于延迟的相关函数
*/

volatile uint32_t board_systick_ms = 0;

/*
    SysTick中断服务函数：为后续延时、超时检测提供毫秒计数。
    这里的中断用的是内核自带的 SysTick 系统定时器，在当前 72 MHz 下，它每计数 72000 个内核时钟就产生一次中断，也就是每 1 ms 进入一次
    那么board_systick_ms增加一次就代表1ms过去
    这也是精确获得延迟时间的做法
*/
void SysTick_Handler(void)
{
    board_systick_ms++;
}

void delay_ms(uint32_t ms)
{
    uint32_t start = board_systick_ms;

    while ((board_systick_ms - start) < ms)
    {
    }
}