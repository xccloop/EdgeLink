#ifndef HMI_RENDER_H_
#define HMI_RENDER_H_

#include <stdint.h>
#include "Presentation/Hmi/Widget/hmi_widget.h"

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

#endif
