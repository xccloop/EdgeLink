ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
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
}
