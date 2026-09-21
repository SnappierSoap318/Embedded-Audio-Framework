# Vendored third-party sources

These files are vendored (committed) rather than fetched at build time so that
codec builds are reproducible without network access. Each entry records the
exact upstream revision. Only the files listed here are vendored; the rest of
each upstream project is not.

Keep this file and `docs/licensing.md` in sync when a vendored file is updated.

## dr_libs

- Upstream: https://github.com/mackron/dr_libs
- Revision: `dfe8377631000664666519fdb83da193fd8037f4`
- Files: `dr_libs/dr_flac.h` (v0.13.4), `dr_libs/dr_mp3.h` (v0.7.4),
  `dr_libs/dr_wav.h` (v0.14.6), `dr_libs/LICENSE`
- License: public domain (Unlicense) or MIT-0, at your option (see
  `dr_libs/LICENSE`; the choice is also embedded at the top of each header)
- Implementation translation unit: `impl/dr_flac_impl.c` (compiled with
  warnings suppressed, outside EAF's lint/format scope)

## stb

- Upstream: https://github.com/nothings/stb
- Revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`
- Files: `stb/stb_vorbis.c` (v1.22)
- License: public domain (Unlicense) or MIT, at your option (embedded at the top
  of the file)

## SHA-256

```
111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c  dr_libs/dr_flac.h
997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f  dr_libs/dr_mp3.h
03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493  dr_libs/dr_wav.h
4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636  stb/stb_vorbis.c
```

## Opus

Opus has no single-header decoder, so it is tracked as a git submodule instead
(see `.gitmodules`, `third_party/opus`, pinned to tag `v1.6.1`). Builds that
consume it must initialise submodules (`git submodule update --init`). See
`docs/licensing.md`.
