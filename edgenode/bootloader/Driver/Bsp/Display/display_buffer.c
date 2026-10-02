#include "display_buffer.h"
#include "IPS/ips.h"
#include <stddef.h>   /* NULL */

/*
    Bootloader 版的 display_buffer：单缓冲 + 同步刷。

    应用那份用双行缓冲，是因为它跑在 FreeRTOS 里，刷屏不能卡住任务循环：
    一块缓冲区正被 DMA 发送时，CPU 用另一块准备下一行，两边并行。

    Bootloader 里没有别的任务，也没有"不能卡住"的约束 ——
    反正都卡住等救援了。所以这里退化成最朴素的做法：

        提交本行 → 等本行真的发完 → 才返回

    每次 display_render_span() 返回时，DMA 一定空闲，
    调用方可以安全地改写 render_line 准备下一行。

    代价是吞吐低：一行 640 字节 @ SPI 9MHz ≈ 570us，
    整屏 240 行约 137ms。只在卡住时刷一次，可以接受。
    换来的是不需要 279 行的双缓冲状态机、也不需要第二块 640 字节缓冲。
*/

uint8_t display_buffer_init(void)
{
    /*
        单缓冲没有自己的缓冲区要清（hmi_render 的 render_line 不需要预置内容）。
        这里只确认 IPS 那边没有遗留的 DMA 在跑，让起点干净。
    */
    while (ips_flush_line(NULL, 0U, 0U, 0U) == IPS_FLUSH_BUSY)
    {
    }

    return DISPLAY_BUFFER_SUCCESS;
}

display_submit_enum display_render_span(const uint8_t *data,
                                        uint16_t x,
                                        uint16_t y,
                                        uint16_t width)
{
    ips_flush_result_enum result;

    if ((data == 0) || (width == 0U))
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    /*
        ① 提交本行。

        ips_flush_line() 每次都会先 poll 上一笔 DMA：
        如果还没发完，它返回 BUSY 并且【根本不会提交】本次数据。
        所以这里循环重试，既完成了"等上一笔"，也完成了"提交本行"。
    */
    do
    {
        result = ips_flush_line(data, x, y, width);
    } while (result == IPS_FLUSH_BUSY);

    if (result != IPS_FLUSH_ACCEPTED)
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    /*
        ② 等本行真的发完 —— 这是单缓冲的关键。

        此刻 data 已经被 DMA 接管。如果现在就返回，调用方会立刻改写
        render_line 去准备下一行，而 DMA 还在读那块内存 —— 屏幕会出现
        撕裂的像素。所以必须等 DMA 收尾才能把控制权交回去。
    */
    do
    {
        result = ips_flush_line(NULL, 0U, 0U, 0U);
    } while (result == IPS_FLUSH_BUSY);

    if (result != IPS_FLUSH_COMPLETE)
    {
        return DISPLAY_SUBMIT_ERROR;
    }

    return DISPLAY_SUBMIT_OK;
}

uint8_t display_buffer_service(void)
{
    /*
        单缓冲下，display_render_span() 返回时 DMA 已经空闲，
        没有"待推进的上一笔"。保留这个函数只是为了对上 hmi_render.c
        每次服务都会调它的接口。
    */
    return DISPLAY_BUFFER_SUCCESS;
}
