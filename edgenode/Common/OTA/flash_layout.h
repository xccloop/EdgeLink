#ifndef FLASH_LAYOUT_H_
#define FLASH_LAYOUT_H_


//此文件用于规范OTA升级中MCU内部flash分区

#include <stdint.h>

#define OTA_FLASH_BASE_ADDRESS       0x08000000UL
#define OTA_FLASH_TOTAL_SIZE         (256UL * 1024UL) //一共256kb

#define BOOTLOADER_BASE_ADDRESS      OTA_FLASH_BASE_ADDRESS
#define BOOTLOADER_SIZE              (16UL * 1024UL) //预留给bootloader程序16kb

#define OTA_IMAGE_HEADER_SIZE        256UL

#define OTA_SLOT_SIZE                (118UL * 1024UL) //一个槽位118kb

#define OTA_SLOT_A_BASE_ADDRESS      \
    (BOOTLOADER_BASE_ADDRESS + BOOTLOADER_SIZE) //A分区首地址

#define OTA_SLOT_B_BASE_ADDRESS      \
    (OTA_SLOT_A_BASE_ADDRESS + OTA_SLOT_SIZE)//B分区首地址

#define OTA_METADATA_BASE_ADDRESS    \
    (OTA_SLOT_B_BASE_ADDRESS + OTA_SLOT_SIZE)//OTA-FLAG分区首地址

#define OTA_METADATA_SIZE            (4UL * 1024UL) //OTA-FLAG占用flash大小

#define OTA_SLOT_A_VECTOR_ADDRESS    \
    (OTA_SLOT_A_BASE_ADDRESS + OTA_IMAGE_HEADER_SIZE)//A分区真正应用所在位置，前256字节是镜像头等校验

#define OTA_SLOT_B_VECTOR_ADDRESS    \
    (OTA_SLOT_B_BASE_ADDRESS + OTA_IMAGE_HEADER_SIZE)

#endif