#include "tcp_receive.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Service/Ota/ota.h"
#include <string.h>
#include "FreeRtos/Queue/rtos_queue.h"
#include "INTERRUPT/USART/usart.h"

static uint16_t tcp_receive_index;
static uint8_t tcp_ota_header[TCP_OTA_DATA_HEADER_LENGTH];
static uint8_t tcp_ota_block[TCP_OTA_DATA_MAX_LENGTH];
static volatile uint8_t tcp_ota_block_busy;
static uint16_t tcp_ota_remaining;
static uint16_t tcp_ota_length;
static uint8_t tcp_ota_discard;
static uint8_t tcp_receive_invalid;
static TickType_t tcp_receive_last_tick;
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
        tcp_ota_remaining = 0U;
        tcp_receive_invalid = 0U;
        /* 已入队的缓冲仍由 OtaTask 释放，不能被新连接覆盖。 */
    }
    if(tcp_receive_invalid != 0U)
    {
        return;
    }
    tcp_receive_last_tick = xTaskGetTickCountFromISR();
    if(tcp_ota_remaining != 0U)
    {
        if(tcp_ota_discard == 0U)
        {
            tcp_ota_block[tcp_ota_length - tcp_ota_remaining] = data;
        }
        tcp_ota_remaining--;
        if((tcp_ota_remaining == 0U) && (tcp_ota_discard == 0U))
        {
            tcp_ota_block_busy = 1U;
            if(rtos_tcp_ota_block_send_from_isr(tcp_ota_block, tcp_ota_length,
                                               generation, &woken) != RTOS_QUEUE_SUCCESS)
            {
                tcp_ota_block_busy = 0U;
            }
        }
        portYIELD_FROM_ISR(woken);
        return;
    }
    if((tcp_receive_index != 0U) && (tcp_ota_header[0] == TCP_OTA_DATA_TYPE))
    {
        tcp_ota_header[tcp_receive_index++] = data;
        if(tcp_receive_index == TCP_OTA_DATA_HEADER_LENGTH)
        {
            uint16_t length = ((uint16_t)tcp_ota_header[7] << 8) | tcp_ota_header[8];
            tcp_receive_index = 0U;
            if((length == 0U) || (length > TCP_OTA_DATA_MAX_PAYLOAD))
            {
                /* 长度损坏后不扫描二进制 payload；等待连接切换或接收超时。 */
                tcp_receive_invalid = 1U;
                return;
            }
            tcp_ota_remaining = length;
            tcp_ota_length = TCP_OTA_DATA_HEADER_LENGTH + length;
            tcp_ota_discard = tcp_ota_block_busy;
            if(tcp_ota_discard == 0U)
            {
                memcpy(tcp_ota_block, tcp_ota_header, TCP_OTA_DATA_HEADER_LENGTH);
            }
        }
        return;
    }
    if((tcp_receive_index == 0U) && (data == TCP_OTA_DATA_TYPE))
    {
        tcp_ota_header[0] = data;
        tcp_receive_index = 1U;
        return;
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
        if(data == TCP_OTA_DATA_TYPE)
        {
            tcp_ota_header[0] = data;
            tcp_receive_index = 1U;
            return;
        }
        if((data != TCP_OTA_QUERY_TYPE) && (data != 0x45U))
        {
            return;
        }
    }
    if(tcp_receive_index == 0U)
    {
        tcp_ota_header[0] = data;
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

void tcp_receive_block_release(void)
{
    taskENTER_CRITICAL();
    tcp_ota_block_busy = 0U;
    taskEXIT_CRITICAL();
}

void tcp_receive_service(void)
{
    TickType_t now;
    taskENTER_CRITICAL();
    now = xTaskGetTickCount();
    if(((tcp_receive_index != 0U) || (tcp_ota_remaining != 0U) ||
        (tcp_receive_invalid != 0U)) &&
       ((now - tcp_receive_last_tick) >= pdMS_TO_TICKS(OTA_TRANSFER_TIMEOUT_MS)))
    {
        tcp_receive_index = 0U;
        tcp_ota_remaining = 0U;
        tcp_receive_invalid = 0U;
    }
    taskEXIT_CRITICAL();
}
