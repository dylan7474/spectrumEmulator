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
