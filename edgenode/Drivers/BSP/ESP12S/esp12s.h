#ifndef ESP12S_H_
#define ESP12S_H_

#include <stdint.h>

void esp12s_init();
void esp12s_cmd_send(const char *data);
void esp12s_data_send(const uint8_t *data, uint8_t length);

//以下为ESP12S的常用AT指令
/* 基础测试 */
#define ESP_AT_TEST              "AT\r\n"
#define ESP_AT_RST               "AT+RST\r\n"

/* 查询固件版本 */
#define ESP_AT_GMR               "AT+GMR\r\n"

/* WiFi 工作模式 */
#define ESP_AT_CWMODE_GET        "AT+CWMODE?\r\n"
#define ESP_AT_CWMODE_SET        "AT+CWMODE="

#define ESP_WIFI_MODE_STA        1
#define ESP_WIFI_MODE_AP         2
#define ESP_WIFI_MODE_STA_AP     3

/* 连接 WiFi */
#define ESP_AT_CWJAP_GET         "AT+CWJAP?\r\n"
#define ESP_AT_CWJAP_SET         "AT+CWJAP="

/* 断开 WiFi */
#define ESP_AT_CWQAP             "AT+CWQAP\r\n"

/* 查询 IP */
#define ESP_AT_CIFSR             "AT+CIFSR\r\n"

/* 查询连接状态 */
#define ESP_AT_CIPSTATUS         "AT+CIPSTATUS\r\n"

/* 单连接 / 多连接 */
#define ESP_AT_CIPMUX_GET        "AT+CIPMUX?\r\n"
#define ESP_AT_CIPMUX_SET        "AT+CIPMUX="

#define ESP_CIPMUX_SINGLE        0
#define ESP_CIPMUX_MULTI         1

/* 建立 TCP / UDP 连接 */
#define ESP_AT_CIPSTART          "AT+CIPSTART="

#define ESP_PROTOCOL_TCP         "TCP"
#define ESP_PROTOCOL_UDP         "UDP"

/* 发送数据 */
#define ESP_AT_CIPSEND           "AT+CIPSEND="

/* 关闭连接 */
#define ESP_AT_CIPCLOSE          "AT+CIPCLOSE\r\n"

/* 查询本机 IP */
#define ESP_AT_CIPSTA_GET        "AT+CIPSTA?\r\n"

/* 自动连接 */
#define ESP_AT_CWAUTOCONN_SET    "AT+CWAUTOCONN="

#define ESP_AUTOCONN_DISABLE     0
#define ESP_AUTOCONN_ENABLE      1

/* 透传模式 */
#define ESP_AT_CIPMODE_SET       "AT+CIPMODE="

#define ESP_NORMAL_MODE          0
#define ESP_TRANSPARENT_MODE     1
#endif
