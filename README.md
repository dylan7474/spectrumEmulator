# Spectrum Emulator

## Overview
This project is a homebrew ZX Spectrum emulator written in C with SDL2 for video, audio, and input. It focuses on accurately modelling the original hardware's Z80 CPU, display timing, and keyboard matrix while remaining small enough to hack on for learning and experimentation.

## Prerequisites
Before building, make sure the following tools and development headers are available:

- `gcc` or another C11-compatible compiler
- `make`
- SDL2 development libraries (headers and runtime)
- `pkg-config` to locate SDL2 on Unix-like systems
- Optional: `sdl2-config` for additional configuration helpers on some platforms

The provided `./configure` helper script can verify these dependencies and suggest installation commands on Linux distributions.

## Building

### Linux
1. Run `./configure` to verify that the compiler and SDL2 development files are available.
2. Build the emulator with `make`.
3. The resulting executable (`spectrum`) will be placed in the project root.

If you are working from the Codex environment used by the automation tooling in
this repository, you can bootstrap the required dependencies and export sane
defaults by running:

```bash
source scripts/codex_env.sh --install-deps --configure
```

Sourcing the script without arguments simply exports the recommended compiler
flags while allowing you to manage dependencies manually.

### Windows
1. Ensure a POSIX-compatible shell environment such as MSYS2 or Cygwin with SDL2 development packages installed.
2. Use `./configure` (optional on Windows) or confirm that `gcc`, `pkg-config`, and SDL2 are available in your environment.
3. Build with `make -f Makefile.win` to produce the Windows binary.

## Running
Launch the compiled executable from the command line with a 16 KB Spectrum ROM image:

```bash
./spectrum path/to/48k.rom
```

The emulator will load the ROM into memory and immediately begin execution once SDL initialisation succeeds.

## Controls
The emulator mirrors the original ZX Spectrum's keyboard matrix. The primary host-to-Spectrum key mapping is:

| Spectrum Key Row | Host Keyboard |
| ---------------- | -------------- |
| Row 0            | Left Shift, Z, X, C, V |
| Row 1            | A, S, D, F, G |
| Row 2            | Q, W, E, R, T |
| Row 3            | 1, 2, 3, 4, 5 |
| Row 4            | 0, 9, 8, 7, 6 (Backspace emulates `SHIFT+0`) |
| Row 5            | P, O, I, U, Y |
| Row 6            | Enter, L, K, J, H |
| Row 7            | Space, Left/Right Ctrl, M, N, B |

Additional host shortcuts:

- Close the emulator window or use your window manager's close button to exit.
- Toggle the internal beeper through the Spectrum's standard `BEEP` command.

## Roadmap
- Load tape and snapshot formats for authentic software support.
- Improve CPU accuracy with undocumented opcode coverage and interrupt timing.
- Add configurable key bindings and joystick emulation.
- Provide automated tests and continuous integration builds across platforms.
