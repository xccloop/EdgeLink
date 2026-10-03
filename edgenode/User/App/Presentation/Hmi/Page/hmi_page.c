#include "hmi_page.h"
#include "Hmi/hmi_widget.h"
#include "Presentation/Hmi/hmi_types.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* 颜色 —— 从旧代码原样抄 */
#define COLOR_BG     0x10C3U   /* 深色底 */
#define COLOR_TEXT   0xE77DU   /* 主文字 */
#define COLOR_MUTED  0x9D75U   /* 次要文字 */
#define COLOR_LINE   0x3228U   /* 分隔线 */
#define COLOR_ACCENT 0xBF70U   /* 强调（绿） */
#define COLOR_ERROR  0xFCCFU   /* 离线（红） */
#define COLOR_WAIT   0xEE6FU   /* 未知（黄） */

/* widget 实例池：同一时刻只画一页，所以所有页共用这 16 个槽位。 */
static hmi_widget_t page_widgets[HMI_PAGE_MAX_ITEMS];

/* 导航标题。顺序必须和 hmi_page_id_t 一致。 */
static const char *const titles[] = {"HOME", "LINKS", "EVENTS", "BACKLOG"};

/* 导航条：4 格平分，左右各留 5 像素。 */
#define NAV_ITEM_X_STEP   78U
#define NAV_ITEM_X_FIRST  5U
#define NAV_ITEM_WIDTH    73U

/* 往页里加一块色块。成功返回 1，页满了返回 0。 */
static uint8_t page_add_rect(hmi_page_t *page,
                                   uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   uint16_t color)
{
    hmi_widget_t    *w;
    hmi_page_item_t *item;

    if(page->count >= HMI_PAGE_MAX_ITEMS)
    {
        return 0U;
    }

    w    = &page_widgets[page->count];
    item = &page->items[page->count];

    w->width              = width;
    w->height             = height;
    w->kind               = HMI_WIDGET_RECT;
    w->content.rect.color = color;

    item->widget = w;      /* ★ 位置进 item，不进 widget */
    item->x      = x;
    item->y      = y;

    page->count++;
    return 1U;
}

/* 往页里加一段文字。宽高按内容自动算，不用手填。成功返回 1，页满了返回 0。 */
static uint8_t page_add_text(hmi_page_t *page,
                                   uint16_t x, uint16_t y,
                                   const char *value, uint8_t scale,
                                   uint16_t color)
{
    hmi_widget_t    *w;
    hmi_page_item_t *item;

    if(page->count >= HMI_PAGE_MAX_ITEMS)
    {
        return 0U;
    }

    w    = &page_widgets[page->count];
    item = &page->items[page->count];

    w->kind               = HMI_WIDGET_TEXT;
    w->content.text.color = color;
    w->content.text.scale = scale;
    (void)snprintf(w->content.text.text, HMI_TEXT_CAPACITY, "%s", value);

    /* 宽高按实际内容算：每字 6 列（5 列字模 + 1 列间隔），字模 7 行高 */
    w->width  = (uint16_t)(strlen(w->content.text.text) * 6U * scale);
    w->height = (uint16_t)(7U * scale);

    item->widget = w;
    item->x      = x;
    item->y      = y;

    page->count++;
    return 1U;
}

static const char *state_name(hmi_link_state_t state)
{
    if(state == HMI_LINK_OFFLINE) return "OFFLINE";
    if(state == HMI_LINK_ONLINE)  return "ONLINE";
    return "UNKNOWN";     
}

static uint16_t state_color(hmi_link_state_t state)
{
    if(state == HMI_LINK_OFFLINE) return COLOR_ERROR;
    if(state == HMI_LINK_ONLINE)  return COLOR_ACCENT;
    return COLOR_WAIT;    
}

static uint8_t links_build(hmi_page_t *page, const hmi_view_data_t *data)
{
    const char *tcp_detail = data->tcp_state == HMI_LINK_ONLINE ? "LAST COMMUNICATION CONFIRMED" :
                             data->tcp_state == HMI_LINK_OFFLINE ? "LAST COMMUNICATION FAILED" : "NO CONFIRMED RESULT";
    const char *can_detail = data->can_state == HMI_LINK_ONLINE ? "LAST COMMUNICATION CONFIRMED" :
                             data->can_state == HMI_LINK_OFFLINE ? "LAST COMMUNICATION FAILED" : "STANDBY / NOT YET VERIFIED";
    return page_add_text(page, 16U, 45U, "CONNECTIVITY", 1U, COLOR_MUTED) &&
           page_add_text(page, 16U, 66U, "TCP", 3U, COLOR_TEXT) &&
           page_add_text(page, 262U, 73U, state_name(data->tcp_state), 1U, state_color(data->tcp_state)) &&
           page_add_text(page, 16U, 99U, tcp_detail, 1U, COLOR_MUTED) &&
           page_add_rect(page, 16U, 116U, 288U, 1U, COLOR_LINE) &&
           page_add_text(page, 16U, 129U, "CAN", 3U, COLOR_TEXT) &&
           page_add_text(page, 262U, 136U, state_name(data->can_state), 1U, state_color(data->can_state)) &&
           page_add_text(page, 16U, 162U, can_detail, 1U, COLOR_MUTED) &&
           page_add_rect(page, 16U, 181U, 288U, 1U, COLOR_LINE) &&
           page_add_text(page, 16U, 193U, "STATUS BASED ON LAST OBSERVATION", 1U, COLOR_MUTED);
}

/*
    EVENTS 页只有两个东西，彼此没有位置关系：

    1. 正文区右上角一行格式说明，字号小，只是告诉看的人每行数据的四段分别是什么；
    2. 左边是滚动日志区，logs[0] 最老、logs[log_count - 1] 最新，最新的在最下面。

    每行的字数由生成日志的一侧按 HMI_LOG_LINE_SIZE 截断，这里不再换行、不再截断。
*/
static uint8_t events_build(hmi_page_t *page, const hmi_view_data_t *data)
{
    uint8_t line;

    /* 16 个字符 × 6 像素 = 96 像素，从 x=208 起，右边正好留出 16 像素边距。 */
    if(!page_add_text(page, 208U, 40U, "DATA/SCALE/TX/RX", 1U, COLOR_MUTED))
    {
        return 0U;
    }

    /* 一条都还没攒到时给一句话，免得看着像屏幕坏了。 */
    if(data->log_count == 0U)
    {
        return page_add_text(page, 16U, 68U, "No log received.", 2U, COLOR_MUTED);
    }

    for(line = 0U; line < data->log_count; line++)
    {
        if(!page_add_text(page, 16U, (uint16_t)(68U + line * 22U),
                          data->logs[line], 2U, COLOR_TEXT))
        {
            return 0U;
        }
    }

    return 1U;
}


/* 此函数用于绘制home页面 */
static uint8_t home_build(hmi_page_t *page, const hmi_view_data_t *data)
{
    char buffer[HMI_TEXT_CAPACITY];
    uint8_t scale, length;
    int32_t value = data->temperature_deci_c;
    uint32_t magnitude = (uint32_t)((value < 0) ? -value : value);

    //以下都是调用然后绘制文本
    if(data->temperature_valid != 0U)
        (void)snprintf(buffer, sizeof(buffer), "%s%lu.%lu", (value < 0) ? "-" : "",
                       (unsigned long)(magnitude / 10U),
                       (unsigned long)(magnitude % 10U));
    else
        (void)snprintf(buffer, sizeof(buffer), "--.-");

    length = (uint8_t)strlen(buffer);
    /* 常用温度用 7 倍大字；极端值缩小，负号和摄氏度单位不会越界。 */
    scale = (length <= 5U) ? 7U : 5U;

    if(!page_add_text(page, 16U, 45U, "TEMPERATURE", 1U, COLOR_MUTED) ||
       !page_add_text(page, 16U, 78U, buffer, scale, COLOR_TEXT) ||
       !page_add_text(page, (uint16_t)(24U + length * 6U * scale), 96U,
             "\x7f""C", 2U, COLOR_MUTED) ||
       !page_add_rect(page, 16U, 151U, 288U, 1U, COLOR_LINE))
    {
        return 0U;
    }

    (void)snprintf(buffer, sizeof(buffer), "SSID: %.32s",
                   (data->wifi_state == HMI_LINK_ONLINE && data->wifi_ssid[0])
                       ? data->wifi_ssid : "--");
    if(!page_add_text(page, 76U, 163U, buffer, 1U, COLOR_MUTED)) return 0U;

    (void)snprintf(buffer, sizeof(buffer), "IP: %.15s",
                   (data->wifi_state == HMI_LINK_ONLINE && data->local_ip[0])
                       ? data->local_ip : "--");
    return page_add_text(page, 76U, 179U, buffer, 1U, COLOR_MUTED);
}

/*
    BACKLOG 页只回答一个问题：还有多少条数据压在 Flash 里、没被 Hub 确认。
    0 表示全都上报过了；非 0 说明补发还没追上，或者链路本身有问题。
    （显示的是 log 模块的积压量；屏幕用 BACKLOG 是因为它对现场人更直观。）
*/
static uint8_t backlog_build(hmi_page_t *page, const hmi_view_data_t *data)
{
    char buffer[HMI_TEXT_CAPACITY];
    uint8_t length;
    uint8_t scale;
    uint8_t all_synced = (data->pending_count == 0U) ? 1U : 0U;

    (void)snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)data->pending_count);
    length = (uint8_t)strlen(buffer);
    /* 位数多了就缩小字号，别顶出右边界。 */
    scale = (length <= 3U) ? 7U : 5U;

    return page_add_text(page, 16U, 60U, "PENDING RECORDS", 1U, COLOR_MUTED) &&
           page_add_text(page, 16U, 88U, buffer, scale, COLOR_TEXT) &&
           page_add_rect(page, 16U, 160U, 288U, 1U, COLOR_LINE) &&
           page_add_text(page, 16U, 172U,
                         (all_synced != 0U) ? "ALL DATA UPLOADED"
                                            : "WAITING TO BE SENT",
                         1U,
                         (all_synced != 0U) ? COLOR_ACCENT : COLOR_WAIT);
}

uint8_t hmi_page_build(hmi_page_t *page, const hmi_view_data_t *data,hmi_page_id_t current_page, hmi_page_id_t selected_page)
{
    char buffer[HMI_TEXT_CAPACITY];

    //参数校验
    if((page == NULL) || (data == NULL) ||(current_page >= HMI_PAGE_COUNT) || (selected_page >= HMI_PAGE_COUNT))
    {
        return 0U;
    }

    //初始化page
    memset(page, 0, sizeof(*page));

    page->background = COLOR_BG;

    //这两句话是三个页面都有的顶部界面
    snprintf(buffer, sizeof(buffer), "EDGE / NODE %02u", (unsigned)data->node_id);
    if(!page_add_text(page, 12U, 10U, buffer, 1U, COLOR_ACCENT))
    {
        return 0U;
    }

    //这里是绘制当前页面是什么
    if(!page_add_text(page, (uint16_t)((HMI_WIDTH - strlen(titles[current_page]) * 6U) / 2U),
                  10U, titles[current_page], 1U, COLOR_ACCENT))
    {
        return 0U;
    }

    snprintf(buffer, sizeof(buffer), "WI-FI %s", state_name(data->wifi_state));
    if(!page_add_text(page, 230U, 10U, buffer, 1U, state_color(data->wifi_state)) ||
    !page_add_rect(page, 0U, 28U, HMI_WIDTH, 1U, COLOR_LINE))
    {
        return 0U;
    }

    uint8_t page_result;
    //以下是选择绘制哪个页面
    if(current_page == HMI_PAGE_HOME)
    {
        page_result = home_build(page, data);
    }
    else if(current_page == HMI_PAGE_LINKS)
    {
        page_result = links_build(page, data);
    }
    else if(current_page == HMI_PAGE_BACKLOG)
    {
        page_result = backlog_build(page, data);
    }
    else
    {
        page_result = events_build(page, data);
    }
    if(page_result == 0U)
    {
        return 0U;
    }

    //这里绘制的是底部导航页：4 格平分
    for(uint8_t i = 0U; i < HMI_PAGE_COUNT; i++)
    {
        uint16_t x = (uint16_t)(NAV_ITEM_X_FIRST + NAV_ITEM_X_STEP * i);

        /* 只有"选中"项画实心块。当前是哪页看头部，导航条里不区分。 */
        if(i == selected_page)
        {
            if(!page_add_rect(page, x, 216U, NAV_ITEM_WIDTH, 20U, COLOR_ACCENT))
            {
                return 0U;
            }
        }

        /* 所有标题都画：选中项用深色字（配亮绿底），其他用灰字 */
        if(!page_add_text(page,
                        (uint16_t)(x + (NAV_ITEM_WIDTH - strlen(titles[i]) * 6U) / 2U),
                        222U, titles[i], 1U,
                        (i == selected_page) ? COLOR_BG : COLOR_MUTED))
        {
            return 0U;
        }
    }
    return 1U;//success
}