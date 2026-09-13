#ifndef CONFIG_H_
#define CONFIG_H_

//这里是配置应用层的基础配置，比如wifi连接的热点，板卡id等等等等

#include <stdint.h>
extern volatile uint8_t board_id;

/*
    一条Message的采样间隔由APP策略决定，而不是由Storage决定。
    默认每秒记录一次；需要改变采样频率时只修改这里，TCP/CAN/HMI会复用同一节奏。
*/
#define NODE_SAMPLE_PERIOD_MS  1000U

#endif
