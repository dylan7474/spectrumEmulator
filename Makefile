# --- Makefile for Z80 Emulator ---
# The build is parameterised by the CPU architecture we are emulating.
# By default we build the ZX Spectrum Z80 variant.
ARCH ?= z80

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 `sdl2-config --cflags`
LDFLAGS = `sdl2-config --libs` -lm  # <-- ADD -lm HERE
SRCS = z80.c

TARGET = spectrum_$(ARCH)

ifeq ($(ARCH),z80)
CFLAGS += -DTARGET_CPU_Z80
endif

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) 48.rom

clean:
	rm -f $(TARGET)

.PHONY: all run clean
