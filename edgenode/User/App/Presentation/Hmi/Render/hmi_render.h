#ifndef HMI_RENDER_H_
#define HMI_RENDER_H_

#include <stdint.h>
#include "Presentation/Hmi/Widget/hmi_widget.h"
#include "Presentation/Hmi/Page/hmi_page.h"

typedef enum
{
    HMI_RENDER_ACCEPTED = 0U,   /* 收下了 */
    HMI_RENDER_BUSY,            /* 上一份还没画完，这次没收 */
    HMI_RENDER_ERROR            /* 参数不对 */
} hmi_render_result_enum;


/* 提交：把 widget 画在屏幕 (x,y)，其余像素填 background。忙就返回 BUSY。 */
hmi_render_result_enum hmi_render_draw(const hmi_widget_t *widget,
                                       uint16_t x, uint16_t y,
                                       uint16_t background);

/* 每 1ms 调一次，每次画一行。空闲时也要调。 */
void hmi_render_service(void);

/* 忙不忙 */
uint8_t hmi_render_busy(void);

/*
    绘制一整页：把 page 里所有 widget 按数组顺序叠在一起，
    只画 first_y 到 last_y 这个行区间（闭区间），用来做局部刷新。

    page 会被整份拷进记账本，调用方传完就可以随便改它（可以放栈上）。
    但 page 里指向的 widget 必须一直存活、内容不许改，直到 busy 变 0。
*/
hmi_render_result_enum hmi_render_page(const hmi_page_t *page,
                                       uint16_t first_y,
                                       uint16_t last_y);

#endif
