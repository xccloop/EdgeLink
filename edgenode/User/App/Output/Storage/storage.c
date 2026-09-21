#include "storage.h"
#include "GD25Q32/gd25.h"
#include "Protocol/CRC/crc.h"
#include <stdint.h>

/*
    这个文件用于存储，主要是数据和flash的交互逻辑
    我们先看一下整个的流程message提供了除了成功发送位之外的所有函数，因此我们需要将他解码，但是pending位只有TCP/CAN成功之后才会进行的值
    我们规定16位如下
    0-3 sample_uptime_ms;
    4-7 sequence
    8-9 tempreature_raw;
    10 temperature_scale;
    11-14 CRC32
    15 pending
    CRC用于计算前十位
*/

#define STORAGE_PENDING   0xFFU
#define STORAGE_CONFIRMED 0x00U

//定义slot大小和扫描的页数
#define STORAGE_SCAN_PAGE_SIZE    256U

//定义日志存储区域
#define STORAGE_LOG_BASE_ADDRESS  0x000000UL
#define STORAGE_LOG_END_ADDRESS   0x003BF000UL


#define STORAGE_CRC_OFFSET        11U
#define STORAGE_CRC_LENGTH        4U
#define STORAGE_CRC_COVER_LENGTH  11U

/* gd25_clear() 的最小擦除单位；当前 BSP 未将该常量导出到 gd25.h。 */
#define STORAGE_SECTOR_SIZE 4096U

static uint8_t storage_scan_page[STORAGE_SCAN_PAGE_SIZE];

static uint32_t storage_next_address;
static uint8_t storage_can_append;
static uint8_t storage_ready;

static void storage_record_message_encode(uint8_t record[STORAGE_SLOT_SIZE],
                                          const telemetry_sample_struct *message)
{
    record[0] = (uint8_t)(message->sample_uptime_ms >> 24);
    record[1] = (uint8_t)(message->sample_uptime_ms >> 16);
    record[2] = (uint8_t)(message->sample_uptime_ms >> 8);
    record[3] = (uint8_t)message->sample_uptime_ms;

    record[4] = (uint8_t)(message->sequence >> 24);
    record[5] = (uint8_t)(message->sequence >> 16);
    record[6] = (uint8_t)(message->sequence >> 8);
    record[7] = (uint8_t)message->sequence;

    record[8] = (uint8_t)(message->temperature >> 8);
    record[9] = (uint8_t)message->temperature;

    record[10] = (uint8_t)message->temperature_scale;

    uint32_t crc_value = crc32_generate(record, 11);
    record[11] = (uint8_t)(crc_value >> 24);
    record[12] = (uint8_t)(crc_value >> 16);
    record[13] = (uint8_t)(crc_value >> 8);
    record[14] = (uint8_t)crc_value;

    record[15] = STORAGE_PENDING;//第一次写入不知道pending，直接赋值为ff
}

/*
    这个函数寻找哪里的一个record里面是否为空
*/
static uint8_t storage_slot_is_empty(const uint8_t record[STORAGE_SLOT_SIZE])
{
    uint8_t i;

    for(i = 0U; i < STORAGE_SLOT_SIZE; i++)
    {
        if(record[i] != 0xFFU)
        {
            return 0U;
        }
    }

    return 1U;
}

/*
    这个函数读取一个数组中的前四位，主要是为了给CRC校验做准备
*/
static uint32_t storage_u32_read_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U)  |
           ((uint32_t)data[3]);
}

/*
    Flash 中 temperature 固定使用两个字节大端补码保存。
    先组合为 uint16_t，正数直接转换；最高位为 1 时减去 65536，恢复负温度。
    不能直接把 data 强转成 int16_t 指针，否则会受 CPU 对齐和字节序影响。
*/
static int16_t storage_i16_read_be(const uint8_t *data)
{
    uint16_t raw;

    raw = ((uint16_t)data[0] << 8U) | (uint16_t)data[1];
    if(raw <= 0x7FFFU)
    {
        return (int16_t)raw;
    }

    return (int16_t)((int32_t)raw - 0x10000L);
}

/*
    这个函数计算一个record里面的crc是否有效
*/
static uint8_t storage_record_crc_is_valid(
    const uint8_t record[STORAGE_SLOT_SIZE])
{
    uint32_t stored_crc;

    stored_crc = storage_u32_read_be(&record[STORAGE_CRC_OFFSET]);

    return crc32_check(record, STORAGE_CRC_COVER_LENGTH, stored_crc);
}

/*
    此函数只做“固定 16 字节 Storage 格式 -> 业务 Message”的字段解码。
    调用者必须已经通过 storage_record_crc_is_valid()；这里不重复读 Flash、
    不检查 pending 状态，也不生成 sequence。status/CRC 都是 Storage 私有字段，
    因此不会进入 Message 或 TCP/CAN 协议。
*/
static void storage_record_message_decode(
    const uint8_t record[STORAGE_SLOT_SIZE],
    telemetry_sample_struct *message)
{
    message->sample_uptime_ms = storage_u32_read_be(&record[0]);
    message->sequence = storage_u32_read_be(&record[4]);
    message->temperature = storage_i16_read_be(&record[8]);
    message->temperature_scale = (int8_t)record[10];
}

/*
    完整扫描结束后恢复下一 sequence。
    can_append 只由 sequence 是否耗尽决定，物理写地址是否回绕不影响它。
*/
static void storage_scan_sequence_finish(storage_init_result_t *result,
                                         uint8_t has_valid_record,
                                         uint32_t max_sequence)
{
    if(has_valid_record == 0U)
    {
        result->next_sequence = 0U;
        result->can_append = 1U;
        return;
    }

    if(max_sequence == 0xFFFFFFFFUL)
    {
        result->next_sequence = 0xFFFFFFFFUL;
        result->can_append = 0U;
        return;
    }

    result->next_sequence = max_sequence + 1U;
    result->can_append = 1U;
}

/* 返回日志区内物理顺序的下一个 16 字节槽位；末尾回绕到日志起点。 */
static uint32_t storage_address_next(uint32_t address)
{
    address += STORAGE_SLOT_SIZE;

    if(address >= STORAGE_LOG_END_ADDRESS)
    {
        return STORAGE_LOG_BASE_ADDRESS;
    }

    return address;
}

/* 返回地址所在 4 KiB 擦除扇区的起始地址，供进入扇区前的擦除判断使用。 */
static uint32_t storage_sector_base(uint32_t address)
{
    return address - (address % STORAGE_SECTOR_SIZE);
}

/*
    擦除前只检查目标 4 KiB 扇区。返回 STORAGE_SUCCESS 表示扫描已完成，
    has_pending 为 1 时该扇区仍含有效 pending，调用者不得擦除。
    CRC 错记录不具备可恢复业务数据，不作为 pending；但任意 Flash 读取失败
    都必须返回 STORAGE_FAIL，不能把未知内容误判成可擦除。
*/
static uint8_t storage_sector_pending_check(uint32_t sector_address,
                                            uint8_t *has_pending)
{
    uint32_t page_address;
    uint32_t slot_offset;
    const uint8_t *record;

    if((has_pending == 0) ||
       (sector_address > (STORAGE_LOG_END_ADDRESS - STORAGE_SECTOR_SIZE)) ||
       ((sector_address % STORAGE_SECTOR_SIZE) != 0U))
    {
        return STORAGE_FAIL;
    }

    *has_pending = 0U;

    for(page_address = sector_address;
        page_address < (sector_address + STORAGE_SECTOR_SIZE);
        page_address += STORAGE_SCAN_PAGE_SIZE)
    {
        if(gd25_read(page_address,
                     storage_scan_page,
                     STORAGE_SCAN_PAGE_SIZE) == 0U)
        {
            return STORAGE_FAIL;
        }

        for(slot_offset = 0U;
            slot_offset < STORAGE_SCAN_PAGE_SIZE;
            slot_offset += STORAGE_SLOT_SIZE)
        {
            record = &storage_scan_page[slot_offset];

            if((storage_slot_is_empty(record) == 0U) &&
               (storage_record_crc_is_valid(record) != 0U) &&
               (record[15] != STORAGE_CONFIRMED))
            {
                *has_pending = 1U;
                return STORAGE_SUCCESS;
            }
        }
    }

    return STORAGE_SUCCESS;
}

/*
    一次遍历完成 StorageTask 启动时需要的全部 Flash 恢复信息：
    1. 第一个全 FF 槽位，作为后续顺序追加的写地址；
    2. 最大合法 sequence，推导 next_sequence；
    3. CRC 正确且 status != 0x00 的最小 sequence，作为首次恢复发送对象。

    每次从 GD25Q32 读取 256 字节到 storage_scan_page，再在 RAM 中解析 16 个
    固定槽位，避免每个槽位都单独发起 SPI 读取。这个函数不发送 TCP/CAN，也不
    写 Flash；它只把扫描结果填入 result。
*/
static uint8_t storage_scan_log(storage_init_result_t *result)
{
    /*
        page_address
        当前正在扫描 Flash 的哪一个 256B 页面
        slot_offset
            当前检查这个 256B 页面里的哪个 16B record
        sequence
            当前 record 的 sequence
        max_sequence
            扫描到目前为止最大的合法 sequence
        has_valid_record
            到目前为止有没有发现过合法 record
        record
            指向当前 16B record
    */
    uint32_t page_address;
    uint32_t slot_offset;
    uint32_t sequence;
    uint32_t max_sequence = 0U;
    uint32_t max_sequence_address = STORAGE_LOG_BASE_ADDRESS;
    uint32_t oldest_pending_sequence = 0U;
    uint8_t has_valid_record = 0U;
    const uint8_t *record;

    //一次读取一页
    for(page_address = STORAGE_LOG_BASE_ADDRESS;
        page_address < STORAGE_LOG_END_ADDRESS;
        page_address += STORAGE_SCAN_PAGE_SIZE)
    {
        //获取一页的信息，然后存储进入位于RAM的数组
        if(gd25_read(page_address,
                     storage_scan_page,
                     STORAGE_SCAN_PAGE_SIZE) == 0U)
        {
            return STORAGE_FAIL;
        }

        //16个字节一个slot所以一次处理16
        for(slot_offset = 0U;
            slot_offset < STORAGE_SCAN_PAGE_SIZE;
            slot_offset += STORAGE_SLOT_SIZE)
        {
            //定义record
            record = &storage_scan_page[slot_offset];

            /* 循环日志中空槽后仍可能有回绕前的历史记录，不能提前结束扫描。 */
            if(storage_slot_is_empty(record) != 0U)
            {
                continue;
            }

            /*
                非全 FF 但 CRC 错：可能是首次写入时掉电。
                它已不能覆盖，但也不能参与 sequence 恢复。
            */
            if(storage_record_crc_is_valid(record) == 0U)
            {
                continue;
            }

            /*
                当前定版布局：
                [0..3] uptime
                [4..7] sequence
            */
            sequence = storage_u32_read_be(&record[4]);

            /*
                当前所有合法记录里面最大的 sequence。
            */
            if((has_valid_record == 0U) ||
               (sequence > max_sequence))
            {
                max_sequence = sequence;
                /* 记录最大 sequence 的槽位，扫描结束后由它恢复写指针。 */
                max_sequence_address = page_address + slot_offset;
                has_valid_record = 1U;
            }

            /* status 只要不是 0x00 都视为 pending，包括确认写入时掉电留下的中间值。 */
            if(record[15] != STORAGE_CONFIRMED)
            {
                if((result->has_pending == 0U) ||
                   (sequence < oldest_pending_sequence))
                {
                    storage_record_message_decode(record,
                                                  &result->oldest_pending_message);
                    result->oldest_pending_address = page_address + slot_offset;
                    oldest_pending_sequence = sequence;
                    result->has_pending = 1U;
                }
            }
        }
    }

    /* 空日志从起点写；否则从最大 sequence 的下一物理槽位继续写。 */
    if(has_valid_record == 0U)
    {
        storage_next_address = STORAGE_LOG_BASE_ADDRESS;
    }
    else
    {
        storage_next_address = storage_address_next(max_sequence_address);
    }
    /*
        上次尝试写入 sequence = 5 时可能掉电，导致该槽位非 FF 但 CRC 错。
        扫描后最大合法 sequence 仍为 4，因此下一次仍应分配 sequence = 5；
        但不能复用这个半写槽位，因为 NOR Flash 只能把位从 1 写成 0。
        因此从最大合法记录的下一个槽位开始，在同一扇区内跳过所有非空槽。
        找到第一个全 FF 槽位时可安全写入；若已走到下一扇区，则停止，
        后续进入该扇区前会由写入逻辑先擦除整个扇区。
    */
    if(has_valid_record != 0U)
    {
        while(storage_sector_base(storage_next_address) ==
            storage_sector_base(max_sequence_address))
        {
            if(gd25_read(storage_next_address,
                        storage_scan_page,
                        STORAGE_SLOT_SIZE) == 0U)
            {
                return STORAGE_FAIL;
            }

            if(storage_slot_is_empty(storage_scan_page) != 0U)
            {
                break;
            }

            storage_next_address = storage_address_next(storage_next_address);
        }
    }
    result->next_write_address = storage_next_address;
    storage_scan_sequence_finish(result, has_valid_record, max_sequence);
    return STORAGE_SUCCESS;
}

/*
    Storage 的初始化入口：先完成 GD25Q32 初始化，再调用 storage_scan_log()。
    成功后 storage_ready 与 storage_can_append 才会生效，其他 Storage API 才可
    调用。它不擦除 Flash、不发送 TCP/CAN；将来 StorageTask 取得 result 后，
    自己决定先投递 oldest_pending_message 还是放行 CollectTask。
*/
uint8_t storage_init(storage_init_result_t *result)
{
    if(result == 0)
    {
        return STORAGE_FAIL;
    }

    storage_ready = 0U;
    storage_next_address = STORAGE_LOG_BASE_ADDRESS;
    storage_can_append = 0U;
    result->next_sequence = 0U;
    result->next_write_address = STORAGE_LOG_BASE_ADDRESS;
    result->can_append = 0U;
    result->has_pending = 0U;
    result->oldest_pending_address = STORAGE_LOG_BASE_ADDRESS;
    result->oldest_pending_message.sample_uptime_ms = 0U;
    result->oldest_pending_message.sequence = 0U;
    result->oldest_pending_message.temperature = 0;
    result->oldest_pending_message.temperature_scale = 0;

    if(gd25_init() == 0U)
    {
        return STORAGE_FAIL;
    }

    if(storage_scan_log(result) == STORAGE_FAIL)
    {
        return STORAGE_FAIL;
    }

    storage_can_append = result->can_append;
    storage_ready = 1U;
    return STORAGE_SUCCESS;
}

/*
    这个函数用于数据采集到以后的第一次存储
*/
uint8_t storage_write_pending(const telemetry_sample_struct *message,
                              uint32_t *flash_address)
{
    uint8_t record[STORAGE_SLOT_SIZE];//定义一个槽位
    uint8_t target_sector_has_pending;

    if((message == 0) || (flash_address == 0) ||
       (storage_ready == 0U) || (storage_can_append == 0U))
    {
        return STORAGE_FAIL;
    }
    //回环判断
    if(storage_next_address >= STORAGE_LOG_END_ADDRESS)
    {
        storage_next_address = STORAGE_LOG_BASE_ADDRESS;
    }
    //进入一个新的扇区前，必须确认旧扇区没有任何有效 pending。
    if((storage_next_address % STORAGE_SECTOR_SIZE) == 0U)
    {
        if(storage_sector_pending_check(
               storage_sector_base(storage_next_address),
               &target_sector_has_pending) != STORAGE_SUCCESS)
        {
            return STORAGE_FAIL;
        }

        if(target_sector_has_pending != 0U)
        {
            return STORAGE_FAIL;
        }

        if(gd25_clear(storage_next_address) == 0U)
        {
            return STORAGE_FAIL;
        }
    }
    //message解码为record
    storage_record_message_encode(record, message);

    //对storage_next_adress存储record
    if(gd25_write(storage_next_address,
                  record,
                  STORAGE_SLOT_SIZE) == 0U)
    {
        return STORAGE_FAIL;
    }

    //这里的flash_adress就是为了给后续第二次写入不知道在哪里做准备
    //然后sroage_next_adress推进
    *flash_address = storage_next_address;
    storage_next_address = storage_address_next(storage_next_address);

    return STORAGE_SUCCESS;
}

uint8_t storage_write_pending_ready(void)
{
    uint32_t next_address;
    uint8_t target_sector_has_pending;

    if((storage_ready == 0U) || (storage_can_append == 0U))
    {
        return STORAGE_FAIL;
    }

    next_address = storage_next_address;
    if(next_address >= STORAGE_LOG_END_ADDRESS)
    {
        next_address = STORAGE_LOG_BASE_ADDRESS;
    }

    if((next_address % STORAGE_SECTOR_SIZE) != 0U)
    {
        return STORAGE_SUCCESS;
    }

    if(storage_sector_pending_check(storage_sector_base(next_address),
                                    &target_sector_has_pending)
       != STORAGE_SUCCESS)
    {
        return STORAGE_FAIL;
    }

    if(target_sector_has_pending != 0U)
    {
        return STORAGE_FAIL;
    }

    return STORAGE_SUCCESS;
}

/*
    这个函数时将数据发送好以后判断是否发送完第二次写入数据的
*/
uint8_t storage_confirm(uint32_t flash_address,
                        uint32_t ack_sequence)
{
    uint8_t record[STORAGE_SLOT_SIZE];
    uint8_t confirmed = STORAGE_CONFIRMED;
    uint8_t status;
    uint32_t record_sequence;

    /* 确认事件只能指向日志区内一个完整槽位的起始地址。 */
    if((storage_ready == 0U) ||
       (flash_address > (STORAGE_LOG_END_ADDRESS - STORAGE_SLOT_SIZE)) ||
       (((flash_address - STORAGE_LOG_BASE_ADDRESS) % STORAGE_SLOT_SIZE) != 0U))
    {
        return STORAGE_FAIL;
    }

    /* 先读取原记录。 */
    if(gd25_read(flash_address, record, STORAGE_SLOT_SIZE) == 0U)
    {
        return STORAGE_FAIL;
    }

    /* 原记录坏了，不能确认。 */
    if(storage_record_crc_is_valid(record) == 0U)
    {
        return STORAGE_FAIL;
    }

    /* Flash 格式中 sequence 在 [4..7]。 */
    record_sequence = storage_u32_read_be(&record[4]);

    /* ACK 不能确认错误地址上的另一条记录。 */
    if(record_sequence != ack_sequence)
    {
        return STORAGE_FAIL;
    }

    /* 已确认，重复 ACK 直接成功。 */
    if(record[15] == STORAGE_CONFIRMED)
    {
        return STORAGE_SUCCESS;
    }

    /* 第二次：只写状态字节。 */
    if(gd25_write(flash_address + 15U, &confirmed, 1U) == 0U)
    {
        return STORAGE_FAIL;
    }

    /* 仅当读回值精确为 0x00，才可把本条记录视为 confirmed。 */
    if((gd25_read(flash_address + 15U, &status, 1U) == 0U) ||
       (status != STORAGE_CONFIRMED))
    {
        return STORAGE_FAIL;
    }

    return STORAGE_SUCCESS;
}

/*
    last_process_adress:找到的最后一条pending并且ACK的记录
    next_pending_message：返回下一条没有找到的记录信息
    next_pending_adress:返回下一条pending地址
*/
uint8_t storage_find_next_pending(
    uint32_t last_processed_address,
    telemetry_sample_struct *next_pending_message,
    uint32_t *next_pending_address)
{
    uint32_t first_search_address;
    uint32_t current_address;
    uint8_t record[STORAGE_SLOT_SIZE];

    /*
        1. 调用者必须给我两个“存放输出结果的位置”；
           Storage 也必须已经初始化完成。
    */
    if((next_pending_message == 0) ||
    (next_pending_address == 0) ||
    (storage_ready == 0U) ||
    (last_processed_address >
            (STORAGE_LOG_END_ADDRESS - STORAGE_SLOT_SIZE)) ||
    (((last_processed_address - STORAGE_LOG_BASE_ADDRESS) %
            STORAGE_SLOT_SIZE) != 0U))
    {
        return STORAGE_FAIL;
    }
    /*
        2. 从已处理记录的下一个槽位开始找。
    */
    first_search_address =
        storage_address_next(last_processed_address);

    current_address = first_search_address;

    while(1)
    {
        /*
            3. 读取当前 16 字节槽位。
        */
        if(gd25_read(current_address,
                     record,
                     STORAGE_SLOT_SIZE) == 0U)
        {
            return STORAGE_FAIL;
        }

        /*
            4. 这就是我们要找的下一条 pending。
        */
        if((storage_slot_is_empty(record) == 0U) &&
           (storage_record_crc_is_valid(record) != 0U) &&
           (record[15] != STORAGE_CONFIRMED))
        {
            storage_record_message_decode(
                record,
                next_pending_message);

            *next_pending_address = current_address;

            return STORAGE_SUCCESS;
        }

        /*
            5. 当前槽位不是目标，继续下一个。
        */
        current_address =
            storage_address_next(current_address);

        /*
            6. 又回到起点，说明整圈都没有 pending。
        */
        if(current_address == first_search_address)
        {
            return STORAGE_NO_PENDING;
        }
    }
}
