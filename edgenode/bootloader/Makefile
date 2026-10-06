PROJECT := bootloader
BUILD_DIR := build

TOOLCHAIN := C:/Program Files (x86)/GNU Tools ARM Embedded/5.4 2016q3/bin
CC := "$(TOOLCHAIN)/arm-none-eabi-gcc.exe"
OBJCOPY := "$(TOOLCHAIN)/arm-none-eabi-objcopy.exe"
SIZE := "$(TOOLCHAIN)/arm-none-eabi-size.exe"

MCU := -mcpu=cortex-m3 -mthumb
DEFS := -DGD32F10X_HD -DUSE_STDPERIPH_DRIVER
# -I../Drivers/BSP 放在最后：它里面是 gd25.h 和一堆外设驱动，
# 一旦和 -IDriver/Bsp 里有同名头文件，必须让 Bootloader 自己那份赢。
INCLUDES := -IUser -IDriver/Core/Inc -IDriver/Bsp -I../Common -I../Common/OTA -I../Common/CRC -I../User -I../Drivers/CMSIS/Include -I../Drivers/CMSIS/Device/GD/GD32F10x/Include -I../Drivers/GD32F10x_Standard_Peripheral/Inc -I../Drivers/BSP
CFLAGS := $(MCU) $(DEFS) $(INCLUDES) -std=c11 -g3 -Og -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS := $(MCU) -TOCD/linker/gd32f103rct6_bootloader.ld -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map -specs=nano.specs -specs=nosys.specs

OBJECTS := $(BUILD_DIR)/main.o \
           $(BUILD_DIR)/boot_screen.o \
           $(BUILD_DIR)/hmi_render.o \
           $(BUILD_DIR)/hmi_widget.o \
           $(BUILD_DIR)/display_buffer.o \
           $(BUILD_DIR)/boot_jump.o \
           $(BUILD_DIR)/boot_relay.o \
           $(BUILD_DIR)/image_verify.o \
           $(BUILD_DIR)/ota_metadata.o \
           $(BUILD_DIR)/gd25.o \
           $(BUILD_DIR)/ch340.o \
           $(BUILD_DIR)/board_time.o \
           $(BUILD_DIR)/board_debug.o \
           $(BUILD_DIR)/ips.o \
           $(BUILD_DIR)/crc32.o \
           $(BUILD_DIR)/system_gd32f10x.o \
           $(BUILD_DIR)/gd32f10x_fmc.o \
           $(BUILD_DIR)/gd32f10x_misc.o \
           $(BUILD_DIR)/gd32f10x_gpio.o \
           $(BUILD_DIR)/gd32f10x_rcu.o \
           $(BUILD_DIR)/gd32f10x_usart.o \
           $(BUILD_DIR)/gd32f10x_spi.o \
           $(BUILD_DIR)/gd32f10x_dma.o \
           $(BUILD_DIR)/gd32f10x_dbg.o \
           $(BUILD_DIR)/startup_gd32f10x_hd.o

.PHONY: all clean

all: $(BUILD_DIR)/$(PROJECT).elf $(BUILD_DIR)/$(PROJECT).bin

$(BUILD_DIR)/$(PROJECT).elf: $(OBJECTS)
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/main.o: User/main.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/boot_jump.o: Driver/Core/Src/boot_jump.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/boot_relay.o: User/boot_relay.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/image_verify.o: Driver/Core/Src/image_verify.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ota_metadata.o: ../Common/OTA/ota_metadata.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd25.o: Driver/Bsp/gd25.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ch340.o: Driver/Bsp/CH340/ch340.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/crc32.o: ../Common/CRC/crc32.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_gpio.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_gpio.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_rcu.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_rcu.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_usart.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_usart.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_spi.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_spi.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_dma.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_dma.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_dbg.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_dbg.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/board_time.o: Driver/Bsp/board_time.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/board_debug.o: Driver/Bsp/board_debug.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ips.o: ../Common/IPS/ips.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/boot_screen.o: User/boot_screen.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/hmi_render.o: Driver/Bsp/Display/hmi_render.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/hmi_widget.o: ../Common/Hmi/hmi_widget.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/display_buffer.o: Driver/Bsp/Display/display_buffer.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_fmc.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_fmc.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/system_gd32f10x.o: ../Drivers/CMSIS/Device/GD/GD32F10x/Source/Templates/system_gd32f10x.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/gd32f10x_misc.o: ../Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_misc.c
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/startup_gd32f10x_hd.o: ../OCD/startup/startup_gd32f10x_hd.s
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	$(CC) $(MCU) -x assembler-with-cpp -MMD -MP -c $< -o $@

clean:
	@if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"

-include $(OBJECTS:.o=.d)
