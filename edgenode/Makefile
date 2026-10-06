PROJECT := edegnode

# 默认编译三种布局；SLOT=plain/a/b 可单独编译一种。
#   plain —— 老的 0x08000000 布局，Bootloader 还没接管时用
#   a / b —— A / B 槽应用：分别链接到各自槽内地址，并给 SystemInit 传
#            对应的 VTOR 偏移，否则应用启动时会把向量表指回 0x08000000
#            （也就是 Bootloader 的向量表），第一个中断就跑飞了。
SLOT ?= all

ifeq ($(SLOT),a)
BUILD_DIR := build/a
LDSCRIPT  := OCD/linker/gd32f103rct6_slot_a.ld
VECT_DEF  := -DVECT_TAB_OFFSET=0x4100U
else ifeq ($(SLOT),b)
BUILD_DIR := build/b
LDSCRIPT  := OCD/linker/gd32f103rct6_slot_b.ld
VECT_DEF  := -DVECT_TAB_OFFSET=0x21900U
else ifeq ($(SLOT),plain)
BUILD_DIR := build/default
LDSCRIPT  := OCD/linker/gd32f103rct6.ld
VECT_DEF  :=
else ifeq ($(SLOT),all)
BUILD_DIR := build/default
LDSCRIPT  := OCD/linker/gd32f103rct6.ld
VECT_DEF  :=
else
$(error SLOT must be all, plain, a or b)
endif

# 写进镜像头的固件版本号
IMAGE_VERSION ?= 1

TOOLCHAIN := C:/Program Files (x86)/GNU Tools ARM Embedded/5.4 2016q3/bin
CC := "$(TOOLCHAIN)/arm-none-eabi-gcc.exe"
OBJCOPY := "$(TOOLCHAIN)/arm-none-eabi-objcopy.exe"
SIZE := "$(TOOLCHAIN)/arm-none-eabi-size.exe"

MCU := -mcpu=cortex-m3 -mthumb
DEFS := -DGD32F10X_HD -DUSE_STDPERIPH_DRIVER $(VECT_DEF)
INCLUDES := -IUser -IUser/App -ICommon -ICommon/CRC -IDrivers/BSP -IDrivers/BOARD -IDrivers/CMSIS/Include -IDrivers/CMSIS/Device/GD/GD32F10x/Include -IDrivers/GD32F10x_Standard_Peripheral/Inc -IThirdParty/FreeRTOS-Kernel/include -IThirdParty/FreeRTOS-Kernel/portable/GCC/ARM_CM3
CFLAGS := $(MCU) $(DEFS) $(INCLUDES) -std=c11 -g3 -Og -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS := $(MCU) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map -specs=nano.specs -specs=nosys.specs

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
FREERTOS_SOURCES := ThirdParty/FreeRTOS-Kernel/tasks.c \
                    ThirdParty/FreeRTOS-Kernel/queue.c \
                    ThirdParty/FreeRTOS-Kernel/list.c \
                    ThirdParty/FreeRTOS-Kernel/portable/GCC/ARM_CM3/port.c
# Common/OTA/ota_metadata.c：应用侧销账要写元数据（ota_confirm_boot）。
# gd32f10x_fmc.c：内部 Flash 的擦/写，只有元数据写入用到。
SOURCES := $(wildcard User/*.c) $(APP_SOURCES) $(BSP_SOURCES) $(BOARD_SOURCES) $(FREERTOS_SOURCES) Common/CRC/crc32.c Common/IPS/ips.c Common/Hmi/hmi_widget.c Common/OTA/ota_metadata.c Drivers/GD32F10x_Standard_Peripheral/Src/gd32f10x_fmc.c Drivers/CMSIS/Device/GD/GD32F10x/Source/Templates/system_gd32f10x.c $(DRIVER_SOURCES)
STARTUP := OCD/startup/startup_gd32f10x_hd.s
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES)) $(BUILD_DIR)/$(STARTUP:.s=.o)

.PHONY: all firmware clean image FORCE

ifeq ($(SLOT),all)
all:
	$(MAKE) SLOT=plain firmware
	$(MAKE) SLOT=a firmware
	$(MAKE) SLOT=b firmware
else
all: firmware
endif

firmware: $(BUILD_DIR)/$(PROJECT).elf $(BUILD_DIR)/$(PROJECT).bin
ifneq ($(filter $(SLOT),a b),)
firmware: image
endif

# 带镜像头的可烧镜像。只有 SLOT=a / SLOT=b 时才该用这个目标 ——
# plain 布局下加了头反而烧不了。
ifeq ($(filter $(SLOT),a b),)
image:
	@echo Image requires SLOT=a or SLOT=b
	@exit 1
else
image: $(BUILD_DIR)/$(PROJECT)_image.bin
endif

FORCE:

$(BUILD_DIR)/$(PROJECT)_image.bin: $(BUILD_DIR)/$(PROJECT).bin tools/make_ota_image.py FORCE
	python tools/make_ota_image.py --input $(BUILD_DIR)/$(PROJECT).bin --output $@ --version $(IMAGE_VERSION) --slot $(SLOT)

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
ifeq ($(SLOT),all)
	@if exist "build" rmdir /S /Q "build"
else
	@if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
endif

-include $(OBJECTS:.o=.d)
