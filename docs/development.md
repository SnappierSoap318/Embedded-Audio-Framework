# Development workflow

Commit each completed, validated implementation stage locally. Use focused
messages explaining the resulting behavior. Do not push without a request.

The initial history was reconstructed from the existing working tree because
`.git` was empty. Milestone commits group the current implementation by dependency;
they are not recovered historical snapshots. They include the current formatting
and lint cleanup. Later work should be committed as it is completed.

## Clang tooling

Clangd uses `.clang-format` for editor formatting. `.clangd` points at `build-lint`:

```sh
cmake -S . -B build-lint -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DEAF_LIBSBC_ROOT=/path/to/libsbc
cmake --build build-lint
python3 tools/check_code.py
ctest --test-dir build-lint --output-on-failure
```

Omit EAF_LIBSBC_ROOT when not testing the optional decoder. The checker runs
clang-format in verification mode, clang-tidy analyzer/bugprone checks and clangd
parsing on project translation units and included headers. It excludes external
vendor/generated sources. Clangd refactoring-tweak self-tests are disabled because
Clang 22 reports replacement-overlap failures unrelated to code diagnostics.

For Zephyr, configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 tools/check_code.py build-zephyr-bt-lms
```

The checker writes an adapted compile database inside the build directory,
removing GCC-only optimization flags and translating `-fno-freestanding` to
Clang's `-fhosted`. It preserves defines and include paths; the normal Zephyr
build still uses its original compiler arguments. To edit Zephyr sources in
clangd, point `.clangd` at `build-zephyr-bt-lms/eaf-clang` locally after generating it.

Clang-tidy excludes the blanket Annex K replacement warning because these
platforms use bounded standard C buffer operations, not optional `_s` APIs.
Reserved POSIX feature macros and linker-wrapper identifiers are allowed by name.
Two local annotations document binary opcode bytes and intentional integer
rounding in the DSP reference test. Other configured warnings remain errors.

C sources, headers and fixture includes use clang-format. It does not format
Python, Markdown, Kconfig or CMake files. Those retain their native syntax.

For future work, choose a task/acceptance gate from `TASKS.md`, consult the audit,
and add an independent failing regression before correcting behavior. CTest's
`architecture` case guards portable source boundaries. Hardware results must be
recorded separately from simulator/host passes. See `docs/audit.md` for the
squeezelite-esp32 reference-test methodology.

## Architecture and backend checks

[Architecture v0.5](architecture-v0.5.md) defines current ownership/lifecycle
contracts. `arch_example` compiles and runs its finite-source example. The native
players link a CMake-selected ALSA/null adapter and have no feature preprocessor
branches. Check the fallback explicitly, even on machines with ALSA installed:

```sh
cmake -S . -B build-no-alsa -DEAF_ENABLE_ALSA=OFF -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-no-alsa
ctest --test-dir build-no-alsa --output-on-failure
python3 tools/check_code.py build-no-alsa
```

`sink_lifecycle` injects partial init, START, write and cleanup failures;
`alsa_failures` injects write/DROP/drain failures and a deterministic drain deadline.
Zephyr smoke repeats write/START/DROP errors beyond the four-slot pool capacity,
then tests EOS drain failure and success. These mocks check software ownership;
ESP32 DMA timing, actual EOF tail and driver starvation recovery remain board gates.
Sink implementers must return checked errors from deinit and preserve a retry-safe
context. Update custom sink operation tables for the new `int` return signature.
