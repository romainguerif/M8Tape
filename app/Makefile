ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain — run via build.sh)
endif

CURRENT_WORKING_DIR = $(shell pwd)

PLATFORM ?= tg5040
LD_LIBRARY_PATH = $(CURRENT_WORKING_DIR)/platform/$(PLATFORM)/lib/
PREFIX = $(CURRENT_WORKING_DIR)/platform/$(PLATFORM)

-include minui/workspace/$(PLATFORM)/platform/makefile.env
SDL?=SDL
# NOTE: keep -flto (from minui's makefile.env). It's not just perf — without it
# this aarch64 cross-gcc 8.3.0 miscompiles under -Ofast (emits ARM32 movcs/rrx
# mnemonics → assembler errors). -flto changes the codegen path and avoids it.
# Downside: lto-wrapper ICEs intermittently → build.sh is re-run on failure.

TARGET = m8tape
PRODUCT = $(TARGET)

INCDIR = -I. -Iplatform/$(PLATFORM)/include/ -Iminui/workspace/all/common/ -Iminui/workspace/$(PLATFORM)/platform/
SOURCE = $(TARGET).c ui.c wav.c player.c h3000.c pitchcore.c dsputil.c $(wildcard fx_*.c) minui/workspace/all/common/scaler.c minui/workspace/all/common/utils.c minui/workspace/all/common/api.c minui/workspace/$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(ARCH) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -DUSE_$(SDL) -Ofast -std=gnu99
FLAGS = -L$(LD_LIBRARY_PATH) -ldl -lmsettings $(LIBS) -l$(SDL) -l$(SDL)_image -l$(SDL)_ttf -lasound -lpthread -lm -lz

all: minui $(PREFIX)/include/msettings.h
	LD_LIBRARY_PATH=$(LD_LIBRARY_PATH) $(CC) $(SOURCE) -o $(PRODUCT)-$(PLATFORM) $(CFLAGS) $(FLAGS)

clean:
	rm -rf $(PRODUCT)-$(PLATFORM)

minui:
	git clone --depth 1 https://github.com/shauninman/MinUI minui

platform/$(PLATFORM)/lib:
	mkdir -p platform/$(PLATFORM)/lib

platform/$(PLATFORM)/include:
	mkdir -p platform/$(PLATFORM)/include

$(PREFIX)/include/msettings.h: platform/$(PLATFORM)/lib platform/$(PLATFORM)/include
	cd $(CURRENT_WORKING_DIR)/minui/workspace/$(PLATFORM)/libmsettings && make
