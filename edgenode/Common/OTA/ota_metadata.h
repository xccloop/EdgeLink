#ifndef OTA_METADATA_H_
#define OTA_METADATA_H_

#include <stdint.h>
#include "flash_layout.h"

/*
    OTA 元数据 = Bootloader 的"记事本"。
    它只回答一个问题："这次上电该启动哪个槽？"

    为什么不能存在 RAM 里：一断电就没了，Bootloader 每次上电都
    从零开始，试启动失败后永远回退不了。

    为什么要存两份：Flash 擦除是按页进行的，擦到一半掉电会让整页
    变成损坏内容。只存一份的话，这一份被写坏就永久失去了活动槽信息。
    存两份并轮流写，才能保证任何时刻至少有一份是完整的。
*/

/* 标识"这确实是一条元数据记录"，用来和空白 Flash（全 0xFF）区分开。 */
#define OTA_METADATA_MAGIC          0x314D544FUL    /* "OTM1" */

/*
    一份记录独占一个 Flash 页 —— 这是"轮流擦"能成立的前提。
    如果两份挤在同一页里，擦其中一份必然连带擦掉另一份。

    2KB 是 GD32F103RCT6（256KB 版）的页大小。
    *** 待核实 *** 上硬件时用 fmc_page_erase() 实测页粒度，
    若实际不是 2KB，4KB 元数据区装不下两份记录，分区需要重新划分。
*/
#define OTA_METADATA_RECORD_SIZE    (2UL * 1024UL)

/*
    两份记录都住在元数据区里，跟 A/B 槽没有任何对应关系。
    故意用 0/1 而不是 A/B 命名，避免被误读成"这份记录属于那个槽"。
*/
#define OTA_METADATA_RECORD_0_ADDRESS   (OTA_METADATA_BASE_ADDRESS)
#define OTA_METADATA_RECORD_1_ADDRESS   \
    (OTA_METADATA_BASE_ADDRESS + OTA_METADATA_RECORD_SIZE)

/* pending_slot 用 0 表示"没有待试启动"。两个槽的真实地址都不可能是 0。 */
#define OTA_SLOT_NONE   0UL

/*
    返回码分两套，取值不重叠，免得把"用了默认值"误读成"失败了"。
    load() 的 0 是参数非法，1 是"两份都无效但已兜底"，都拿到可用的结构体。
*/
#define OTA_METADATA_LOAD_FAIL      0U  /* 参数非法，record 未被写入 */
#define OTA_METADATA_LOAD_DEFAULT   1U  /* 两份记录都无效，已填入安全默认值 */
#define OTA_METADATA_LOAD_REAL      2U  /* 读到真实记录 */

#define OTA_METADATA_OK     1U
#define OTA_METADATA_FAIL   0U

/*
    记录固定 256 字节，与 ota_image_header_t 同一套路：
    前 252 字节参与 CRC，最后 4 字节存 CRC 自己。
*/
#define OTA_METADATA_DATA_SIZE      252U
#define OTA_METADATA_FIXED_SIZE     24U
#define OTA_METADATA_RESERVED_SIZE  \
    (OTA_METADATA_DATA_SIZE - OTA_METADATA_FIXED_SIZE)

#define OTA_METADATA_CRC_LENGTH     OTA_METADATA_DATA_SIZE

typedef struct
{
    uint32_t magic;             /* 固定标识，判断这条记录是不是元数据 */
    uint32_t sequence;          /* 序号，越大越新；由 save() 维护，调用者不要填 */
    uint32_t active_slot;       /* 活动槽基地址 */
    uint32_t pending_slot;      /* 待试启动槽基地址；OTA_SLOT_NONE 表示没有 */
    uint32_t boot_attempts;     /* 已试启动次数，用于判断是否该回退 */
    uint32_t pending_version;   /* 待试启动固件的版本号，仅用于记录和上报 */

    uint8_t reserved[OTA_METADATA_RESERVED_SIZE];

    uint32_t record_crc32;      /* 本记录自身内容的 CRC32，由 save() 维护 */
} ota_metadata_t;

_Static_assert(sizeof(ota_metadata_t) == 256U,
               "OTA metadata record size must be 256 bytes");

/*
    读出当前有效记录。
    两份都无效（全新设备、或元数据被写坏）时填入安全默认值：
    活动槽 = A、无待试启动、尝试次数 = 0，并返回 OTA_METADATA_LOAD_DEFAULT。
    两种情况下调用者拿到的都是可用的记录，区别只在于要不要上报"我兜底了"。
*/
uint8_t ota_metadata_load(ota_metadata_t *record);

/*
    写入一条新记录。
    sequence 自动取"当前有效序号 + 1"，写到"不是当前有效记录"的那一页。
    返回 OTA_METADATA_OK / OTA_METADATA_FAIL。
*/
uint8_t ota_metadata_save(const ota_metadata_t *record);

#endif
