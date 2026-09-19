#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include "Model/message.h"

/* Flash 中每条遥测记录固定占 16 字节。 */
#define STORAGE_SLOT_SIZE 16U

/* Storage 函数统一返回值。 */
#define STORAGE_SUCCESS 1U
#define STORAGE_FAIL    0U

/*
    Storage 初始化时一次扫描得到的结果。
    pending 记录不一定连续，因此只保留 sequence 最小的一条，恢复时严格一条一条发送。
*/
typedef struct
{
    /* 下一次由 Message 分配给新采样的 sequence；不是当前 pending 的 sequence。 */
    uint32_t next_sequence;

    /* 顺序追加时下一条新记录应写入的槽位首地址；写满时等于 STORAGE_LOG_END_ADDRESS。 */
    uint32_t next_write_address;

    /* 1 表示既有空槽位又未发生 sequence 耗尽；0 时 Storage 拒绝新的 pending 写入。 */
    uint8_t can_append;

    /* 1 表示本次扫描至少找到一条 CRC 正确且尚未确认的历史记录。 */
    uint8_t has_pending;

    /* 最小 pending sequence 对应的业务 Message，已经从 Flash 固定槽位解码。 */
    telemetry_sample_struct oldest_pending_message;

    /* oldest_pending_message 在 Flash 中的 16 字节槽位首地址，ACK 确认时必须带回它。 */
    uint32_t oldest_pending_address;
} storage_init_result_t;

/* 初始化 GD25Q32 并一次扫描恢复写指针、下一 sequence 和最小 pending 记录。 */
uint8_t storage_init(storage_init_result_t *result);

uint8_t storage_write_pending(const telemetry_sample_struct *message,uint32_t *flash_address);

uint8_t storage_confirm(uint32_t flash_address,uint32_t ack_sequence);

#endif
