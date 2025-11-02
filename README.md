# Spectrum Emulator

## Overview
This project is a homebrew ZX Spectrum emulator written in C with SDL2 for video, audio, and input. It focuses on accurately modelling the original hardware's Z80 CPU, display timing, and keyboard matrix while remaining small enough to hack on for learning and experimentation. The execution core now implements the entire documented and undocumented Z80 opcode space, including all prefixed tables.

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
3. The resulting executable (`z80`) will be placed in the project root.

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
Launch the compiled executable from the command line. By default the emulator powers up as a 48 KB Spectrum when provided with a standard ROM dump:

```bash
./z80 path/to/48k.rom
```

If no ROM is specified the bundled `48.rom` image in the project root is used automatically. The emulator will load the ROM into memory and immediately begin execution once SDL initialisation succeeds.

The new memory mapper also understands the full 128 KB family. Provide the paired ROM image and select the model explicitly (the shorthand flags `--128k`, `--plus2a`, and `--plus3` work the same way):

```bash
./z80 --model 128k   path/to/128k.rom
./z80 --model plus2a path/to/plus2a.rom
./z80 --model plus3  path/to/plus3.rom
```

When a 32 KB image is supplied for the early 128K machines the loader automatically populates both ROM banks; otherwise the first bank is mirrored. The +2A/+3 models accept a 64 KB dump and split it into all four 16 KB ROMs so the DOS pair can be paged in through port `0x1FFD`. The late gate-array emulation also honours the all-RAM and special paging modes, bringing the bank-switching quirks used by CP/M and +3DOS utilities in line with the real hardware. You can force the classic configuration at any time with `--model 48k` or `--48k`.
The extended models share the revised ULA contention and interrupt handling code with the 48 KB machines, so bank paging, screen switching, and NMIs now follow the 128K timing quirks expected by diagnostics suites.

Contention timing can be tuned without changing the core model. Pass `--contention <profile>` to pick from the original 48K bus sharing, the 128K "toastrack/+2" pattern, or the later +2A/+3 gate array behaviour. The late gate-array timings now honour the diagnostic-verified one-tick offset used by the +2A/+3 family, so screen fetch contention lines up with hardware-level tests:

```bash
./z80 --model 128k --contention plus2a path/to/128k.rom
```

For software that expects Interface 1 style bus delays, enable the shared peripheral wait-state generator with `--peripheral if1`. Late gate-array models now install the +3 peripheral wait-states by default so AY, disk, and other port-heavy routines see the hardware's extra 3T delays. Override the behaviour explicitly with `--peripheral plus3` or `--peripheral none`. Reads from otherwise unclaimed ports still return the floating bus value captured from the ULA, so keyboard scanners and Kempston autodetection routines observe the same byte stream as on the real hardware.

For audio debugging you can mirror the generated beeper samples to a WAV file with the optional dump flag:

```bash
./z80 --audio-dump beeper.wav path/to/48k.rom
```

The WAV stream is captured directly from the audio callback, allowing offline analysis with tools such as Audacity.

If you need to troubleshoot the beeper timing internals, pass `--beeper-log` to re-enable the detailed latency logs that are now
disabled by default:

```bash
./z80 --beeper-log path/to/48k.rom
```

For cassette investigations, enable `--tape-debug` to mirror block metadata and
the individual bits emitted during playback to stderr. The logs include header
names, payload lengths, and MSB-first bit traces so you can confirm that TAP
and TZX images expose the expected filenames and data ordering:

```bash
./z80 --tap loader.tap --tape-debug
```

## Testing

The emulator ships with a lightweight CPU regression harness that exercises the undocumented opcode helpers and verifies
interrupt sequencing. With the remaining undocumented instructions now implemented, the suite gives quick confidence that the
full opcode matrix still matches the hardware. Run it directly from the binary:

```bash
./z80 --run-tests
```

The command returns a non-zero exit status if any unit check fails. Optional CPU exercisers are available when their CP/M binaries
are present. Place `zexdoc.com`, `zexall.com`, and/or `z80full.com` inside `tests/roms/` (or point the harness at an alternate
directory via `--test-rom-dir <dir>`), then rerun the test command to execute the suites and record their output. `z80full.com` is
treated as a smoke test that fails only when it reports errors, letting the harness run the more exhaustive checks without
requiring string updates for every upstream release.

For convenience a dedicated make target wraps the test invocation:

```bash
make test
```

The harness now checks NMI stack semantics alongside the 128K paging and contention paths, keeping the shared interrupt model honest as the emulator evolves. Recent additions exercise the floating bus sampler, the +2A/+3 contention masks, the late gate-array ROM pager, the calibrated +2A/+3 contention slot offset, and both Interface 1 and +3 peripheral wait-state emulations so future timing tweaks remain compatible. A GitHub Actions workflow (`.github/workflows/ci.yml`) runs `make` and `make test` on every push and pull request so timing regressions are flagged automatically.

### Loading and saving tapes

Real-time cassette emulation is available for standard `.tap` dumps, `.tzx` images that use standard speed data blocks, and mono PCM `.wav` captures. Supply
one of the tape options when launching the emulator and the EAR input will be driven automatically during `LOAD`/`MERGE` calls:

```bash
./z80 --tap software.tap
./z80 --tzx demo.tzx
./z80 --wav digitized.wav
```

You can also skip the explicit flags and pass a tape image directly. The emulator
infers `.tap`, `.tzx`, and `.wav` formats from positional arguments, so running
`./z80 digitized.wav` loads the bundled ROM and cues the specified tape at
startup.

Loaded tapes remain cued at the start. Press **F5** to begin playback when the Spectrum is ready to `LOAD`, use **F6** to pause/stop, and tap **F7** to rewind to the beginning at any time. Playback now resumes from the last head position instead of rewinding automatically, so multi-part programs can continue loading sequential blocks. When the tape reaches the end, press **F7** (or click the on-screen rewind control) before hitting play again to restart from the top.

While a tape source or recording destination is configured, a status panel in the upper border shows the current deck mode and a running counter so you can monitor playback and capture progress without leaving the emulator window. The panel exposes on-screen play, stop, rewind, and record controls so you can drive the deck entirely with the mouse when preferred.

Tape playback and recording audio is mirrored to the host speaker so you can hear cassette activity during `LOAD` and `SAVE` operations without leaving the emulator.

To capture the MIC output generated by `SAVE`, provide `--save-tap` for a decoded `.tap` container or `--save-wav` for a raw audio
dump. The emulator records the cassette pulses while the ROM routines execute and writes the selected format on exit:

```bash
./z80 --save-tap recording.tap
./z80 --save-wav recording.wav
```

When a recording destination is configured, press **F8** or click the record button to arm the virtual deck. A normal press clears any previous capture, while holding **Shift** (or Shift-clicking the on-screen control) appends the new data to the existing WAV instead. If you have mounted a WAV image without passing `--save-wav`, the record control reuses that file so you can overwrite it or extend it in place.

Loading and saving can be combined, allowing you to simultaneously play an input tape and archive new content:

```bash
./z80 --tap loader.tap --save-tap my_dump.tap
./z80 --tzx loader.tzx --save-wav capture.wav
./z80 --wav digitized.wav --save-tap restore.tap
```

The WAV loader expects mono 8- or 16-bit PCM streams and derives tape transitions by tracking zero-crossings, making it suitable for replaying the emulator's own `--save-wav` output or other digitized cassette recordings.

At present the TZX reader accepts standard speed blocks (`0x10`). Future updates may broaden support for additional block types
and direct snapshot formats.

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
- F5 Play, F6 Stop, F7 Rewind, F8 Record (Shift+F8 appends to the current WAV when available).
- F11 or Alt+Enter toggles fullscreen mode.

## Roadmap
- **Tape and snapshot formats** – Extend cassette support beyond standard-speed `.tap`/`.tzx` images by decoding additional TZX
  block types such as turbo, custom tone, and direct recording data. Add popular snapshot containers (for example `.sna` and
  `.z80`) so software that relies on quick-load images can launch without the tape deck entirely.
- **Input flexibility** – Introduce configurable key bindings and emulate common joystick standards like Kempston, Sinclair, and
  Interface 2 to broaden controller support for games.
- **Automation and CI** – Expand the new Linux CI pipeline with Windows builds and long-running cassette regressions so audio,
  timing, and tape decoding stay stable as new features land.
