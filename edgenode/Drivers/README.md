# Drivers layout

This layout follows the reference HAL example's `Drivers` organization while
using the GD32F103RCT6-supported library:

- `CMSIS/Include`: ARM Cortex-M3 core headers.
- `CMSIS/Device/GD/GD32F10x`: GD32 device headers and `system_gd32f10x.c`.
- `GD32F10x_Standard_Peripheral`: GD32 official standard peripheral driver.
- `BSP`: board-specific drivers to add later, such as LED or display drivers.
- `SYSTEM`: board-independent support modules to add later, such as delay.

GD32F103 does not use STM32Cube HAL. Do not copy `STM32F1xx_HAL_Driver` files
into this project: their device definitions and APIs target STM32, not GD32.
