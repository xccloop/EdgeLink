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
    target_slot	这个镜像链给哪个槽（A 或 B 的槽基地址）。
    application_length	应用实际字节数，不包含 256 字节镜像头。
    application_crc32	应用内容的 CRC32；校验 CAN 接收、Flash 写入后有没有损坏。
    header_crc32	镜像头自身的 CRC；防止长度、版本等元数据损坏。
    reserved[]	填满到 256 字节，留给后续字段。

    target_slot 的定位（2026-10-05 加）——它【不决定】目标槽，只做交叉校验：

        决定方：元数据（active_slot）。目标槽 = 非活动槽 —— 这条是硬规则，
                因为往活动槽写会把正在跑的固件覆盖掉。
        参考方：本字段。它说的是"这个二进制编译时是按哪个槽链的"。

        搬运动作：先由元数据挑出目标槽，再拿本字段对一遍。
                 对不上就拒绝搬运 —— 说明 Hub 和设备的认知脱节了
                 （Hub 以为设备在 A，其实设备在 B，于是发错了一版）。

        为什么不能反过来信本字段：Hub 的记录可能是过时的，而元数据是设备
        自己写、自己读的。把决定权交给一个外来值，等于把"覆盖掉正在跑的
        固件"这种事的开关交出去。

    顺带说明它为什么不是多余的：Reset_Handler 的范围检查能【推出】一个镜像
    属于哪个槽（XIP，链接地址=运行地址），但那是间接的、要读完整个应用才知道。
    本字段是构建时就绑好的、一眼可见的标签，用来早一步发现"发错版本"。
*/

#include <stdint.h>
#include <stddef.h>          /* offsetof：下面用来钉死字段偏移 */
#include "flash_layout.h"

#define OTA_IMAGE_MAGIC 0x3141544FUL //OTA1

/*
    镜像头加了 target_slot（占用原来 reserved 的 4 字节）。

    版本号【故意保持在 1】：字段是从 reserved 里挪出来的，头和总长都没变。
    代价是"老镜像的 target_slot 读出来是 0"—— 这不影响 A/B 槽的启动路径
    （那条路没人读这个字段），只有搬运路径会因为它不是合法槽基址而拒绝，
    而那种情况本来也不该被当成有效镜像。

    ⚠️ 改布局（字段顺序、结构体长度）时记得同步 tools/make_ota_image.py，
       并考虑是否该把版本号抬上去。
*/
#define OTA_IMAGE_FORMAT_VERSION   1U

/*
    应用体最小长度：至少 8 字节，向量表前两项（MSP 初值 + Reset_Handler）得放得下。
    ota.c（接收侧校验）和 bootloader 的 image_verify.c（搬运前校验）共用这一个数，
    避免两处判据分叉 —— 分叉的后果就是「设备说好、bootloader 说坏」。
*/
#define OTA_APPLICATION_MIN_LENGTH 8U

#define OTA_IMAGE_HEADER_DATA_SIZE 252U //像头里“数据部分”占 252 字节。为什么是 252 而不是 256？因为最后 4 字节要留给 header_crc32。
#define OTA_IMAGE_HEADER_FIXED_SIZE 28U//结构体里固定字段的字节数（加了 target_slot：24 → 28）。
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
    uint32_t target_slot;       /* OTA_SLOT_A_BASE_ADDRESS 或 OTA_SLOT_B_BASE_ADDRESS */

    uint32_t application_length;
    uint32_t application_crc32;

    uint8_t reserved[OTA_IMAGE_RESERVED_SIZE];

    uint32_t header_crc32;
} ota_image_header_t;

//在编译阶段验证条件是否成立，如果为假，输出：OTA image header size must be 256 bytes
_Static_assert(sizeof(ota_image_header_t) == OTA_IMAGE_HEADER_SIZE,
               "OTA image header size must be 256 bytes");

/*
    偏移是【两边共用的契约】：tools/make_ota_image.py 按同样的偏移打包。
    把关键字段的位置钉死 —— 以后谁调了字段顺序，这里直接编译报错，
    而不是等到设备上 Bootloader 说一句"镜像无效"，然后你对着两边的代码找差异。
*/
_Static_assert(offsetof(ota_image_header_t, firmware_version)    == 12U, "firmware_version 偏移变了");
_Static_assert(offsetof(ota_image_header_t, target_slot)         == 16U, "target_slot 偏移变了");
_Static_assert(offsetof(ota_image_header_t, application_length)  == 20U, "application_length 偏移变了");
_Static_assert(offsetof(ota_image_header_t, application_crc32)   == 24U, "application_crc32 偏移变了");
_Static_assert(offsetof(ota_image_header_t, header_crc32)        == 252U, "header_crc32 偏移变了");

#endif
