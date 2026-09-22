#include "display_buffer.h"
#include "IPS/ips.h"

#include <string.h>

/*
    IPS BSP已经负责一次DMA发送，APP这一层只负责管理两块行缓冲区。

    整屏RGB565数据需要240*320*2=153600字节，不能放进GD32F103RCT6的48KB SRAM；
    因此我们只准备两行：A正在被DMA发送时，CPU可以向B填写下一行。

    这里不能在每次提交前都等待DMA完成，否则CPU会一直等A，B就失去意义，
    双行缓冲会退化成单行阻塞发送。正确方式是用状态位记录：
    FREE表示可以填写，READY表示已经填写好等待发送，TRANSMITTING表示DMA正在读取。
*/
#define DISPLAY_BUFFER_COUNT          2U
#define DISPLAY_BUFFER_INVALID_INDEX  0xFFU
#define DISPLAY_WAIT_SPIN_LIMIT       1000000UL

typedef enum
{
    DISPLAY_BUFFER_FREE = 0U,
    DISPLAY_BUFFER_READY,
    DISPLAY_BUFFER_TRANSMITTING
} display_buffer_state_enum;

typedef struct
{
    uint8_t data[IPS_DMA_LINE_BYTES];

    uint16_t x;
    uint16_t y;
    uint16_t width;//在缓冲区新增长度而不是固定填满一整个

    display_buffer_state_enum state;
} display_buffer_slot_struct;

static display_buffer_slot_struct display_buffer_slot[DISPLAY_BUFFER_COUNT];
static uint8_t display_ready_queue[DISPLAY_BUFFER_COUNT];
static uint8_t display_ready_count;
static uint8_t display_dma_buffer_index = DISPLAY_BUFFER_INVALID_INDEX;

/*
    这个函数不是发送函数，而是“重新开始一帧”时统一清理软件状态。

    它只修改APP自己记录的FREE、READY、TRANSMITTING状态，不会直接停止DMA。
    所以只能在确认DMA没有继续读取行缓冲区后调用：初始化时由display_buffer_init()确认，
    传输错误时则由IPS BSP先停止DMA，再回到这里释放本帧占用的两块缓冲区。
*/
static void display_buffer_reset(void)
{
    uint8_t i;

    for(i = 0U; i < DISPLAY_BUFFER_COUNT; i++)
    {
        display_buffer_slot[i].state = DISPLAY_BUFFER_FREE;
    }

    display_ready_count = 0U;
    display_dma_buffer_index = DISPLAY_BUFFER_INVALID_INDEX;
}

/*
    这个函数用来寻找下一块可以让CPU填写的行缓冲区。

    我们不能只看数组下标，因为A、B哪一块空闲会随着DMA发送不断交换；
    必须根据state确认它是FREE，才能把新的像素数据复制进去。
    返回0xFF表示两块缓冲区分别在READY或TRANSMITTING，CPU需要稍后再试。
*/
static uint8_t display_buffer_free_index_get(void)
{
    uint8_t i;

    for(i = 0U; i < DISPLAY_BUFFER_COUNT; i++)
    {
        if(display_buffer_slot[i].state == DISPLAY_BUFFER_FREE)
        {
            return i;
        }
    }

    return DISPLAY_BUFFER_INVALID_INDEX;
}

/*
    READY队列保存“已经填好、等待发送”的缓冲区下标。

    双行缓冲不一定总按A再B的顺序提交，所以不能直接遍历数组发送；
    这个函数在一行已经成功交给DMA后，移除队首；发送失败时会直接清空整帧队列，
    保证后面准备好的行仍然按display_render_line()的提交顺序发送。
*/
static void display_ready_queue_pop(void)
{
    if(display_ready_count == 2U)
    {
        display_ready_queue[0] = display_ready_queue[1];
    }

    if(display_ready_count != 0U)
    {
        display_ready_count--;
    }
}

/*
    这是APP使用双行缓冲前的初始化入口。

    它不配置SPI、DMA或ST7789，那些已经由ips_init()完成；这里仅把两块APP行缓冲区标记为空闲。
    如果DMA还在读取上一帧的数据，直接重置状态会让CPU误以为那块缓冲区可写，
    因此先通过ips_flush_line(NULL,...)确认。返回0表示DMA仍忙，不能初始化。
*/
uint8_t display_buffer_init(void)
{
    ips_flush_result_enum flush_result;

    /* DMA仍在读取某行时不能重新初始化，否则会把正在发送的缓冲区误标记为空闲。 */
    if(display_dma_buffer_index != DISPLAY_BUFFER_INVALID_INDEX)
    {
        flush_result = ips_flush_line(0, 0U, 0U, 0U);
        if(flush_result == IPS_FLUSH_BUSY)
        {
            return DISPLAY_BUFFER_FAIL;
        }
    }

    /* COMPLETE或ERROR时，IPS已经收尾或中止DMA；此时才允许丢弃旧帧的READY数据。 */
    display_buffer_reset();
    return DISPLAY_BUFFER_SUCCESS;
}

/*
    这是双行缓冲最重要的“服务函数”，APP主循环需要周期性调用它。

    推进一次显示状态：
    1. 如果有DMA正在发送，先询问IPS是否已经完全结束；
    2. DMA结束后释放那块缓冲区；
    3. 如果有READY缓冲区，再按commit顺序启动下一行。

    这个函数不等待BUSY状态。DMA发送期间立即返回，让CPU继续处理按键、传感器等工作；
    下一次主循环再调用时，才会继续检查DMA或启动READY队列中的下一行。
*/
uint8_t display_buffer_service(void)
{
    uint8_t next_index;
    ips_flush_result_enum flush_result;

    if(display_dma_buffer_index != DISPLAY_BUFFER_INVALID_INDEX)
    {
        flush_result = ips_flush_line(0, 0U, 0U, 0U);
        if(flush_result == IPS_FLUSH_BUSY)
        {
            return DISPLAY_BUFFER_SUCCESS;
        }
        if(flush_result == IPS_FLUSH_ERROR)
        {
            /* DMA异常后旧帧的数据已经不可信，READY队列也一起清空，不能发送到下一帧。 */
            display_buffer_reset();
            return DISPLAY_BUFFER_FAIL;
        }

        /* 只有DMA和SPI都结束后，TRANSMITTING缓冲区才可以重新填写。 */
        display_buffer_slot[display_dma_buffer_index].state = DISPLAY_BUFFER_FREE;
        display_dma_buffer_index = DISPLAY_BUFFER_INVALID_INDEX;
    }

    if(display_ready_count == 0U)
    {
        return DISPLAY_BUFFER_SUCCESS;
    }

    next_index = display_ready_queue[0];
    flush_result = ips_flush_line(
        display_buffer_slot[next_index].data,
        display_buffer_slot[next_index].x,
        display_buffer_slot[next_index].y,
        display_buffer_slot[next_index].width);
    if(flush_result == IPS_FLUSH_BUSY)
    {
        return DISPLAY_BUFFER_SUCCESS;
    }
    if(flush_result != IPS_FLUSH_ACCEPTED)
    {
        /* 窗口或DMA启动失败时，当前帧剩余的READY行也不能继续发送。 */
        display_buffer_reset();
        return DISPLAY_BUFFER_FAIL;
    }

    display_buffer_slot[next_index].state = DISPLAY_BUFFER_TRANSMITTING;
    display_dma_buffer_index = next_index;
    display_ready_queue_pop();
    return DISPLAY_BUFFER_SUCCESS;
}

display_submit_enum display_render_line(const uint8_t *data, uint16_t y)
{
    return display_render_span(data,
                                0U,
                                y,
                                IPS_WIDTH);
}

/*
    这是“这一帧收尾”时才使用的等待函数。

    平时不要在每一行后调用它，否则会把非阻塞的双行缓冲重新变成每行等待DMA；
    只有一帧最后一行已经提交、APP确实需要确认整帧都已经送入IPS时才调用。
    IPS内部有DMA超时保护；发生错误时本函数返回0，避免永远卡在等待最后一行。
*/
uint8_t display_wait_frame_done(void)
{
    uint32_t spin_count = DISPLAY_WAIT_SPIN_LIMIT;

    while((display_dma_buffer_index != DISPLAY_BUFFER_INVALID_INDEX) || (display_ready_count != 0U))
    {
        if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
        {
            return DISPLAY_BUFFER_FAIL;
        }
        if(spin_count == 0U)
        {
            return DISPLAY_BUFFER_FAIL;
        }
        spin_count--;
    }

    return DISPLAY_BUFFER_SUCCESS;
}

/*
    此函数用于
*/
display_submit_enum display_render_span(const uint8_t *data,
                                        uint16_t x,
                                        uint16_t y,
                                        uint16_t width)
{
    uint8_t free_index;

    if((data == 0) ||
       (width == 0U) ||
       (y >= IPS_HEIGHT) ||
       (x >= IPS_WIDTH) ||
       (width > (IPS_WIDTH - x)))
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    free_index = display_buffer_free_index_get();

    if(free_index == DISPLAY_BUFFER_INVALID_INDEX)
    {
        return DISPLAY_SUBMIT_BUSY;
    }

    memcpy(display_buffer_slot[free_index].data,
           data,
           width * 2U);

    display_buffer_slot[free_index].x = x;
    display_buffer_slot[free_index].y = y;
    display_buffer_slot[free_index].width = width;

    display_buffer_slot[free_index].state = DISPLAY_BUFFER_READY;

    display_ready_queue[display_ready_count] = free_index;
    display_ready_count++;

    if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    return DISPLAY_SUBMIT_OK;
}
