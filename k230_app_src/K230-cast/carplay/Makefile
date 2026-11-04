# K230 CarPlay — 交叉编译 (SDK 自带 Xuantie glibc 工具链)
#
#   make            编 build/carplay-hal (HAL 冒烟)
#   make install    装到板子 (需 BOARD=root@<ip>)
#   make clean

BOARD   ?= root@192.168.31.130
SDK     ?= ../../../sdk/k230_linux_sdk
OUT     ?= $(SDK)/output/k230_canmv_01studio_defconfig
CROSS   ?= $(OUT)/host/bin/riscv64-unknown-linux-gnu-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDFLAGS :=
LDLIBS  := -lm

BUILD   := build
SRCS    := src/selftest.c src/hal/display_sink.c src/hal/audio_sink.c \
           src/hal/h264_decoder.c src/hal/touch_evdev.c
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TARGET  := $(BUILD)/carplay-hal
ARBITER := $(BUILD)/carplay-arbiter
MEDIA   := $(BUILD)/carplay-media

.PHONY: all install clean rust

all: $(TARGET) $(ARBITER) $(MEDIA)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	$(STRIP) $@
	@echo "== built $@ =="

$(MEDIA): $(BUILD)/media_plane.o $(BUILD)/hal/h264_decoder.o $(BUILD)/hal/touch_evdev.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $^
	$(STRIP) $@

$(ARBITER): $(BUILD)/arbiter_stub.o
	@mkdir -p $(dir $@)
	$(CC) -o $@ $^
	$(STRIP) $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	sshpass -p root scp -O $< $(BOARD):/usr/local/bin/carplay-hal
	sshpass -p root ssh $(BOARD) 'chmod +x /usr/local/bin/carplay-hal && /usr/local/bin/carplay-hal screen 2'

# K1 工具链探针 (Rust): 需要 riscv64gc target
rust:
	cd tools/k1probe && \
	  CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER=$(CROSS)gcc \
	  cargo build --release --target riscv64gc-unknown-linux-gnu

clean:
	rm -rf $(BUILD)
