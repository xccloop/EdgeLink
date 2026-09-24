#ifndef HMI_TYPES_H_
#define HMI_TYPES_H_
#include <stdint.h>
#define HMI_WIDTH 320U
#define HMI_HEIGHT 240U
#define HMI_NAV_Y 212U
#define HMI_SSID_CAPACITY 33U
#define HMI_IP_CAPACITY 16U
#define HMI_LOG_CAPACITY 257U
typedef enum { HMI_PAGE_HOME = 0, HMI_PAGE_LINKS, HMI_PAGE_LOG, HMI_PAGE_COUNT } hmi_page_id_t;
typedef enum { HMI_LINK_UNKNOWN = 0, HMI_LINK_ONLINE, HMI_LINK_OFFLINE } hmi_link_state_t;
typedef enum { HMI_KEY_PREVIOUS = 0, HMI_KEY_NEXT, HMI_KEY_CONFIRM } hmi_key_t;
/*
    HMI只接收显示快照，不读取BMP280、ESP或Flash。
    温度以0.1摄氏度为单位，246表示24.6；valid为0时显示--.-。
    SSID是实际连接的Wi-Fi名称，local_ip是节点地址而不是服务端地址。
    链路状态由调用方根据最近通信结果提供；UNKNOWN不等于OFFLINE。
    字符串为内嵌数组，接收时有界复制，不保留调用方的指针。
*/
typedef struct
{
    uint8_t node_id;
    uint8_t temperature_valid;
    int16_t temperature_deci_c;
    hmi_link_state_t wifi_state, tcp_state, can_state;
    char wifi_ssid[HMI_SSID_CAPACITY];
    char local_ip[HMI_IP_CAPACITY];
    char latest_log[HMI_LOG_CAPACITY];
    uint32_t log_uptime_seconds;
} hmi_view_data_t;
#endif
