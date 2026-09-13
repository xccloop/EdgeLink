PROJECT := edegnode
BUILD_DIR := build

TOOLCHAIN := C:/Program Files (x86)/GNU Tools ARM Embedded/5.4 2016q3/bin
CC := "$(TOOLCHAIN)/arm-none-eabi-gcc.exe"
OBJCOPY := "$(TOOLCHAIN)/arm-none-eabi-objcopy.exe"
SIZE := "$(TOOLCHAIN)/arm-none-eabi-size.exe"

MCU := -mcpu=cortex-m3 -mthumb
DEFS := -DGD32F10X_HD -DUSE_STDPERIPH_DRIVER
INCLUDES := -IUser -IUser/App -IDrivers/BSP -IDrivers/BSP/IPS -IDrivers/BOARD -IDrivers/CMSIS/Include -IDrivers/CMSIS/Device/GD/GD32F10x/Include -IDrivers/GD32F10x_Standard_Peripheral/Inc
CFLAGS := $(MCU) $(DEFS) $(INCLUDES) -std=c11 -g3 -Og -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS := $(MCU) -TOCD/linker/gd32f103rct6.ld -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map -specs=nano.specs -specs=nosys.specs

DRIVER_SOURCES := Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_rcu.c \
                  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_misc.c \
				  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_dbg.c \
				  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_gpio.c \
				  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_exti.c \
				  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_usart.c \
			  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_can.c \
			  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_adc.c \
			  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_spi.c \
				  Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_dma.c
BSP_SOURCES := $(shell powershell -NoProfile -Command "$$root = (Get-Location).Path; Get-ChildItem -LiteralPath Drivers/BSP -Filter *.c -Recurse -File | ForEach-Object { $$_.FullName.Substring($$root.Length + 1).Replace('\','/') }")
APP_SOURCES := $(shell powershell -NoProfile -Command "$$root = (Get-Location).Path; Get-ChildItem -LiteralPath User/App -Filter *.c -Recurse -File | ForEach-Object { $$_.FullName.Substring($$root.Length + 1).Replace('\','/') }")
BOARD_SOURCES := $(wildcard Drivers/BOARD/*.c)
SOURCES := $(wildcard User/*.c) $(APP_SOURCES) $(BSP_SOURCES) $(BOARD_SOURCES) Drivers/CMSIS/Device/GD/GD32F10x/Source/Templates/system_gd32f10x.c $(DRIVER_SOURCES)
STARTUP := OCD/startup/startup_gd32f10x_hd.s
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES)) $(BUILD_DIR)/$(STARTUP:.s=.o)

.PHONY: all clean

all: $(BUILD_DIR)/$(PROJECT).elf $(BUILD_DIR)/$(PROJECT).bin

$(BUILD_DIR)/$(PROJECT).elf: $(OBJECTS)
	@if not exist "$(@D)" mkdir "$(@D)"
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/%.o: %.c
	@if not exist "$(@D)" mkdir "$(@D)"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@if not exist "$(@D)" mkdir "$(@D)"
	$(CC) $(MCU) -x assembler-with-cpp -MMD -MP -c $< -o $@

clean:
	@if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"

-include $(OBJECTS:.o=.d)
