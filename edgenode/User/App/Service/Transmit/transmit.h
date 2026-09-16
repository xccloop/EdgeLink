#ifndef TRANSMIT_H_
#define TRANSMIT_H_

#include <stdint.h>
#include "Model/message.h"


uint8_t tcp_frame_transmit(telemetry_sample_struct *message,uint16_t sequence);
uint8_t can_frame_transmit(uint8_t node_id,uint16_t sequence,const telemetry_sample_struct *message);

#define TCP_TRANSMIT_FAIL 0U
#define TCP_TRANSMIT_SUCCESS 1U
#define CAN_TRANSMIT_FAIL 0U
#define CAN_TRANSMIT_SUCCESS 1U

#endif
