#ifndef HMI_PAGE_H_
#define HMI_PAGE_H_

#include <stdint.h>
#include "hmi_widget.h"

/*
    从应用的 User/App/Presentation/Hmi/Page/hmi_page.h 裁剪而来：
    保留两个结构体，去掉了 hmi_page_build() —— 那个是应用的页面构建器，
    要读温度 / WiFi / 日志，而且每次刷新都重建一遍。

    Bootloader 只有一屏，而且内容是固定的，所以在 boot_screen.c 里
    用静态数组定义一次就够，不需要"构建"这一步。
*/

#define HMI_PAGE_MAX_ITEMS 32U

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

#endif
