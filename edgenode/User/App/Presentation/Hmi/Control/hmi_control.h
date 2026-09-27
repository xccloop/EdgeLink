#ifndef HMI_CONTROL_H_
#define HMI_CONTROL_H_

#include <stdint.h>
void hmi_control_init(uint8_t node_id);

typedef enum
{
    HMI_KEY_PREVIOUS = 0U,
    HMI_KEY_NEXT,
    HMI_KEY_CONFIRM
} hmi_key_t;
//这里负责按键切换
void hmi_key_event(hmi_key_t key);

uint8_t hmi_service(void);

#endif