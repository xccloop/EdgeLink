#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>

void Can_init();
uint8_t can0_data_send(uint16_t standard_id, const uint8_t *data, uint8_t data_length);

#endif
