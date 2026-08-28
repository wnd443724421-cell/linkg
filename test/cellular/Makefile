ROOT := ../..
BUILD_DIR := build

HOST_CC ?= cc
TOOLCHAIN_DIR ?= /home/dawn/t113_tina5.0_v1.2/prebuilt/rootfsbuilt/arm/toolchain-sunxi-glibc-gcc-830/toolchain
ARM_CC ?= $(TOOLCHAIN_DIR)/bin/arm-openwrt-linux-gcc
STAGING_DIR ?= /home/dawn/t113_tina5.0_v1.2/openwrt/openwrt/staging_dir
export STAGING_DIR

CPPFLAGS := \
	-I$(ROOT)/include/linkg/base \
	-I$(ROOT)/include/linkg/config \
	-I$(ROOT)/include/linkg/modules/cellular \
	-I$(ROOT)/include/linkg/platform/uart \
	-I$(ROOT)/modules/cellular/at \
	-I$(ROOT)/modules/cellular/rg255 \
	-I$(ROOT)/third_party/cjson

COMMON_CFLAGS := -std=gnu17 -Wall -Wextra -Wpedantic -Werror -O0 -g
ARM_CFLAGS := $(COMMON_CFLAGS) -march=armv7-a -mfloat-abi=hard -mfpu=neon-vfpv4

FIXTURE_BIN := $(BUILD_DIR)/rg255_query_fixture_test
NETWORK_CARD_FIXTURE_BIN := $(BUILD_DIR)/rg255_query_network_card_fixture_test
ARM_BIN := $(BUILD_DIR)/rg255_query_test

FIXTURE_SOURCES := \
	rg255_query_fixture_test.c \
	$(ROOT)/modules/cellular/rg255/rg255_query.c

NETWORK_CARD_FIXTURE_SOURCES := \
	rg255_query_network_card_fixture_test.c \
	$(ROOT)/modules/cellular/rg255/rg255_query.c

ARM_SOURCES := \
	rg255_query_test.c \
	$(ROOT)/platform/uart/linkg_uart.c \
	$(ROOT)/modules/cellular/at/at_channel.c \
	$(ROOT)/modules/cellular/rg255/rg255_cmd.c \
	$(ROOT)/modules/cellular/rg255/rg255_query.c \
	$(ROOT)/infra/base/linkg_thread.c \
	$(ROOT)/infra/base/linkg_time.c \
	$(ROOT)/infra/base/linkg_log.c \
	$(ROOT)/infra/base/linkg_file.c

.PHONY: all fixture fixture-run arm clean

all: fixture arm

$(BUILD_DIR):
	mkdir -p $@

fixture: $(NETWORK_CARD_FIXTURE_BIN) $(FIXTURE_BIN)

$(FIXTURE_BIN): $(FIXTURE_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(FIXTURE_SOURCES) -o $@

$(NETWORK_CARD_FIXTURE_BIN): $(NETWORK_CARD_FIXTURE_SOURCES) | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(NETWORK_CARD_FIXTURE_SOURCES) -o $@

fixture-run: $(NETWORK_CARD_FIXTURE_BIN) $(FIXTURE_BIN)
	./$(NETWORK_CARD_FIXTURE_BIN)
	./$(FIXTURE_BIN)

arm: $(ARM_BIN)

$(ARM_BIN): $(ARM_SOURCES) | $(BUILD_DIR)
	$(ARM_CC) $(CPPFLAGS) $(ARM_CFLAGS) $(ARM_SOURCES) -pthread -lrt -lm -o $@

clean:
	rm -rf $(BUILD_DIR)
