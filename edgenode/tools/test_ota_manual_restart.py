"""Host regression for a verified OTA image waiting for a user restart."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import struct
import sys
sys.dont_write_bytecode = True
from cryptography.hazmat.primitives.asymmetric import ec
from make_ota_image import build_header, SLOTS

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
static uint8_t metadata_valid;
static ota_metadata_t metadata;
uint8_t ota_metadata_load(ota_metadata_t *meta) {
    if(!metadata_valid) return OTA_METADATA_LOAD_FAIL;
    *meta = metadata; return OTA_METADATA_LOAD_REAL;
}
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
    static const uint8_t signed_image[] = {SIGNED_IMAGE};
    memcpy(&image, signed_image, sizeof(image));
    if(corrupt == 2) {
        image.header.signature[0] ^= 1U;
        image.header.header_crc32 = crc32_generate((const uint8_t *)&image.header, OTA_IMAGE_HEADER_CRC_LENGTH);
    }
    ota_reset(); writes = erases = 0; corrupt_read = (corrupt == 1);
    for(sequence = 0; sequence < sizeof(image) / OTA_FRAME_PAYLOAD; sequence++) {
        uint8_t frame[OTA_FRAME_LENGTH];
        frame[0] = (uint8_t)(sequence >> 8); frame[1] = (uint8_t)sequence;
        memcpy(frame + 2, (const uint8_t *)&image + sequence * OTA_FRAME_PAYLOAD, OTA_FRAME_PAYLOAD);
        result = ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    }
    return result;
}

static void check_signed_tcp(void) {
    static const uint8_t signed_image[] = {SIGNED_IMAGE};
    uint8_t frame[TCP_OTA_DATA_HEADER_LENGTH + sizeof(signed_image)] = {2U,0U,0U,0U,123U,0U,0U};
    ota_image_header_t header;
    ota_result_t result;
    unsigned mode;
    for(mode = 0U; mode < 3U; ++mode) {
        metadata_valid = 1; metadata.active_slot = OTA_SLOT_A_BASE_ADDRESS;
        metadata.pending_slot = OTA_SLOT_NONE;
        ota_reset(); writes = erases = 0; corrupt_read = 0;
        memcpy(frame + TCP_OTA_DATA_HEADER_LENGTH, signed_image, sizeof(signed_image));
        memcpy(&header, signed_image, sizeof(header));
        if(mode == 1U) header.signature[0] ^= 1U;
        if(mode == 2U) header.header_version = 1U;
        header.header_crc32 = crc32_generate((const uint8_t *)&header, OTA_IMAGE_HEADER_CRC_LENGTH);
        memcpy(frame + TCP_OTA_DATA_HEADER_LENGTH, &header, sizeof(header));
        frame[7] = (uint8_t)(sizeof(signed_image) >> 8); frame[8] = (uint8_t)sizeof(signed_image);
        result = ota_tcp_on_frame(frame, sizeof(frame));
        assert(result.tcp_data_reply && result.tcp_transfer_id == 123U);
        if(mode == 0U) {
            unsigned saved_writes = writes;
            assert(result.success == TCP_OTA_DATA_SUCCESS && ota_restart_required_get());
            result = ota_tcp_on_frame(frame, sizeof(frame));
            assert(result.success == TCP_OTA_DATA_SUCCESS && writes == saved_writes);
        } else {
            assert(result.success == TCP_OTA_DATA_FAIL && !ota_restart_required_get());
            if(mode == 2U) assert(writes == 0U && erases == 0U);
        }
    }
    ota_reset(); writes = erases = 0; metadata_valid = 0;
}

static void check_tcp_queries(void) {
    uint8_t query[2] = {1U, board_id};
    uint8_t can_query[CAN_OTA_QUERY_LENGTH] = {CAN_OTA_QUERY_KIND_SLOT, 0U, 7U, 0U};
    ota_result_t result;
    ota_rx_state_t saved;
    ota_reset(); metadata_valid = 1; metadata.active_slot = OTA_SLOT_A_BASE_ADDRESS;
    metadata.pending_slot = OTA_SLOT_NONE;
    saved = ota_state;
    result = ota_tcp_on_frame(query, sizeof(query));
    assert(result.reply && result.target_slot == OTA_SLOT_B_BASE_ADDRESS);
    assert(memcmp(&saved, &ota_state, sizeof(saved)) == 0 && writes == 0 && erases == 0);
    metadata.active_slot = OTA_SLOT_B_BASE_ADDRESS;
    result = ota_tcp_on_frame(query, sizeof(query));
    assert(result.reply && result.target_slot == OTA_SLOT_A_BASE_ADDRESS);
    query[1]++; assert(!ota_tcp_on_frame(query, sizeof(query)).reply); query[1]--;
    query[0] = 2; assert(!ota_tcp_on_frame(query, sizeof(query)).reply); query[0] = 1;
    assert(!ota_tcp_on_frame(NULL, 2).reply && !ota_tcp_on_frame(query, 1).reply);
    metadata_valid = 0;
    assert(ota_tcp_on_frame(query, 2).target_slot == 0);
    metadata_valid = 1; metadata.pending_slot = OTA_SLOT_A_BASE_ADDRESS;
    assert(ota_tcp_on_frame(query, 2).target_slot == 0);
    metadata.pending_slot = OTA_SLOT_NONE; ota_state.received_bytes = 6;
    assert(ota_tcp_on_frame(query, 2).target_slot == 0);
    result = ota_can_on_frame(CAN_OTA_QUERY_BASE_ID + board_id, can_query, sizeof(can_query));
    assert(result.target_slot == OTA_SLOT_A_BASE_ADDRESS); /* CAN behavior unchanged. */
    ota_state.done = 1;
    assert(ota_tcp_on_frame(query, 2).target_slot == 0);
    ota_reset(); metadata_valid = 0;
    ota_state.received_bytes = 6;
    result = ota_on_idle(OTA_TRANSFER_TIMEOUT_MS);
    assert(ota_state.received_bytes == 0U); /* Actual elapsed time, independent of query traffic. */
}

int main(void) {
    check_signed_tcp();
    check_tcp_queries();
    unsigned i, completed_writes, completed_erases;
    uint8_t frame[OTA_FRAME_LENGTH] = {0};
    uint8_t query[CAN_OTA_QUERY_LENGTH] = {CAN_OTA_QUERY_KIND_SLOT, 0U, 7U, 0U};
    uint16_t last_sequence;
    ota_result_t result;
    ota_reset(); assert(ota_restart_required_get() == 0U); check_pages(0);
    result = receive_image(0);
    assert(result.finished == 1U && result.success == 1U && ota_restart_required_get() == 1U);
    last_sequence = result.contiguous;
    result = ota_can_on_frame(CAN_OTA_QUERY_BASE_ID + board_id, query, sizeof(query));
    assert(result.reply == 1U && result.query_status == CAN_OTA_SLOT_QUERY_PENDING && result.request_id == 7U);
    check_pages(ota_restart_required_get());
    completed_writes = writes; completed_erases = erases;
    for(i = 0; i < (OTA_TRANSFER_TIMEOUT_MS * 4U) / OTA_IDLE_REPLY_MS; i++) {
        result = ota_on_idle(OTA_IDLE_REPLY_MS);
        assert(result.reply == 0U && result.finished == 0U);
    }
    assert(ota_restart_required_get() == 1U); /* Verified image survives the user wait. */
    (void)ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    assert(writes == completed_writes && erases == completed_erases);
    assert(ota_restart_required_get() == 1U);
    frame[0] = (uint8_t)(last_sequence >> 8); frame[1] = (uint8_t)last_sequence;
    result = ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    assert(result.finished == 1U && result.success == 1U && result.contiguous == last_sequence);
    assert(writes == completed_writes && erases == completed_erases);
    result = ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id + 1U, frame, sizeof(frame));
    assert(result.finished == 0U);
    result = ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame) - 1U);
    assert(result.finished == 0U);
    memset(frame, 0, sizeof(frame));
    ota_reset(); assert(ota_restart_required_get() == 0U); check_pages(0);
    result = receive_image(1);
    assert(result.finished == 0U && ota_restart_required_get() == 0U);
    corrupt_read = 0;
    (void)ota_can_on_frame(CAN_OTA_DATA_BASE_ID + board_id, frame, sizeof(frame));
    for(i = 0; i <= OTA_TRANSFER_TIMEOUT_MS / OTA_IDLE_REPLY_MS; i++) (void)ota_on_idle(OTA_IDLE_REPLY_MS);
    assert(ota_state.received_bytes == 0U && ota_restart_required_get() == 0U);
    result = receive_image(2);
    assert(result.success == 0U && ota_restart_required_get() == 0U);
    puts("OTA manual-restart and all-page restart hint regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="edgelink-ota-restart-") as temp:
    directory = Path(temp)
    (directory / "gd32f10x.h").write_text(
        "#include <stdint.h>\ntypedef struct { uint32_t VTOR; } test_scb_t;\n"
        "extern test_scb_t test_scb;\n#define SCB (&test_scb)\n")
    test_key = ec.derive_private_key(7, ec.SECP256R1())
    app = struct.pack('<II', 0x2000C000, SLOTS['b'] + 257)
    signed_image = build_header(app, 2, SLOTS['b'], test_key) + app
    (directory / "test.c").write_text(SOURCE.replace('SIGNED_IMAGE', ','.join(str(b) for b in signed_image)))
    numbers = test_key.public_key().public_numbers()
    raw = numbers.x.to_bytes(32, 'big') + numbers.y.to_bytes(32, 'big')
    (directory / "test_key.c").write_text('#include <stdint.h>\nconst uint8_t ota_signature_public_key[64] = {' + ','.join(str(b) for b in raw) + '};\n')
    compiler = os.environ.get("HOST_CC", "gcc")
    if not shutil.which(compiler):
        raise SystemExit("Host C compiler missing; set HOST_CC")
    binary = directory / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-int-to-pointer-cast", "-Wno-unknown-pragmas",
                    "-DuECC_SUPPORTS_secp160r1=0", "-DuECC_SUPPORTS_secp192r1=0",
                    "-DuECC_SUPPORTS_secp224r1=0", "-DuECC_SUPPORTS_secp256k1=0",
                    "-DuECC_SUPPORT_COMPRESSED_POINT=0",
                    "-I" + str(directory), "-I" + str(ROOT / "User/App/Service/Ota"),
                    "-I" + str(ROOT / "User/App"), "-I" + str(ROOT / "Common"),
                    "-I" + str(ROOT / "Common/Crypto/micro-ecc"),
                    "-I" + str(ROOT / "Common/Crypto/sha256"),
                    "-I" + str(ROOT / "Drivers/BSP"),
                    "-I" + str(ROOT / "Drivers/BOARD"),
                    str(directory / "test.c"), str(directory / "test_key.c"),
                    str(ROOT / "Common/OTA/ota_signature.c"),
                    str(ROOT / "Common/Crypto/micro-ecc/uECC.c"),
                    str(ROOT / "Common/Crypto/sha256/sha256.c"), str(ROOT / "Common/CRC/crc32.c"),
                    str(ROOT / "User/App/Presentation/Hmi/Page/hmi_page.c"),
                    str(ROOT / "Common/Hmi/hmi_widget.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
