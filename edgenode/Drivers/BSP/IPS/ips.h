#ifndef IPS_H_
#define IPS_H_

#include <stdint.h>

void ips_init(void);

/* 我们要的是横屏效果，显示层使用X=0~319、Y=0~239的坐标。 */
#define IPS_WIDTH                     320U
#define IPS_HEIGHT                    240U
#define IPS_RGB565_BYTES_PER_PIXEL    2U
#define IPS_DMA_LINE_BYTES            (IPS_WIDTH * IPS_RGB565_BYTES_PER_PIXEL)

typedef enum
{
    IPS_DMA_STATE_IDLE = 0U,
    IPS_DMA_STATE_BUSY,
    IPS_DMA_STATE_COMPLETE,
    IPS_DMA_STATE_ERROR
} ips_dma_state_enum;

/*
    启动一次非阻塞的SPI2 DMA发送。data和byte_count由APP层提供，
    但DMA寄存器的修改、状态判断和收尾全部由IPS BSP负责。
*/
uint8_t ips_dma_start(const uint8_t *data, uint16_t byte_count);

/*
    轮询当前DMA传输状态。只有返回IPS_DMA_STATE_COMPLETE后，
    APP层才可以重新使用刚才传入的缓冲区。
*/
ips_dma_state_enum ips_dma_poll(void);

/*
    设置一块RGB565写入窗口，并发送RAMWR命令。
    后续ips_dma_start()发送的像素数据会从这个窗口的左上角开始写入。
*/
uint8_t ips_memory_write_begin(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

#endif
