#ifndef OTA_IMAGE_H_
#define OTA_IMAGE_H_

/*
    这个文件我们来定义一下A/B分区镜像头包含的信息有有哪些，这也是双重校验的一部分
    我们考虑如下：

    字段	用途
    magic	固定标识，例如 OTA1；判断该槽不是空白 Flash 或普通数据。
    header_version	镜像头格式版本；以后字段变化时，旧 Bootloader 可以拒绝不认识的格式。
    header_size	当前应为 256；防止读取到错误格式。
    hardware_model	固件适用的 Node 硬件型号。
    firmware_version	固件版本号，例如 V7；用于记录和升级策略。
    application_length	应用实际字节数，不包含 256 字节镜像头。
    application_crc32	应用内容的 CRC32；校验 CAN 接收、Flash 写入后有没有损坏。
    header_crc32	镜像头自身的 CRC；防止长度、版本等元数据损坏。
    reserved[]	填满到 256 字节，留给后续字段。
*/

#include <stdint.h>
#include "flash_layout.h"

#define OTA_IMAGE_MAGIC 0x3141544FUL //OTA1
#define OTA_IMAGE_FORMAT_VERSION   1U

/*
    应用体最小长度：至少 8 字节，向量表前两项（MSP 初值 + Reset_Handler）得放得下。
    ota.c（接收侧校验）和 bootloader 的 image_verify.c（搬运前校验）共用这一个数，
    避免两处判据分叉 —— 分叉的后果就是「设备说好、bootloader 说坏」。
*/
#define OTA_APPLICATION_MIN_LENGTH 8U

#define OTA_IMAGE_HEADER_DATA_SIZE 252U //像头里“数据部分”占 252 字节。为什么是 252 而不是 256？因为最后 4 字节要留给 header_crc32。
#define OTA_IMAGE_HEADER_FIXED_SIZE 24U//结构体里固定字段的字节数。
#define OTA_IMAGE_RESERVED_SIZE \
    (OTA_IMAGE_HEADER_DATA_SIZE - OTA_IMAGE_HEADER_FIXED_SIZE) //空闲字段的大小

#define OTA_IMAGE_HEADER_CRC_OFFSET  0U
#define OTA_IMAGE_HEADER_CRC_LENGTH  OTA_IMAGE_HEADER_DATA_SIZE

#define OTA_APPLICATION_OFFSET       OTA_IMAGE_HEADER_SIZE

#define OTA_HARDWARE_MODEL_EDGENODE_V1  0x00000001UL
#define OTA_CURRENT_HARDWARE_MODEL      OTA_HARDWARE_MODEL_EDGENODE_V1
typedef struct
{
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;

    uint32_t hardware_model;
    uint32_t firmware_version;

    uint32_t application_length;
    uint32_t application_crc32;

    uint8_t reserved[OTA_IMAGE_RESERVED_SIZE];

    uint32_t header_crc32;
} ota_image_header_t;

//在编译阶段验证条件是否成立，如果为假，输出：OTA image header size must be 256 bytes
_Static_assert(sizeof(ota_image_header_t) == OTA_IMAGE_HEADER_SIZE,
               "OTA image header size must be 256 bytes");

#endif
