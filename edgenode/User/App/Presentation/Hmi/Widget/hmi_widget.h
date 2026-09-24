#ifndef HMI_WIDGET_H_
#define HMI_WIDGET_H_
#include <stdint.h>
/* Widget描述资源，不布局、不访问Buffer/SPI/DMA。
   pixels保存按行排列的普通RGB565数值（红色0xF800），Render负责发送字节序。 */
typedef struct
{
    uint16_t width, height;
    const uint16_t *pixels;
} hmi_widget_t;
#define HMI_SCENE_MAX_ITEMS 32U
#define HMI_TEXT_CAPACITY 49U
typedef enum { HMI_ELEMENT_RECT = 0, HMI_ELEMENT_TEXT } hmi_element_kind_t;
typedef struct
{
    hmi_element_kind_t kind;
    uint16_t x, y, width, height;
    uint16_t color;
    uint8_t scale;
    char text[HMI_TEXT_CAPACITY];
} hmi_element_t;
typedef struct
{
    uint16_t background;
    uint8_t count;
    hmi_element_t items[HMI_SCENE_MAX_ITEMS];
} hmi_scene_t;
/* ASCII 5x7点阵，0x7F专用于摄氏度圆圈，其他字符显示问号。 */
uint8_t hmi_font_column(uint8_t character, uint8_t column);
#endif
