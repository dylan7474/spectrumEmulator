# Spectrum Emulator Agent Instructions

- Follow C11 conventions with 4-space indentation in C source files.
- Prefer standard library calls and avoid platform-specific extensions unless wrapped in `#ifdef` guards.
- Update `README.md` whenever build prerequisites, user-facing workflows, or CPU opcode coverage details change.
- Keep the snapshot loader documentation aligned with the actual `.z80` header parsing and decompression behaviour.
- Make sure README tape deck documentation stays in sync with changes to the loader border graphics and the Tab-invoked tape manager so users know how the status indicators behave.
- Keep the Tab-invoked tape manager workflow documented in `README.md` whenever the runtime loader or popup controls change.
- Document host-facing affordances such as Escape-to-exit handling and the `--fullscreen` launch switch in `README.md` when they change.
- Shell scripts must use `#!/usr/bin/env bash` and start with `set -euo pipefail`.
- Keep the late gate-array contention tables and +3 peripheral wait-state tests in sync with the documented behaviour whenever timing changes are made.
- Keep the snapshot stress-test docs (`README.md` and `tests/snapshots/README.md`) aligned with the compatibility probe harness so users always know how to feed new `.sna`/`.z80` suites into `tests/snapshots/probes/`.
- Keep the README roadmap entries (especially the snapshot stress-test section) current so contributors know which compatibility probes and paging scenarios still need coverage.
