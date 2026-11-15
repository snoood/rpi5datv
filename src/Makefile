# rpi5datv - build with `make` in this directory (or `make -C src` from the
# repository root). The binary is written to ../bin/rpi5datv.

PKG_CONFIG ?= pkg-config
CC         ?= gcc
BINDIR     := ../bin
BUILDDIR   := build

FFMPEG_PKGS   := libavformat libavcodec libavutil libswscale libswresample
FFMPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PKGS) 2>/dev/null)
FFMPEG_LIBS   ?= $(shell $(PKG_CONFIG) --libs $(FFMPEG_PKGS) 2>/dev/null)

PIOLIB_DIR := vendor/piolib
PIOLIB_SRC := $(PIOLIB_DIR)/piolib.c $(PIOLIB_DIR)/pio_rp1.c $(PIOLIB_DIR)/library_piochips.c

OPT     ?= -O3
CFLAGS  += $(OPT) -g -std=gnu11 -Wall -Wextra -pthread -I. -I$(PIOLIB_DIR)/include
LDLIBS  += -lm -pthread

# Cortex-A76 tuning when building natively on a Pi 5
ARCH := $(shell uname -m)
ifeq ($(ARCH),aarch64)
CFLAGS += -mcpu=cortex-a76
endif

APP_SRC := main.c dvbs.c modulator.c ringbuf.c source.c testcard.c pio_out.c
APP_OBJ := $(APP_SRC:%.c=$(BUILDDIR)/%.o) $(PIOLIB_SRC:%.c=$(BUILDDIR)/%.o)

TEST_SRC := tests/test_dvbs.c dvbs.c modulator.c
TEST_BIN := $(BUILDDIR)/test_dvbs

.PHONY: all clean test install

all: $(BINDIR)/rpi5datv

$(BINDIR)/rpi5datv: $(APP_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(FFMPEG_LIBS) $(LDFLAGS) $(LDLIBS)

$(BUILDDIR)/source.o $(BUILDDIR)/main.o: $(BUILDDIR)/%.o: %.c $(wildcard *.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) -c -o $@ $<

$(BUILDDIR)/$(PIOLIB_DIR)/%.o: $(PIOLIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DLIBRARY_BUILD=1 -w -c -o $@ $<

$(BUILDDIR)/%.o: %.c $(wildcard *.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(TEST_SRC) $(wildcard *.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

PREFIX ?= /usr/local
install: $(BINDIR)/rpi5datv
	install -D -m 0755 $(BINDIR)/rpi5datv $(DESTDIR)$(PREFIX)/bin/rpi5datv

clean:
	rm -rf $(BUILDDIR) $(BINDIR)/rpi5datv

# Fail early with a clear message if the FFmpeg development packages are missing.
ifeq ($(filter clean test,$(MAKECMDGOALS)),)
ifeq ($(strip $(FFMPEG_LIBS)),)
$(error FFmpeg development libraries not found via pkg-config. Install: sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev (or run ../install.sh))
endif
endif
