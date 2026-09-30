PROJECT := bootloader
BUILD_DIR := build

TOOLCHAIN := C:/Program Files (x86)/GNU Tools ARM Embedded/5.4 2016q3/bin
CC := "$(TOOLCHAIN)/arm-none-eabi-gcc.exe"
OBJCOPY := "$(TOOLCHAIN)/arm-none-eabi-objcopy.exe"
SIZE := "$(TOOLCHAIN)/arm-none-eabi-size.exe"

MCU := -mcpu=cortex-m3 -mthumb
DEFS := -DGD32F10X_HD -DUSE_STDPERIPH_DRIVER
INCLUDES := -IUser -IDriver/Core/Inc -I../Common/OTA -I../User -I../Drivers/CMSIS/Include -I../Drivers/CMSIS/Device/GD/GD32F10x/Include -I../Drivers/GD32F10x_Standard_Peripheral/Inc
CFLAGS := $(MCU) $(DEFS) $(INCLUDES) -std=c11 -g3 -Og -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS := $(MCU) -TOCD/linker/gd32f103rct6_bootloader.ld -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map -specs=nano.specs -specs=nosys.specs

OBJECTS := $(BUILD_DIR)/main.o \
           $(BUILD_DIR)/boot_jump.o \
           $(BUILD_DIR)/system_gd32f10x.o \
           $(BUILD_DIR)/gd32f10x_misc.o \
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
