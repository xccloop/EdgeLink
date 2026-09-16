#include "config.h"

/* 当前节点身份：TCP Frame 和 CAN ID 都可使用它。 */
const uint8_t board_id = 0x01U;

/* 本机测试用：EdgeHub 的局域网地址与 TCP 监听端口。 */
static const tcp_config_struct tcp_config =
{
    .wifi_ssid = "",
    .wifi_password = "",
    .server_ip = "192.168.1.112",
    .server_port = 8888
};

const tcp_config_struct *config_tcp_get(void)
{
    return &tcp_config;
}