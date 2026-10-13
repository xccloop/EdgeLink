"""Host tests for the real Log/LogTask paths with NOR Flash and queue simulation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "log.c"
#include "log_task.c"

static uint8_t flash[GD25Q32_CAPACITY_BYTES];
static unsigned reads;
static unsigned page_reads[LOG_PAGE_COUNT];
static uint32_t fail_read = UINT32_MAX;
uint8_t gd25_init(void) { return 1; }
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length) {
    assert(address + length <= sizeof(flash));
    reads++;
    if(length == 256) page_reads[address / 256]++;
    if(address == fail_read) return 0;
    memcpy(data, flash + address, length); return 1;
}
uint8_t gd25_write(uint32_t address, const uint8_t *data, uint16_t length) {
    unsigned i;
    assert(address + length <= sizeof(flash));
    for(i = 0; i < length; i++) flash[address + i] &= data[i];
    return 1;
}
uint8_t gd25_clear(uint32_t address) {
    memset(flash + address, 0xFF, GD25Q32_SECTOR_SIZE); return 1;
}
static void record(uint32_t address, uint32_t sequence, uint8_t confirmed) {
    telemetry_sample_struct message = {123, sequence, -125, -1};
    uint8_t raw[LOG_SLOT_SIZE];
    log_record_message_encode(raw, &message);
    if(confirmed) raw[15] = LOG_CONFIRMED;
    memcpy(flash + address, raw, sizeof(raw));
}
static void setup(void) {
    log_init_result_t result;
    memset(flash, 0xFF, sizeof(flash));
    record(16, 2, 0);
    record(32, 3, 1);
    record(LOG_END_ADDRESS - 16, 4, 0);
    assert(log_init(&result) == LOG_SUCCESS);
    assert(log_pending_count_get() == 2);
    reads = 0;
    memset(page_reads, 0, sizeof(page_reads));
    fail_read = UINT32_MAX;
}

/* Queue mocks run the real task helpers; only UART/CAN delivery is simulated. */
struct mock_queue {
    unsigned size, head, tail;
    uint8_t items[32][32];
};
static struct mock_queue permissions, collected, confirmations, transmissions, sequences;
TaskHandle_t hmi_task_handle;
volatile uint8_t link_restored;
static unsigned delays, sent_count, failed_attempts;
static uint32_t sent[32], failed_sequence = UINT32_MAX;
static int require_complete_scan, inject_other_ack, collect_on_delay;
static int run_task_scenario;
static jmp_buf task_finished;
static void push(QueueHandle_t q, const void *item) {
    assert(q->tail - q->head < 32);
    memcpy(q->items[q->tail++ % 32], item, q->size);
}
int xQueueReceive(QueueHandle_t q, void *item, unsigned wait) {
    if(run_task_scenario == 2 && q == &collected && wait == 100) {
        assert(sent_count == 1 && sent[0] == 6);
        assert(log_pending_count_get() == 0 && permissions.tail - permissions.head == 1);
        assert(delays >= LOG_PAGE_COUNT);
        run_task_scenario = 0; longjmp(task_finished, 1);
    }
    if(q->head == q->tail) return 0;
    memcpy(item, q->items[q->head++ % 32], q->size); return pdPASS;
}
int xQueueSend(QueueHandle_t q, const void *item, unsigned wait) {
    (void)wait;
    if(q == &transmissions) {
        const transmit_work_item_t *work = item;
        log_confirm_event_t ack = {work->message.sequence, work->flash_address, 1};
        assert(!require_complete_scan || log_pending_scan_page == LOG_PAGE_COUNT);
        assert(sent_count < 32);
        sent[sent_count++] = work->message.sequence;
        if(work->message.sequence == failed_sequence) {
            ack.success = 0; failed_attempts++;
        }
        if(inject_other_ack) {
            log_confirm_event_t other = {3, 32, 1};
            push(&confirmations, &other); inject_other_ack = 0;
        }
        push(&confirmations, &ack);
    } else push(q, item);
    return pdPASS;
}
void vTaskDelay(unsigned ticks) {
    assert(ticks == 1); /* task_block_forever would fail here */
    delays++;
    if(collect_on_delay == 1) {
        telemetry_sample_struct sample = {456, 6, 200, -1};
        push(&collected, &sample); collect_on_delay = 0;
    } else if(collect_on_delay == 2) {
        uint8_t permission = 1;
        push(&permissions, &permission); collect_on_delay = 0;
    }
}
unsigned ulTaskNotifyTake(int clear, unsigned wait) { (void)clear; (void)wait; return 1; }
void xTaskNotifyGive(TaskHandle_t handle) {
    (void)handle;
    if(run_task_scenario == 1) {
        uint8_t permission;
        telemetry_sample_struct sample = {456, 6, 200, -1};
        assert(xQueueReceive(&permissions, &permission, 0) == pdPASS);
        push(&collected, &sample); /* permit already consumed at reconnect */
        link_restored = 1;
        sent_count = delays = 0; require_complete_scan = 1;
        run_task_scenario = 2;
    }
}
TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name, unsigned depth,
                              void *arg, unsigned priority, StackType_t *stack, StaticTask_t *tcb) {
    (void)entry; (void)name; (void)depth; (void)arg; (void)priority; (void)stack; return tcb;
}
QueueHandle_t rtos_log_to_collect_sequence_queue_get(void) { return &sequences; }
QueueHandle_t rtos_log_to_collect_permission_queue_get(void) { return &permissions; }
QueueHandle_t rtos_collect_to_log_queue_get(void) { return &collected; }
QueueHandle_t rtos_log_to_transmit_queue_get(void) { return &transmissions; }
QueueHandle_t rtos_transmit_to_log_confirm_queue_get(void) { return &confirmations; }
static void reset_queues(void) {
    memset(&permissions, 0, sizeof(permissions)); permissions.size = 1;
    memset(&collected, 0, sizeof(collected)); collected.size = sizeof(telemetry_sample_struct);
    memset(&confirmations, 0, sizeof(confirmations)); confirmations.size = sizeof(log_confirm_event_t);
    memset(&transmissions, 0, sizeof(transmissions)); transmissions.size = sizeof(transmit_work_item_t);
    memset(&sequences, 0, sizeof(sequences)); sequences.size = 4;
    delays = sent_count = failed_attempts = 0;
    failed_sequence = UINT32_MAX;
    require_complete_scan = inject_other_ack = collect_on_delay = 0;
}
int main(void) {
    unsigned pages = 0;
    uint8_t status;
    setup();
    assert(log_pending_scan_begin() == LOG_SUCCESS);
    do {
        unsigned before = reads;
        status = log_pending_scan_step();
        assert(reads == before + 1); /* bounded to one page per call */
        pages++;
    } while(status == LOG_MORE);
    assert(status == LOG_SUCCESS);
    assert(pages == (LOG_END_ADDRESS - LOG_BASE_ADDRESS) / 256);

    /* Full scan before first send; sparse records on first/last pages, confirmed hole. */
    setup(); reset_queues(); require_complete_scan = 1;
    log_pending_index_build(&confirmations);
    assert(sent_count == 0 && delays == LOG_PAGE_COUNT);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 2 && sent[0] == 2 && sent[1] == 4);
    assert(log_pending_count_get() == 0);
    assert(page_reads[0] == 2 && page_reads[LOG_PAGE_COUNT - 1] == 2);
    assert(page_reads[1] == 1); /* unmarked page never reread for replay */
    assert(delays > LOG_PAGE_COUNT); /* replay yields between marked pages */

    /* Startup reuses log_init's complete index; physical order can cross sequence wrap. */
    setup(); reset_queues();
    record(16, 100, 0); /* newer sequence at low physical address */
    { log_init_result_t result; assert(log_init(&result) == LOG_SUCCESS); }
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 2 && sent[0] == 100 && sent[1] == 4);

    /* Multiple records in one cached page; unrelated ACK updates that same cache. */
    setup(); reset_queues();
    record(32, 3, 0); record(48, 6, 0); flash[63] = 0x7F;
    { log_init_result_t result; assert(log_init(&result) == LOG_SUCCESS); }
    memset(page_reads, 0, sizeof(page_reads));
    inject_other_ack = 1;
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 3 && sent[0] == 2 && sent[1] == 6 && sent[2] == 4);

    assert(log_pending_count_get() == 0 && page_reads[0] == 1);

    /* Three failures stop this replay; partial page bit and Flash pending survive. */
    setup(); reset_queues(); record(48, 6, 0);
    { log_init_result_t result; assert(log_init(&result) == LOG_SUCCESS); }
    failed_sequence = 6;
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 4 && failed_attempts == 3 && log_pending_count_get() == 2);
    assert((log_pending_page_bitmap[0] & 1U) != 0 && flash[63] != LOG_CONFIRMED);
    reset_queues(); log_pending_index_build(&confirmations);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 2 && sent[0] == 6 && sent[1] == 4);
    assert(log_pending_count_get() == 0);

    /* Partial scan cannot be replayed, and retry rebuilds from the beginning. */
    setup(); reset_queues();
    assert(log_pending_scan_begin() == LOG_SUCCESS);
    assert(log_pending_scan_step() == LOG_MORE);
    fail_read = 256;
    assert(log_pending_scan_step() == LOG_FAIL);
    { telemetry_sample_struct sample; uint32_t address;
      assert(log_pending_replay_next(&sample, &address) == LOG_FAIL); }
    fail_read = UINT32_MAX; log_pending_index_build(&confirmations);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 2);

    /* Rebuild discards old bits and ignores corrupt CRC records. */
    setup(); reset_queues(); flash[16 + 11] ^= 1;
    { log_init_result_t result; assert(log_init(&result) == LOG_SUCCESS); }
    log_pending_index_build(&confirmations);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 1 && sent[0] == 4);
    reset_queues(); log_pending_index_build(&confirmations);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 0);

    /* Pause can revoke an unused permit, wait for a failed sample to return it,
       or persist an in-flight successful sample before building the snapshot. */
    setup(); reset_queues();
    record(48, 5, 1); /* next slot is writable, as required before issuing a permit */
    { log_init_result_t result; assert(log_init(&result) == LOG_SUCCESS); }
    { uint8_t outstanding = 1, permission = 1;
      push(&permissions, &permission);
      log_collect_pause(&permissions, &collected, &confirmations, &outstanding);
      assert(outstanding == 0 && delays == 0 && permissions.head == permissions.tail);
      outstanding = 1; collect_on_delay = 2;
      log_collect_pause(&permissions, &collected, &confirmations, &outstanding);
      assert(outstanding == 0 && delays == 1);
      outstanding = 1; collect_on_delay = 1;
      log_collect_pause(&permissions, &collected, &confirmations, &outstanding);
      assert(outstanding == 0 && log_pending_count_get() == 3 && sent_count == 0); }
    require_complete_scan = 1; log_pending_index_build(&confirmations);
    log_replay_pending(&transmissions, &confirmations);
    assert(sent_count == 3 && sent[0] == 2 && sent[1] == 6 && sent[2] == 4);

    /* Exercise the actual infinite task's startup -> reconnect -> collect-resume path. */
    setup(); reset_queues(); record(48, 5, 1);
    link_restored = 0; run_task_scenario = 1;
    if(setjmp(task_finished) == 0) log_task(NULL);
    assert(run_task_scenario == 0);

    puts("Log replay regression tests passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="edgelink-log-test-") as temp:
    directory = Path(temp)
    (directory / "FreeRTOS.h").write_text(r'''
#pragma once
#include <stdint.h>
#include <assert.h>
typedef uint32_t StackType_t;
typedef int StaticTask_t;
typedef void *TaskHandle_t;
typedef struct mock_queue *QueueHandle_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(x) assert(x)
''')
    (directory / "task.h").write_text(r'''
#pragma once
#include "FreeRTOS.h"
void vTaskDelay(unsigned);
unsigned ulTaskNotifyTake(int, unsigned);
void xTaskNotifyGive(TaskHandle_t);
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, unsigned, void *,
                              unsigned, StackType_t *, StaticTask_t *);
''')
    (directory / "FreeRtos/Queue").mkdir(parents=True)
    (directory / "FreeRtos/Queue/rtos_queue.h").write_text(r'''
#pragma once
#include "FreeRTOS.h"
#include "Model/message.h"
typedef struct { telemetry_sample_struct message; uint32_t flash_address; } transmit_work_item_t;
typedef struct { uint32_t sequence, flash_address; uint8_t success; } log_confirm_event_t;
int xQueueReceive(QueueHandle_t, void *, unsigned);
int xQueueSend(QueueHandle_t, const void *, unsigned);
QueueHandle_t rtos_log_to_collect_sequence_queue_get(void);
QueueHandle_t rtos_log_to_collect_permission_queue_get(void);
QueueHandle_t rtos_collect_to_log_queue_get(void);
QueueHandle_t rtos_log_to_transmit_queue_get(void);
QueueHandle_t rtos_transmit_to_log_confirm_queue_get(void);
''')
    (directory / "test.c").write_text(SOURCE, encoding="utf-8")
    compiler = os.environ.get("HOST_CC", "gcc")
    if not shutil.which(compiler):
        raise SystemExit("Host C compiler missing; set HOST_CC")
    binary = directory / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(directory),
                    "-I" + str(ROOT / "User/App/FreeRtos/Tasks/Log"),
                    "-I" + str(ROOT / "User/App/Output/Log"),
                    "-I" + str(ROOT / "User/App"),
                    "-I" + str(ROOT / "Drivers/BSP"),
                    "-I" + str(ROOT / "Common"),
                    "-I" + str(ROOT / "Common/CRC"),
                    str(directory / "test.c"), str(ROOT / "Common/CRC/crc32.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
