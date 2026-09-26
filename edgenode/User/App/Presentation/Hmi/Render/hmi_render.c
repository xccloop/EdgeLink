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
    hmi_page_t page;          /* ★ 整页的副本 —— 不是指针 */
    uint16_t   last_row;      /* 画到屏幕第几行为止 */
    uint16_t   row;           /* 现在画到屏幕第几行 —— 那枚书签 */
    uint8_t    active;        /* 手上有没有活 */
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

            for(n = 0U; (n < HMI_TEXT_CAPACITY) && (widget->content.text.text[n] != '\0'); n++)
            {
                for(col = 0U; col < 5U; col++)
                {
                    uint8_t byte = hmi_font_column(widget->content.text.text[n], col);

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

/*
    合成一整行：先铺背景，再把所有覆盖这一行的 widget 依次叠上去。
    数组顺序就是图层顺序 —— 后面的盖在前面的上面。
*/
static void page_row_compose(uint16_t screen_y)
{
    uint16_t i;
    uint8_t  k;

    /* ① 整行铺背景。★ 一整行只铺一次，所以上一个 widget 不会被擦掉。 */
    for(i = 0U; i < HMI_WIDTH; i++)
    {
        pixel_put(i, job.page.background);
    }

    /* ② 逐个 item 叠上去 */
    for(k = 0U; k < job.page.count; k++)
    {
        const hmi_page_item_t *item = &job.page.items[k];

        /* 这个 widget 盖住 screen_y 这一行吗？没盖住就跳过。
           （用 uint32_t 算，避免 y + height 越过 65535 绕回来） */
        if(((uint32_t)screen_y < (uint32_t)item->y) ||
           ((uint32_t)screen_y >= ((uint32_t)item->y + (uint32_t)item->widget->height)))
        {
            continue;
        }

        /*
            盖住了，画它这一行。
            注意这个减法：widget 不记自己在屏幕哪儿，
            所以"屏幕第几行 → 它自己的第几行"只能在同时知道两边的这一层做。
        */
        widget_row(item->widget,
                   (uint16_t)(screen_y - item->y),
                   item->x);
    }
}

/* 这个函数负责将一页进行绘制 */
hmi_render_result_enum hmi_render_page(const hmi_page_t *page,
                                       uint16_t first_y,
                                       uint16_t last_y)
{
    uint8_t k;

    if((page == NULL) ||
       (page->count > HMI_PAGE_MAX_ITEMS) ||
       (first_y > last_y) ||
       (first_y >= HMI_HEIGHT))
    {
        return HMI_RENDER_ERROR;
    }

    /* 每个 item 都得指向一个真能画的 widget */
    for(k = 0U; k < page->count; k++)
    {
        if((page->items[k].widget == NULL) ||
           (page->items[k].widget->height == 0U))
        {
            return HMI_RENDER_ERROR;
        }
    }

    if(job.active != 0U)
    {
        return HMI_RENDER_BUSY;   /* 上一份还在画，这次不收 */
    }

    /*
        ★ 整份拷进记账本，不是存指针。
        hmi_render_draw() 会在栈上造一个临时 page，出了函数就没了；
        只存指针的话，service 过一会儿读到的就是已经不属于你的内存。
    */
    job.page = *page;

    job.last_row = last_y;
    if(job.last_row >= HMI_HEIGHT)
    {
        job.last_row = (uint16_t)(HMI_HEIGHT - 1U);   /* 夹到屏幕底边 */
    }

    job.row    = first_y;   /* 书签从第一行开始 */
    job.active = 1U;

    return HMI_RENDER_ACCEPTED;
}

/*这个函数负责填充job*/
hmi_render_result_enum hmi_render_draw(const hmi_widget_t *widget,
                                       uint16_t x,
                                       uint16_t y,
                                       uint16_t background)
{
    hmi_page_t page;

    if(widget == NULL)
    {
        return HMI_RENDER_ERROR;
    }

    /*
        造一个只放一个 item 的页。
        这个 page 在栈上，不要紧 —— hmi_render_page() 会把它整份拷走。
    */
    page.background      = background;
    page.count           = 1U;
    page.items[0].widget = widget;
    page.items[0].x      = x;
    page.items[0].y      = y;

    return hmi_render_page(&page, y, (uint16_t)(y + widget->height - 1U));
}

/* 这个函数负责将job的内容交给flush来进行工作 */
void hmi_render_service(void)
{
    display_submit_enum submit;

    //推进行缓冲
    if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
    {
        job.active = 0U;
    }

    //没活就下班。注意上面那步已经把最后一次 DMA 推过了，不能省略。
    if(job.active == 0U)
    {
        return;
    }

    //合成这一行：铺背景 + 叠上所有覆盖这一行的 widget
    page_row_compose(job.row);

    //提交。job.row 现在就是屏幕行号，不用再加 job.y 了。
    submit = display_render_span(render_line,
                                 0U,
                                 job.row,
                                 HMI_WIDTH);

    if(submit == DISPLAY_SUBMIT_OK)
    {
        if(job.row >= job.last_row)
        {
            job.active = 0U;        /* 整页画完了，收工 */
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
    /* 剩下的是 DISPLAY_SUBMIT_BUSY：什么都不做，下次重试同一行 */
}

uint8_t hmi_render_busy(void)
{
    return job.active;
}
