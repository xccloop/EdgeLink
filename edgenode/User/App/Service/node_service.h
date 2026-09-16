#ifndef NODE_SERVICE_H_
#define NODE_SERVICE_H_

#include <stdint.h>

/* 主循环每次调用一次：采集一条Message并交给当前已启用的输出分支。 */
uint8_t node_service_run_once();

#endif
