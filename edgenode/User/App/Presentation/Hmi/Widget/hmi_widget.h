#ifndef HMI_WIDGET_H_
#define HMI_WIDGET_H_

//这里我们需要定义一个元素是怎么描述的
#include <stdint.h>

#define HMI_TEXT_CAPACITY 49U

/*
    我们这里发现，为什么要用union去定义一个具体的内容
    直接uint16_t color,uint8_t  scale....这样定义当不要的直接清0不就好了么
    但是这样做会让结构体占用的内存加大，如果让不同种类的定义占用同一块内存就好了
    这就是union的定义使用
*/
typedef union
{
    struct{
        uint16_t color;
    }rect;//色块

    struct{
        uint16_t color;
        uint8_t  scale;
        char text[HMI_TEXT_CAPACITY];
    }text;

    struct{
        const uint16_t *pixels;
    }image;
}hmi_widget_content_t;

/*
    不同种类的使用enum
*/
typedef enum
{
    HMI_WIDGET_RECT = 0,
    HMI_WIDGET_TEXT = 1,
    HMI_WIDGET_IMAGE = 2
}hmi_widget_kind_t;

/*
    这个结构体就是我们去描述一个元素具体长什么样子
    height,widget决定一个元素有多大（注意是uint16，如果是uint8那最大才255，我们的屏幕是320*240）
    kind决定了整个元素是文字还是色块
    content定义不同种类的具体的描述
*/
typedef struct
{
    uint16_t height;
    uint16_t width;
    hmi_widget_kind_t kind;
    hmi_widget_content_t content;
}hmi_widget_t;

uint8_t hmi_font_column(uint8_t character, uint8_t column);

#endif