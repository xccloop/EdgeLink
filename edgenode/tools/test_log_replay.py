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
static unsigned reads, page_reads[LOG_PAGE_COUNT], record_writes;
static uint32_t fail_read = UINT32_MAX;
static uint8_t fail_write;
uint8_t gd25_init(void) { return 1; }
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length) {
    assert(address + length <= sizeof(flash)); reads++;
    if(length == 256) page_reads[address / 256]++;
    if(address == fail_read) return 0;
    memcpy(data, flash + address, length); return 1;
}
uint8_t gd25_write(uint32_t address, const uint8_t *data, uint16_t length) {
    unsigned i;
    if(fail_write) return 0;
    assert(address + length <= sizeof(flash));
    if(length == LOG_SLOT_SIZE) record_writes++;
    for(i = 0; i < length; i++) flash[address + i] &= data[i];
    return 1;
}
uint8_t gd25_clear(uint32_t address) {
    assert(address % GD25Q32_SECTOR_SIZE == 0);
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
    memset(flash, 0xFF, sizeof(flash)); fail_read = UINT32_MAX;
    record(16, 2, 0); record(32, 3, 1); record(48, 5, 1);
    record(LOG_END_ADDRESS - 16, 4, 0);
    assert(log_init(&result) == LOG_SUCCESS);
    assert(log_pending_count_get() == 2);
    reads = record_writes = 0; memset(page_reads, 0, sizeof(page_reads));
}
static void scan_all(void) {
    uint8_t result;
    assert(log_pending_scan_begin() == LOG_SUCCESS);
    do { unsigned before = reads;
         result = log_pending_scan_step(); assert(reads <= before + 1);
    } while(result == LOG_MORE);
    assert(result == LOG_SUCCESS);
}
static uint8_t next(telemetry_sample_struct *sample, uint32_t *address) {
    uint8_t result;
    do { result = log_pending_replay_next(sample, address); } while(result == LOG_MORE);
    return result;
}

/* Simulated scheduler: real LogTask, bounded queues, one serial TransmitTask,
   periodic CollectTask and deliberately slow historical ACKs. */
struct mock_queue { unsigned size, head, tail, capacity; uint8_t items[16][32]; };
static struct mock_queue permissions, collected, confirmations, transmissions, sequences;
TaskHandle_t hmi_task_handle;
volatile uint8_t link_restored;
static jmp_buf task_finished;
static unsigned now, iterations, sample_count, sample_during_scan, sample_during_ack;
static unsigned normal_during_scan, queue_rejections, history_attempts[256], normal_sends[256];
static unsigned initial_pages, scan_steps, history_sends, normal_after_fail;
static unsigned ack_due;
static transmit_work_item_t transmitting;
static int busy, running, scenario;
static uint32_t next_sequence;
static void push(QueueHandle_t q, const void *item) {
    assert(q->tail - q->head < q->capacity && q->size <= 32);
    memcpy(q->items[q->tail++ % 16], item, q->size);
}
static int pop(QueueHandle_t q, void *item) {
    if(q->head == q->tail) return 0;
    memcpy(item, q->items[q->head++ % 16], q->size); return pdPASS;
}
int xQueueReceive(QueueHandle_t q, void *item, unsigned wait) {
    assert(wait == 0); /* LogTask must never wait indefinitely on an ACK or sample. */
    return pop(q, item);
}
int xQueueSend(QueueHandle_t q, const void *item, unsigned wait) {
    if(q != &sequences) assert(wait == 0);
    if(q == &transmissions && scenario == 3 && now < 50) {
        queue_rejections++; return 0;
    }
    if(q->tail - q->head == q->capacity) {
        if(q == &transmissions) queue_rejections++;
        return 0;
    }
    if(q == &transmissions) {
        const transmit_work_item_t *work = item;
        assert(work->message.sequence < 256);
        if(work->message.sequence >= 6 && !normal_sends[work->message.sequence]) {
            normal_sends[work->message.sequence]++;
            if(log_pending_scan_active) normal_during_scan++;
            if(history_attempts[2] >= 3) normal_after_fail++;
        } else {
            /* First pass uses boot index; runtime passes must finish building first. */
            assert(log_pending_scan_active == 0 && log_pending_index_ready != 0);
            history_attempts[work->message.sequence]++; history_sends++;
        }
    }
    push(q, item); return pdPASS;
}
static void finish_task(void) {
    if(collected.head != collected.tail) return; /* let LogTask persist the just-produced sample */
    assert(sample_count >= 6 && record_writes == sample_count);
    if(scenario != 4) assert(sample_during_ack > 0); /* full Flash intentionally withholds permits */
    if(scenario == 2) {
        assert(history_attempts[2] == 3 && normal_after_fail > 0);
        assert(flash[31] != LOG_CONFIRMED);
    } else if(scenario != 4) {
        assert(flash[31] == LOG_CONFIRMED);
        assert(flash[LOG_END_ADDRESS - 1] == LOG_CONFIRMED);
    }
    if(scenario == 4) assert(history_attempts[2] == 1 && sample_count >= 6);
    if(scenario == 1) {
        assert(sample_during_scan > 0 && normal_during_scan > 0);
        assert(history_attempts[6] > 0); /* newer failed sample retried in a later pass */
        assert(scan_steps >= LOG_PAGE_COUNT - 1); /* last step clears scan_active before yielding */
    }
    if(scenario == 3) assert(queue_rejections > 0 && normal_sends[6] == 1);
    running = 0; longjmp(task_finished, 1);
}
void vTaskDelay(unsigned ticks) {
    uint8_t permission;
    if(ticks == 1000 && scenario >= 5) {
        assert(log_task_storage_failed_get() == 1U);
        assert(running == 1); /* HMI was released even on initialization failure. */
        if(scenario == 5) assert(sequences.head == sequences.tail && sample_count == 0);
        if(scenario == 6) assert(sample_count > 0);
        running = 0;
        longjmp(task_finished, 1);
    }
    assert(ticks == 1 || ticks == 10); /* fatal stop uses 1000, caught by the mock */
    assert(++iterations < 60000);
    now += ticks;
    if(!running) return;

    if(log_pending_scan_active) scan_steps++;
    if(busy && now >= ack_due) {
        log_confirm_event_t ack = {transmitting.message.sequence, transmitting.flash_address, 1};
        if(scenario == 2 && ack.sequence == 2) ack.success = 0;
        if(scenario == 1 && ack.sequence == 6 && history_attempts[6] == 0) ack.success = 0;
        push(&confirmations, &ack); busy = 0;
    }
    if(!busy && pop(&transmissions, &transmitting) == pdPASS) {
        busy = 1;
        ack_due = now + ((transmitting.message.sequence <= 5) ? 2000 : 2);
    }
    if(now >= sample_count * 1000 && pop(&permissions, &permission) == pdPASS) {
        telemetry_sample_struct sample = {now, next_sequence++, 200, -1};
        push(&collected, &sample); sample_count++;
        if(log_pending_scan_active) sample_during_scan++;
        if(busy && transmitting.message.sequence <= 5) sample_during_ack++;
    }
    if(scenario == 1) {
        if(history_attempts[6] && flash[31] == LOG_CONFIRMED && !busy &&
           transmissions.head == transmissions.tail && sample_count >= 22) finish_task();
    } else if(scenario == 2) {
        if(history_attempts[2] == 3 && normal_after_fail > 0 && sample_count >= 9) finish_task();
    } else if(scenario == 4) {
        if(sample_count >= 7 && !busy && transmissions.head == transmissions.tail) finish_task();
    } else if(flash[31] == LOG_CONFIRMED && flash[LOG_END_ADDRESS - 1] == LOG_CONFIRMED &&
              sample_count >= 7 && !busy && transmissions.head == transmissions.tail) finish_task();
}
unsigned ulTaskNotifyTake(int clear, unsigned wait) { (void)clear; (void)wait; return 1; }
void xTaskNotifyGive(TaskHandle_t handle) {
    (void)handle;
    /* HMI must be released before any historical send, unlike the old blocking path. */
    assert(history_sends == 0);
    next_sequence = log_latest_sequence + 1;
    initial_pages = reads;
    running = 1;
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
static void reset_task(int mode) {
    struct mock_queue *queues[] = {&permissions, &collected, &confirmations, &transmissions, &sequences};
    unsigned i;
    for(i = 0; i < 5; i++) { memset(queues[i], 0, sizeof(*queues[i])); queues[i]->capacity = 10; }
    permissions.size = 1; permissions.capacity = 1;
    collected.size = sizeof(telemetry_sample_struct);
    confirmations.size = sizeof(log_confirm_event_t);
    transmissions.size = sizeof(transmit_work_item_t);
    sequences.size = 4; sequences.capacity = 1;
    now = iterations = sample_count = sample_during_scan = sample_during_ack = 0;
    normal_during_scan = queue_rejections = scan_steps = history_sends = normal_after_fail = 0;
    memset(history_attempts, 0, sizeof(history_attempts));
    memset(normal_sends, 0, sizeof(normal_sends));
    busy = running = 0; scenario = mode; link_restored = 0;
    record_writes = 0;
}

int main(void) {
    telemetry_sample_struct sample = {456, 6, 200, -1}, out;
    uint32_t address, selected;
    uint8_t result;
    unsigned pages = 0;

    /* Exactly one page per scan; append after a page was scanned, fixed history boundary. */
    setup(); assert(log_pending_scan_begin() == LOG_SUCCESS);
    assert(log_pending_scan_step() == LOG_MORE); pages++;
    assert(log_write_pending(&sample, &address) == LOG_SUCCESS);
    do { unsigned before = reads; result = log_pending_scan_step();
         assert(reads == before + 1); pages++;
    } while(result == LOG_MORE);
    assert(result == LOG_SUCCESS && pages == LOG_PAGE_COUNT);
    assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 2);
    assert(log_confirm(selected, out.sequence) == LOG_SUCCESS);
    assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 4);
    assert(log_confirm(selected, out.sequence) == LOG_SUCCESS);
    assert(next(&out, &selected) == LOG_NO_PENDING && log_pending_count_get() == 1);
    scan_all(); assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 6);

    /* Appending in an unread page must not add that page to this history snapshot. */
    setup(); memset(flash + 48, 0xFF, 16); record(240, 5, 1);
    { log_init_result_t init; assert(log_init(&init) == LOG_SUCCESS); }
    assert(log_pending_scan_begin() == LOG_SUCCESS);
    assert(log_pending_scan_step() == LOG_MORE);
    assert(log_write_pending(&sample, &address) == LOG_SUCCESS && address == 256);
    do { result = log_pending_scan_step(); } while(result == LOG_MORE);
    assert((log_pending_page_bitmap[0] & 2U) == 0);
    assert(log_pending_index_ready != 0 && log_pending_count_get() == 3);

    /* Same-page append invalidates cache, not snapshot; other ACK updates cached status. */
    setup(); record(32, 3, 0);
    { log_init_result_t init; assert(log_init(&init) == LOG_SUCCESS); }
    assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 2);
    assert(log_confirm(selected, 2) == LOG_SUCCESS);
    assert(log_write_pending(&sample, &address) == LOG_SUCCESS);
    assert(log_replay_page_loaded == 0 && log_pending_index_ready != 0);
    assert(log_confirm(32, 3) == LOG_SUCCESS);
    assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 4);

    /* New-sector reuse invalidates all cached pages of that sector; stale ACK safe. */
    setup(); record(16, 2, 1); record(256, 3, 0);
    record(LOG_END_ADDRESS - 16, 10, 1);
    { log_init_result_t init; assert(log_init(&init) == LOG_SUCCESS); }
    assert(log_write_pending_ready() == LOG_FULL);
    assert(next(&out, &selected) == LOG_SUCCESS && selected == 256);
    assert(log_confirm(256, 3) == LOG_SUCCESS);
    assert(log_write_pending_ready() == LOG_SUCCESS);
    sample.sequence = 11;
    assert(log_write_pending(&sample, &address) == LOG_SUCCESS && address == 0);
    assert(log_replay_page_loaded == 0);
    assert(log_confirm(256, 3) == LOG_STALE && log_pending_count_get() == 1);
    assert(next(&out, &selected) == LOG_NO_PENDING);
    assert(log_confirm(0, 2) == LOG_STALE && flash[15] != LOG_CONFIRMED);
    fail_read = 0; assert(log_confirm(0, 11) == LOG_FAIL); fail_read = UINT32_MAX;

    /* Empty history must not accidentally include the first newly collected sequence 0. */
    memset(flash, 0xFF, sizeof(flash));
    { log_init_result_t init; assert(log_init(&init) == LOG_SUCCESS); }
    assert(log_pending_scan_begin() == LOG_SUCCESS);
    sample.sequence = 0; assert(log_write_pending(&sample, &address) == LOG_SUCCESS);
    assert(log_pending_scan_step() == LOG_SUCCESS);
    assert(next(&out, &selected) == LOG_NO_PENDING);

    /* Partial scan fails closed; corrupt CRC omitted; physical ordering across wrap. */
    setup(); assert(log_pending_scan_begin() == LOG_SUCCESS);
    assert(log_pending_scan_step() == LOG_MORE); fail_read = 256;
    assert(log_pending_scan_step() == LOG_FAIL);
    assert(log_pending_replay_next(&out, &selected) == LOG_FAIL);
    fail_read = UINT32_MAX;
    record(16, 100, 0); flash[32 + 11] ^= 1;
    { log_init_result_t init; assert(log_init(&init) == LOG_SUCCESS); }
    scan_all(); assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 100);
    assert(log_confirm(selected, 100) == LOG_SUCCESS);
    assert(next(&out, &selected) == LOG_SUCCESS && out.sequence == 4);

    /* Real infinite task: startup, continuing collection while ACK delayed,
       later runtime rebuild on failed new sample, three failures, and queue backpressure. */
    for(scenario = 0; scenario < 7; scenario++) {
        int mode = scenario;
        setup(); reset_task(mode);
        storage_failed = 0U; /* each scenario represents a fresh boot */
        if(mode == 5) fail_read = LOG_BASE_ADDRESS;
        fail_write = (mode == 6);
        if(mode == 3) transmissions.capacity = 1; /* exercise actual full queue as well as temporary refusal */
        if(mode == 4) record(LOG_END_ADDRESS - 16, 10, 1); /* full target sector pauses only until ACK */
        if(setjmp(task_finished) == 0) log_task(NULL);
        assert(running == 0);
        if(mode != 5) assert(initial_pages >= LOG_PAGE_COUNT);
        assert(log_task_storage_failed_get() == ((mode >= 5) ? 1U : 0U));
    }
    puts("Live collection and historical replay regression tests passed");
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
