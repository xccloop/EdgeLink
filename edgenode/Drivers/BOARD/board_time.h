#ifndef BOARD_TIME_H_
#define BOARD_TIME_H_

#include <stdint.h>

/* SysTick每1ms加一，可作为全局毫秒计时基准。 */
extern volatile uint32_t board_systick_ms;

void delay_ms(uint32_t ms);

#endif
