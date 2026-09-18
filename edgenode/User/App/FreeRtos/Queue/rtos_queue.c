#include "rtos_queue.h"

static StaticQueue_t collect_to_storage_queue_control;
static uint8_t collect_to_storage_queue_storage[
    RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH * sizeof(telemetry_sample_struct)];
static QueueHandle_t collect_to_storage_queue;

static StaticQueue_t storage_to_transmit_queue_control;
static uint8_t storage_to_transmit_queue_storage[
    RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH * sizeof(transmit_work_item_t)];
static QueueHandle_t storage_to_transmit_queue;

static StaticQueue_t confirm_to_storage_queue_control;
static uint8_t confirm_to_storage_queue_storage[
    RTOS_CONFIRM_TO_STORAGE_QUEUE_LENGTH * sizeof(storage_confirm_event_t)];
static QueueHandle_t confirm_to_storage_queue;

static uint8_t rtos_queue_ready;

uint8_t rtos_queue_init(void)
{
    if(rtos_queue_ready != 0U)
    {
        return RTOS_QUEUE_SUCCESS;
    }

    collect_to_storage_queue = xQueueCreateStatic(
        RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH,
        sizeof(telemetry_sample_struct),
        collect_to_storage_queue_storage,
        &collect_to_storage_queue_control);

    storage_to_transmit_queue = xQueueCreateStatic(
        RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH,
        sizeof(transmit_work_item_t),
        storage_to_transmit_queue_storage,
        &storage_to_transmit_queue_control);

    confirm_to_storage_queue = xQueueCreateStatic(
        RTOS_CONFIRM_TO_STORAGE_QUEUE_LENGTH,
        sizeof(storage_confirm_event_t),
        confirm_to_storage_queue_storage,
        &confirm_to_storage_queue_control);

    if((collect_to_storage_queue == NULL) ||
       (storage_to_transmit_queue == NULL) ||
       (confirm_to_storage_queue == NULL))
    {
        collect_to_storage_queue = NULL;
        storage_to_transmit_queue = NULL;
        confirm_to_storage_queue = NULL;
        return RTOS_QUEUE_FAIL;
    }

    rtos_queue_ready = 1U;
    return RTOS_QUEUE_SUCCESS;
}

QueueHandle_t rtos_collect_to_storage_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return collect_to_storage_queue;
}

QueueHandle_t rtos_storage_to_transmit_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return storage_to_transmit_queue;
}

QueueHandle_t rtos_confirm_to_storage_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return confirm_to_storage_queue;
}

