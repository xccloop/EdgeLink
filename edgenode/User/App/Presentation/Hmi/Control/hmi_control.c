#include "hmi_control.h"
#include "Presentation/Hmi/Page/hmi_page.h"
#include "Presentation/Hmi/Render/hmi_render.h"
#include <string.h>

#define DIRTY_HEADER 1U
#define DIRTY_BODY   2U
#define DIRTY_NAV    4U
#define DIRTY_ALL    7U
static hmi_view_data_t current_data;
static hmi_scene_t drawing_scene;
static hmi_page_id_t current_page, selected_page;
static uint8_t initialized, dirty, page_error;

/*
    当前显示数据和正在绘制的场景分开保存。更新只改current_data；
    Render读取drawing_scene期间绝不重建它，避免一帧上下行来自不同版本。
    下次空闲时用最新数据生成场景，中间更新自然合并，不无限堆积刷新请求。
*/
static void string_copy(char *destination, const char *source, uint16_t capacity, uint8_t log_text)
{
    uint16_t i;
    memset(destination, 0, capacity);
    for(i = 0U; (i < capacity - 1U) && (source[i] != '\0'); i++)
    {
        uint8_t c = (uint8_t)source[i];
        if((c >= 32U) && (c <= 126U)) destination[i] = (char)c;
        else if(log_text && c == '\n') destination[i] = '\n';
        else if(c == '\r' || c == '\t') destination[i] = ' ';
        else destination[i] = '?';
    }
    /* 超长日志在内部截断时也标明省略，防止看起来像完整消息。 */
    if(log_text && (i == capacity - 1U) && (source[i] != '\0'))
        memcpy(&destination[capacity - 4U], "...", 3U);
}

void hmi_init(uint8_t node_id)
{
    /* 幂等初始化，不在DMA工作时清理Buffer或正在使用的场景。 */
    if(initialized) return;
    memset(&current_data, 0, sizeof(current_data));
    current_data.node_id = node_id;
    current_page = selected_page = HMI_PAGE_HOME;
    dirty = DIRTY_ALL;
    initialized = 1U;
}

uint8_t hmi_set_view_data(const hmi_view_data_t *data)
{
    hmi_view_data_t next;
    if(!initialized || data == 0 ||
       (unsigned)data->wifi_state > HMI_LINK_OFFLINE ||
       (unsigned)data->tcp_state > HMI_LINK_OFFLINE ||
       (unsigned)data->can_state > HMI_LINK_OFFLINE) return 0U;
    memset(&next, 0, sizeof(next));
    next.node_id = data->node_id;
    next.temperature_valid = (data->temperature_valid != 0U);
    next.temperature_deci_c = data->temperature_deci_c;
    next.wifi_state = data->wifi_state;
    next.tcp_state = data->tcp_state;
    next.can_state = data->can_state;
    next.log_uptime_seconds = data->log_uptime_seconds;
    string_copy(next.wifi_ssid, data->wifi_ssid, sizeof(next.wifi_ssid), 0U);
    string_copy(next.local_ip, data->local_ip, sizeof(next.local_ip), 0U);
    string_copy(next.latest_log, data->latest_log, sizeof(next.latest_log), 1U);
    if(next.node_id != current_data.node_id || next.wifi_state != current_data.wifi_state)
        dirty |= DIRTY_HEADER;
    if(current_page == HMI_PAGE_HOME &&
       (next.temperature_valid != current_data.temperature_valid ||
        next.temperature_deci_c != current_data.temperature_deci_c ||
        next.wifi_state != current_data.wifi_state ||
        strcmp(next.wifi_ssid, current_data.wifi_ssid) != 0 ||
        strcmp(next.local_ip, current_data.local_ip) != 0)) dirty |= DIRTY_BODY;
    if(current_page == HMI_PAGE_LINKS &&
       (next.tcp_state != current_data.tcp_state || next.can_state != current_data.can_state))
        dirty |= DIRTY_BODY;
    if(current_page == HMI_PAGE_LOG &&
       (next.log_uptime_seconds != current_data.log_uptime_seconds ||
        strcmp(next.latest_log, current_data.latest_log) != 0)) dirty |= DIRTY_BODY;
    current_data = next;
    return 1U;
}

void hmi_key_event(hmi_key_t key)
{
    if(!initialized) return;
    if(key == HMI_KEY_PREVIOUS)
    {
        selected_page = (hmi_page_id_t)((selected_page + HMI_PAGE_COUNT - 1U) % HMI_PAGE_COUNT);
        dirty |= DIRTY_NAV;
    }
    else if(key == HMI_KEY_NEXT)
    {
        selected_page = (hmi_page_id_t)((selected_page + 1U) % HMI_PAGE_COUNT);
        dirty |= DIRTY_NAV;
    }
    else if(key == HMI_KEY_CONFIRM && current_page != selected_page)
    {
        current_page = selected_page;
        dirty |= DIRTY_BODY | DIRTY_NAV;
    }
}

uint8_t hmi_service(void)
{
    uint16_t first_y, last_y;
    hmi_render_result_enum result;
    if(!initialized) return 0U;
    hmi_render_service();
    if(page_error || hmi_render_failed()) return 0U;
    if(hmi_render_busy() || !dirty) return 1U;
    if(!hmi_page_build(&drawing_scene, &current_data, current_page, selected_page))
    {
        page_error = 1U;
        return 0U;
    }
    /* 按区域合并重画：导航只刷28行，正文从29行开始，旧文字随背景一起覆盖。 */
    first_y = (dirty & DIRTY_HEADER) ? 0U : (dirty & DIRTY_BODY) ? 29U : HMI_NAV_Y;
    last_y = (dirty & DIRTY_NAV) ? HMI_HEIGHT - 1U : (dirty & DIRTY_BODY) ? HMI_NAV_Y - 1U : 28U;
    result = hmi_render_scene(&drawing_scene, first_y, last_y);
    if(result == HMI_RENDER_ERROR)
    {
        page_error = 1U;
        return 0U;
    }
    if(result == HMI_RENDER_ACCEPTED) dirty = 0U;
    return 1U;
}

void hmi_refresh(void)
{
    if(!initialized) return;
    hmi_render_clear_error();
    page_error = 0U;
    dirty = DIRTY_ALL;
}
hmi_page_id_t hmi_current_page(void) { return current_page; }
hmi_page_id_t hmi_selected_page(void) { return selected_page; }
