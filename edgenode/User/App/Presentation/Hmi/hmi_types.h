#ifndef HMI_TYPES_H_
#define HMI_TYPES_H_

#include <stdint.h>

/*
    判断标准是"横还是竖"，不是"哪个更长"。
    一行是横着走的，所以"一行有多少个格子"用的是 HMI_WIDTH。
    自检：HMI_WIDTH * 2 必须等于 640，否则就是名字配错了。
*/
#define HMI_WIDTH  320U
#define HMI_HEIGHT 240U

#define HMI_SSID_CAPACITY 33U     /* 32 个字符 + 1 个结束标记 */
#define HMI_IP_CAPACITY   16U     /* 15 个字符 + 1 个结束标记 */
#define HMI_LOG_CAPACITY  257U    /* 256 个字符 + 1 个结束标记 */

/* 页面编号。HMI_PAGE_COUNT 只用来数"一共几页"。 */
typedef enum
{
    HMI_PAGE_HOME = 0,
    HMI_PAGE_LINKS,
    HMI_PAGE_LOG,
    HMI_PAGE_COUNT
} hmi_page_id_t;

/* UNKNOWN 不等于 OFFLINE：前者是"还没得到结果"，后者是"确认失败了"。 */
typedef enum
{
    HMI_LINK_UNKNOWN = 0,
    HMI_LINK_ONLINE,
    HMI_LINK_OFFLINE
} hmi_link_state_t;

/*
    HMI 只接收显示快照，不自己去读 BMP280 / ESP / Flash，
    也不自行探测或推断链路状态。

    温度单位 0.1°C：246 表示 24.6，-1 表示 -0.1；
    temperature_valid 为 0 时界面显示 --.-（不是 0°C，是"没有数据"）。

    wifi_ssid 是实际连上的 Wi-Fi 名称；local_ip 是节点自己的地址，不是服务端的。
    字符串都是内嵌数组，接收时有界复制，不保留调用方的指针。
*/
typedef struct
{
    uint8_t          node_id;
    uint8_t          temperature_valid;   /* 0 = 没有有效温度，显示 --.- */
    int16_t          temperature_deci_c;  /* 0.1 °C，有符号 */
    hmi_link_state_t wifi_state;
    hmi_link_state_t tcp_state;
    hmi_link_state_t can_state;
    char             wifi_ssid[HMI_SSID_CAPACITY];
    char             local_ip[HMI_IP_CAPACITY];
    char             latest_log[HMI_LOG_CAPACITY];
    uint32_t         log_uptime_seconds;  /* 日志产生时的开机时长，小时可超过 24 */
} hmi_view_data_t;

#endif
