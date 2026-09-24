#include "hmi_page.h"
#include <stdio.h>
#include <string.h>

#define COLOR_BG     0x10C3U
#define COLOR_TEXT   0xE77DU
#define COLOR_MUTED  0x9D75U
#define COLOR_LINE   0x3228U
#define COLOR_ACCENT 0xBF70U
#define COLOR_ERROR  0xFCCFU
#define COLOR_WAIT   0xEE6FU

static uint8_t rect(hmi_scene_t *scene, uint16_t x, uint16_t y,
                    uint16_t width, uint16_t height, uint16_t color)
{
    hmi_element_t *element;
    if(scene->count >= HMI_SCENE_MAX_ITEMS) return 0U;
    element = &scene->items[scene->count++];
    element->kind = HMI_ELEMENT_RECT;
    element->x = x; element->y = y;
    element->width = width; element->height = height;
    element->color = color;
    return 1U;
}

static uint8_t text(hmi_scene_t *scene, uint16_t x, uint16_t y,
                    const char *value, uint8_t scale, uint16_t color)
{
    hmi_element_t *element;
    if(scene->count >= HMI_SCENE_MAX_ITEMS) return 0U;
    element = &scene->items[scene->count++];
    element->kind = HMI_ELEMENT_TEXT;
    element->x = x; element->y = y;
    element->color = color; element->scale = scale;
    (void)snprintf(element->text, sizeof(element->text), "%s", value);
    return 1U;
}

static const char *state_name(hmi_link_state_t state)
{
    if(state == HMI_LINK_ONLINE) return "ONLINE";
    if(state == HMI_LINK_OFFLINE) return "OFFLINE";
    return "UNKNOWN";
}

static uint16_t state_color(hmi_link_state_t state)
{
    if(state == HMI_LINK_ONLINE) return COLOR_ACCENT;
    if(state == HMI_LINK_OFFLINE) return COLOR_ERROR;
    return COLOR_WAIT;
}

static uint8_t home_build(hmi_scene_t *scene, const hmi_view_data_t *data)
{
    char buffer[HMI_TEXT_CAPACITY];
    uint8_t scale, length;
    int32_t value = data->temperature_deci_c;
    uint32_t magnitude = (uint32_t)((value < 0) ? -value : value);
    if(data->temperature_valid != 0U)
        (void)snprintf(buffer, sizeof(buffer), "%s%lu.%lu", (value < 0) ? "-" : "",
                       (unsigned long)(magnitude / 10U), (unsigned long)(magnitude % 10U));
    else (void)snprintf(buffer, sizeof(buffer), "--.-");
    length = (uint8_t)strlen(buffer);
    /* 常用温度使用7倍大字；极端值缩小，负号和摄氏度单位不会越界。 */
    scale = (length <= 5U) ? 7U : 5U;
    if(!text(scene, 16U, 45U, "TEMPERATURE", 1U, COLOR_MUTED) ||
       !text(scene, 16U, 78U, buffer, scale, COLOR_TEXT) ||
       !text(scene, (uint16_t)(24U + length * 6U * scale), 96U, "\x7f""C", 2U, COLOR_MUTED) ||
       !rect(scene, 16U, 151U, 288U, 1U, COLOR_LINE) ||
       !text(scene, 16U, 163U, "BMP280", 1U, COLOR_MUTED)) return 0U;
    (void)snprintf(buffer, sizeof(buffer), "SSID: %.32s",
                   (data->wifi_state == HMI_LINK_ONLINE && data->wifi_ssid[0]) ? data->wifi_ssid : "--");
    if(!text(scene, 76U, 163U, buffer, 1U, COLOR_MUTED)) return 0U;
    (void)snprintf(buffer, sizeof(buffer), "IP: %.15s",
                   (data->wifi_state == HMI_LINK_ONLINE && data->local_ip[0]) ? data->local_ip : "--");
    return text(scene, 76U, 179U, buffer, 1U, COLOR_MUTED);
}

static uint8_t links_build(hmi_scene_t *scene, const hmi_view_data_t *data)
{
    const char *tcp_detail = data->tcp_state == HMI_LINK_ONLINE ? "LAST COMMUNICATION CONFIRMED" :
                             data->tcp_state == HMI_LINK_OFFLINE ? "LAST COMMUNICATION FAILED" : "NO CONFIRMED RESULT";
    const char *can_detail = data->can_state == HMI_LINK_ONLINE ? "LAST COMMUNICATION CONFIRMED" :
                             data->can_state == HMI_LINK_OFFLINE ? "LAST COMMUNICATION FAILED" : "STANDBY / NOT YET VERIFIED";
    return text(scene, 16U, 45U, "CONNECTIVITY", 1U, COLOR_MUTED) &&
           text(scene, 16U, 66U, "TCP", 3U, COLOR_TEXT) &&
           text(scene, 262U, 73U, state_name(data->tcp_state), 1U, state_color(data->tcp_state)) &&
           text(scene, 16U, 99U, tcp_detail, 1U, COLOR_MUTED) &&
           rect(scene, 16U, 116U, 288U, 1U, COLOR_LINE) &&
           text(scene, 16U, 129U, "CAN", 3U, COLOR_TEXT) &&
           text(scene, 262U, 136U, state_name(data->can_state), 1U, state_color(data->can_state)) &&
           text(scene, 16U, 162U, can_detail, 1U, COLOR_MUTED) &&
           rect(scene, 16U, 181U, 288U, 1U, COLOR_LINE) &&
           text(scene, 16U, 193U, "STATUS BASED ON LAST OBSERVATION", 1U, COLOR_MUTED);
}

/*
    日志只保留最新一条：每行最多24字，6行，优先在空格处换行。
    超出显示区就在第六行末尾放...。所有扫描都受HMI_LOG_CAPACITY限制，
    即使底层调用者误传未终止数组也不会越界。中文等非ASCII由Control替换为?。
*/
static uint8_t log_build(hmi_scene_t *scene, const hmi_view_data_t *data)
{
    char buffer[HMI_TEXT_CAPACITY];
    uint16_t offset = 0U, total = 0U;
    uint8_t line;
    const char *message = data->latest_log;
    uint32_t seconds = data->log_uptime_seconds;
    while((total < HMI_LOG_CAPACITY - 1U) && (message[total] != '\0')) total++;
    if(total == 0U) { message = "No log received."; total = 16U; }
    /* 时间为启动以来的时长，超过24小时继续显示，不伪装为实时时钟。 */
    (void)snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu",
                   (unsigned long)(seconds / 3600U),
                   (unsigned long)((seconds / 60U) % 60U), (unsigned long)(seconds % 60U));
    if(!text(scene, 16U, 44U, "LATEST EVENT", 1U, COLOR_MUTED) ||
       !text(scene, 220U, 44U, buffer, 1U, COLOR_MUTED)) return 0U;
    for(line = 0U; (line < 6U) && (offset < total); line++)
    {
        uint16_t available = total - offset;
        uint8_t count = 0U, last_space = 0U, consumed;
        if(available > 24U) available = 24U;
        while((count < available) && (message[offset + count] != '\n'))
        {
            if(message[offset + count] == ' ') last_space = count;
            count++;
        }
        consumed = count;
        if((count == 24U) && (offset + count < total) &&
           (message[offset + count] != ' ') && (message[offset + count] != '\n') && (last_space > 0U))
        {
            count = last_space;
            consumed = (uint8_t)(last_space + 1U);
        }
        memcpy(buffer, &message[offset], count);
        buffer[count] = '\0';
        offset += consumed;
        if((offset < total) && (message[offset] == '\n')) offset++;
        else while((offset < total) && (message[offset] == ' ')) offset++;
        if((line == 5U) && (offset < total))
        {
            if(count > 21U) count = 21U;
            memcpy(&buffer[count], "...", 4U);
        }
        if(!text(scene, 16U, (uint16_t)(62U + line * 19U), buffer, 2U, COLOR_TEXT)) return 0U;
    }
    return rect(scene, 16U, 184U, 288U, 1U, COLOR_LINE) &&
           text(scene, 16U, 195U, "LATEST ENTRY ONLY / UPTIME", 1U, COLOR_MUTED);
}

uint8_t hmi_page_build(hmi_scene_t *scene, const hmi_view_data_t *data,
                       hmi_page_id_t current_page, hmi_page_id_t selected_page)
{
    static const char *const titles[] = {"HOME", "LINKS", "LOG"};
    char buffer[HMI_TEXT_CAPACITY];
    uint8_t i, result;
    if((scene == 0) || (data == 0) ||
       ((unsigned)current_page >= HMI_PAGE_COUNT) || ((unsigned)selected_page >= HMI_PAGE_COUNT)) return 0U;
    memset(scene, 0, sizeof(*scene));
    scene->background = COLOR_BG;
    (void)snprintf(buffer, sizeof(buffer), "EDGE / NODE %02u", (unsigned)data->node_id);
    if(!text(scene, 12U, 10U, buffer, 1U, COLOR_ACCENT)) return 0U;
    (void)snprintf(buffer, sizeof(buffer), "WI-FI %s", state_name(data->wifi_state));
    if(!text(scene, 230U, 10U, buffer, 1U, state_color(data->wifi_state)) ||
       !rect(scene, 0U, 28U, HMI_WIDTH, 1U, COLOR_LINE)) return 0U;
    if(current_page == HMI_PAGE_HOME) result = home_build(scene, data);
    else if(current_page == HMI_PAGE_LINKS) result = links_build(scene, data);
    else result = log_build(scene, data);
    if(!result || !rect(scene, 0U, HMI_NAV_Y, HMI_WIDTH, 1U, COLOR_LINE)) return 0U;
    for(i = 0U; i < HMI_PAGE_COUNT; i++)
    {
        uint16_t x = (uint16_t)(5U + 105U * i);
        if(i == current_page)
        {
            if(!rect(scene, x, 216U, 100U, 20U, COLOR_ACCENT)) return 0U;
        }
        else if(i == selected_page)
        {
            if(!rect(scene, x, 216U, 100U, 1U, COLOR_ACCENT) ||
               !rect(scene, x, 235U, 100U, 1U, COLOR_ACCENT) ||
               !rect(scene, x, 216U, 1U, 20U, COLOR_ACCENT) ||
               !rect(scene, x + 99U, 216U, 1U, 20U, COLOR_ACCENT)) return 0U;
        }
        if(!text(scene, (uint16_t)(x + (100U - strlen(titles[i]) * 6U) / 2U),
                  222U, titles[i], 1U, (i == current_page) ? COLOR_BG : COLOR_MUTED)) return 0U;
    }
    return 1U;
}
