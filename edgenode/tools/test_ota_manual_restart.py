"""Host regression for a verified OTA image waiting for a user restart."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ota.c"
#include "Presentation/Hmi/Page/hmi_page.h"
#include "IPS/ips.h"

static uint8_t relay[4096];
static unsigned writes, erases;
static int corrupt_read;
const uint8_t board_id = 3U;
test_scb_t test_scb;
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length) {
    assert(address >= OTA_RELAY_IMAGE_BASE);
    assert(address - OTA_RELAY_IMAGE_BASE + length <= sizeof(relay));
    memcpy(data, relay + address - OTA_RELAY_IMAGE_BASE, length);
    if(corrupt_read && length) data[0] ^= 1U;
    return 1U;
}
uint8_t gd25_write(uint32_t address, const uint8_t *data, uint16_t length) {
    assert(address >= OTA_RELAY_IMAGE_BASE);
    assert(address - OTA_RELAY_IMAGE_BASE + length <= sizeof(relay));
    memcpy(relay + address - OTA_RELAY_IMAGE_BASE, data, length);
    writes++;
    return 1U;
}
uint8_t gd25_clear(uint32_t address) {
    assert(address == OTA_RELAY_IMAGE_BASE);
    memset(relay, 0xFF, sizeof(relay)); erases++;
    return 1U;
}
uint8_t ota_metadata_load(ota_metadata_t *meta) { (void)meta; return OTA_METADATA_LOAD_FAIL; }
uint8_t ota_metadata_save(const ota_metadata_t *meta) { (void)meta; return OTA_METADATA_FAIL; }
void delay_ms(uint32_t ms) { (void)ms; assert(0); }
ips_flush_result_enum ips_flush_line(const uint8_t *data, uint16_t x, uint16_t y, uint16_t width) {
    (void)data; (void)x; (void)y; (void)width; assert(0); return IPS_FLUSH_ERROR;
}

static void check_pages(uint8_t ready) {
    hmi_view_data_t data = {0};
    hmi_page_t page;
    unsigned current, valid;
    data.node_id = board_id;
    data.firmware_version = UINT32_MAX;
    data.ota_restart_required = ready;
    data.log_count = HMI_LOG_LINES;
    for(valid = 0; valid < 2; valid++) {
        data.firmware_version_valid = (uint8_t)valid;
        for(current = 0; current < HMI_PAGE_COUNT; current++) {
            unsigned i, hints = 0;
            uint16_t version_end = 0;
            assert(hmi_page_build(&page, &data, (hmi_page_id_t)current, (hmi_page_id_t)current));
            for(i = 0; i < page.count; i++) {
                const hmi_page_item_t *item = &page.items[i];
                const hmi_widget_t *widget = item->widget;
                if(widget->kind != HMI_WIDGET_TEXT) continue;
                if(strncmp(widget->content.text.text, "FW ", 3) == 0) version_end = item->x + widget->width;
                if(strcmp(widget->content.text.text, "New Version please Restart") == 0) {
                    hints++;
                    assert(item->y == 30 && widget->content.text.scale == 1);
                    assert(item->x >= version_end && item->x + widget->width <= HMI_WIDTH);
                }
            }
            assert(version_end > 0 && hints == ready);
        }
    }
}

static ota_result_t receive_image(int corrupt) {
    struct { ota_image_header_t header; uint8_t application[8]; } image;
    uint16_t sequence;
    ota_result_t result = {0};
    memset(&image, 0, sizeof(image));
    image.header.magic = OTA_IMAGE_MAGIC;
    image.header.header_version = OTA_IMAGE_FORMAT_VERSION;
    image.header.header_size = OTA_IMAGE_HEADER_SIZE;
    image.header.hardware_model = OTA_CURRENT_HARDWARE_MODEL;
    image.header.firmware_version = 2U;
    image.header.target_slot = OTA_SLOT_B_BASE_ADDRESS;
    image.header.application_length = sizeof(image.application);
    image.header.application_crc32 = crc32_generate(image.application, sizeof(image.application));
    image.header.header_crc32 = crc32_generate((const uint8_t *)&image.header, OTA_IMAGE_HEADER_CRC_LENGTH);
    ota_reset(); writes = erases = 0; corrupt_read = corrupt;
    for(sequence = 0; sequence < sizeof(image) / OTA_FRAME_PAYLOAD; sequence++) {
        uint8_t frame[OTA_FRAME_LENGTH];
        frame[0] = (uint8_t)(sequence >> 8); frame[1] = (uint8_t)sequence;
        memcpy(frame + 2, (const uint8_t *)&image + sequence * OTA_FRAME_PAYLOAD, OTA_FRAME_PAYLOAD);
        result = ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    }
    return result;
}

int main(void) {
    unsigned i, completed_writes, completed_erases;
    uint8_t frame[OTA_FRAME_LENGTH] = {0};
    uint8_t query[CAN_OTA_QUERY_LENGTH] = {CAN_OTA_QUERY_KIND_SLOT, 0U, 7U, 0U};
    uint16_t last_sequence;
    ota_result_t result;
    ota_reset(); assert(ota_restart_required_get() == 0U); check_pages(0);
    result = receive_image(0);
    assert(result.finished == 1U && result.success == 1U && ota_restart_required_get() == 1U);
    last_sequence = result.contiguous;
    result = ota_on_frame(CAN_OTA_QUERY_BASE_ID + board_id, query, sizeof(query));
    assert(result.reply == 1U && result.query_status == CAN_OTA_SLOT_QUERY_PENDING && result.request_id == 7U);
    check_pages(ota_restart_required_get());
    completed_writes = writes; completed_erases = erases;
    for(i = 0; i < (OTA_TRANSFER_TIMEOUT_MS * 4U) / OTA_IDLE_REPLY_MS; i++) {
        result = ota_on_idle();
        assert(result.reply == 0U && result.finished == 0U);
    }
    assert(ota_restart_required_get() == 1U); /* Verified image survives the user wait. */
    (void)ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    assert(writes == completed_writes && erases == completed_erases);
    assert(ota_restart_required_get() == 1U);
    frame[0] = (uint8_t)(last_sequence >> 8); frame[1] = (uint8_t)last_sequence;
    result = ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    assert(result.finished == 1U && result.success == 1U && result.contiguous == last_sequence);
    assert(writes == completed_writes && erases == completed_erases);
    result = ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id + 1U, frame, sizeof(frame));
    assert(result.finished == 0U);
    result = ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame) - 1U);
    assert(result.finished == 0U);
    memset(frame, 0, sizeof(frame));
    ota_reset(); assert(ota_restart_required_get() == 0U); check_pages(0);
    result = receive_image(1);
    assert(result.finished == 0U && ota_restart_required_get() == 0U);
    corrupt_read = 0;
    (void)ota_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    for(i = 0; i <= OTA_TRANSFER_TIMEOUT_MS / OTA_IDLE_REPLY_MS; i++) (void)ota_on_idle();
    assert(ota_state.received_bytes == 0U && ota_restart_required_get() == 0U);
    puts("OTA manual-restart and all-page restart hint regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="edgelink-ota-restart-") as temp:
    directory = Path(temp)
    (directory / "gd32f10x.h").write_text(
        "#include <stdint.h>\ntypedef struct { uint32_t VTOR; } test_scb_t;\n"
        "extern test_scb_t test_scb;\n#define SCB (&test_scb)\n")
    (directory / "test.c").write_text(SOURCE)
    compiler = os.environ.get("HOST_CC", "gcc")
    if not shutil.which(compiler):
        raise SystemExit("Host C compiler missing; set HOST_CC")
    binary = directory / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-int-to-pointer-cast",
                    "-I" + str(directory), "-I" + str(ROOT / "User/App/Service/Ota"),
                    "-I" + str(ROOT / "User/App"), "-I" + str(ROOT / "Common"),
                    "-I" + str(ROOT / "Drivers/BSP"),
                    "-I" + str(ROOT / "Drivers/BOARD"),
                    str(directory / "test.c"), str(ROOT / "Common/CRC/crc32.c"),
                    str(ROOT / "User/App/Presentation/Hmi/Page/hmi_page.c"),
                    str(ROOT / "Common/Hmi/hmi_widget.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
