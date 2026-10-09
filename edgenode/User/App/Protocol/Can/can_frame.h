#ifndef CAN_FRAME_H_
#define CAN_FRAME_H_

#include <stdint.h>
#include "Model/message.h"

#define CAN_FRAME_SUCCESS  1U
#define CAN_FRAME_FAIL     0U

#define CAN_TELEMETRY_BASE_ID  0x280U
#define CAN_TELEMETRY_LENGTH   7U
#define CAN_ACK_BASE_ID        0x300U
#define CAN_ACK_LENGTH         5U

/*
    CAN 总线 ID 分配表（标准帧 11 位）。新增段请加在末尾，避免撞车。

    每段占 0x80：节点号占低 7 位（0~127），与 can_output.c 里
    "CAN_TELEMETRY_BASE_ID + node_id" 的拼法一致。所以后一段的基址
    必须比前一段高出 0x80，否则会与上一段的某个节点号撞车。

        0x280 + node    遥测
        0x300 + node    ACK
        0x380 + node    固件数据帧（Hub -> Node，2B 序号 + 6B 载荷）
        0x400 + node    槽位查询帧（Hub -> Node，查询可升级槽位）
        0x480 + node    固件回复帧（Node -> Hub，进度 / 结果）
        0x500 ~ 0x7FF   未分配
*/
#define CAN_OTA_DATA_BASE_ID   0x380U
#define CAN_OTA_QUERY_BASE_ID   0x400U
#define CAN_OTA_REPLY_BASE_ID  0x480U

/* 每段可用的 ID 数量（节点号 7 位）。 */
#define CAN_ID_SEGMENT_SIZE    128U

/* 回复帧 0x480 + node 的载荷长度。 */
#define CAN_OTA_REPLY_LENGTH   4U

#define CAN_OTA_REPLY_KIND_PROGRESS  1U   /* 进度：我连续收到第 N 号 */
#define CAN_OTA_REPLY_KIND_RESULT    2U   /* 收完结果：成没成 */
#define CAN_OTA_REPLY_KIND_SLOT      3U   /* 查询另一个槽位的结果 */

/* 槽位查询：命令、请求编号高/低字节、保留字节 0。 */
#define CAN_OTA_QUERY_KIND_SLOT  1U
#define CAN_OTA_QUERY_LENGTH     4U
/* 查询回复：类型、请求编号高/低字节、状态、4 字节目标地址（高位在前）。 */
#define CAN_OTA_REPLY_SLOT_LENGTH     8U
#define CAN_OTA_SLOT_QUERY_OK         0U
#define CAN_OTA_SLOT_QUERY_INVALID    1U
#define CAN_OTA_SLOT_QUERY_PENDING    2U

/*
    一条"该回复什么"的请求。

    放在协议层而不是队列层，理由和 can_receive_frame_t 当初的取舍一样：
    它的字段对应 0x480 的回复载荷，是协议形状，不是排队形状。
    放队列层的话，can_output.c 要为了拿到这个类型去 include rtos_queue.h，
    等于把 FreeRTOS 拖进 CAN 输出层。
*/
typedef struct
{
    uint8_t  kind;        /* CAN_OTA_REPLY_KIND_xxx */
    uint16_t contiguous;  /* kind == PROGRESS 时有效：我连续收到第 N 号 */
    uint8_t  success;     /* kind == RESULT 时有效 */
    uint16_t request_id;  /* kind == SLOT 时有效 */
    uint8_t  query_status;
    uint32_t target_slot; /* 查询成功时有效，否则为 0 */
} ota_reply_request_t;

/*
    这里只定义CAN遥测协议，不访问CAN硬件。
    Message提供统一业务数据；编码结果交给Output/Can中的发送出口写入CAN BSP。
*/
uint8_t can_frame_encode(uint8_t data[CAN_TELEMETRY_LENGTH],
                         const telemetry_sample_struct *message);

/*
    仅解析标准 CAN ACK 帧。CAN 控制器已完成帧级 CRC 校验；此处继续校验
    仲裁 ID、DLC 和目标节点，避免错误节点的 ACK 确认本节点记录。
*/
uint8_t can_ack_decode(uint16_t standard_id,
                       const uint8_t data[CAN_ACK_LENGTH],
                       uint8_t data_length,
                       uint8_t expected_node_id,
                       uint32_t *sequence,
                       uint8_t *ack_status);

#endif
