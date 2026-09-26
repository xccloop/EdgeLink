#include "hmi_render.h"
#include "Presentation/Hmi/hmi_types.h"
#include "Presentation/Buffer/display_buffer.h"
#include <stdint.h>
#include <string.h>

/* 一行的 320 个格子，每格 2 字节，所以这块工作区正好是 640 字节。 */
static uint8_t render_line[HMI_WIDTH * 2];

/* 这个结构体保存工作状态 */
static struct
{
    const hmi_widget_t *widget;   /* 画哪个 —— 存地址，不拷贝 */
    uint16_t x, y;                /* 画在屏幕哪一列、哪一行 */
    uint16_t last_row;            /* 最后画到第几行（被屏幕底边裁过） */
    uint16_t row;                 /* 现在画到第几行 —— 那枚书签 */
    uint16_t background;
    uint8_t  active;              /* 手上有没有活 */
} job;


/* 往 render_line 的第 x 个格子里写 color */
static void pixel_put(uint16_t x, uint16_t color)
{
    /*
        一个颜色占 16 位，一个字节是 8 位，所以占 2 个字节。
        第 x 格在 render_line 里的位置：它前面 x 个格子已经占了 x*2 个字节。
        高 8 位先存（右移 8 位把它挪到最低 8 位），低 8 位后存（& 0xFF 只留最低 8 位）。
    */
    render_line[x * 2] = color >> 8;
    render_line[x * 2 + 1] = color & 0xFF;
}

/*
    │ widget │ 要画的那个 widget（知道自己多大、什么颜色）  │
    │ row    │ 这是 widget 内部的第几行（从 0 数起）        │
    │ out_x  │ 这个 widget 从屏幕的第几列开始画             │

    本函数只负责把 widget 的这一行画出来。
    到底画的是第几行，它不决定，由调用方传进来；
    它也不记进度，画完就返回，所以下一行还得再叫它一次。
*/
static void widget_row(const hmi_widget_t *widget, uint16_t row, uint16_t out_x)
{
    if(widget == NULL)
    {
        return;
    }

    switch(widget->kind)
    {
        case HMI_WIDGET_RECT:
        {
            uint16_t i;
            for(i = 0U; i < widget->width; i++)
            {
                uint16_t x = out_x + i;   /* i 是圈数，加上 out_x 才是格子号 */

                if(x >= HMI_WIDTH)
                {
                    break;   /* 已经到屏幕右边外面了，后面的只会更出界 */
                }

                pixel_put(x, widget->content.rect.color);
            }
            break;
        }

        /*
            我们来仔细分析一下绘制text的分支
            首先font_raw代表我们正在画屏幕的第 0 行，它对应字模的第 0 行，因为我们可能会让字符进行缩放，实际上就是同一笔画了几行
            可以理解为这一个是对于实际字符行的映射
            随后是一个for循环，我们知道，一个字符其实有五列组成，这里就是把每一列给显示出来
        */
        case HMI_WIDGET_TEXT:
        {
            uint16_t font_row = row / widget->content.text.scale;
            uint16_t col;
            uint16_t n;
            if(font_row >= 7U)//字模只有 7 行（0~6）。如果算出来是 9，说明这一行落在字的下面了，那就不用画
            {
                break;
            }
            for(n = 0U;(n < HMI_TEXT_CAPACITY) && (widget->content.text.text[n] != '\0');n++)
            {
                 for(col = 0U; col < 5U; col++)
                {
                    uint8_t byte = hmi_font_column(widget->content.text.text[n],col);
                    if(((byte >> font_row) & 1U) != 0U)//这个字符的第 col 列，第 font_row 行，有没有点？如果没有不进行绘制继续下一轮循环
                    {
                        uint16_t k;
                        for(k = 0U; k < widget->content.text.scale; k++)
                        {
                            uint16_t origin_x = out_x + n * 6U * widget->content.text.scale;
                            uint16_t x = origin_x + col * widget->content.text.scale + k;
                            if(x >= HMI_WIDTH)
                            {
                                break;
                            }
                            pixel_put(x, widget->content.text.color);
                        }
                    }
                }
            }
            break;
        }
        case HMI_WIDGET_IMAGE:
        break;
    }
}

/*这个函数负责填充job*/
hmi_render_result_enum hmi_render_draw(const hmi_widget_t *widget,uint16_t x, uint16_t y,uint16_t background)
{
    if(widget == NULL || widget->height == 0U ||x >= HMI_WIDTH || y >= HMI_HEIGHT)
    {
        return HMI_RENDER_ERROR;
    }

    if(job.active != 0U)
    {
        return HMI_RENDER_BUSY;
    }

    //赋值
    job.widget = widget;
    job.x = x;
    job.y = y;
    job.background = background;
    job.row = 0U;          

    //job的last_row描述的是最后画到第几行
    //如果超出就取屏幕的边界
    uint16_t rows = widget->height;
    if(rows > (HMI_HEIGHT - y))     
    {
        rows = HMI_HEIGHT - y;      
    }
    job.last_row = rows - 1U;       

    //此时job工作
    job.active = 1U;
    return HMI_RENDER_ACCEPTED;
}

/* 这个函数负责将job的内容交给flush来进行工作 */
void hmi_render_service(void)
{
    display_submit_enum submit;
    uint16_t i;

    //推进行缓冲
    //每一页最底下那一行永远刷不上去（显示的是上一帧的残留）。
    //所以那一行 display_buffer_service() 服务的不是 HMI，是行缓冲自己。 它必须在"没活就 return"之前跑
    if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
    {
        job.active = 0U;
    }

    //如果是busy直接不做
    if(job.active == 0U)
    {
        return;
    }

    //先将背景铺满
    for(i = 0U; i < HMI_WIDTH; i++)
    {
        pixel_put(i, job.background);
    }

    //将renderline里面放进这一次widegt
    widget_row(job.widget, job.row, job.x);

    //提交
    submit = display_render_span(render_line,0U,(uint16_t)(job.y + job.row),HMI_WIDTH);
    

    if(submit == DISPLAY_SUBMIT_OK)
        {
            if(job.row >= job.last_row)
            {
                job.active = 0U;        /* 整个 widget 画完了，收工 */
            }
            else
            {
                job.row++;              /* 交出去了，书签才往前推 */
            }
        }
    else if(submit == DISPLAY_SUBMIT_ERROR)
    {
        job.active = 0U;            /* 出错就收工，别卡死 */
    }
}

uint8_t hmi_render_busy(void)
{
    return job.active;
}