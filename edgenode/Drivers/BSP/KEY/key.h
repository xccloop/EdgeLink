#ifndef KEY_H_
#define KEY_H_

#include <stdint.h>

/* KEY1=PA15, KEY2=PA8, KEY3=PC1；按键为低电平有效。 */
typedef enum
{
    KEY_EVENT_1 = 1U,
    KEY_EVENT_2,
    KEY_EVENT_3
} key_event_t;

void key_init(void);

/*
    EXTI只调用key_event_record_from_isr()记录一次边沿，不打印、不绘制、
    不调用FreeRTOS。key_event_take()由唯一消费者任务调用，返回位图：
    bit0=KEY1、bit1=KEY2、bit2=KEY3；读取后清空已取出的事件。
*/
void key_event_record_from_isr(key_event_t event);
uint8_t key_event_take(void);

#endif
