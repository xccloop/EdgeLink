#ifndef DISPLAY_BUFFER_H_
#define DISPLAY_BUFFER_H_

#include <stdint.h>

/* 初始化两个横屏扫描线缓冲区的状态，不会启动DMA。DMA仍在发送时拒绝重新初始化。 */
uint8_t display_buffer_init(void);

/*
    取得一块空闲缓冲区供APP填写一行RGB565像素。
    返回NULL说明两个缓冲区分别处于填写、等待发送或DMA发送状态。
*/
uint8_t *display_buffer_acquire(void);

/*
    表示buffer中的一整行数据已经填写完成，等待display_buffer_service()发送。
    只能提交由display_buffer_acquire()取得的缓冲区。
*/
uint8_t display_buffer_commit(uint8_t *buffer, uint16_t x, uint16_t y, uint16_t width);

/* 放弃正在填写但尚未提交的缓冲区，避免填写被取消后永久占用一个槽位。 */
uint8_t display_buffer_cancel(uint8_t *buffer);

/*
    推进DMA发送状态。APP主循环需要周期性调用它，
    DMA完成后该函数才会释放对应的缓冲区供下一次填写。
*/
void display_buffer_service(void);

/* DMA或窗口设置失败时置位，APP可据此放弃当前帧并重新开始。 */
uint8_t display_buffer_error_get(void);
void display_buffer_error_clear(void);

#endif
