#ifndef BOOTLOADER_GD25_H_
#define BOOTLOADER_GD25_H_

#include <stdint.h>

/*
    Bootloader 版的 GD25Q32 驱动。

    它和应用的 Drivers/BSP/GD25Q32/gd25.c 是【两个东西】，不是同一份代码的副本：

        应用：  读 + 写 + 擦；SPI0 和 BMP280 共享；多任务访问，要 FreeRTOS 互斥量
        Boot：  只读 + 一次"作废"；SPI0 上只有它自己；裸机单线程，没有锁

    差异足够大，所以分成两份实现。共用的只有"线序"—— 那部分两边都照着数据手册写，
    改一处记得看另一处。

    ⚠️ 包含守卫故意叫 BOOTLOADER_GD25_H_，不是 GD25_H_：
       Common/ExFlash/external_flash_layout.h 会 include 应用那份 "GD25Q32/gd25.h"
       （它要用 GD25Q32_CAPACITY_BYTES）。两边守卫同名的话，先被包含的那个会把另一个
       整个挡掉，boot_relay.c 就拿不到下面的声明了。
       好在 gd25_init / gd25_read 在两个头里的签名完全相同，同时包含也不冲突。
*/

#define BOOT_GD25_CAPACITY_BYTES 0x00400000UL   /* 4 MiB */
#define BOOT_GD25_PAGE_SIZE      256U           /* 一次写不能跨页；读没有这个限制 */

/*
    上电后调用一次：初始化 SPI0（GPIO + SPI 外设）+ 探测芯片型号。
    返回 1 = 芯片认出来了。

    Bootloader 里 SPI0 上只挂 GD25 一颗芯片，所以"总线初始化"直接并进这里，
    不再单独做一层 spi0_bus —— 那一层存在的理由是"多设备共享"，这里没有共享。
*/
uint8_t gd25_init(void);

/* 从 address 读 length 字节。返回 1 = 成功。 */
uint8_t gd25_read(uint32_t address, uint8_t *data, uint32_t length);

/*
    把 address 起的 length 个字节清成 0 —— 用来把镜像头的 magic 作废，
    表示"这份镜像我处理过了"，下次上电就不会再搬一遍。

    为什么用"清 0"而不是"擦除"：Flash 只能把 1 写成 0。magic 现在是非 0 值，
    往上面写 0 一定盖得掉，效果等同于作废，而且不用等一次扇区擦除（45ms）。
    对已经写过的字节再写一次是安全的（按位与），所以这个操作可重复做。

    调用者保证 [address, address + length) 不跨 256 字节页。
*/
uint8_t gd25_invalidate(uint32_t address, uint16_t length);

#endif
