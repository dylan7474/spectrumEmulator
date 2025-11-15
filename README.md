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
1. Install a POSIX-compatible shell such as MSYS2 or Cygwin and ensure a GCC toolchain is available.
2. Install the SDL2 development package. If you downloaded the official development ZIP, set `SDL2_DIR` to its root (or pass `SDL2_INCLUDEDIR`/`SDL2_LIBDIR`) so the makefile can locate headers and import libraries.
3. Optionally run `./configure` from the shell to confirm the compiler and SDL2 files are visible.
4. Build with `make -f Makefile.win` (or `mingw32-make -f Makefile.win` on MinGW environments). The resulting executable is `z80.exe` in the project root.

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

When a complete 32 KB image is supplied for the early 128K machines the loader automatically populates both ROM banks. If you only provide a single 16 KB bank (for example `128-0.rom`) the emulator now searches for a matching sibling such as `128-1.rom` or `128_1.rom` in the same directory before mirroring the first page. The loader also inspects the paired dumps for the "128K" menu credits and the Sinclair Research BASIC banner so it can swap them into the canonical order, accepting either uppercase or mixed-case banners in the process. Even when every required bank arrives with an explicit numeric suffix the loader double-checks the canonical order and overrides conflicting hints, so mismatched pairs like `128-0`/`128-1` still boot straight into the menu instead of falling back to 48K BASIC. If the menu strings are missing but exactly one bank still looks like the 48K ROM, the loader simply assumes the remaining bank must be the 128K menu and keeps the boot order correct. Every ROM boot now prints a breakdown of the detected banks, their source files, and any menu/BASIC signatures so you can verify that the paging logic spotted the right images before the CPU starts executing. The +2A/+3 models accept a 64 KB dump and split it into all four 16 KB ROMs so the DOS pair can be paged in through port `0x1FFD`. The late gate-array emulation also honours the all-RAM and special paging modes, bringing the bank-switching quirks used by CP/M and +3DOS utilities in line with the real hardware. You can force the classic configuration at any time with `--model 48k` or `--48k`.
The extended models share the revised ULA contention and interrupt handling code with the 48 KB machines, so bank paging, screen switching, and NMIs now follow the 128K timing quirks expected by diagnostics suites.
The AY-3-8912 control and data ports are latched even when audio output is disabled, allowing the 128K boot ROM to probe the sound chip registers successfully and remain in the built-in menu instead of immediately returning to the 48K BASIC ROM.

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

Snapshot regression coverage now rides alongside the CPU checks. The harness loads the fixtures in `tests/snapshots/` to verify
that 48K and 128K `.sna` dumps restore their paging state, that +2A special maps and +3 ROM selectors are honoured, and that both
compressed V1 and extended V3 `.z80` files decompress into the expected RAM images. Supply `--snapshot-test-dir <dir>` to point the
harness at additional snapshot bundles without touching the bundled set. To keep the repository binary-free, the `.z80` fixtures are
stored as `.b64` text and the `.sna` cases are synthesised on the fly. The harness automatically materialises everything into
`tests/snapshots/generated/` before running the tests, so you rarely need to decode them manually.

To stress-test the broadened loader against real-world captures, drop `.sna` or `.z80` files into `tests/snapshots/probes/` (or
the `probes/` folder inside your custom `--snapshot-test-dir`). The compatibility pass that runs alongside the synthetic suite
will attempt to load every snapshot found there, reporting PASS/FAIL outcomes without needing to edit the harness. It's an easy
way to keep regression coverage in sync with tricky paging setups pulled from your own collection.

For convenience a dedicated make target wraps the test invocation:

```bash
make test
```

The harness now checks NMI stack semantics alongside the 128K paging and contention paths, keeping the shared interrupt model honest as the emulator evolves. Recent additions exercise the floating bus sampler, the +2A/+3 contention masks, the late gate-array ROM pager, the calibrated +2A/+3 contention slot offset, and both Interface 1 and +3 peripheral wait-state emulations so future timing tweaks remain compatible. A GitHub Actions workflow (`.github/workflows/ci.yml`) runs `make` and `make test` on every push and pull request so timing regressions are flagged automatically.

#### Snapshot stress-test roadmap

Snapshot stress tests are now part of the default regression story—`make test` (or `./z80 --run-tests`) automatically runs the synthetic `.sna`/`.z80` fixtures alongside every probe found in `tests/snapshots/probes/`. With the automated harness shipped, the roadmap focus shifts to growing that compatibility shelf. Keep feeding it with awkward paging captures (late +3 special maps, Interface 1 wait-state dumps, 48K edge cases, etc.) so that every instant-boot path stays covered. Feel free to keep personal bundles elsewhere and point the harness at them with `--snapshot-test-dir <dir>`; just mirror the `probes/` subfolder layout and the loader will pick them up without further glue.

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

Loaded tapes remain cued at the start. Press **F5** to begin playback when the Spectrum is ready to `LOAD`, use **F6** to pause/stop, and tap **F7** to rewind to the beginning at any time. Playback now resumes from the last head position instead of rewinding automatically, so multi-part programs can continue loading sequential blocks. When the tape reaches the end, press **F7** before hitting play again to restart from the top.

Press **Tab** at any time to summon the tape manager popup. The centered overlay pauses Spectrum key routing and renders a deck-style control panel with the loaded tape, the active recorder destination, and a large digital counter. The illuminated play/stop/rewind/record buttons respond to clicks, while the shortcut strip along the bottom lists the **P**, **S**, **W**, and **R** bindings (hold **Shift** with **R** to append to an existing WAV). A second row highlights the Load, Browse, Eject, and Close actions and the same shortcut strip calls out their keyboard equivalents so the available gestures stay visible without duplicating labels. Press **L** to open the inline file prompt, type or paste a `.tap`, `.tzx`, or `.wav` path, then hit **Return** to mount it immediately; entering the name of a new file automatically creates an empty container in the chosen format so you can prepare blank tapes for recording without leaving the emulator. Hit **B** to enter the built-in file browser, navigate with the arrow keys, press **Return** to load the highlighted tape, and tap **Backspace** to climb to the parent directory. **Esc** cancels the prompt or browser and **Tab** closes the manager from any mode. The status strip updates after every command so you can confirm deck changes without leaving the overlay, and the text automatically scales down when needed so the panel always fits on-screen.

The ROM's own tape loader colour bursts are now reproduced in the emulator border. Pilot tones and data pulses fed to the virtual EAR input drive the same alternating blue/yellow stripes and colour flashes that appear on real hardware, making it easier to follow along with `LOAD` activity or spot when the loader is listening for headers. Because the rendering is tied to the ULA timing model, the border reacts immediately to manual `BREAK`/`STOP` commands and to pauses injected by custom loaders.

Tape playback and recording audio is mirrored to the host speaker so you can hear cassette activity during `LOAD` and `SAVE` operations without leaving the emulator.

To capture the MIC output generated by `SAVE`, provide `--save-tap` for a decoded `.tap` container or `--save-wav` for a raw audio
dump. The emulator records the cassette pulses while the ROM routines execute and writes the selected format on exit:

```bash
./z80 --save-tap recording.tap
./z80 --save-wav recording.wav
```

When a recording destination is configured, press **F8** or click the record button in the tape manager overlay to arm the virtual deck. A normal press clears any previous capture, while holding **Shift** (or Shift-clicking the overlay control) appends the new data to the existing WAV instead. If you have mounted a WAV image without passing `--save-wav`, the record control reuses that file so you can overwrite it or extend it in place.

Once the emulator finishes writing a recording, the deck now stops automatically after a short period of silence and rewinds the playback counter to the start, mirroring how the transport halts after a `LOAD` completes. The recorder remembers where the previous capture ended, so hitting record again immediately continues from that point; rewind first if you want the next save to overwrite from the beginning instead.

Loading and saving can be combined, allowing you to simultaneously play an input tape and archive new content:

```bash
./z80 --tap loader.tap --save-tap my_dump.tap
./z80 --tzx loader.tzx --save-wav capture.wav
./z80 --wav digitized.wav --save-tap restore.tap
```

The WAV loader expects mono 8- or 16-bit PCM streams and derives tape
transitions by tracking zero-crossings, making it suitable for replaying the
emulator's own `--save-wav` output or other digitized cassette recordings.

Snapshot containers (`.sna`, `.z80`) follow the same rules. Pass them explicitly
with `--snapshot path/to/state.z80` to boot straight into a frozen machine, or
provide one as the positional argument and the loader will detect it
automatically.

The TZX parser now understands the turbo data (`0x11`), pure tone (`0x12`),
pulse sequence (`0x13`), pure data (`0x14`), and direct recording (`0x15`)
block types in addition to the classic standard-speed (`0x10`) records. Custom
loaders that rely on tuned pilot lengths, bespoke tone tables, or raw waveform
captures play back without falling back to WAV conversion, keeping popular
demos and fast loaders working straight from their archival images.

### Upcoming compatibility and feature work

To keep 128K-era titles progressing, the next milestones focus on observability, sound, and media coverage:

1. **AY-3-8912 audio path.** The 128K control/data ports are already latched, but the emulator still ignores the register contents when mixing audio. Wiring a simple AY mixer alongside the beeper callback (and exposing gain/pan controls) will unlock the signature 128K soundtracks and help diagnose loaders that poll the chip before continuing.

2. **Paging diagnostics and regression probes.** Instrument `spectrum_apply_memory_configuration()`/`io_write()` with a `--trace-paging` helper (or structured logging) so troublesome 128K programs that "load but never start" can be captured and replayed. Any failing `.z80`/`.sna` should be dropped into `tests/snapshots/probes/` so `make test` covers the scenario automatically.

3. **Broader tape/disk coverage.** The TZX deck currently implements blocks `0x10–0x15`. Extending support to the additional tone/control records (`0x18`, `0x19`, etc.) plus prototyping +3 disk controller I/O will satisfy the media formats many late 128K releases expect.

4. **128K bring-up helpers.** Build UI/CLI affordances—ROM bank pre-flight reports, auto-typing macros that issue `LOAD ""` or poke the 128K menu, and a "bank watch" overlay fed by `current_screen_bank`/`current_paged_bank`—so diagnosing stubborn loaders requires fewer manual steps.

### Snapshots

Quick-load snapshot support complements the tape deck when you want to launch
software without the ROM loader delays. The emulator recognises 48 KB and 128
KB `.sna` images alongside the `.z80` family (including the common compressed
variants) either via the `--snapshot` flag or through positional
auto-detection. Snapshot loads happen before SDL initialisation so the Spectrum
appears on-screen already running the captured program, complete with CPU
registers and RAM restored from the saved state. 128K `.sna` files automatically
recover the last `0x7FFD` and (when present) `0x1FFD` gate-array writes so the
loader can select the 128K, +2A, or +3 models as needed without user
intervention, giving every machine an instant boot path.

Recent fixes corrected the Version 1 `.z80` header parser so the compression
flag, the high bit of the `R` register, and the surrounding register fields are
restored without the off-by-one skew that previously scrambled CPU state. The
RLE unpacker for Version 2/3 block payloads now verifies that both the repeat
count and data byte are present before expanding a sequence, preventing
truncated compressed pages from aborting snapshot loads. The loader also
refreshes the visible RAM pages immediately after copying snapshot data, so the
CPU resumes execution with the restored memory image, and prints the recovered
program counter, stack pointer, and interrupt mode to stderr for quick
diagnostics when a file refuses to boot.

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

- Press Escape or close the emulator window to exit.
- Launch the emulator with `--fullscreen` to start in desktop fullscreen mode; F11 or Alt+Enter toggles fullscreen at runtime.
- Toggle the internal beeper through the Spectrum's standard `BEEP` command.
- F5 Play, F6 Stop, F7 Rewind, F8 Record (Shift+F8 appends to the current WAV when available).

## Roadmap
- **Snapshot stress tests (shipped)** – The automated `.sna`/`.z80` regression
  harness now rides alongside `make test` and exercises both the synthetic
  fixtures and any user-provided probes in `tests/snapshots/probes/`. Roadmap
  work here focuses on growing that compatibility shelf with trickier captures
  so every paging combination keeps its instant boot path.
- **Input flexibility** – Introduce configurable key bindings and emulate common joystick standards like Kempston, Sinclair, and
  Interface 2 to broaden controller support for games.
- **Automation and CI** – Expand the new Linux CI pipeline with Windows builds and long-running cassette regressions so audio,
  timing, and tape decoding stay stable as new features land.
