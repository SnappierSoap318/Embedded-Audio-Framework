# Licensing

EAF itself is licensed under **GPL-3.0-or-later** (see [../LICENSE](../LICENSE)).

All optional third-party audio dependencies are permissively licensed and
GPL-compatible. This file records where each dependency comes from, its license,
and how it is integrated, so that redistribution obligations remain clear.

## Vendored single-file sources

Committed under [`third_party/`](../third_party/README.md) and compiled from a
dedicated implementation translation unit with warnings suppressed (vendor code
is not governed by EAF's warning policy). The upstream license text is retained
in each file and, where present, as a sibling `LICENSE`.

| Component | Files | Upstream revision | License |
|---|---|---|---|
| dr_flac | `dr_libs/dr_flac.h` | `dfe8377` | public domain (Unlicense) / MIT-0 |
| dr_mp3 | `dr_libs/dr_mp3.h` | `dfe8377` | public domain (Unlicense) / MIT-0 |
| dr_wav | `dr_libs/dr_wav.h` | `dfe8377` | public domain (Unlicense) / MIT-0 |
| stb_vorbis | `stb/stb_vorbis.c` | `2c980bb` | public domain (Unlicense) / MIT |

## Git submodules

Tracked as references (no upstream code committed to this repository).

| Component | Submodule | License |
|---|---|---|
| libopus | `third_party/opus` (`xiph/opus`) | BSD-3-Clause |

## Optional external dependencies

Not vendored and not required for a default build.

| Component | Source | License | Notes |
|---|---|---|---|
| OI/libsbc | Zephyr `libsbc` checkout via `EAF_LIBSBC_ROOT` | Apache-2.0 | A2DP SBC decode; see [embedded.md](embedded.md) |

## Adding a dependency

1. Prefer a permissive license that is compatible with GPL-3.0.
2. Pin an exact revision (tag or commit); never track a moving branch.
3. For a single-file library, vendor the file under `third_party/`, record it in
   [`third_party/README.md`](../third_party/README.md) (revision, license,
   SHA-256), and add a row above.
4. For a multi-file library, prefer a git submodule with a pinned tag.
5. Keep the dependency out of the default build where possible, behind a CMake
   selection with a `*_null.c` fallback.
