ENTRY(Reset_Handler)

/*
    A 槽应用链接脚本。

    和 gd32f103rct6.ld（从 0x08000000 起的老布局）的区别只有 FLASH 起点：

        槽 A 基址        0x08004000   ← 镜像头住这里（前 256 字节）
        应用入口         0x08004100   ← 本脚本从这里开始放代码

    为什么是 +0x100 而不是 +256：OTA_APPLICATION_OFFSET = OTA_IMAGE_HEADER_SIZE
    = 256 = 0x100，定义在 Common/OTA/ota_image.h 里。

    链接到这里还不够 —— 还得给应用传 -DVECT_TAB_OFFSET=0x4100U，
    否则应用启动时 SystemInit() 会把 VTOR 重新指回 0x08000000
    （也就是 Bootloader 的向量表），第一个中断就跑到 Bootloader 里去。

    容量：槽一共 118K，头占掉 256 字节，剩下的 117K 才是应用的可用空间。
*/
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08004100, LENGTH = 117K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 48K
}

__stack = ORIGIN(RAM) + LENGTH(RAM);
_estack = __stack;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.vectors))
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text*)
    *(.rodata*)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH

  __data_load = LOADADDR(.data);
  _sidata = __data_load;
  .data :
  {
    . = ALIGN(4);
    __data_start = .;
    _sdata = __data_start;
    *(.data*)
    . = ALIGN(4);
    __data_end = .;
    _edata = __data_end;
  } > RAM AT > FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    __bss_start = .;
    _sbss = __bss_start;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
    _ebss = __bss_end;
  } > RAM

  . = ALIGN(8);
  _end = .;
  end = _end;
}
