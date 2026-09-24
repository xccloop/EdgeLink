#include "hmi_render.h"
#include "Presentation/Hmi/hmi_types.h"
#include "Presentation/Buffer/display_buffer.h"

/*
    640字节是CPU合成工作区，display_render_span会复制它，不交给DMA持有。
    DMA仍然只使用原有两个行缓冲槽，不增加全屏缓存，不改变Buffer状态机。
*/
static uint8_t render_line[HMI_WIDTH * 2U];
static struct
{
    const hmi_scene_t *scene;
    hmi_widget_t image;
    uint16_t x, y, row, last_row, width;
    uint8_t active, failed;
} render_job;

static void pixel_put(uint16_t x, uint16_t color)
{
    render_line[x * 2U] = (uint8_t)(color >> 8);
    render_line[x * 2U + 1U] = (uint8_t)color;
}

static void scene_line_compose(uint16_t y)
{
    uint16_t x;
    uint8_t item_index;
    const hmi_scene_t *scene = render_job.scene;
    for(x = 0U; x < HMI_WIDTH; x++) pixel_put(x, scene->background);
    /* 后面的元素覆盖前面的；文字空白点保留已有背景。 */
    for(item_index = 0U; item_index < scene->count; item_index++)
    {
        const hmi_element_t *item = &scene->items[item_index];
        if((y < item->y) || (item->x >= HMI_WIDTH)) continue;
        if(item->kind == HMI_ELEMENT_RECT)
        {
            uint32_t end = (uint32_t)item->x + item->width;
            if((uint32_t)(y - item->y) >= item->height) continue;
            if(end > HMI_WIDTH) end = HMI_WIDTH;
            for(x = item->x; x < end; x++) pixel_put(x, item->color);
        }
        else
        {
            uint8_t character_index;
            uint16_t glyph_row = (uint16_t)((y - item->y) / item->scale);
            if(glyph_row >= 7U) continue;
            for(character_index = 0U; character_index < HMI_TEXT_CAPACITY; character_index++)
            {
                uint8_t column;
                uint8_t character = (uint8_t)item->text[character_index];
                uint32_t origin = item->x + (uint32_t)character_index * 6U * item->scale;
                if((character == 0U) || (origin >= HMI_WIDTH)) break;
                for(column = 0U; column < 5U; column++)
                {
                    uint8_t repeat;
                    if((hmi_font_column(character, column) & (1U << glyph_row)) == 0U) continue;
                    for(repeat = 0U; repeat < item->scale; repeat++)
                    {
                        uint32_t destination = origin + column * item->scale + repeat;
                        if(destination < HMI_WIDTH) pixel_put((uint16_t)destination, item->color);
                    }
                }
            }
        }
    }
}

hmi_render_result_enum hmi_render_widget(const hmi_widget_t *widget, uint16_t x, uint16_t y)
{
    if((widget == 0) || (widget->pixels == 0) || (widget->width == 0U) ||
       (widget->height == 0U) || (x >= HMI_WIDTH) || (y >= HMI_HEIGHT)) return HMI_RENDER_ERROR;
    if(render_job.failed != 0U) return HMI_RENDER_ERROR;
    if(render_job.active != 0U) return HMI_RENDER_BUSY;
    render_job.scene = 0;
    render_job.image = *widget;
    render_job.x = x;
    render_job.y = y;
    render_job.row = 0U;
    render_job.width = widget->width;
    if(render_job.width > HMI_WIDTH - x) render_job.width = HMI_WIDTH - x;
    render_job.last_row = widget->height;
    if(render_job.last_row > HMI_HEIGHT - y) render_job.last_row = HMI_HEIGHT - y;
    render_job.last_row--;
    render_job.active = 1U;
    return HMI_RENDER_ACCEPTED;
}

hmi_render_result_enum hmi_render_scene(const hmi_scene_t *scene, uint16_t first_y, uint16_t last_y)
{
    uint8_t i;
    if((scene == 0) || (scene->count > HMI_SCENE_MAX_ITEMS) ||
       (first_y > last_y) || (last_y >= HMI_HEIGHT)) return HMI_RENDER_ERROR;
    for(i = 0U; i < scene->count; i++)
    {
        if((scene->items[i].kind != HMI_ELEMENT_RECT) && (scene->items[i].kind != HMI_ELEMENT_TEXT)) return HMI_RENDER_ERROR;
        if((scene->items[i].kind == HMI_ELEMENT_TEXT) &&
           ((scene->items[i].scale == 0U) || (scene->items[i].scale > 8U))) return HMI_RENDER_ERROR;
    }
    if(render_job.failed != 0U) return HMI_RENDER_ERROR;
    if(render_job.active != 0U) return HMI_RENDER_BUSY;
    render_job.scene = scene;
    render_job.x = 0U;
    render_job.y = 0U;
    render_job.row = first_y;
    render_job.last_row = last_y;
    render_job.width = HMI_WIDTH;
    render_job.active = 1U;
    return HMI_RENDER_ACCEPTED;
}

void hmi_render_service(void)
{
    display_submit_enum result;
    uint16_t x;
    /* 即使空闲也释放已发送槽并启动最后的READY行；错误锁存到明确重画。 */
    if(display_buffer_service() == DISPLAY_BUFFER_FAIL)
    {
        render_job.active = 0U;
        render_job.failed = 1U;
    }
    if((render_job.active == 0U) || (render_job.failed != 0U)) return;
    if(render_job.scene != 0) scene_line_compose(render_job.row);
    else
    {
        uint32_t offset = (uint32_t)render_job.row * render_job.image.width;
        for(x = 0U; x < render_job.width; x++) pixel_put(x, render_job.image.pixels[offset + x]);
    }
    result = display_render_span(render_line, render_job.x,
                                 render_job.y + render_job.row, render_job.width);
    /* BUSY不推进row；下次重试同一行。每次最多提交一行，避免占满CPU。 */
    if(result == DISPLAY_SUBMIT_OK)
    {
        if(render_job.row == render_job.last_row) render_job.active = 0U;
        else render_job.row++;
    }
    else if(result == DISPLAY_SUBMIT_ERROR)
    {
        render_job.active = 0U;
        render_job.failed = 1U;
    }
}
uint8_t hmi_render_busy(void) { return render_job.active; }
uint8_t hmi_render_failed(void) { return render_job.failed; }
void hmi_render_clear_error(void) { render_job.failed = 0U; }
