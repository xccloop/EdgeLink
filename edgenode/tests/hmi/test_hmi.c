#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "Presentation/Hmi/Control/hmi_control.h"
#include "Presentation/Hmi/Page/hmi_page.h"
#include "Presentation/Hmi/Render/hmi_render.h"
#include "Presentation/Buffer/display_buffer.h"
#include "IPS/ips.h"

/* Host-only framebuffer: not compiled into firmware. Real Buffer, mocked IPS. */
static uint16_t screen[240][320], saved[240][320];
static const uint8_t *dma_data;
static uint8_t dma_snapshot[640];
static uint16_t dma_x, dma_y, dma_width;
static unsigned dma_polls, accepted, completed, min_y = 240U;
static unsigned hold_dma, fail_dma;

ips_flush_result_enum ips_flush_line(const uint8_t *data, uint16_t x, uint16_t y, uint16_t width)
{
    unsigned i;
    if(dma_data != NULL)
    {
        /* DMA持有的数据在完成前绝不能被CPU改写。 */
        assert(memcmp(dma_data, dma_snapshot, dma_width * 2U) == 0);
        if(fail_dma)
        {
            fail_dma = 0U; dma_data = NULL;
            return IPS_FLUSH_ERROR;
        }
        if(hold_dma || dma_polls++ < 5U) return IPS_FLUSH_BUSY;
        for(i = 0; i < dma_width; i++)
            screen[dma_y][dma_x + i] = (uint16_t)((dma_data[2U*i] << 8) | dma_data[2U*i+1U]);
        completed++;
        dma_data = NULL;
    }
    if(data == NULL) return IPS_FLUSH_COMPLETE;
    assert(width > 0U && width <= 320U && x + width <= 320U && y < 240U);
    dma_data = data; dma_x = x; dma_y = y; dma_width = width;
    memcpy(dma_snapshot, data, width * 2U);
    dma_polls = 0U; accepted++;
    if(y < min_y) min_y = y;
    return IPS_FLUSH_ACCEPTED;
}

static void pump(void)
{
    unsigned i;
    for(i = 0U; i < 4000U; i++) assert(hmi_service() == 1U);
    assert(!hmi_render_busy());
    assert(dma_data == NULL);
}

static void capture(const char *name)
{
    FILE *output = fopen(name, "wb");
    unsigned x, y;
    assert(output != NULL);
    fprintf(output, "P6\n320 240\n255\n");
    for(y = 0; y < 240; y++) for(x = 0; x < 320; x++)
    {
        uint16_t pixel = screen[y][x];
        fputc(((pixel >> 11) & 31U) * 255U / 31U, output);
        fputc(((pixel >> 5) & 63U) * 255U / 63U, output);
        fputc((pixel & 31U) * 255U / 31U, output);
    }
    assert(fclose(output) == 0);
}

static int scene_contains(const hmi_scene_t *scene, const char *needle)
{
    unsigned i;
    for(i = 0; i < scene->count; i++)
        if(scene->items[i].kind == HMI_ELEMENT_TEXT && strstr(scene->items[i].text, needle)) return 1;
    return 0;
}

int main(void)
{
    hmi_view_data_t data = {0};
    hmi_scene_t scene;
    unsigned before, i;
    static const uint16_t colors[] = {0xF800U, 0x07E0U, 0x001FU, 0xFFFFU};
    hmi_widget_t image = {2U, 2U, colors};
    assert(hmi_service() == 0U);
    assert(!hmi_set_view_data(&data));
    assert(display_buffer_init() == DISPLAY_BUFFER_SUCCESS);
    hmi_init(1U);
    pump();
    capture("boot.ppm");
    assert(accepted == 240U && completed == 240U);
    before = accepted; pump(); assert(accepted == before);

    data.node_id = 1U; data.temperature_valid = 1U; data.temperature_deci_c = 246;
    data.wifi_state = HMI_LINK_ONLINE; data.tcp_state = HMI_LINK_ONLINE;
    strcpy(data.wifi_ssid, "EdgeLink-Lab"); strcpy(data.local_ip, "192.168.1.101");
    strcpy(data.latest_log, "TCP ACK received. Sequence 1042 persisted by gateway.");
    data.log_uptime_seconds = 45248U;
    assert(hmi_set_view_data(&data)); pump(); capture("home.ppm");
    before = accepted; assert(hmi_set_view_data(&data)); pump(); assert(accepted == before);
    memcpy(saved, screen, sizeof(saved));

    min_y = 240U; before = accepted;
    hmi_key_event(HMI_KEY_NEXT); pump();
    assert(hmi_current_page() == HMI_PAGE_HOME && hmi_selected_page() == HMI_PAGE_LINKS);
    assert(min_y == 212U && accepted - before == 28U);
    assert(memcmp(saved, screen, 212U * 320U * sizeof(uint16_t)) == 0);
    capture("candidate.ppm");
    hmi_key_event(HMI_KEY_CONFIRM); pump(); capture("links.ppm");
    assert(hmi_current_page() == HMI_PAGE_LINKS);
    hmi_key_event(HMI_KEY_NEXT); hmi_key_event(HMI_KEY_CONFIRM); pump(); capture("log.ppm");
    hmi_key_event(HMI_KEY_NEXT); assert(hmi_selected_page() == HMI_PAGE_HOME);
    hmi_key_event(HMI_KEY_PREVIOUS); assert(hmi_selected_page() == HMI_PAGE_LOG);
    hmi_key_event((hmi_key_t)100); assert(hmi_selected_page() == HMI_PAGE_LOG);

    memset(data.latest_log, 'W', sizeof(data.latest_log));
    assert(hmi_set_view_data(&data)); pump(); capture("long-log.ppm");
    assert(hmi_page_build(&scene, &data, HMI_PAGE_LOG, HMI_PAGE_HOME));
    assert(scene_contains(&scene, "..."));
    for(i = 0; i < scene.count; i++)
    {
        const hmi_element_t *item = &scene.items[i];
        if(item->kind == HMI_ELEMENT_TEXT)
        {
            assert(item->x + strlen(item->text) * 6U * item->scale <= 320U);
            assert(item->y + 7U * item->scale <= 240U);
        }
    }
    strcpy(data.latest_log, "OK"); assert(hmi_set_view_data(&data)); pump();
    memcpy(saved, screen, sizeof(saved)); hmi_refresh(); pump();
    assert(memcmp(saved, screen, sizeof(screen)) == 0); /* no old log remains */

    hmi_key_event(HMI_KEY_NEXT); hmi_key_event(HMI_KEY_CONFIRM); pump();
    data.temperature_deci_c = -1;
    assert(hmi_set_view_data(&data));
    for(i = 0; i < 20; i++) assert(hmi_service());
    data.temperature_deci_c = INT16_MIN; memset(data.wifi_ssid, 'S', sizeof(data.wifi_ssid));
    assert(hmi_set_view_data(&data)); pump(); capture("negative.ppm");
    memcpy(saved, screen, sizeof(saved)); hmi_refresh(); pump();
    assert(memcmp(saved, screen, sizeof(screen)) == 0); /* update during draw coalesced */
    assert(hmi_page_build(&scene, &data, HMI_PAGE_HOME, HMI_PAGE_HOME));
    assert(scene_contains(&scene, "-3276.8"));
    data.wifi_state = HMI_LINK_OFFLINE;
    assert(hmi_set_view_data(&data)); pump();
    assert(hmi_page_build(&scene, &data, HMI_PAGE_HOME, HMI_PAGE_HOME));
    assert(scene_contains(&scene, "SSID: --") && scene_contains(&scene, "IP: --"));
    data.wifi_state = (hmi_link_state_t)-1;
    assert(!hmi_set_view_data(&data)); assert(!hmi_set_view_data(NULL));

    /* 真实Buffer被填满时Render保持当前行，且错误后必须显式恢复。 */
    hold_dma = 1U; hmi_refresh();
    for(i = 0; i < 50; i++) assert(hmi_service());
    assert(hmi_render_busy());
    fail_dma = 1U; assert(!hmi_service()); assert(!hmi_service());
    hold_dma = 0U; hmi_refresh(); pump();

    /* 图片接口裁剪、RGB565高字节在前、只提交一次也能收尾。 */
    assert(hmi_render_widget(&image, 319U, 239U) == HMI_RENDER_ACCEPTED);
    assert(hmi_render_widget(&image, 0U, 0U) == HMI_RENDER_BUSY);
    hmi_render_service(); assert(!hmi_render_busy()); assert(dma_data != NULL);
    pump(); assert(screen[239][319] == 0xF800U);
    assert(hmi_render_widget(&image, 0U, 0U) == HMI_RENDER_ACCEPTED);
    pump(); assert(screen[0][0] == 0xF800U && screen[0][1] == 0x07E0U);
    assert(screen[1][0] == 0x001FU && screen[1][1] == 0xFFFFU);
    assert(hmi_render_widget(&image, 320U, 0U) == HMI_RENDER_ERROR);

    /* 最后一行已提交后发生DMA错误，也必须由空闲service报告。 */
    assert(hmi_render_widget(&image, 319U, 239U) == HMI_RENDER_ACCEPTED);
    hmi_render_service(); assert(!hmi_render_busy()); fail_dma = 1U;
    assert(!hmi_service()); hmi_refresh(); pump();
    assert(!hmi_page_build(NULL, &data, HMI_PAGE_HOME, HMI_PAGE_HOME));
    memset(&scene, 0, sizeof(scene)); scene.count = 1U;
    scene.items[0].kind = HMI_ELEMENT_TEXT; scene.items[0].scale = 0U;
    assert(hmi_render_scene(&scene, 0U, 239U) == HMI_RENDER_ERROR);
    puts("PASS: real Buffer + simulated DMA; navigation, dirty regions, snapshots, wrapping, colors, clipping, BUSY, errors, tail drain.");
    return 0;
}
