#include "boot_screen.h"
#include "Display/hmi_page.h"
#include "Display/hmi_render.h"
#include "Display/hmi_types.h"
#include "Display/hmi_widget.h"

/*
    Bootloader 的"卡住"屏。

    视觉上和应用 HMI 保持一致：同一套配色、同一套三档字号（scale 1 / 2 / 3）、
    同样用 1 像素色块当分隔线。现场人员看到的界面语言不会变。

    版面分成两块：

        上半 —— "体检表"：元数据和两个槽现在到底什么状态，现场实测
        下半 —— "结论"：这次的故障原因，以及该怎么办

    体检表里每一行的状态值用颜色区分（绿=正常 / 红=异常），
    所以即使不看文字，扫一眼颜色也能知道坏在哪。
*/

/* 配色 —— 从应用的 hmi_page.c 原样抄 */
#define COLOR_BG     0x10C3U   /* 深色底 */
#define COLOR_TEXT   0xE77DU   /* 主文字 */
#define COLOR_MUTED  0x9D75U   /* 次要文字 */
#define COLOR_LINE   0x3228U   /* 分隔线 */
#define COLOR_OK     0xBF70U   /* 绿 —— 正常 */
#define COLOR_ERROR  0xFCCFU   /* 红 —— 异常 */

/* 版面常量 */
#define SCREEN_MARGIN      16U
#define SCREEN_LINE_WIDTH  288U    /* 320 - 16*2 */
#define TITLE_SCALE        3U
#define MSG_SCALE          2U
#define BODY_SCALE         1U

#define LABEL_X            SCREEN_MARGIN       /* 标签列：16 */
#define VALUE_X            76U                 /* 状态列：16 + 10 个字符宽 */

/*
    widget 实例池。这一屏最多 11 个元素（1 标题 + 2 分隔线
    + 3 组标签/状态 + 2 行结论），16 个槽位留了余量。
*/
#define BOOT_SCREEN_MAX_ITEMS  16U
static hmi_widget_t screen_widgets[BOOT_SCREEN_MAX_ITEMS];

/* 每种故障对应的"结论"两行字。顺序必须和 boot_halt_reason_t 一致。 */
typedef struct
{
    const char *line1;   /* 大字号，说明发生了什么 */
    const char *line2;   /* 小字号，说明该怎么办 */
} screen_message_t;

static const screen_message_t screen_messages[BOOT_HALT_REASON_COUNT] =
{
    { "NO BOOTABLE IMAGE",   "WRITE FIRMWARE VIA CAN" },   /* NO_USABLE_IMAGE */
    { "ACTIVE SLOT DAMAGED", "SERVICE REQUIRED" },         /* TARGET_INVALID */
    { "BOTH SLOTS DAMAGED",  "SERVICE REQUIRED" },         /* FALLBACK_INVALID */
    { "INTERNAL ERROR",      "SERVICE REQUIRED" },         /* JUMP_RETURNED */
};

/* 状态好就是绿，不好就是红。 */
static uint16_t state_color(uint8_t ok)
{
    return (ok != 0U) ? COLOR_OK : COLOR_ERROR;
}

/* 往 widget 里拷一段文字。不用 snprintf —— Bootloader 里没有 stdio。 */
static void screen_set_text(hmi_widget_t *widget, const char *value)
{
    uint8_t index = 0U;

    while ((index < (HMI_TEXT_CAPACITY - 1U)) && (value[index] != '\0'))
    {
        widget->content.text.text[index] = value[index];
        index++;
    }

    widget->content.text.text[index] = '\0';
}

static uint8_t screen_add_rect(hmi_page_t *page,
                               uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height,
                               uint16_t color)
{
    hmi_widget_t    *widget;
    hmi_page_item_t *item;

    if (page->count >= BOOT_SCREEN_MAX_ITEMS)
    {
        return 0U;
    }

    widget = &screen_widgets[page->count];
    item   = &page->items[page->count];

    widget->width              = width;
    widget->height             = height;
    widget->kind               = HMI_WIDGET_RECT;
    widget->content.rect.color = color;

    item->widget = widget;   /* 位置进 item，不进 widget —— 和应用的做法一致 */
    item->x      = x;
    item->y      = y;

    page->count++;
    return 1U;
}

static uint8_t screen_add_text(hmi_page_t *page,
                               uint16_t x, uint16_t y,
                               const char *value, uint8_t scale,
                               uint16_t color)
{
    hmi_widget_t    *widget;
    hmi_page_item_t *item;
    uint16_t         length = 0U;

    if (page->count >= BOOT_SCREEN_MAX_ITEMS)
    {
        return 0U;
    }

    widget = &screen_widgets[page->count];
    item   = &page->items[page->count];

    widget->kind               = HMI_WIDGET_TEXT;
    widget->content.text.color = color;
    widget->content.text.scale = scale;
    screen_set_text(widget, value);

    /* 宽度按实际内容算：每字 6 列（5 列字模 + 1 列间隔），字模 7 行高 */
    while (widget->content.text.text[length] != '\0')
    {
        length++;
    }

    widget->width  = (uint16_t)(length * 6U * scale);
    widget->height = (uint16_t)(7U * scale);

    item->widget = widget;
    item->x      = x;
    item->y      = y;

    page->count++;
    return 1U;
}

void boot_screen_show_halt(const boot_halt_info_t *info)
{
    hmi_page_t               page;
    const screen_message_t  *message;

    if ((info == 0) || (info->reason >= BOOT_HALT_REASON_COUNT))
    {
        return;
    }

    message = &screen_messages[info->reason];

    page.background = COLOR_BG;
    page.count      = 0U;

    /* ── 标题 ── */
    (void)screen_add_text(&page, SCREEN_MARGIN, 16U,
                          "BOOT FAILED", TITLE_SCALE, COLOR_ERROR);

    (void)screen_add_rect(&page, SCREEN_MARGIN, 52U,
                          SCREEN_LINE_WIDTH, 1U, COLOR_LINE);

    /* ── 体检表：元数据 ── */
    (void)screen_add_text(&page, LABEL_X, 68U,
                          "META", BODY_SCALE, COLOR_MUTED);
    (void)screen_add_text(&page, VALUE_X, 68U,
                          (info->meta_ok != 0U) ? "OK" : "UNREADABLE",
                          BODY_SCALE, state_color(info->meta_ok));

    /* ── 体检表：槽 A ── */
    (void)screen_add_text(&page, LABEL_X, 88U,
                          "SLOT A", BODY_SCALE, COLOR_MUTED);
    (void)screen_add_text(&page, VALUE_X, 88U,
                          (info->slot_a_ok != 0U) ? "VALID" : "INVALID",
                          BODY_SCALE, state_color(info->slot_a_ok));

    /* ── 体检表：槽 B ── */
    (void)screen_add_text(&page, LABEL_X, 108U,
                          "SLOT B", BODY_SCALE, COLOR_MUTED);
    (void)screen_add_text(&page, VALUE_X, 108U,
                          (info->slot_b_ok != 0U) ? "VALID" : "INVALID",
                          BODY_SCALE, state_color(info->slot_b_ok));

    (void)screen_add_rect(&page, SCREEN_MARGIN, 140U,
                          SCREEN_LINE_WIDTH, 1U, COLOR_LINE);

    /* ── 结论：这次是哪种故障、该怎么办 ── */
    (void)screen_add_text(&page, SCREEN_MARGIN, 165U,
                          message->line1, MSG_SCALE, COLOR_TEXT);
    (void)screen_add_text(&page, SCREEN_MARGIN, 195U,
                          message->line2, BODY_SCALE, COLOR_MUTED);

    /*
        提交整屏，然后同步刷完。

        hmi_render_page() 只把页拷进记账本并置忙；真正一行一行刷出去
        靠反复调用 hmi_render_service() —— 应用是在 1ms 任务里调它，
        Bootloader 没有任务循环，就地转到底。
    */
    if (hmi_render_page(&page, 0U, (uint16_t)(HMI_HEIGHT - 1U)) != HMI_RENDER_ACCEPTED)
    {
        return;
    }

    while (hmi_render_busy() != 0U)
    {
        hmi_render_service();
    }
}
