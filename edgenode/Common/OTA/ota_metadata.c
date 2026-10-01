#include "ota_metadata.h"
#include "crc32.h"
#include "gd32f10x_fmc.h"

/*
    第一层:为什么要有"记事本"
    image_verify 和 boot_jump 只回答"这个槽能不能跳"
    但没人回答"我该跳哪个槽"        ← 缺的就是这一层
            ↓
    这个信息不能放 RAM（断电就没）
            ↓
    必须放 Flash —— 这就是 OTA 元数据区
    记的东西:活动槽、待试启动槽、试启动次数、待试固件版本。
    第二层:为什么要记两份
    Flash 更新一条记录 ≠ 改几个字节
    而是 = 擦掉一整页 + 重写
            ↓
    擦到一半掉电 → 整页变成花脸
            ↓
    只有一份 → 唯一的记录没了 → 变砖
            ↓
    写两份，轮流擦
    关键规则：永远擦「不是当前有效」的那一份
            ↓
    所以正在用的那份，整次写入过程中完全没被碰过
    读的时候的顺序:先 CRC 淘汰花脸的 → 再在活下来里比序号取大的。
    两份都无效呢? → 用默认值兜底(活动槽 = A),绝不卡死。
    但兜底不等于保证对 —— 元数据只是给出"候选",还得靠 image_verify 来"裁决"。
    第三层:代码怎么落地
    - ota_metadata_t:256 字节,magic + 字段 + reserved + 末尾 CRC —— 跟你写的镜像头同一个套路
    - 能推导的字段不存:"升级状态"由 pending_slot 推导,不单独存
    - CRC 只证明"没被写坏",不证明"内容合理" → 语义检查单独做一遍
    - sequence = "往元数据区写下的第几条记录",唯一用途是在两份之间分新旧
    ---
    六对最容易混的概念(逐条对照)
    ┌──────────────────────────────┬──────────────────────────────────────────────────────────────┐
    │           容易混的           │                             区别                             │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ 标志位 vs 计数               │ 标志只有 0/1,表达不了"试了 3 次";试启动上限 > 1 就必须用计数 │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ 两份记录 vs 两个槽           │ 两份记录都在元数据区;槽是放固件的地方                        │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ 记录 0/1 vs 槽 A/B           │ 毫无关系。记录 0/1 只是"第几张便签"                          │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ sequence vs firmware_version │ 前者是记录表自己的编号;后者是固件自己的版本,写在镜像头里     │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ 页号 vs 序号                 │ 页号 0→1→0→1 来回跳;序号 1→2→3→4 一路涨                      │
    ├──────────────────────────────┼──────────────────────────────────────────────────────────────┤
    │ CRC 通过 vs 内容合理         │ CRC 只说"没写坏";地址合不合理要另外查                        │
    └──────────────────────────────┴──────────────────────────────────────────────────────────────┘
*/

/*
    元数据的读写实现。核心只有两件事：

    读：两份都读出来 → 先淘汰 CRC 不过的 → 在活下来的里面选序号最大的。
    写：算出新序号 → 只擦"不是当前有效记录"的那一页 → 写进去。

    两条规则合起来保证：任何时刻至少有一份记录是完整的，
    且写的动作永远不会碰到当前正在生效的那一份。
*/

/* 两份记录各自的页首地址。数组下标 0/1 就是"第几份"。 */
static const uint32_t metadata_record_address[2] =
{
    OTA_METADATA_RECORD_0_ADDRESS,
    OTA_METADATA_RECORD_1_ADDRESS
};

/* find_current() 的返回值：没有任何一份可用。 */
#define METADATA_NO_CURRENT  0xFFU

/*
    只做"语义检查"，不碰 CRC。
    save() 在写入前也用它自检，避免把一条自己都读不回来的记录写进 Flash。
*/
static uint8_t metadata_fields_are_sane(const ota_metadata_t *record)
{
    if (record->magic != OTA_METADATA_MAGIC)
    {
        return 0U;
    }

    if ((record->active_slot != OTA_SLOT_A_BASE_ADDRESS) &&
        (record->active_slot != OTA_SLOT_B_BASE_ADDRESS))
    {
        return 0U;
    }

    if ((record->pending_slot != OTA_SLOT_NONE) &&
        (record->pending_slot != OTA_SLOT_A_BASE_ADDRESS) &&
        (record->pending_slot != OTA_SLOT_B_BASE_ADDRESS))
    {
        return 0U;
    }

    return 1U;
}

/*
    一条记录是否"完整可信"。
    注意 CRC 只能证明内容没被写坏，不能证明内容本身合理 ——
    所以语义检查（metadata_fields_are_sane）必须单独做一遍。
*/
static uint8_t metadata_record_is_valid(const ota_metadata_t *record)
{
    if (metadata_fields_are_sane(record) == 0U)
    {
        return 0U;
    }

    if (crc32_check((const uint8_t *)record,
                    OTA_METADATA_CRC_LENGTH,
                    record->record_crc32) == 0U)
    {
        return 0U;
    }

    return 1U;
}

/*
    读出两份记录，返回"当前有效记录"所在的页索引（0 或 1）。
    两份都不可用时返回 METADATA_NO_CURRENT。
    current 非空且找到时，把内容拷出来。
*/
static uint8_t metadata_find_current(ota_metadata_t *current)
{
    const ota_metadata_t *stored;
    ota_metadata_t best;
    uint8_t best_index = METADATA_NO_CURRENT;
    uint8_t index;

    for (index = 0U; index < 2U; index++)
    {
        stored = (const ota_metadata_t *)metadata_record_address[index];

        /* 先看有效性。损坏记录的 sequence 是垃圾，绝不能拿它比大小。 */
        if (metadata_record_is_valid(stored) == 0U)
        {
            continue;
        }

        if ((best_index == METADATA_NO_CURRENT) ||
            (stored->sequence > best.sequence))
        {
            best = *stored;
            best_index = index;
        }
    }

    if ((best_index != METADATA_NO_CURRENT) && (current != 0))
    {
        *current = best;
    }

    return best_index;
}

/*
    擦掉一页，再把一条记录逐字写进去。
    擦除和写入都必须先解锁 FMC，且无论成败都要上锁。
*/
static uint8_t metadata_page_write(uint32_t page_address,
                                   const ota_metadata_t *record)
{
    ota_metadata_t staged;
    const uint32_t *word;
    uint32_t index;

    staged = *record;
    staged.magic = OTA_METADATA_MAGIC;
    /* CRC 覆盖前 252 字节；record_crc32 自己在第 252 字节起，不参与计算。 */
    staged.record_crc32 =
        crc32_generate((const uint8_t *)&staged, OTA_METADATA_CRC_LENGTH);

    fmc_unlock();

    if (fmc_page_erase(page_address) != FMC_READY)
    {
        fmc_lock();
        return 0U;
    }

    word = (const uint32_t *)&staged;
    for (index = 0U; index < (sizeof(staged) / sizeof(uint32_t)); index++)
    {
        if (fmc_word_program(page_address + (index * 4U), word[index]) != FMC_READY)
        {
            fmc_lock();
            return 0U;
        }
    }

    fmc_lock();
    return 1U;
}

uint8_t ota_metadata_load(ota_metadata_t *record)
{
    ota_metadata_t current;

    if (record == 0)
    {
        return OTA_METADATA_LOAD_FAIL;
    }

    if (metadata_find_current(&current) != METADATA_NO_CURRENT)
    {
        *record = current;
        return OTA_METADATA_LOAD_REAL;
    }

    /*
        两份都不可用：全新设备（Flash 全 0xFF），或元数据被写坏。
        这里绝不返回"读不到"，而是给一个能继续走下去的默认值 ——
        只有当默认槽的镜像也校验不过时，才轮到 Bootloader 报错。
    */
    record->magic = OTA_METADATA_MAGIC;
    record->sequence = 0U;
    record->active_slot = OTA_SLOT_A_BASE_ADDRESS;
    record->pending_slot = OTA_SLOT_NONE;
    record->boot_attempts = 0U;
    record->pending_version = 0U;
    record->record_crc32 = 0U;

    return OTA_METADATA_LOAD_DEFAULT;
}

uint8_t ota_metadata_save(const ota_metadata_t *record)
{
    ota_metadata_t current;
    ota_metadata_t staged;
    uint8_t current_index;
    uint8_t target_index;

    if (record == 0)
    {
        return OTA_METADATA_FAIL;
    }

    staged = *record;

    /* 写入前自检：宁可这一步失败，也不要写进一条将来读不回来的记录。 */
    if (metadata_fields_are_sane(&staged) == 0U)
    {
        return OTA_METADATA_FAIL;
    }

    current_index = metadata_find_current(&current);

    /* 新序号永远向旧记录问出来，不另外维护计数器。 */
    staged.sequence = (current_index == METADATA_NO_CURRENT) ?
                      1U : (current.sequence + 1U);

    /*
        只擦"不是当前有效记录"的那一页。
        两份都无效时 current_index 是 NO_CURRENT，此处落到 0 号页 ——
        两份都没有可用信息，擦哪一份都不会丢东西。
    */
    target_index = (current_index == 0U) ? 1U : 0U;

    if (metadata_page_write(metadata_record_address[target_index], &staged) == 0U)
    {
        return OTA_METADATA_FAIL;
    }

    return OTA_METADATA_OK;
}
