#!/bin/sh
# Configure an interactive shell for working on the Spectrum emulator in Codex-like environments.

ARCH=${ARCH:-z80}
export ARCH

# Preserve existing flags while ensuring the Z80 define is applied.
export CFLAGS="${CFLAGS:+$CFLAGS }-DTARGET_CPU_Z80"

# Provide a helpful alias for explicitly targeting the Z80 build from the current shell.
alias make_z80='ARCH=z80 make'

printf 'Codex environment configured for ARCH=%s. Use `make` or `make_z80` to build.\n' "$ARCH"
