#ifndef DISPLAY_BUFFER_H_
#define DISPLAY_BUFFER_H_

#include <stdint.h>

#define DISPLAY_BUFFER_SUCCESS  1U
#define DISPLAY_BUFFER_FAIL     0U

/*
    display_buffer_init()：紧跟 IPS 初始化后调用一次。
    display_render_line()：每准备好一行就调用；成功才 y++。
    display_buffer_service()：主循环里反复调用，它负责检查 DMA 是否完成并启动等待的下一行。
    display_wait_frame_done()：只在整帧最后一行提交后调用一次，不能每行都调用，否则又变回阻塞发送。
*/

/*
    在ips_init()成功后调用一次，只清空APP自己的两块行缓冲区，不会启动DMA。
    DMA仍在发送时返回0并拒绝重置，避免覆盖DMA正在读取的像素数据。
*/
uint8_t display_buffer_init(void);

/*
    将一行320像素的RGB565数据提交给双行缓冲。data至少要有640字节，y是逻辑横屏坐标。
    返回1表示数据已经复制到APP缓冲区；返回0表示两块缓冲区均不可写或IPS发生错误，
    此时不要丢掉当前这一行，应先继续调用display_buffer_service()，稍后重试。
*/
uint8_t display_render_line(const uint8_t *data, uint16_t y);

/* 推进DMA和READY队列，不等待DMA忙状态；APP主循环应周期性调用。 */
uint8_t display_buffer_service(void);

/* 一帧的最后一行提交后调用，等待最后的DMA完成。 */
uint8_t display_wait_frame_done(void);

#endif
