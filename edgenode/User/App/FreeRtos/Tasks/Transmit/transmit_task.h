#ifndef TRANSMIT_TASK_H_
#define TRANSMIT_TASK_H_

#include "FreeRTOS.h"
#include "task.h"
#include "Presentation/Hmi/hmi_types.h"
#include <stdint.h>

extern TaskHandle_t transmit_task_handle;
extern volatile uint8_t link_restored;
extern volatile hmi_link_state_t can_state;
void transmit_task_create(void);

#endif
