# --- Makefile for Z80 Emulator ---
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 `sdl2-config --cflags`
LDFLAGS = `sdl2-config --libs` -lm  # <-- ADD -lm HERE
TARGET = z80
SRCS = z80.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) 48.rom

clean:
	rm -f $(TARGET)

.PHONY: all run clean
