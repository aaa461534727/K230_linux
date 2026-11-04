# Makefile —— 后端 (守护进程 + 调试 CLI) 的本机/交叉编译
#
# 板端: 用 SDK 的交叉工具链, 见 tools/board-build.sh (会带上 CROSS_COMPILE)
# 本机: make            -> build/k230-ui-backend  build/k230ui-ctl
#       make clean
#
# 交叉示例:
#   make CROSS_COMPILE=riscv64-unknown-linux-gnu-
CC      ?= gcc
CROSS_COMPILE ?=
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif

CFLAGS  ?= -O2 -Wall -Wextra
BUILD   ?= build

BINS = $(BUILD)/k230-ui-backend $(BUILD)/k230ui-ctl

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/k230-ui-backend: k230-ui-backend.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/k230ui-ctl: tools/k230ui-ctl.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean
