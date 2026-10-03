#ifndef EXTERNAL_FLASH_LAYOUT_H_
#define EXTERNAL_FLASH_LAYOUT_H_

#include "GD25Q32/gd25.h"

/* GD25Q32 project partition layout. End addresses are exclusive. */
#define LOG_BASE_ADDRESS 0x000000UL
#define LOG_END_ADDRESS  0x003BF000UL

#define OTA_RELAY_BASE_ADDRESS   LOG_END_ADDRESS
#define OTA_RELAY_END_ADDRESS    GD25Q32_CAPACITY_BYTES

#endif
