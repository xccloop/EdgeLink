# GD32F103RCT6 VS Code workspace

This workspace is prepared for a GD32F103RCT6 with ST-Link SWD and OpenOCD.
No application or peripheral code has been generated.

## Before the first build

1. Add your `main.c` under `User/`.

The official GD32F10x CMSIS and standard-peripheral library files are already
arranged under `Drivers/`. This is GD32's supported driver library, rather than
STM32Cube HAL; STM32 HAL source must not be mixed into this GD32 project.

`DRIVER_SOURCES` already contains the RCU and MISC modules required by the
system clock/vector initialization. When another peripheral is used, include
its header in the matching source file and add only its matching
`Drivers/GD32F10x_Standard_Peripheral/Src/*.c` file to `DRIVER_SOURCES`.

Place board drivers in any subdirectory under `Drivers/BSP/`, for example
`Drivers/BSP/LED/led.c`. The Makefile recursively discovers BSP `.c` files;
the `-IDrivers/BSP` compiler and clangd path let application files include
their headers by a path such as `LED/led.h`.

## VS Code commands

- `Ctrl+Shift+B`: build the ELF/BIN into `build/`.
- **Tasks: Run Task** → `Flash GD32F103RCT6 (OpenOCD)`: build, program, verify,
  and reset through ST-Link SWD.
- `F5`: build, start OpenOCD, download the ELF through GDB, and stop at reset.

`openocd.cfg` uses the local ST-Link OpenOCD interface at 1 MHz and works with
SWDIO, SWCLK, GND and 3V3. If you also wire ST-Link NRST to the MCU NRST pin,
the optional reset line in that file can be enabled. Change only the interface
section if your probe is not an ST-Link.
