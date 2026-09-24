#ifndef HMI_RENDER_H_
#define HMI_RENDER_H_
#include "Presentation/Hmi/Widget/hmi_widget.h"
typedef enum
{
    HMI_RENDER_ACCEPTED = 0U,
    HMI_RENDER_BUSY,
    HMI_RENDER_ERROR
} hmi_render_result_enum;
/* 复制图片描述；pixels必须在busy变0前保持只读有效。 */
hmi_render_result_enum hmi_render_widget(const hmi_widget_t *widget, uint16_t x, uint16_t y);
/* 场景由Page持有，busy变0前不可修改。first_y..last_y为闭区间。 */
hmi_render_result_enum hmi_render_scene(const hmi_scene_t *scene, uint16_t first_y, uint16_t last_y);
/* 空闲时也要调用，继续推进最后的DMA。每次最多提交一行。 */
void hmi_render_service(void);
/* busy=0只代表源像素已提交，不代表DMA排空；现有Buffer没有非阻塞排空查询。 */
uint8_t hmi_render_busy(void);
uint8_t hmi_render_failed(void);
/* 明确重画时解除错误锁存，不重置或停止DMA。 */
void hmi_render_clear_error(void);
#endif
