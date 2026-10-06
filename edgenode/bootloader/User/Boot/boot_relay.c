#include "boot_relay.h"
#include "flash_layout.h"
#include "ota_image.h"
#include "ExFlash/external_flash_layout.h"
#include "gd25.h"
#include "gd32f10x_fmc.h"
#include "crc32.h"
#include "CH340/ch340.h"
#include <stdint.h>
#include <string.h>

/*
    把外置 Flash 中转区里的镜像搬进内部槽。判据见 boot_relay.h 顶部那段。

    这一层要处理的核心矛盾：**两份 Flash 的读法不一样。**
        内部 Flash：内存映射，能直接当指针解引用（image_verify 就是这么干的）
        外置 GD25：SPI，必须 read 进 RAM 才能看
    所以搬运的过程就是"读一块 → 写一块"，中间用 relay_chunk 当摆渡。
*/

/* 一次从外置 Flash 读多少。必须是 4 的倍数 —— 写内部 Flash 是按字（4 字节）的。 */
#define RELAY_READ_CHUNK  256U

/* GD32F103RC 一页 2 KiB，是擦除的最小单位。 */
#define FMC_PAGE_SIZE     2048U

/* 向量表里：第 0 项 MSP 初值，第 1 项 Reset_Handler。 */
#define VECTOR_TABLE_SIZE 8U

/* SRAM 范围（GD32F103RCT6 是 48KB），用来判断 MSP 像不像话。 */
#define MCU_SRAM_BASE_ADDRESS  0x20000000UL
#define MCU_SRAM_SIZE          (48UL * 1024UL)
#define MCU_SRAM_END_ADDRESS   (MCU_SRAM_BASE_ADDRESS + MCU_SRAM_SIZE)

/* 256 字节的镜像头 + 256 字节的摆渡缓冲 —— 都不放栈上。 */
static ota_image_header_t relay_header;
static uint8_t relay_chunk[RELAY_READ_CHUNK];

/* GD25 只初始化一次（它有几百毫秒级别的固定序列，每次进来都做一遍没必要）。 */
static uint8_t relay_flash_ready;

/* ------------------------------------------------------------------ */
/* 读的辅助函数                                                        */
/* ------------------------------------------------------------------ */

static uint8_t relay_read(uint32_t offset, uint8_t *data, uint32_t length)
{
    return gd25_read(OTA_RELAY_BASE_ADDRESS + offset, data, length);
}

static uint8_t relay_ensure_flash(void)
{
    if(relay_flash_ready != 0U)
    {
        return 1U;
    }
    if(gd25_init() == 0U)
    {
        return 0U;
    }
    relay_flash_ready = 1U;
    return 1U;
}

/* ------------------------------------------------------------------ */
/* 校验                                                                */
/* ------------------------------------------------------------------ */

/*
    判据逐条对应 Driver/Core/Src/image_verify.c。
    唯一的差别在 Reset_Handler 那条：那边拿的是真实槽基址，
    这边只能拿【镜像自己声明的目标槽】—— 因为它还没搬过去。
*/
static uint8_t relay_verify(void)
{
    uint32_t app_length;
    uint32_t app_start;
    uint32_t crc;
    uint32_t addr;
    uint32_t remain;
    uint32_t app_msp;
    uint32_t app_reset;
    uint8_t  vector[VECTOR_TABLE_SIZE];

    if(relay_read(0U, (uint8_t *)&relay_header, sizeof(relay_header)) == 0U)
    {
        return 0U;
    }

    if(relay_header.magic != OTA_IMAGE_MAGIC)
    {
        return 0U;
    }
    if(relay_header.header_version != OTA_IMAGE_FORMAT_VERSION)
    {
        return 0U;
    }
    if((uint32_t)relay_header.header_size != (uint32_t)OTA_IMAGE_HEADER_SIZE)
    {
        return 0U;
    }
    if(crc32_check((const uint8_t *)&relay_header,
                   OTA_IMAGE_HEADER_CRC_LENGTH,
                   relay_header.header_crc32) == 0U)
    {
        return 0U;
    }
    if(relay_header.hardware_model != OTA_CURRENT_HARDWARE_MODEL)
    {
        return 0U;
    }

    /* 上下界都要查，而且必须在后面用它之前查 —— 坏掉的长度会让搬运跑出槽外。 */
    app_length = relay_header.application_length;
    if((app_length < OTA_APPLICATION_MIN_LENGTH) ||
       (app_length > (OTA_SLOT_SIZE - OTA_APPLICATION_OFFSET)))
    {
        return 0U;
    }

    /* target_slot 必须是两个真实槽基址之一 —— 否则下面拿它算区间毫无意义。 */
    if((relay_header.target_slot != OTA_SLOT_A_BASE_ADDRESS) &&
       (relay_header.target_slot != OTA_SLOT_B_BASE_ADDRESS))
    {
        return 0U;
    }

    /* 应用本体读回来算 CRC。一段一段喂给分块接口，不能整个搬进 RAM。 */
    crc = crc32_begin();
    addr = OTA_IMAGE_HEADER_SIZE;
    remain = app_length;
    while(remain > 0U)
    {
        uint16_t chunk = (remain > RELAY_READ_CHUNK) ? (uint16_t)RELAY_READ_CHUNK
                                                     : (uint16_t)remain;

        if(relay_read(addr, relay_chunk, chunk) == 0U)
        {
            return 0U;
        }
        crc = crc32_update(crc, relay_chunk, chunk);
        addr += (uint32_t)chunk;
        remain -= (uint32_t)chunk;
    }
    if(crc32_finish(crc) != relay_header.application_crc32)
    {
        return 0U;
    }

    /* 向量表自检：MSP 落在 SRAM 且 8 字节对齐，Reset_Handler 带 Thumb 位。 */
    if(relay_read(OTA_IMAGE_HEADER_SIZE, vector, VECTOR_TABLE_SIZE) == 0U)
    {
        return 0U;
    }
    (void)memcpy(&app_msp, vector, 4U);
    (void)memcpy(&app_reset, &vector[4], 4U);

    if((app_msp < MCU_SRAM_BASE_ADDRESS) || (app_msp > MCU_SRAM_END_ADDRESS) ||
       ((app_msp & 0x07U) != 0U))
    {
        return 0U;
    }
    if((app_reset & 1U) == 0U)
    {
        return 0U;
    }
    app_reset &= ~1U;

    /*
        这一步就是 target_slot 存在的意义：
        中转区里这份镜像还没搬过去，拿中转区地址算"它在不在自己的区间里"没有意义。
        但拿【它声明的目标槽】算就有意义 —— 那正是它搬过去之后会住的地方。
        装错槽的镜像（A 版被标成 B）在这里就会露馅。
    */
    app_start = relay_header.target_slot + OTA_APPLICATION_OFFSET;
    if((app_reset < app_start) || (app_reset >= (app_start + app_length)))
    {
        return 0U;
    }

    return 1U;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

uint8_t boot_relay_image_ready(void)
{
    if(relay_ensure_flash() == 0U)
    {
        ch340_puts("[relay] GD25 不在\r\n");
        return 0U;
    }
    if(relay_verify() == 0U)
    {
        return 0U;
    }

    ch340_puts("[relay] 中转区有可用镜像, target=");
    ch340_put_hex32(relay_header.target_slot);
    ch340_puts(" len=");
    ch340_put_dec(relay_header.application_length);
    ch340_puts("\r\n");
    return 1U;
}

uint8_t boot_relay_copy_to_slot(uint32_t target_slot)
{
    uint32_t total = OTA_IMAGE_HEADER_SIZE + relay_header.application_length;
    uint32_t addr = 0U;
    uint32_t page;

    /*
        先擦整个槽。Flash 只能把 1 写成 0 —— 不擦就写，新数据会和旧数据
        按位与，得到的既不是新也不是旧，而且 fmc_word_program 还会返回成功。
    */
    fmc_unlock();       /* 返回 void —— 它不会失败，只有擦/写会 */
    for(page = 0U; page < (OTA_SLOT_SIZE / FMC_PAGE_SIZE); page++)
    {
        if(fmc_page_erase(target_slot + (page * FMC_PAGE_SIZE)) != FMC_READY)
        {
            (void)fmc_lock();
            return 0U;
        }
    }

    /* 再分页搬：读一块外置 Flash → 按字写进内部 Flash。 */
    while(addr < total)
    {
        uint16_t chunk = (uint16_t)((total - addr) > RELAY_READ_CHUNK
                                        ? RELAY_READ_CHUNK
                                        : (total - addr));
        uint16_t i;

        if(relay_read(addr, relay_chunk, chunk) == 0U)
        {
            (void)fmc_lock();
            return 0U;
        }

        /* 末尾那一块可能不是 4 的倍数，补成 0xFF（擦除后的值）再按字写。 */
        if((chunk % 4U) != 0U)
        {
            uint16_t padded = (uint16_t)(chunk + (4U - (chunk % 4U)));
            (void)memset(&relay_chunk[chunk], 0xFF, (uint32_t)(padded - chunk));
            chunk = padded;
        }

        for(i = 0U; i < chunk; i += 4U)
        {
            uint32_t word;

            (void)memcpy(&word, &relay_chunk[i], 4U);
            if(fmc_word_program(target_slot + addr + (uint32_t)i, word) != FMC_READY)
            {
                (void)fmc_lock();
                return 0U;
            }
        }

        addr += (uint32_t)chunk;
    }

    (void)fmc_lock();
    return 1U;
}

uint8_t boot_relay_install(ota_metadata_t *meta)
{
    uint32_t target;

    if(meta == 0)
    {
        return 0U;
    }

    if(boot_relay_image_ready() == 0U)
    {
        return 0U;
    }

    /*
        目标槽由镜像头说了算 —— 它自带 target_slot，而打包工具保证它和
        二进制的链接地址一致（Reset_Handler 那条检查在上面已经验过了）。
    */
    target = relay_header.target_slot;

    /*
        【硬规矩】不能往活动槽里写。
        那会把唯一一份能跑的固件覆盖掉，A/B 存在的意义（出问题能退回去）当场作废。
        校验通过不代表"装这里安全"—— 这两件事无关。
    */
    if(target == meta->active_slot)
    {
        ch340_puts("[relay] 拒绝: 目标槽就是活动槽, 会覆盖正在跑的固件\r\n");
        return 0U;
    }

    ch340_puts("[relay] 搬运开始 -> ");
    ch340_put_hex32(target);
    ch340_puts("\r\n");

    if(boot_relay_copy_to_slot(target) == 0U)
    {
        ch340_puts("[relay] 搬运失败\r\n");
        return 0U;
    }

    /*
        搬完才算数。顺序不能颠倒：
        搬 → 写 pending_slot → 最后作废镜像头。
        反过来（先作废）中途掉电，"要装"的记录没了，货却还躺在中转区。
    */
    meta->pending_slot = target;
    meta->boot_attempts = 0U;
    if(ota_metadata_save(meta) != OTA_METADATA_OK)
    {
        ch340_puts("[relay] 写 pending 失败\r\n");
        return 0U;
    }

    /*
        作废镜像头：把 magic 那 4 个字节清成 0。
        少了这一步，下次上电又会读到有效镜像头、又搬一遍 —— 在两个槽之间无限弹。
    */
    if(gd25_invalidate(OTA_RELAY_BASE_ADDRESS, 4U) == 0U)
    {
        /* 便条没撕掉：下次上电会重搬一遍（幂等，浪费但无害）。不能因为这一步失败就
           把已经成功的搬运判为失败 —— 那会让 pending 与我们刚写的记录不一致。 */
        ch340_puts("[relay] 警告: 镜像头作废失败, 下次上电会重搬\r\n");
    }

    ch340_puts("[relay] 搬运完成\r\n");
    return 1U;
}
