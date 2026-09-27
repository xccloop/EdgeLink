#ifndef KEY_H_
#define KEY_H_

#include <stdint.h>

/* KEY1=PA15, KEY2=PA8, KEY3=PC1；按键为低电平有效。 */
typedef enum
{
    KEY_EVENT_1 = 1U,   
    KEY_EVENT_2 = 2U,   
    KEY_EVENT_3 = 3U,    
} key_event_t;

void key_init(void);
void key_event_record_from_isr(key_event_t event);
uint8_t key_event_take(void);

#endif
