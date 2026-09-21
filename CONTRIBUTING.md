# Contributing

Thanks for your interest. This is a C11 embedded audio framework targeting
Zephyr, with Linux as its test harness. Contributions that keep the design
contracts intact and add tests are welcome.

## Getting started

Requirements:

- CMake 3.20+ and Ninja
- A C11 compiler, pthreads, libm
- glibc with `sem_clockwait` (Linux 2.30+)
- Python 3 (optional tests and tooling)
- Clang 22 (`clang-format`, `clang-tidy`, `clangd`) for the lint check

Build and run the host tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the static checks against a compilation database:

```sh
cmake -S . -B build-lint -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-lint
python3 tools/check_code.py build-lint
```

Sanitizer builds (Address+UB, and Thread) are described in the
[development workflow](docs/development.md).

## Before opening a pull request

- Format owned C sources and headers with the repository's `.clang-format`.
- Preserve the architecture constraints:
  - No `#if defined(CONFIG_*)` in owned code; use Kconfig values and
    CMake-selected source files.
  - Keep embedded allocations bounded and static; the audio path allocates no
    heap memory.
  - One producer and one consumer per reservoir; node callbacks must not change
    sample rate, block length or storage.
- Add or update a host regression for any behavior change.
- Update `PLAN.md`, `TASKS.md` and the relevant docs when scope changes.
- Record hardware results separately from host and simulator passes.

## Hardware testing

- Never commit Wi-Fi credentials or local server settings. Use the git-ignored
  `credentials.local.h` and `local.conf` files (see the board README).
- Put bench procedures and results in `docs/bench/`, including the board,
  firmware revision and observed counters.
- Redact device MACs, server identities and LAN addresses before publishing.

## Commit style

- Use focused commits with a scope, for example `fix(sendspin): ...`,
  `feat(board): ...`, `docs: ...` or `chore: ...`.
- Describe the resulting behavior and the verification you ran.
- Do not commit build output or external dependency checkouts.

## Reporting issues

Include the board revision, firmware revision, and the relevant web `/logs` or
UART output, with personal identifiers redacted.

## License

By contributing you agree that your contributions are licensed under the
project's [GPL-3.0 license](LICENSE).

## AI-assisted development

Parts of this project were developed with AI assistance (DeepSeek and GPT
Astra). Every generated change is reviewed, built and tested by the maintainer
before it is committed.
