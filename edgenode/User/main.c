//这个项目位采集节点的设计，还是一样，先做硬件基础然后有了基础才可以构建应用层内容
//我们先在BSP中实现我们要实现的外设，包括LED,KEY,ESP-12S,GD25Q32,ADC,CAN,IPS,BMP280

#include "CH340/ch340.h"
#include "board_config.h"
#include "board_time.h"
#include "Output/Tcp/tcp.h"
#include "ESP12S/esp12s.h"
#include "INTERRUPT/USART/usart.h"

#include <stdint.h>
#include <stdio.h>

/*
    现在让我们尝试完整的数据链路，不加入HMI,RS485,FAN
    我们来梳理一下
    数据链路从BMP280原始温度获取，进入message统一内部模型
    此时分三路一路用TCP通讯，一路用CAN通讯，一路给flash进行存储
    由于上发的数据都是给edgehub，我们从节点侧注意先用TCP，再用CAN
    两路都用怕引起数据重复
*/

int main(void)
{
    esp12s_response_t response;
    uint32_t deadline_ms;
    uint8_t send_prompt_received;
    uint8_t send_ok_received;

    board_config_init();
    ch340_init();

    tcp_config_struct config;
    config.wifi_ssid = "";
    config.wifi_password = "";
    config.server_ip = "";
    config.server_port = 8888;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Edgenode start\r\n");

    if(tcp_init(&config) == TCP_SUCCESS)
    {
        printf("TCP CONNECTED\r\n");

        /* CIPSEND先声明后续原始负载长度；只有收到ESP的'>'提示才能发送5字节HELLO。 */
        esp12s_response_reset();
        esp12s_cmd_send("AT+CIPSEND=5\r\n");
        deadline_ms = board_systick_ms + 10000U;
        send_prompt_received = 0U;

        while((uint32_t)(board_systick_ms - deadline_ms) >= 0x80000000U)
        {
            if(esp12s_response_get(&response) != 0U)
            {
                if(response == ESP12S_RESPONSE_PROMPT)
                {
                    send_prompt_received = 1U;
                    break;
                }
                if((response == ESP12S_RESPONSE_ERROR) || (response == ESP12S_RESPONSE_FAIL) ||
                   (response == ESP12S_RESPONSE_BUSY) || (response == ESP12S_RESPONSE_CLOSED))
                {
                    break;
                }
            }
        }

        if(send_prompt_received == 0U)
        {
            printf("TCP TEST SEND FAILED: no CIPSEND prompt\r\n");
        }
        else
        {
            esp12s_response_reset();
            esp12s_cmd_send("HELLO");
            deadline_ms = board_systick_ms + 10000U;
            send_ok_received = 0U;

            while((uint32_t)(board_systick_ms - deadline_ms) >= 0x80000000U)
            {
                if(esp12s_response_get(&response) != 0U)
                {
                    if(response == ESP12S_RESPONSE_SEND_OK)
                    {
                        send_ok_received = 1U;
                        break;
                    }
                    if((response == ESP12S_RESPONSE_ERROR) || (response == ESP12S_RESPONSE_FAIL) ||
                       (response == ESP12S_RESPONSE_BUSY) || (response == ESP12S_RESPONSE_CLOSED))
                    {
                        break;
                    }
                }
            }

            if(send_ok_received != 0U)
            {
                printf("TCP TEST SEND OK: HELLO\r\n");
            }
            else
            {
                printf("TCP TEST SEND FAILED: no SEND OK\r\n");
            }
        }
    }
    else
    {
        printf("TCP CONNECT FAILED\r\n");
    }

    while (1) {
        delay_ms(200);
    }
}
