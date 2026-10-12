#include "tcp_receive.h"
#include "FreeRTOS.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "INTERRUPT/USART/usart.h"

static uint8_t tcp_receive_index;
static tcp_ack_frame_t tcp_receive_frame;
static uint32_t tcp_receive_generation;

/* 在串口中断中执行；组帧状态跨 +IPD 保留，连接切换后丢弃旧半包。 */
static void tcp_receive_byte_from_isr(uint8_t data, uint32_t generation)
{
    BaseType_t woken = pdFALSE;
    if(generation != tcp_receive_generation)
    {
        tcp_receive_index = 0U;
        tcp_receive_generation = generation;
    }
    if(tcp_receive_index == 0U)
    {
        if((data != TCP_OTA_QUERY_TYPE) && (data != 0x45U))
        {
            return;
        }
    }
    else if((tcp_receive_index == 1U) &&
            (tcp_receive_frame.data[0] == 0x45U) && (data != 0x48U))
    {
        tcp_receive_index = 0U;
        if((data != TCP_OTA_QUERY_TYPE) && (data != 0x45U))
        {
            return;
        }
    }
    tcp_receive_frame.data[tcp_receive_index++] = data;
    if((tcp_receive_frame.data[0] == TCP_OTA_QUERY_TYPE) &&
       (tcp_receive_index == TCP_OTA_QUERY_LENGTH))
    {
        (void)rtos_tcp_ota_query_send_from_isr(tcp_receive_frame.data,
                                              generation, &woken);
        tcp_receive_index = 0U;
    }
    else if(tcp_receive_index == TCP_FRAME_LENGTH)
    {
        (void)rtos_tcp_ack_frame_send_from_isr(&tcp_receive_frame, &woken);
        tcp_receive_index = 0U;
    }
    portYIELD_FROM_ISR(woken);
}

void tcp_receive_init(void)
{
    esp12s_data_receive_callback_set(tcp_receive_byte_from_isr);
}
