SHELL := /bin/bash

# ============================================================================
# Paths
# ============================================================================
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
APP_DIR := $(ROOT_DIR)
BOARDS_DIR := $(ROOT_DIR)/Board
BOARD_CONFIG_DIR := $(ROOT_DIR)/Board/hpm_board_config
DRIVER_DIR := $(ROOT_DIR)/Driver/hpm_impl
BUILD_ROOT := $(ROOT_DIR)/build
APP_BUILD_DIR := $(BUILD_ROOT)/app
BOOTLOADER_BUILD_DIR := $(BUILD_ROOT)/bootloader
SLOT_A_BUILD_DIR := $(BUILD_ROOT)/slot-a
SLOT_B_BUILD_DIR := $(BUILD_ROOT)/slot-b
OUTPUT_DIR := $(BUILD_ROOT)/output
OTA_OUTPUT_DIR := $(OUTPUT_DIR)/ota
# 主程序（旧整体镜像）CMake 构建树；保留 BUILD_DIR 名称供既有目标复用
BUILD_DIR := $(APP_BUILD_DIR)
PROJECT_NAME ?= $(notdir $(ROOT_DIR))

# 槽镜像版本号（写入镜像头，供上位机展示/防回退参考）
IMAGE_VERSION ?= $(shell git -C $(ROOT_DIR) rev-list --count HEAD 2>/dev/null || echo 0)
PYTHON ?= python3
GEN_IMAGE_HEADER := $(ROOT_DIR)/scripts/gen_image_header.py

# ============================================================================
# Environment repository layout (see <env-repo>/.envrc)
# ============================================================================
WORKSPACE_ROOT ?= $(if $(HPMDEV_ROOT),$(HPMDEV_ROOT),$(abspath $(ROOT_DIR)/../..))
HPMDEV_TOOLS_DIR ?= $(WORKSPACE_ROOT)/tools
SDK_DIR := $(if $(HPM_SDK_BASE),$(HPM_SDK_BASE),$(WORKSPACE_ROOT)/sdk/hpm_sdk)
export HPM_SDK_BASE := $(SDK_DIR)

# ============================================================================
# Host Path Configuration (for debug on Windows host / outside container)
# ============================================================================
HOST_WORKSPACE_DIR ?= $(if $(HPMDEV_HOST_WORKSPACE),$(HPMDEV_HOST_WORKSPACE),$(abspath $(WORKSPACE_ROOT)))

# Debug source path mode:
#   host      - default, remap debug info to HOST_WORKSPACE_DIR for host-side debuggers (Ozone/GDB on Windows)
#   container - keep debug info as container paths for VS Code Remote/Dev Container F5 debug
DEBUG_PATH_MODE ?= host

# ============================================================================
# Board Selection
# ============================================================================
# Default board (can be overridden: make BOARD=user_board)
BOARD ?= HPM53M1_G6618Motor_board

# Derive board path
BOARD_DIR := $(BOARDS_DIR)/$(BOARD)
BOARD_SEARCH_PATH := $(BOARDS_DIR)

# ============================================================================
# Toolchain & Build Options
# ============================================================================
RV_ARCH ?= rv32imafdc
RV_ABI ?= ilp32d
CMAKE_BUILD_TYPE ?= Debug
HPM_BUILD_TYPE ?= flash_xip
GENERATOR ?= Ninja
LAST_BUILD_LOG := $(BUILD_DIR)/last_build.log

# Optimization level (separate Debug/Release)
# Usage: make build OPT_LEVEL_DBG=-Og OPT_LEVEL_REL=-Ofast
# Debug:  -Og (default: debuggable + FOC ISR real-time budget; -O0 too slow for 25kHz loop)
# Release: -O3 (default, maximum speed)
OPT_LEVEL_DBG ?= -Og
OPT_LEVEL_REL ?= -O3

# Extra app-level preprocessor defines (e.g. -DAPP_BENCH_DEBUG_MODE=1 for
# bench-only debug builds that bypass USB/terminal). Empty = normal build.
APP_DEFINES ?=

# ============================================================================
# Flash Configuration
# ============================================================================
FLASH_TOOL ?= openocd
OPENOCD_BIN ?= $(if $(HPM_OPENOCD_PREFIX),$(HPM_OPENOCD_PREFIX)/bin/openocd,openocd)
OCD_SCRIPTS ?= $(if $(HPM_OCD_SCRIPTS),$(HPM_OCD_SCRIPTS),$(SDK_DIR)/boards/openocd)
PROBE_CFG ?= probes/cmsis_dap.cfg
SOC_CFG ?= soc/hpm5300.cfg
BOARD_CFG ?= $(if $(wildcard $(BOARD_DIR)/$(BOARD).cfg),$(BOARD_DIR)/$(BOARD).cfg,boards/$(BOARD).cfg)
JLINK_DEVICE ?= HPM5361xEGx
JLINK_IF ?= JTAG
JLINK_SPEED ?= 1000
# 镜像最低加载地址 = .nor_cfg_option 的 LMA（= flash base + 0x400）。
# 修改 linker script 的 flash 布局后需同步更新（可用
# `riscv32-unknown-elf-objdump -h build/app/output/demo.elf | sort` 复核）。
JLINK_FLASH_ADDR ?= 0x8000400

# ============================================================================
# Scripts (relative paths)
# ============================================================================
BUILD_UI_SCRIPT := $(HPMDEV_TOOLS_DIR)/scripts/build_ui.sh
FLASH_SCRIPT := $(HPMDEV_TOOLS_DIR)/scripts/flash_target.sh

# ============================================================================
# CMake Arguments
# ============================================================================
# Debug source path remap (see HOST_WORKSPACE_DIR / DEBUG_PATH_MODE above).
# Applied to all targets (app + SDK lib) through the SDK's EXTRA_C_FLAGS hook.
ifeq ($(DEBUG_PATH_MODE),container)
  DEBUG_PREFIX_MAP := -fdebug-prefix-map=$(ROOT_DIR)=$(ROOT_DIR) -fdebug-prefix-map=$(SDK_DIR)=$(SDK_DIR)
else
  DEBUG_PREFIX_MAP := -fdebug-prefix-map=$(ROOT_DIR)=$(HOST_WORKSPACE_DIR)/projects/$(PROJECT_NAME) -fdebug-prefix-map=$(SDK_DIR)=$(HOST_WORKSPACE_DIR)/sdk/hpm_sdk
endif

# Optimization level flags for Debug/Release builds
OPT_CMAKE_ARGS := -DCMAKE_C_FLAGS_DEBUG="$(OPT_LEVEL_DBG) -g" -DCMAKE_C_FLAGS_RELEASE="$(OPT_LEVEL_REL) -DNDEBUG"

CMAKE_ARGS := \
	-G$(GENERATOR) \
	-DBOARD=$(BOARD) \
	-DBOARD_SEARCH_PATH=$(BOARD_SEARCH_PATH) \
	-DRV_ARCH=$(RV_ARCH) \
	-DRV_ABI=$(RV_ABI) \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DHPM_BUILD_TYPE=$(HPM_BUILD_TYPE) \
	-DEXTRA_C_FLAGS="$(DEBUG_PREFIX_MAP) $(APP_DEFINES)" \
	$(OPT_CMAKE_ARGS)

# ============================================================================
# Phony Targets
# ============================================================================
.PHONY: all configure build build-core artifacts artifacts-core flash flash-openocd flash-jlink clean distclean rebuild help list-boards banner bootloader slot-a slot-b ota-package flash-ota flash-info

all: artifacts

# ============================================================================
# Build Targets
# ============================================================================
configure:
	@if [ ! -d "$(BOARD_DIR)" ]; then \
		echo "Error: board directory not found: $(BOARD_DIR)"; \
		echo "Available boards:"; \
		@ls $(BOARDS_DIR) 2>/dev/null || echo "  (none)"; \
		echo "Use: make BOARD=<board_name>"; \
		exit 1; \
	fi
	cmake -S $(APP_DIR) -B $(BUILD_DIR) $(CMAKE_ARGS)

build-core: configure
	@echo ""
	@echo "============================================================"
	@echo "[BUILD] Start compiling target"
	@echo "============================================================"
	cmake --build $(BUILD_DIR) -j

build:
	@mkdir -p $(BUILD_DIR); \
	status=0; \
	set -o pipefail; \
	$(MAKE) --no-print-directory build-core 2>&1 | tee $(LAST_BUILD_LOG); \
	status=$$?; \
	$(MAKE) --no-print-directory banner BUILD_STATUS=$$status BUILD_ACTION=build BUILD_LOG="$(LAST_BUILD_LOG)"; \
	exit $$status

artifacts-core: build-core flash-info
	@echo ""
	@echo "============================================================"
	@echo "[ARTIFACTS] Copying to build/output/"
	@echo "============================================================"
	@mkdir -p $(OUTPUT_DIR)
	@for f in $(BUILD_DIR)/output/demo.*; do \
		if [ -f "$$f" ]; then \
			ext=$${f##*.}; \
			cp "$$f" "$(OUTPUT_DIR)/$(PROJECT_NAME).$$ext"; \
			echo "  -> $(OUTPUT_DIR)/$(PROJECT_NAME).$$ext"; \
		fi; \
	done

artifacts:
	@mkdir -p $(BUILD_DIR); \
	status=0; \
	set -o pipefail; \
	$(MAKE) --no-print-directory artifacts-core 2>&1 | tee $(LAST_BUILD_LOG); \
	status=$$?; \
	$(MAKE) --no-print-directory banner BUILD_STATUS=$$status BUILD_ACTION=artifacts BUILD_LOG="$(LAST_BUILD_LOG)"; \
	exit $$status

# ============================================================================
# Flash Targets
# ============================================================================
flash: flash-$(FLASH_TOOL)

flash-openocd: artifacts
	@echo "[FLASH] Using OpenOCD..."
	$(OPENOCD_BIN) \
		-s $(OCD_SCRIPTS) \
		-f $(PROBE_CFG) \
		-f $(SOC_CFG) \
		-f $(BOARD_CFG) \
		-c "program $(OUTPUT_DIR)/$(PROJECT_NAME).elf verify reset exit"

flash-jlink: artifacts
	@echo "[FLASH] Using J-Link..."
	@echo "device $(JLINK_DEVICE)" > /tmp/jlink_flash.jlink
	@echo "if $(JLINK_IF)" >> /tmp/jlink_flash.jlink
	@echo "speed $(JLINK_SPEED)" >> /tmp/jlink_flash.jlink
	@echo "loadfile $(OUTPUT_DIR)/$(PROJECT_NAME).bin $(JLINK_FLASH_ADDR)" >> /tmp/jlink_flash.jlink
	@echo "r" >> /tmp/jlink_flash.jlink
	@echo "q" >> /tmp/jlink_flash.jlink
	JLinkExe /tmp/jlink_flash.jlink

# ============================================================================
# OTA Targets（A/B 槽 + DFU bootloader）
# ============================================================================
# build/output 内的烧录索引（随时可重新生成）
flash-info:
	@mkdir -p $(OUTPUT_DIR)
	@printf '%s\n' \
		'G6618 flash artifacts (build/output)' \
		'' \
		'  legacy full image : HPM53M1_G6618Motor.elf/.bin  @ 0x8000400' \
		'  bootloader        : bootloader.elf/.bin          @ 0x8000400' \
		'  slot A            : slot_a.elf/.bin              @ 0x80020000' \
		'  slot B            : slot_b.elf/.bin              @ 0x8008F000' \
		'  OTA package       : ota/{bootloader,slot_a,slot_b,manifest.json}' \
		'' \
		'  slot_x.elf 已写入 64B 镜像头，Ozone/J-Link 可直接加载；' \
		'  只用 .bin 时按上面的地址烧录。' \
		> $(OUTPUT_DIR)/FLASHING.txt

# 槽构建：$(1)=slot id，$(2)=build dir，$(3)=槽名（a/b）
define build_slot
	@echo "[SLOT $(3)] configure + build (APP_SLOT_ID=$(1))"
	cmake -S $(APP_DIR) -B $(2) $(CMAKE_ARGS) -DAPP_SLOT_ID=$(1)
	cmake --build $(2) -j
	cp $(2)/output/demo.elf $(OUTPUT_DIR)/slot_$(3).elf
	$(PYTHON) $(GEN_IMAGE_HEADER) --input $(2)/output/demo.bin \
		--output $(OUTPUT_DIR)/slot_$(3).bin --slot $(1) --version $(IMAGE_VERSION) \
		--elf $(OUTPUT_DIR)/slot_$(3).elf
endef

bootloader:
	@mkdir -p $(OUTPUT_DIR) $(OTA_OUTPUT_DIR)
	@echo "[BOOTLOADER] configure + build"
	cmake -S $(ROOT_DIR)/bootloader -B $(BOOTLOADER_BUILD_DIR) $(CMAKE_ARGS)
	cmake --build $(BOOTLOADER_BUILD_DIR) -j
	cp $(BOOTLOADER_BUILD_DIR)/output/demo.bin $(OUTPUT_DIR)/bootloader.bin
	cp $(BOOTLOADER_BUILD_DIR)/output/demo.elf $(OUTPUT_DIR)/bootloader.elf
	@ls -l $(OUTPUT_DIR)/bootloader.elf $(OUTPUT_DIR)/bootloader.bin

slot-a:
	@mkdir -p $(OUTPUT_DIR) $(OTA_OUTPUT_DIR)
	$(call build_slot,0,$(SLOT_A_BUILD_DIR),a)

slot-b:
	@mkdir -p $(OUTPUT_DIR) $(OTA_OUTPUT_DIR)
	$(call build_slot,1,$(SLOT_B_BUILD_DIR),b)

# 发布包：bootloader + 两个槽镜像 + manifest（清空后按顺序重建 manifest）
ota-package: bootloader slot-a slot-b flash-info
	@rm -f $(OTA_OUTPUT_DIR)/manifest.json
	$(PYTHON) $(GEN_IMAGE_HEADER) --verify-existing --input $(OUTPUT_DIR)/slot_a.bin \
		--slot 0 --manifest $(OTA_OUTPUT_DIR)/manifest.json
	$(PYTHON) $(GEN_IMAGE_HEADER) --verify-existing --input $(OUTPUT_DIR)/slot_b.bin \
		--slot 1 --manifest $(OTA_OUTPUT_DIR)/manifest.json
	@cp $(OUTPUT_DIR)/bootloader.bin $(OUTPUT_DIR)/slot_a.bin $(OUTPUT_DIR)/slot_b.bin $(OTA_OUTPUT_DIR)/
	@echo ""
	@echo "[OTA] package ready: $(OTA_OUTPUT_DIR)"
	@ls -l $(OTA_OUTPUT_DIR)

# 首次落地：J-Link 整片擦除后烧 bootloader + Slot A（参数/齿槽表一并清空）
flash-ota: ota-package
	@echo "[FLASH-OTA] J-Link: erase + bootloader@0x8000400 + slot A@0x80020000"
	@echo "WARN: 整片擦除会清空参数区与齿槽表（需重新 cal / param 配置）"
	@echo "device $(JLINK_DEVICE)" > /tmp/jlink_flash_ota.jlink
	@echo "if $(JLINK_IF)" >> /tmp/jlink_flash_ota.jlink
	@echo "speed $(JLINK_SPEED)" >> /tmp/jlink_flash_ota.jlink
	@echo "erase" >> /tmp/jlink_flash_ota.jlink
	@echo "loadfile $(OUTPUT_DIR)/bootloader.bin 0x8000400" >> /tmp/jlink_flash_ota.jlink
	@echo "loadfile $(OUTPUT_DIR)/slot_a.bin 0x80020000" >> /tmp/jlink_flash_ota.jlink
	@echo "r" >> /tmp/jlink_flash_ota.jlink
	@echo "q" >> /tmp/jlink_flash_ota.jlink
	JLinkExe /tmp/jlink_flash_ota.jlink

# ============================================================================
# Clean Targets
# ============================================================================
clean:
	rm -rf $(BUILD_ROOT) $(ROOT_DIR)/output $(ROOT_DIR)/.cache

distclean: clean
	@echo "Deep clean complete."

rebuild: clean build

# ============================================================================
# Info Targets
# ============================================================================
list-boards:
	@echo "Available boards in $(BOARDS_DIR):"
	@echo ""
	@for d in $(BOARDS_DIR)/*/; do \
		if [ -d "$$d" ]; then \
			bname=$$(basename "$$d"); \
			echo "  - $$bname"; \
		fi; \
	done
	@echo ""
	@echo "Current board: $(BOARD)"
	@echo "Use: make BOARD=<board_name> build"

help:
	@echo "HPM User Template - Build System"
	@echo ""
	@echo "Usage: make [TARGET] [OPTIONS]"
	@echo ""
	@echo "Targets:"
	@echo "  build         Configure and compile (default)"
	@echo "  artifacts     Build and copy outputs to build/output/"
	@echo "  flash         Flash firmware (default: openocd)"
	@echo "  flash-openocd Flash using OpenOCD"
	@echo "  flash-jlink   Flash using J-Link"
	@echo "  clean         Remove build and output directories"
	@echo "  distclean     Deep clean"
	@echo "  rebuild       Clean and rebuild"
	@echo "  list-boards   Show available boards"
	@echo "  help          Show this help"
	@echo ""
	@echo "OTA Targets:"
	@echo "  bootloader    Build DFU bootloader (build/output/bootloader.bin)"
	@echo "  slot-a        Build Slot A image (build/output/slot_a.bin)"
	@echo "  slot-b        Build Slot B image (build/output/slot_b.bin)"
	@echo "  ota-package   Build all + build/output/ota/manifest.json"
	@echo "  flash-ota     J-Link erase + bootloader + Slot A (wipes params)"
	@echo ""
	@echo "Options:"
	@echo "  BOARD=<name>             Board name (default: hpm5301evklite_board)"
	@echo "  CMAKE_BUILD_TYPE=<type>  Debug or Release (default: Debug)"
	@echo "  HPM_BUILD_TYPE=<type>    flash_xip, flash_sdram_xip, etc."
	@echo "  FLASH_TOOL=<tool>        openocd or jlink (default: openocd)"
	@echo ""
	@echo "Examples:"
	@echo "  make build"
	@echo "  make BOARD=user_board build"
	@echo "  make CMAKE_BUILD_TYPE=Release artifacts"
	@echo "  make FLASH_TOOL=jlink flash"

# ============================================================================
# Banner
# ============================================================================
banner:
	@if [ -x "$(BUILD_UI_SCRIPT)" ]; then \
		PROJECT_NAME="$(PROJECT_NAME)" \
		BUILD_ACTION="$(BUILD_ACTION)" \
		BUILD_STATUS="$(BUILD_STATUS)" \
		OUTPUT_DIR="$(OUTPUT_DIR)" \
		BUILD_LOG="$(LAST_BUILD_LOG)" \
		APP_DIR="$(APP_DIR)" \
		BUILD_DIR="$(BUILD_DIR)" \
		BOARD="$(BOARD)" \
		BOARD_SEARCH_PATH="$(BOARD_SEARCH_PATH)" \
		RV_ARCH="$(RV_ARCH)" \
		RV_ABI="$(RV_ABI)" \
		CMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)" \
		HPM_BUILD_TYPE="$(HPM_BUILD_TYPE)" \
		bash $(BUILD_UI_SCRIPT) --action $(BUILD_ACTION) --status $(BUILD_STATUS); \
	else \
		if [ "$(BUILD_STATUS)" -eq 0 ]; then \
			echo ""; \
			echo "============================================================"; \
			echo "  BUILD SUCCEEDED"; \
			echo "============================================================"; \
		else \
			echo ""; \
			echo "============================================================"; \
			echo "  BUILD FAILED (exit code: $(BUILD_STATUS))"; \
			echo "============================================================"; \
		fi; \
	fi
