#include "storage.h"
#include "GD25Q32/gd25.h"
#include "Frame/CRC/crc.h"
#include <stdint.h>

#define STORAGE_FLASH_CAPACITY_BYTES 0x00400000UL
#define STORAGE_SECTOR_SIZE 4096UL
#define STORAGE_ERASED_BYTE 0xFFU
#define STORAGE_RECORD_CRC_OFFSET STORAGE_TEMPERATURE_RECORD_LENGTH
#define STORAGE_RECORD_CRC_LENGTH 4U
#define STORAGE_RECORD_DATA_LENGTH (STORAGE_TEMPERATURE_RECORD_LENGTH + STORAGE_RECORD_CRC_LENGTH)
#define STORAGE_RECORD_COMMIT_OFFSET (STORAGE_TEMPERATURE_RECORD_SLOT_SIZE - 1U)
#define STORAGE_RECORD_COMMIT_VALUE 0x00U

static uint32_t storage_next_address;
static uint8_t storage_ready;

/*
    Storage这一层位于Message和GD25Q32 BSP之间。
    Message只负责给出一次采集结果，GD25Q32 BSP只负责按地址读、页写和扇区擦除；
    Storage负责把两者连接起来，决定“数据写到哪里、何时擦除、掉电后哪些数据还能相信”。

    本文件把4 MiB外置Flash从0x000000开始当作只追加的温度日志区域。
    Flash的最小擦除单位是4 KiB，一个扇区中有4096 / 16 = 256个记录槽位；
    每条记录虽然只有9字节温度数据，但槽位固定为16字节。这样槽位起始地址
    永远是16的倍数，而16能整除256，所以一次13字节页写绝不会跨页。

    一个16字节槽位的固定布局如下：
        [0..3]   sample_uptime_ms，大端
        [4..7]   temperature，大端补码
        [8]      temperature_scale
        [9..12]  前9字节的CRC32，大端
        [13..14] 预留，保持擦除态0xFF
        [15]     commit提交标记：0xFF表示尚未提交，0x00表示已提交

    正常写入顺序必须是：先擦除新扇区 -> 写入载荷和CRC -> 最后单独写commit
    -> 读回再校验。commit放在最后，是因为Flash只能把位从1写成0：如果写到
    一半突然掉电，commit通常仍不是0x00，后续读取就不会把这条半写记录当有效。

    掉电还可能发生在4 KiB扇区擦除期间，此时扇区内容不能相信。为避免重启后
    误写这种扇区，storage_init()不会续写已有记录的扇区，而是只选择“整扇区
    都是0xFF”的区域重新开始。代价是上一个未写满扇区的剩余槽位会被放弃；
    收益是不会为了节省几个槽位而覆盖旧数据或向未知状态的Flash继续编程。

    注意：本文件当前只实现追加保存和本次写后的读回验证，还没有提供历史记录
    的读取/解码接口。未来读取接口应同时检查commit和CRC，只有两者都正确的
    槽位才返回给业务层。
*/
/*
    这个文件基于GD25Q32进行数据存储规划选择
    我们来回顾一下流程，首先我们要将message转化成数组这样才能将数据通过SPI发送给FLASH
    一些注意事项：单次最多一页、不能跨 256 字节、擦除必须 4 KiB 对齐；
    Flash 不能像 RAM 一样随意改写；写入只能把位从 1 变成 0。要把数据恢复成 1，必须先擦除
    要写入数据，需要制定一个扇区，然后擦除某一个扇区，将数据进行存储，存储地址递增，写满后换到下一个扇区以此类推，
    所以我认为要设置一个基本的地址，比如0x00，这样做就是从0x00开始一直往下写
*/

/*
    这个函数辅助我们将message转化为数组
*/
static void message_encode(uint8_t *data, const telemetry_sample_struct *message)
{
    uint32_t temperature;

    /*
        这个函数只做一件事：把内存中的telemetry_sample_struct转换成Flash中的
        9字节业务载荷，不直接读写Flash。

        之所以单独拆出编码，是因为Message结构体在RAM中的排列会受编译器对齐、
        大小端和后续字段变化影响，不能直接把结构体地址交给gd25_write()。这里
        明确指定每个字段占几个字节及其顺序，Storage、TCP和CAN就有一致的解释。

        temperature是有符号int32_t。先转成uint32_t只是为了保留它的32位补码，
        再依次右移取字节；不能先缩窄为uint8_t，否则高24位会在写入前丢失。
    */

    /* Storage和TCP/CAN统一按大端保存多字节字段，后续读取不必针对通道转换字节序。 */
    data[0] = (uint8_t)(message->sample_uptime_ms >> 24U);
    data[1] = (uint8_t)(message->sample_uptime_ms >> 16U);
    data[2] = (uint8_t)(message->sample_uptime_ms >> 8U);
    data[3] = (uint8_t)(message->sample_uptime_ms >> 0U);

    /* 先保留int32_t的全部补码位，再逐字节取出；不能在右移前缩窄为uint8_t。 */
    temperature = (uint32_t)message->temperature;
    data[4] = (uint8_t)(temperature >> 24U);
    data[5] = (uint8_t)(temperature >> 16U);
    data[6] = (uint8_t)(temperature >> 8U);
    data[7] = (uint8_t)(temperature >> 0U);

    data[8] = (uint8_t)message->temperature_scale;
}

static void storage_record_encode(uint8_t *data, const telemetry_sample_struct *message)
{
    uint32_t crc;

    /*
        这个函数在9字节业务载荷之后补上4字节CRC32，得到本次真正要写进Flash的
        13字节数据。CRC覆盖范围只有data[0..8]：它用于判断采集数据本身是否被
        半写或损坏，CRC自己的4字节当然不能再参与自己的计算，commit也不参与。

        CRC使用现有Frame/CRC模块，而不是Storage重新实现一份算法。这样同一个
        工程只有一套CRC32定义，后续调试时不会出现“通信和存储CRC算法不同”的问题。
    */

    message_encode(data, message);
    /* CRC只覆盖9字节业务载荷；CRC自身和最后的commit不参与计算。 */
    crc = crc32_generate(data, STORAGE_TEMPERATURE_RECORD_LENGTH);
    data[STORAGE_RECORD_CRC_OFFSET + 0U] = (uint8_t)(crc >> 24U);
    data[STORAGE_RECORD_CRC_OFFSET + 1U] = (uint8_t)(crc >> 16U);
    data[STORAGE_RECORD_CRC_OFFSET + 2U] = (uint8_t)(crc >> 8U);
    data[STORAGE_RECORD_CRC_OFFSET + 3U] = (uint8_t)(crc >> 0U);
}

static uint8_t storage_sector_is_erased(uint32_t sector_address)
{
    uint8_t data[STORAGE_TEMPERATURE_RECORD_SLOT_SIZE];
    uint32_t address;
    uint32_t i;

    /*
        这个函数确认以sector_address开始的整个4 KiB扇区是否全为0xFF。
        它逐个读取16字节槽位，再逐字节确认，而不是只读扇区开头。

        原因是掉电可能发生在gd25_clear()的内部擦除过程中：此时同一扇区的某些
        地址已变成0xFF，另一些地址仍是旧数据或未知状态。只检查一个空槽会误以为
        扇区可写，后续页写可能失败或留下混杂数据；整扇区检查才能把它排除。
    */

    /*
        重启恢复时必须确认整个4 KiB扇区都是擦除态，不能只检查一个16字节槽位。
        这样即使掉电发生在扇区擦除中，也不会向状态未知的剩余区域继续写。
    */
    for(address = sector_address;
        address < (sector_address + STORAGE_SECTOR_SIZE);
        address += STORAGE_TEMPERATURE_RECORD_SLOT_SIZE)
    {
        if(gd25_read(address, data, STORAGE_TEMPERATURE_RECORD_SLOT_SIZE) == 0U)
        {
            return STORAGE_FAIL;
        }
        for(i = 0U; i < STORAGE_TEMPERATURE_RECORD_SLOT_SIZE; i++)
        {
            if(data[i] != STORAGE_ERASED_BYTE)
            {
                return STORAGE_FAIL;
            }
        }
    }

    return STORAGE_SUCCESS;
}

static uint8_t storage_record_verify(uint32_t address, const uint8_t *data)
{
    uint8_t verify_data[STORAGE_TEMPERATURE_RECORD_SLOT_SIZE];
    uint32_t stored_crc;
    uint32_t i;

    /*
        这个函数是“写后读回”步骤。它从Flash重新读取完整16字节槽位，并依次验证：
        1. 读回的0..12字节与刚刚准备写入的数据完全一致；
        2. 第15字节commit已经是0x00，说明提交动作完成；
        3. 读出的CRC32能校验读出的9字节载荷。

        gd25_write()返回成功只说明SPI事务和Flash忙等待正常结束，不能单独证明
        存储单元内容正确。再读一次可以在本次运行中及时发现写错误；一旦失败，
        上层会停止继续写同一槽位，因为NOR Flash不允许把已经写成0的位恢复为1。
    */

    if(gd25_read(address, verify_data, STORAGE_TEMPERATURE_RECORD_SLOT_SIZE) == 0U)
    {
        return STORAGE_FAIL;
    }
    for(i = 0U; i < STORAGE_RECORD_DATA_LENGTH; i++)
    {
        if(verify_data[i] != data[i])
        {
            return STORAGE_FAIL;
        }
    }
    stored_crc = ((uint32_t)verify_data[STORAGE_RECORD_CRC_OFFSET + 0U] << 24U) |
                 ((uint32_t)verify_data[STORAGE_RECORD_CRC_OFFSET + 1U] << 16U) |
                 ((uint32_t)verify_data[STORAGE_RECORD_CRC_OFFSET + 2U] << 8U) |
                 ((uint32_t)verify_data[STORAGE_RECORD_CRC_OFFSET + 3U] << 0U);
    if((verify_data[STORAGE_RECORD_COMMIT_OFFSET] != STORAGE_RECORD_COMMIT_VALUE) ||
       (crc32_check(verify_data, STORAGE_TEMPERATURE_RECORD_LENGTH, stored_crc) == 0U))
    {
        return STORAGE_FAIL;
    }
    return STORAGE_SUCCESS;
}

uint8_t storage_init(void)
{
    uint32_t address;

    /*
        这是Storage的启动入口，必须在board_config_init()完成共享SPI0初始化后调用。
        它先调用gd25_init()读取JEDEC ID，确认Flash型号和SPI通信正常；通过后再从
        STORAGE_BASE_ADDRESS开始逐扇区寻找一个完整擦除的4 KiB区域。

        本函数不在启动时立即擦除任何扇区，也不续写上次未满的扇区。前者避免“仅仅
        上电就破坏历史”，后者避免“上一次正在擦除时掉电”留下的未知状态。找到的
        地址保存在storage_next_address，真正的擦除留给第一次storage_temperature()
        调用；如果全Flash都没有完整空扇区，保持storage_ready为0并返回失败。
    */

    storage_ready = 0U;
    storage_next_address = STORAGE_BASE_ADDRESS;

    if(gd25_init() == 0U)
    {
        return STORAGE_FAIL;
    }

    /*
        重启后只选择完整4 KiB均为FF的新扇区。若上一个扇区只有部分记录，
        其剩余槽位会被放弃而不会覆盖；这用少量容量换取了擦除中掉电后的安全性。
        因此本版本不会在复位后续写一个已有数据的扇区。
    */
    for(address = STORAGE_BASE_ADDRESS;
        address < STORAGE_FLASH_CAPACITY_BYTES;
        address += STORAGE_SECTOR_SIZE)
    {
        if(storage_sector_is_erased(address) == STORAGE_SUCCESS)
        {
            storage_next_address = address;
            storage_ready = 1U;
            return STORAGE_SUCCESS;
        }
    }

    /* 没有空槽时保持未就绪，避免后续调用回绕覆盖最早的历史数据。 */
    return STORAGE_FAIL;
}

uint8_t storage_temperature(const telemetry_sample_struct *message)
{
    uint8_t data[STORAGE_RECORD_DATA_LENGTH];
    uint8_t commit = STORAGE_RECORD_COMMIT_VALUE;

    /*
        这是APP层保存一条温度记录的公开函数。调用前必须成功执行storage_init()；
        函数会把message编码为13字节“载荷+CRC”，写到当前槽位，最后再写commit，
        并且读回校验。全部步骤成功后才把storage_next_address增加16，指向下一槽位。

        这里的特殊情况处理如下：
        - 未初始化、空message、地址超过4 MiB：拒绝写入，防止野指针或回绕覆盖；
        - 当前地址正好是4 KiB边界：说明进入新扇区，先擦除一次；同一扇区后续
          256条记录不擦除，避免每条记录都损耗一个扇区；
        - 擦除、载荷写入、commit写入或读回校验任一步失败：立刻清storage_ready。
          因为失败的槽位可能已经有部分0位，再次直接写会违反Flash只能1到0的规则；
        - 写到最后一个槽位：标记为未就绪而不回绕，确保历史数据不会被自动覆盖。
    */

    if((storage_ready == 0U) || (message == 0))
    {
        return STORAGE_FAIL;
    }
    if(storage_next_address > (STORAGE_FLASH_CAPACITY_BYTES - STORAGE_TEMPERATURE_RECORD_SLOT_SIZE))
    {
        storage_ready = 0U;
        return STORAGE_FAIL;
    }

    /*
        每个4 KiB扇区首次写入前先擦除一次。扇区为Storage专属区域，
        因此此操作不会影响其它模块的数据；同一扇区内后续256条记录不再擦除。
    */
    if((storage_next_address % STORAGE_SECTOR_SIZE) == 0U)
    {
        if(gd25_clear(storage_next_address) == 0U)
        {
            /* 擦除失败后该扇区内容未知，当前启动内禁止继续向它写入。 */
            storage_ready = 0U;
            return STORAGE_FAIL;
        }
    }

    storage_record_encode(data, message);
    /* 一次写入9字节载荷和4字节CRC；commit仍维持擦除态FF。 */
    if(gd25_write(storage_next_address, data, STORAGE_RECORD_DATA_LENGTH) == 0U)
    {
        /* 页写失败时该槽位可能已经部分写入，不能在不擦除的情况下直接重试。 */
        storage_ready = 0U;
        return STORAGE_FAIL;
    }

    /* 载荷确认写完后才写提交标记；掉电时未提交的槽位会在下次初始化时被跳过。 */
    if(gd25_write(storage_next_address + STORAGE_RECORD_COMMIT_OFFSET, &commit, 1U) == 0U)
    {
        storage_ready = 0U;
        return STORAGE_FAIL;
    }
    if(storage_record_verify(storage_next_address, data) == STORAGE_FAIL)
    {
        storage_ready = 0U;
        return STORAGE_FAIL;
    }

    if(storage_next_address == (STORAGE_FLASH_CAPACITY_BYTES - STORAGE_TEMPERATURE_RECORD_SLOT_SIZE))
    {
        /* 最后一个槽位已写完；不回绕覆盖历史数据。 */
        storage_next_address = STORAGE_FLASH_CAPACITY_BYTES;
        storage_ready = 0U;
    }
    else
    {
        storage_next_address += STORAGE_TEMPERATURE_RECORD_SLOT_SIZE;
    }
    return STORAGE_SUCCESS;
}

uint32_t storage_next_address_get(void)
{
    /*
        这个函数只读出当前运行期间的下一槽位地址，不读Flash、不改变任何状态。
        它主要给HMI、串口日志或调试器观察“本次下一条会写到哪里”；地址真正的
        恢复仍由下一次上电后的storage_init()负责，不能把这个RAM变量当持久数据。
    */
    return storage_next_address;
}

/*
    现在我们就做到了数据的解码加存储
    但是这还没有完，flash存储到关键点还在于掉电保存和写入一半掉电会怎么样
    我们在写入的时候，虽然data只有9位，但是我们却给每一个data分配16位
    我们完全可以用这个16位的区域加上标志位来判断，这一位是否有效
    因此我们需要用一个状态机来描述这个
*/
