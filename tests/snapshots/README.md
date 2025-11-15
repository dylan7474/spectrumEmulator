# Snapshot fixtures

The regression harness keeps non-text assets out of the repository by either
encoding them as Base64 text (`.z80` cases) or synthesising `.sna` payloads on the
fly. When you run `./z80 --run-tests` the harness decodes the `.b64` files into
`tests/snapshots/generated/` and emits deterministic `.sna` snapshots with the
register, paging, and RAM patterns the tests expect. You can regenerate the
binary payloads manually by running:

```
base64 -d tests/snapshots/<fixture>.b64 > tests/snapshots/generated/<fixture>
```

Remove the generated files (or the entire `generated/` directory) if you need to
force a clean rebuild.

The synthesised `.sna` fixtures cover:

- `48k-basic.sna` – baseline paging and register restore.
- `128k-locked-bank5.sna` – locked 7FFD bank-5 paging state.
- `plus2a-special.sna` – +2A/+3 special mapping (1FFD bit 2) coverage.
- `plus3-rompaging.sna` – +3 ROM selection and standard paging verification.

## Compatibility probes

Real `.sna`/`.z80` captures can be exercised without baking them into the
repository. Drop them into `tests/snapshots/probes/` and the `--run-tests`
invocation (or `make test`) will attempt to load each file after the synthetic
fixtures run. The loader auto-detects the extension, so no extra configuration is
required. When you need to exercise snapshots that live elsewhere, pass
`--snapshot-test-dir <dir>` and place your files inside `<dir>/probes/`.

## Growing the compatibility shelf

With the harness riding alongside every `make test` invocation, roadmap progress
comes from expanding the probe library. Populate `tests/snapshots/probes/` with
real-world captures that stress tricky paging paths—late +3 DOS boot states,
Interface 1 wait-state experiments, 48K floating-bus samples, and so on. The
test output tags each probe PASS/FAIL so you can quickly spot regressions and
decide whether a new snapshot belongs in the shared corpus. When a probe needs
to stay private, mirror the same `probes/` structure inside your
`--snapshot-test-dir` folder and the harness will treat it identically without
checking it into the repository.
