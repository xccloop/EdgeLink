#ifndef HMI_PAGE_H_
#define HMI_PAGE_H_

#include <stdint.h>
#include "Presentation/Hmi/Widget/hmi_widget.h"
#include "Presentation/Hmi/hmi_types.h"

#define HMI_PAGE_MAX_ITEMS 32U//一页最多放32个

typedef struct
{
    const hmi_widget_t *widget;   /* 要画哪个 */
    uint16_t x;                   /* 摆在这一列 */
    uint16_t y;                   /* 摆在这一行 */
} hmi_page_item_t;

typedef struct
{
    uint16_t background;                       /* 整页底色 */
    uint8_t  count;                            /* 实际放了几个 */
    hmi_page_item_t items[HMI_PAGE_MAX_ITEMS]; /* 表格 */
} hmi_page_t;

/* 这个函数就是构建page的核心 */
uint8_t hmi_page_build(hmi_page_t *page, const hmi_view_data_t *data,hmi_page_id_t current_page, hmi_page_id_t selected_page);

#endif