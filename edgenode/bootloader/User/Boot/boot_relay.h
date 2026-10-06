#ifndef BOOT_RELAY_H_
#define BOOT_RELAY_H_

#include <stdint.h>
#include "ota_metadata.h"

/*
    把外置 Flash 中转区里那份镜像，搬进内部 Flash 的某个槽。

    为什么单独一个模块，不复用 Driver/Core 里的 image_verify：
        image_verify 直接解引用 slot_base，那是【内存映射】的读法 ——
        内部 Flash 在地址空间里，外部 SPI Flash 不在。

        中转区在 GD25 上，数据必须经 gd25_read 搬进 RAM 才能看，
        读法完全不同；而且它还没搬过去，Reset_Handler 的"住在哪个区间"
        只能按【它声明的目标槽】算。

        所以这里自带一份"读 + 校验"。

    ⚠️ 校验判据必须和 Driver/Core/Src/image_verify.c 逐条对应
       （magic / header_version / header_size / header_crc32 / hardware_model /
         application_length 上下界 / application_crc32 / Reset_Handler 范围）。
       改了一边记得看另一边 —— 两处判据分叉，就是"bootloader 搬得进去、
       启动时装不上"或者反过来这种最难查的问题。
*/

/*
    中转区里有没有一份完整、可用的镜像？
    返回 1 = 有。顺带把 GD25 初始化了（只做一次）。
*/
uint8_t boot_relay_image_ready(void);

/*
    把中转区的镜像搬进 target_slot（先擦整个槽，再分页搬）。
    返回 1 = 搬完。不碰元数据 —— 那是调用者的事。
*/
uint8_t boot_relay_copy_to_slot(uint32_t target_slot);

/*
    对外唯一入口：有货就搬。

    顺序（不能颠倒）：
        读镜像头 + 校验 → 挑目标槽 → 擦槽 → 搬 → 写 pending_slot → 作废镜像头
    反过来（先作废）中途掉电，"要装"的记录没了，货却还躺在中转区。

    会就地修改 meta（写 pending_slot / boot_attempts）。
    返回 1 = 这次真的装了一份；0 = 没有待装镜像，或中途失败（已放弃）。
*/
uint8_t boot_relay_install(ota_metadata_t *meta);

#endif
