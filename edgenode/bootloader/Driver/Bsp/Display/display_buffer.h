#ifndef DISPLAY_BUFFER_H_
#define DISPLAY_BUFFER_H_

#include <stdint.h>

#define DISPLAY_BUFFER_SUCCESS  1U
#define DISPLAY_BUFFER_FAIL     0U

/*
    提交像素段的结果必须区分"忙"和"出错"：
    前者不是丢行条件，hmi_render 会保留当前行、下次继续提交。
*/
typedef enum
{
    DISPLAY_SUBMIT_OK = 0U,
    DISPLAY_SUBMIT_BUSY,
    DISPLAY_SUBMIT_ERROR
} display_submit_enum;

/*
    这是 Bootloader 版的 display_buffer —— 接口和应用的
    User/App/Presentation/Buffer/display_buffer.h 保持一致，
    但只实现 hmi_render.c 真正用到的那两个函数。

    应用那份实现的是【双行缓冲】：一块给 DMA 发，另一块给 CPU 准备下一行，
    目的是不阻塞 RTOS 任务循环，代价是 279 行状态机 + 两块 640 字节缓冲。
    Bootloader 走了另一条路（单缓冲 + 同步刷），详见 display_buffer.c。

    因为接口一致，hmi_render.c 一个字都不用改。
*/

/* ips_init() 成功后调用一次。 */
uint8_t display_buffer_init(void);

/*
    提交屏幕第 y 行的一整行像素（从 x 起共 width 个像素，data 至少 width*2 字节）。

    返回 DISPLAY_SUBMIT_OK 时，这一行已经真的发完了 ——
    DMA 已空闲，调用方可以立刻改写 data 准备下一行。

    （应用版返回 OK 只代表"数据被拷进了缓冲"，DMA 可能还在发。
      Bootloader 版返回 OK 才是真的发完了，区别在这里。）
*/
display_submit_enum display_render_span(const uint8_t *data,
                                        uint16_t x,
                                        uint16_t y,
                                        uint16_t width);

/* hmi_render 每次服务都会调，用来推进上一步提交。
   单缓冲下没有待推进的东西，但保留这个接口。 */
uint8_t display_buffer_service(void);

#endif
