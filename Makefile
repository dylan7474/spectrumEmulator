# --- Makefile for Z80 Emulator ---
CC ?= gcc
PKG_CONFIG ?= pkg-config

TARGET := z80
SRCS := z80.c

# Allow callers to provide custom SDL2 flags via the environment. Otherwise try
# pkg-config first, then fall back to sdl2-config if available.
ifndef SDL2_CFLAGS
SDL2_CFLAGS := $(shell if command -v $(PKG_CONFIG) >/dev/null 2>&1; then \
        $(PKG_CONFIG) --cflags sdl2 2>/dev/null; \
    fi)
endif

ifndef SDL2_LIBS
SDL2_LIBS := $(shell if command -v $(PKG_CONFIG) >/dev/null 2>&1; then \
        $(PKG_CONFIG) --libs sdl2 2>/dev/null; \
    fi)
endif

ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
SDL2_CONFIG := $(shell command -v sdl2-config 2>/dev/null)
endif

ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
ifneq ($(strip $(SDL2_CONFIG)),)
SDL2_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
SDL2_LIBS := $(shell $(SDL2_CONFIG) --libs)
else
$(error SDL2 development files not found. Install libsdl2-dev and ensure pkg-config or sdl2-config is in PATH.)
endif
endif

CFLAGS ?= -Wall -Wextra -O2
CFLAGS += -std=c11 $(SDL2_CFLAGS)

LDFLAGS += $(SDL2_LIBS) -lm

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(TARGET)
	./$< 48.rom

test: $(TARGET)
	./$< --run-tests

clean:
	rm -f $(TARGET)

.PHONY: all run test clean
