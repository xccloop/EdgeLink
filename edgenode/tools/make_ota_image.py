#!/usr/bin/env python3
"""
把纯应用 .bin 打包成 Bootloader 认识的"带头镜像"。

为什么需要这一步
----------------
objcopy 出来的 .bin 只是一段裸机器码。而 Bootloader 的 image_verify()
读的是槽开头的 256 字节镜像头（magic / 版本 / 硬件型号 / 长度 / CRC）。
所以烧进槽之前，必须先把头拼到前面。

字段布局必须和 Common/OTA/ota_image.h 完全一致 —— 差一个字节，
Bootloader 就会判定镜像非法（而且它只会说"无效"，不会告诉你哪里不对）。

CRC 说明
--------
crc32.c 用的就是标准 CRC-32：初值 0xFFFFFFFF、反射多项式 0xEDB88320、
末尾异或 0xFFFFFFFF。这正是 zlib.crc32 算的东西，所以两边能对上。

用法
----
    python tools/make_ota_image.py \\
        --input  build_slot_a/edegnode.bin \\
        --output build_slot_a/edegnode_image.bin \\
        --version 1
"""

import argparse
import struct
import sys
import zlib

# ── 必须和 Common/OTA/ota_image.h 保持一致 ──
OTA_IMAGE_MAGIC = 0x3141544F          # "OTA1"；小端存进去后字节是 4F 54 41 31
OTA_IMAGE_FORMAT_VERSION = 1
OTA_IMAGE_HEADER_SIZE = 256
OTA_HARDWARE_MODEL_EDGENODE_V1 = 1

# 头部里前 252 字节参与 CRC，最后 4 字节（header_crc32）自己不参与。
# 对应 C 里的 OTA_IMAGE_HEADER_CRC_LENGTH。
OTA_HEADER_CRC_LENGTH = 252

# 槽容量，用来挡住"应用太大塞不下"这种情况。
OTA_SLOT_SIZE = 118 * 1024

# SRAM 范围（GD32F103RCT6 是 48KB），用来做向量表自检。
SRAM_BASE = 0x20000000
SRAM_END = 0x2000C000

# Flash 起点。低于这个地址就是 Bootloader 自己的地盘，应用绝不该链到那里。
BOOTLOADER_END = 0x08004000
FLASH_END = 0x08040000


def sanity_check(application):
    """
    在主机上先替 Bootloader 把能查的都查一遍。

    这些检查其实 image_verify() 也会做，但在主机上失败只是"打包报错"，
    在设备上失败就是"设备卡住、屏幕报错、你得拆下来重烧"。同一个问题，
    早一步发现便宜得多。
    """
    if len(application) < 8:
        raise ValueError("文件太短，连向量表前两项都装不下")

    msp, reset_handler = struct.unpack("<II", application[:8])

    if not (SRAM_BASE <= msp <= SRAM_END):
        raise ValueError(
            "向量表第 0 项不像主栈指针：0x%08X（应落在 0x%08X~0x%08X）"
            % (msp, SRAM_BASE, SRAM_END)
        )

    if (reset_handler & 1) == 0:
        raise ValueError("向量表第 1 项缺少 Thumb 位：0x%08X" % reset_handler)

    reset_address = reset_handler & ~1

    if not (BOOTLOADER_END <= reset_address < FLASH_END):
        raise ValueError(
            "Reset_Handler 落在 0x%08X，不在任何应用槽里。\n"
            "        多半是链接脚本没换 —— 应用还是按 0x08000000 链的，\n"
            "        那样它一上电就会覆盖掉 Bootloader。" % reset_address
        )

    return msp, reset_address


def build_header(application, firmware_version):
    """按 ota_image_header_t 的内存布局拼出 256 字节头。"""

    # '<' = 小端、不加对齐填充，必须和 C 结构体在 Cortex-M3 上的实际布局一致。
    #   I  magic                  (4)    偏移 0
    #   H  header_version         (2)    偏移 4
    #   H  header_size            (2)    偏移 6
    #   I  hardware_model         (4)    偏移 8
    #   I  firmware_version       (4)    偏移 12
    #   I  application_length     (4)    偏移 16
    #   I  application_crc32      (4)    偏移 20
    #                                   ── 固定字段共 24 字节
    fixed = struct.pack(
        "<IHHIIII",
        OTA_IMAGE_MAGIC,
        OTA_IMAGE_FORMAT_VERSION,
        OTA_IMAGE_HEADER_SIZE,
        OTA_HARDWARE_MODEL_EDGENODE_V1,
        firmware_version,
        len(application),
        zlib.crc32(application) & 0xFFFFFFFF,
    )

    # reserved 补齐到 252 字节。Bootloader 不检查这段，填 0 就行。
    body = fixed + b"\x00" * (OTA_HEADER_CRC_LENGTH - len(fixed))

    # header_crc32 占最后 4 字节，自己【不】参与计算。
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def main():
    parser = argparse.ArgumentParser(
        description="给应用 .bin 加上 OTA 镜像头，产出可直接烧进槽的镜像"
    )
    parser.add_argument("--input", required=True, help="纯应用 .bin（objcopy 产物）")
    parser.add_argument("--output", required=True, help="输出的带头镜像")
    parser.add_argument("--version", type=int, required=True,
                        help="固件版本号，写进镜像头的 firmware_version")
    args = parser.parse_args()

    with open(args.input, "rb") as handle:
        application = handle.read()

    if not application:
        print("错误：输入文件是空的", file=sys.stderr)
        return 1

    if len(application) > (OTA_SLOT_SIZE - OTA_IMAGE_HEADER_SIZE):
        print(
            "错误：应用 %d 字节，加头之后超过槽容量 %d 字节"
            % (len(application) + OTA_IMAGE_HEADER_SIZE, OTA_SLOT_SIZE),
            file=sys.stderr,
        )
        return 1

    msp, reset_address = sanity_check(application)
    header = build_header(application, args.version)

    with open(args.output, "wb") as handle:
        handle.write(header)
        handle.write(application)

    print("  应用      %d 字节" % len(application))
    print("  向量表    MSP = 0x%08X   Reset_Handler = 0x%08X" % (msp, reset_address))
    print("  版本      v%d" % args.version)
    print("  镜像头    %d 字节（CRC32 = 0x%08X）"
          % (len(header), struct.unpack("<I", header[-4:])[0]))
    print("  输出      %s  共 %d 字节" % (args.output, len(header) + len(application)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
