#include "display_buffer.h"
#include "ips.h"

/*
    这里保存的是APP层的双行缓冲，而不是IPS驱动内部的缓冲。
    一行横屏RGB565像素是320*2=640字节，两个缓冲区一共只使用1280字节SRAM。
    这样DMA发送其中一行时，APP可以开始准备另一行，不需要申请无法放入SRAM的整屏缓冲。
*/
typedef enum
{
    DISPLAY_BUFFER_FREE = 0U,
    DISPLAY_BUFFER_FILLING,
    DISPLAY_BUFFER_READY,
    DISPLAY_BUFFER_TRANSMITTING
} display_buffer_state_enum;

typedef struct
{
    uint8_t data[IPS_DMA_LINE_BYTES];
    uint16_t x;
    uint16_t y;
    uint16_t width;
    display_buffer_state_enum state;
} display_buffer_slot_struct;

static display_buffer_slot_struct display_buffer_slot[2];
static uint8_t display_dma_buffer_index = 0xFFU;
static uint8_t display_buffer_error;

static uint8_t display_buffer_index_get(uint8_t *buffer)
{
    if(buffer == display_buffer_slot[0].data)
    {
        return 0U;
    }
    if(buffer == display_buffer_slot[1].data)
    {
        return 1U;
    }
    return 0xFFU;
}

uint8_t display_buffer_init(void)
{
    if((display_dma_buffer_index != 0xFFU) && (ips_dma_poll() == IPS_DMA_STATE_BUSY))
    {
        return 0U;
    }

    display_buffer_slot[0].state = DISPLAY_BUFFER_FREE;
    display_buffer_slot[1].state = DISPLAY_BUFFER_FREE;
    display_dma_buffer_index = 0xFFU;
    display_buffer_error = 0U;
    return 1U;
}

uint8_t *display_buffer_acquire(void)
{
    uint8_t i;

    for(i = 0U; i < 2U; i++)
    {
        if(display_buffer_slot[i].state == DISPLAY_BUFFER_FREE)
        {
            display_buffer_slot[i].state = DISPLAY_BUFFER_FILLING;
            return display_buffer_slot[i].data;
        }
    }
    return 0;
}

uint8_t display_buffer_commit(uint8_t *buffer, uint16_t x, uint16_t y, uint16_t width)
{
    uint8_t index = display_buffer_index_get(buffer);

    if((index == 0xFFU) || (display_buffer_slot[index].state != DISPLAY_BUFFER_FILLING) ||
       (width == 0U) || (width > IPS_WIDTH) || (x > (IPS_WIDTH - width)) || (y >= IPS_HEIGHT))
    {
        return 0U;
    }

    display_buffer_slot[index].x = x;
    display_buffer_slot[index].y = y;
    display_buffer_slot[index].width = width;
    display_buffer_slot[index].state = DISPLAY_BUFFER_READY;
    return 1U;
}

uint8_t display_buffer_cancel(uint8_t *buffer)
{
    uint8_t index = display_buffer_index_get(buffer);

    if((index == 0xFFU) || (display_buffer_slot[index].state != DISPLAY_BUFFER_FILLING))
    {
        return 0U;
    }

    display_buffer_slot[index].state = DISPLAY_BUFFER_FREE;
    return 1U;
}

void display_buffer_service(void)
{
    uint8_t i;
    ips_dma_state_enum dma_state;

    /* 先确认上一行是否真正完成，DMA未完成前绝不复用它的内存。 */
    if(display_dma_buffer_index != 0xFFU)
    {
        dma_state = ips_dma_poll();
        if(dma_state == IPS_DMA_STATE_COMPLETE)
        {
            display_buffer_slot[display_dma_buffer_index].state = DISPLAY_BUFFER_FREE;
            display_dma_buffer_index = 0xFFU;
        }
        else if(dma_state == IPS_DMA_STATE_ERROR)
        {
            display_buffer_slot[display_dma_buffer_index].state = DISPLAY_BUFFER_FREE;
            display_dma_buffer_index = 0xFFU;
            display_buffer_error = 1U;
        }
        else
        {
            return;
        }
    }

    /* 当前没有DMA占用的行时，从两个已经填写完成的缓冲区中启动下一行。 */
    for(i = 0U; i < 2U; i++)
    {
        if(display_buffer_slot[i].state == DISPLAY_BUFFER_READY)
        {
            uint16_t byte_count = (uint16_t)(display_buffer_slot[i].width * IPS_RGB565_BYTES_PER_PIXEL);

            if((ips_memory_write_begin(display_buffer_slot[i].x, display_buffer_slot[i].y,
                                       display_buffer_slot[i].width, 1U) != 0U) &&
               (ips_dma_start(display_buffer_slot[i].data, byte_count) != 0U))
            {
                display_buffer_slot[i].state = DISPLAY_BUFFER_TRANSMITTING;
                display_dma_buffer_index = i;
            }
            else
            {
                display_buffer_slot[i].state = DISPLAY_BUFFER_FREE;
                display_buffer_error = 1U;
            }
            return;
        }
    }
}

uint8_t display_buffer_error_get(void)
{
    return display_buffer_error;
}

void display_buffer_error_clear(void)
{
    display_buffer_error = 0U;
}
