ENTRY(Reset_Handler)

/*
    B 槽应用链接脚本。与 slot_a 脚本唯一的区别是 FLASH 起点。

        槽 B 基址        0x08021800   ← 镜像头住这里
        应用入口         0x08021900   ← 本脚本从这里开始放代码

    对应地，构建 B 槽时要传 -DVECT_TAB_OFFSET=0x21900U。

    两份镜像的代码完全一样，只是链接地址不同 —— 所以不能互相烧错槽：
    把 A 槽的镜像写进 B 槽，应用会跳到 0x08004100（槽 A），跑到别的固件上去。
    （这一点目前靠人工保证；等做 CAN 接收时要在镜像头里加"目标槽"字段来兜底。）
*/
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08021900, LENGTH = 117K
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
