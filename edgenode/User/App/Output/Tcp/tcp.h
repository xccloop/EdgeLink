#ifndef TCP_H_
#define TCP_H_

#include <stdint.h>

/* WiFi账号与服务端地址由APP启动时传入，不写死在BSP或公开仓库中。 */
typedef struct
{
    const char *wifi_ssid;
    const char *wifi_password;
    const char *server_ip;
    uint16_t server_port;
} tcp_config_struct;

#define TCP_SUCCESS  1U
#define TCP_FAIL     0U

#define TCP_SEND_SUCCESS 1U
#define TCP_SEND_FAIL    0U

/* 在board_config_init()之后调用一次；函数会等待所有AT步骤完成后再返回。 */
uint8_t tcp_init(const tcp_config_struct *config);

/* tcp_init()成功返回后为TCP_SUCCESS；收到关闭事件的在线状态检测后续再单独实现。 */
uint8_t tcp_connected_get(void);

/*
    运行期链路断开后重建到服务器的 TCP 连接（只重发 CIPSTART，不重跑 CWJAP）。
    成功返回 TCP_SUCCESS 并恢复在线状态；失败返回 TCP_FAIL。
    调用者必须加冷却，否则网关不在时每次都卡在这里。
*/
uint8_t tcp_try_reconnect(void);

/* 已建连时按ESP-AT普通模式发送一段原始字节；只有收到SEND OK才报告成功。 */
uint8_t tcp_data_send(const uint8_t *data, uint8_t length);

/* wifi_connected：tcp_init()期间收到WIFI CONNECTED后为TCP_SUCCESS；重连前会先复位。 */
uint8_t wifi_state_get(void);

//获取ip
uint8_t tcp_local_ip_get(char *buffer, uint8_t size);

//获取ssid
uint8_t tcp_ssid_get(char *buffer,uint8_t size);

#endif
