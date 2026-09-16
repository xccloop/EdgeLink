#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>
#include "Output/Tcp/tcp.h"

extern const uint8_t board_id;

const tcp_config_struct *config_tcp_get(void);

#define NODE_SAMPLE_PERIOD_MS  1000U

#endif