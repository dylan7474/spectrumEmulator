# Spectrum Emulator Agent Instructions

- Follow C11 conventions with 4-space indentation in C source files.
- Prefer standard library calls and avoid platform-specific extensions unless wrapped in `#ifdef` guards.
- Update `README.md` whenever build prerequisites, user-facing workflows, or CPU opcode coverage details change.
- Shell scripts must use `#!/usr/bin/env bash` and start with `set -euo pipefail`.
- Keep the late gate-array contention tables and +3 peripheral wait-state tests in sync with the documented behaviour whenever timing changes are made.
