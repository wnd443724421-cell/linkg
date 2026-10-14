# Independent Cortex-M0+ bare-metal build. No flashing targets.
CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size
BUILD ?= build/gcc
TARGET := $(BUILD)/linkg_mcu_v2
CPU := -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft
CPPFLAGS := -DUSE_HAL_DRIVER -DSTM32L031xx -ICore/Inc -Ibsp -Iservice -IDrivers/STM32L0xx_HAL_Driver/Inc -IDrivers/STM32L0xx_HAL_Driver/Inc/Legacy -IDrivers/CMSIS/Device/ST/STM32L0xx/Include -IDrivers/CMSIS/Include
CFLAGS := $(CPU) -std=c99 -Os -g3 -ffunction-sections -fdata-sections -fno-common -fstack-usage -Wall -Wextra -Werror -MMD -MP
LDFLAGS := $(CPU) -nostartfiles --specs=nano.specs --specs=nosys.specs -T GCC/stm32l031g6.ld -Wl,--gc-sections,-Map=$(TARGET).map,--cref -Wl,--print-memory-usage
SOURCES := Core/Src/main.c \
    Core/Src/stm32l0xx_hal_msp.c \
    Core/Src/stm32l0xx_it.c \
    Core/Src/system_stm32l0xx.c \
    bsp/bsp_gpio.c \
    bsp/bsp_uart.c \
    service/sbus_bridge.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_cortex.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_dma.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_flash.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_flash_ex.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_flash_ramfunc.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_gpio.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_iwdg.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_pwr.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_pwr_ex.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_rcc.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_rcc_ex.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_uart.c \
    Drivers/STM32L0xx_HAL_Driver/Src/stm32l0xx_hal_uart_ex.c
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o)) $(BUILD)/GCC/startup_stm32l031xx.o

# Vendor HAL weak hooks and assert-only arguments intentionally stay untouched.
$(BUILD)/Drivers/STM32L0xx_HAL_Driver/Src/%.o: CFLAGS += -Wno-unused-parameter

.PHONY: all clean test check
all: $(TARGET).elf $(TARGET).hex $(TARGET).bin

$(TARGET).elf: $(OBJECTS) GCC/stm32l031g6.ld
	$(CC) $(OBJECTS) $(LDFLAGS) -Wl,--start-group -lc -lgcc -lnosys -Wl,--end-group -o $@
	$(SIZE) $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/GCC/startup_stm32l031xx.o: GCC/startup_stm32l031xx.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -g3 -c $< -o $@

test:
	@mkdir -p build/tests
	gcc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Itests/mocks -Ibsp -Iservice service/sbus_bridge.c tests/test_bridge.c -o build/tests/test_bridge
	./build/tests/test_bridge

check:
	python3 tests/check_project.py

clean:
	rm -rf build/gcc build/tests

-include $(OBJECTS:.o=.d)
