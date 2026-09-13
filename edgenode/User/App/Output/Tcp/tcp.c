#include "tcp.h"
#include "ESP12S/esp12s.h"
#include "INTERRUPT/USART/usart.h"
#include "board_time.h"

#include <stdio.h>

/*
    这里我们先把TCP初始化中的代码来源分清楚，避免后续学习时把所有内容都理解成ESP12S固定要求。
    AT、CWMODE、CWJAP、CIFSR、CIPMUX、CIPMODE和CIPSTART是按照ESP-AT建立单连接普通模式TCP的命令流程，
    esp12s_init()和esp12s_cmd_send()是调用现有BSP的接口；它们是这次初始化真正需要做的事情。
    tcp_command_send_and_wait_ok()和tcp_wait_response()不是ESP12S的API，而是为了让上面的命令能一条一条顺序完成：
    发送后就停在这里等待，成功才执行下一条，失败或超时就立刻从tcp_init()返回。这样APP调用一次tcp_init()，
    从上到下读到的就是实际AT流程，不需要TCP_STATE和tcp_service()这套非阻塞状态机。
    USART1中断仍然需要逐字符拼出OK、ERROR、CONNECT等完整回应，因为串口不会一次送来完整字符串；中断把已经识别的结果
    放进回应队列，等待函数再顺序取出。这个队列不是为了把初始化改复杂，而是避免ESP连续返回CONNECT和OK时前一个结果被覆盖。
    后续移植FreeRTOS后，当前while等待可以替换为“任务等待通知或队列”，初始化代码的AT命令顺序仍然可以保持不变。
*/

#define TCP_AT_TIMEOUT_MS       10000U
#define TCP_WIFI_JOIN_TIMEOUT_MS 30000U
#define TCP_COMMAND_BUFFER_SIZE 128U

static char tcp_command_buffer[TCP_COMMAND_BUFFER_SIZE];
static uint8_t tcp_connected;

static uint8_t tcp_deadline_expired(uint32_t deadline_ms)
{
    return ((uint32_t)(board_systick_ms - deadline_ms) < 0x80000000U) ? TCP_SUCCESS : TCP_FAIL;
}

/*
    等待一条命令的最终结果。普通命令只需要OK；CIPSTART除了OK，还必须等到CONNECT。
    WIFI CONNECTED和WIFI GOT IP是连接过程提示，不是本步骤的最终成功结果，所以这里继续等待OK。
*/
static uint8_t tcp_wait_response(uint8_t wait_connect, uint32_t timeout_ms)
{
    uint32_t deadline_ms = board_systick_ms + timeout_ms;
    uint8_t connect_received = TCP_FAIL;
    uint8_t ok_received = TCP_FAIL;
    esp12s_response_t response;

    while(tcp_deadline_expired(deadline_ms) == TCP_FAIL)
    {
        if(esp12s_response_get(&response) == 0U)
        {
            continue;
        }

        if((response == ESP12S_RESPONSE_ERROR) || (response == ESP12S_RESPONSE_FAIL) ||
           (response == ESP12S_RESPONSE_CLOSED) || (response == ESP12S_RESPONSE_BUSY))
        {
            return TCP_FAIL;
        }

        if(wait_connect == TCP_FAIL)
        {
            if(response == ESP12S_RESPONSE_OK)
            {
                return TCP_SUCCESS;
            }
            continue;
        }

        if(response == ESP12S_RESPONSE_CONNECT)
        {
            connect_received = TCP_SUCCESS;
        }
        else if(response == ESP12S_RESPONSE_OK)
        {
            ok_received = TCP_SUCCESS;
        }

        if((connect_received == TCP_SUCCESS) && (ok_received == TCP_SUCCESS))
        {
            return TCP_SUCCESS;
        }
    }

    return TCP_FAIL;
}

/*
    每发一条新命令都先丢弃旧回应，避免上一条命令残留的OK被误用。
    随后直接等待这条命令的结果，因此这里是阻塞点；等待期间USART1中断和SysTick中断仍能正常运行。
*/
static uint8_t tcp_command_send_and_wait_ok(const char *command, uint32_t timeout_ms)
{
    esp12s_response_reset();
    esp12s_cmd_send(command);
    return tcp_wait_response(TCP_FAIL, timeout_ms);
}

/*
    CIPSTART与普通命令不同：ESP一般会连续返回CONNECT和OK。
    两个回应都拿到，才说明TCP已经真正建立。
*/
static uint8_t tcp_connect_start_and_wait(const char *command)
{
    esp12s_response_reset();
    esp12s_cmd_send(command);
    return tcp_wait_response(TCP_SUCCESS, TCP_AT_TIMEOUT_MS);
}

/* SSID、密码和服务器文本会直接拼入AT命令，因此不允许引号或换行改变命令原本的边界。 */
static uint8_t tcp_at_text_valid(const char *text)
{
    uint16_t index = 0U;

    while(text[index] != '\0')
    {
        if((text[index] == '"') || (text[index] == '\r') || (text[index] == '\n'))
        {
            return TCP_FAIL;
        }
        index++;
    }

    return TCP_SUCCESS;
}

uint8_t tcp_init(const tcp_config_struct *config)
{
    int command_length;

    if((config == 0) || (config->wifi_ssid == 0) || (config->wifi_password == 0) ||
       (config->server_ip == 0) || (config->server_port == 0U) ||
       (config->wifi_ssid[0] == '\0') || (config->server_ip[0] == '\0'))
    {
        return TCP_FAIL;
    }

    if((tcp_at_text_valid(config->wifi_ssid) == TCP_FAIL) ||
       (tcp_at_text_valid(config->wifi_password) == TCP_FAIL) ||
       (tcp_at_text_valid(config->server_ip) == TCP_FAIL))
    {
        return TCP_FAIL;
    }

    /* BOARD先配置USART1的NVIC和SysTick；这里才初始化ESP12S的USART1硬件。 */
    esp12s_init();
    tcp_connected = TCP_FAIL;

    if(tcp_command_send_and_wait_ok(ESP_AT_TEST, TCP_AT_TIMEOUT_MS) == TCP_FAIL)
    {
        return TCP_FAIL;
    }

    if(tcp_command_send_and_wait_ok("AT+CWMODE=1\r\n", TCP_AT_TIMEOUT_MS) == TCP_FAIL)
    {
        return TCP_FAIL;
    }

    command_length = snprintf(tcp_command_buffer, sizeof(tcp_command_buffer),
                              "AT+CWJAP=\"%s\",\"%s\"\r\n",
                              config->wifi_ssid, config->wifi_password);
    if((command_length < 0) || ((uint32_t)command_length >= sizeof(tcp_command_buffer)) ||
       (tcp_command_send_and_wait_ok(tcp_command_buffer, TCP_WIFI_JOIN_TIMEOUT_MS) == TCP_FAIL))
    {
        return TCP_FAIL;
    }

    if(tcp_command_send_and_wait_ok(ESP_AT_CIFSR, TCP_AT_TIMEOUT_MS) == TCP_FAIL)
    {
        return TCP_FAIL;
    }

    if(tcp_command_send_and_wait_ok("AT+CIPMUX=0\r\n", TCP_AT_TIMEOUT_MS) == TCP_FAIL)
    {
        return TCP_FAIL;
    }

    if(tcp_command_send_and_wait_ok("AT+CIPMODE=0\r\n", TCP_AT_TIMEOUT_MS) == TCP_FAIL)
    {
        return TCP_FAIL;
    }

    command_length = snprintf(tcp_command_buffer, sizeof(tcp_command_buffer),
                              "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
                              config->server_ip, (unsigned int)config->server_port);
    if((command_length < 0) || ((uint32_t)command_length >= sizeof(tcp_command_buffer)) ||
       (tcp_connect_start_and_wait(tcp_command_buffer) == TCP_FAIL))
    {
        return TCP_FAIL;
    }

    tcp_connected = TCP_SUCCESS;
    return TCP_SUCCESS;
}

uint8_t tcp_connected_get(void)
{
    return tcp_connected;
}
