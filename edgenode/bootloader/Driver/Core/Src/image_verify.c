#include "image_verify.h"
#include "flash_layout.h"
#include "ota_image.h"
#include "crc32.h"
#include <stdbool.h>
#include <stdint.h>

/*
    这三个是SRAM的区域范围
*/
#define MCU_SRAM_BASE_ADDRESS  0x20000000UL
#define MCU_SRAM_SIZE          (48UL * 1024UL)
#define MCU_SRAM_END_ADDRESS   \
    (MCU_SRAM_BASE_ADDRESS + MCU_SRAM_SIZE)

/*
    这个函数用于校验镜像头是否可用，slot_base为A,B分区首地址（不是应用地址）
*/
bool image_verify(uint32_t slot_base)
{
    if(slot_base != OTA_SLOT_A_BASE_ADDRESS && slot_base != OTA_SLOT_B_BASE_ADDRESS)
    {
        return false;
    }
    
    const ota_image_header_t *header;
    header = (const ota_image_header_t *)slot_base;//一样的将32位数字转化为地址

    if(header->magic != OTA_IMAGE_MAGIC)
    {
        return false;
    }

    //镜像头格式版本，固定v1
    if (header->header_version != OTA_IMAGE_FORMAT_VERSION)
    {
        return false;
    }

    //镜像头实际大小必须为 256 字节
    if (header->header_size != OTA_IMAGE_HEADER_SIZE)
    {
        return false;
    }

    //CRC校验
    if (crc32_check((const uint8_t *)header,OTA_IMAGE_HEADER_CRC_LENGTH,header->header_crc32) == 0U)
    {
        return false;
    }

    //检查model类型是否正确
    if (header->hardware_model != OTA_CURRENT_HARDWARE_MODEL)
    {
        return false;
    }

    //至少 8 B：需要容纳 MSP 和 Reset_Handler；不能超过 槽大小 - 256 B
    //这项检查必须在应用 CRC 之前完成。否则损坏的 application_length 可能让 Bootloader 把 CRC 算到槽外。
    if ((header->application_length < OTA_APPLICATION_MIN_LENGTH) ||
        (header->application_length >
         (OTA_SLOT_SIZE - OTA_APPLICATION_OFFSET)))
    {
        return false;
    }

    //CRC校验固件
    const uint8_t *payload;
    payload = (const uint8_t *)(slot_base + OTA_APPLICATION_OFFSET);
    if (crc32_check(payload,
                    header->application_length,
                    header->application_crc32) == 0U)
    {
        return false;
    }

    //获取栈顶指针+resethandle
    const uint32_t *vector_table;
    uint32_t app_msp;
    uint32_t app_reset_handler;
    vector_table = (const uint32_t*)payload;
    app_msp = vector_table[0];
    app_reset_handler = vector_table[1];

    //判断msp处于sram范围内并且按八字节对齐
    //一个地址要按 8 字节对齐，最低 3 位必须全为 0：如 0x00，0x08后三位都是0，0x07（第三位不是0否决）
    if(app_msp < MCU_SRAM_BASE_ADDRESS || app_msp > MCU_SRAM_END_ADDRESS || ((app_msp & 0x07) != 0U))
    {
        return false;
    }

    //Cortex-M3 只执行 Thumb 指令，因此函数地址最低位必须为 1
    if ((app_reset_handler & 1U) == 0U)
    {
        return false;
    }
    //前面我们说app_reset_handle最低位需要是1，比如0x40001,但是存放的地址是不包含这个1的，这个1是执行模式
    //我们使用&清除这个1获得地址
    //我们在boot_jump的时候没有去除这个1，因为他是跳转，跳转本身是需要那个1进行执行模式的选择的
    app_reset_handler &= ~1U;

    //这一步判断向量表里面的reset_handle是不是这个应用的
    //应用起点：slot_base + OTA_APPLICATION_OFFSET
    //应用终点：应用起点 + application_length
    if ((app_reset_handler <
        (slot_base + OTA_APPLICATION_OFFSET)) ||
        (app_reset_handler >=
        (slot_base + OTA_APPLICATION_OFFSET + header->application_length)))
    {
        return false;
    }
    return true;
}
