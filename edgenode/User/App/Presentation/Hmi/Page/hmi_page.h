#ifndef HMI_PAGE_H_
#define HMI_PAGE_H_
#include "Presentation/Hmi/hmi_types.h"
#include "Presentation/Hmi/Widget/hmi_widget.h"
/* 输出场景由调用方持有；正在被Render读取的场景不可覆盖。失败返回0。 */
uint8_t hmi_page_build(hmi_scene_t *scene, const hmi_view_data_t *data,
                       hmi_page_id_t current_page, hmi_page_id_t selected_page);
#endif
