# Contributing

Thanks for your interest in mth-apclient. This document covers building, testing, and the
conventions the project follows.

## Prerequisites

- CMake >= 3.25, Ninja, and a C++23 compiler (the presets use `clang`/`clang++` (`clang-cl` / `clang++-cl` on Windows)).
- Dependencies are pulled automatically:
  - **vcpkg**: the only git submodule (`external/vcpkg`). It bootstraps itself on first
    configure and provides nlohmann-json and the networking deps (asio, OpenSSL, zlib). The
    Linux overlay's Vulkan and SDL2 headers are
    fetched by CMake, and the Windows overlay's Dear ImGui backends build against the
    D3D12/Win32 headers the platform SDK already ships.
  - **Dear ImGui** (overlay), **Frida-Gum** (Linux hook backend), **MinHook** (Windows hook
    backend), **Catch2** (tests), and the Archipelago client headers (apclientpp / wswrap /
    websocketpp) are fetched at configure time via CMake `FetchContent`.

## Getting the source

```bash
git clone --recurse-submodules https://github.com/Axertin/mth-apclient
cd mth-apclient
# if you forgot --recurse-submodules:
git submodule update --init --recursive
```

## Building

The project is preset-driven. Presets are compiler-led, so `cmake --list-presets` shows only the
ones valid for your host.

### Linux mod (`mod.so`)

```bash
cmake --preset clang-x64-debug      # or clang-x64-release
cmake --build --preset clang-x64-debug
```

### Unit tests

`clang-x64-tests` is `clang-x64-debug` with the unit tests switched on, so it builds the module
too. The tests link `mthap_core` plus a few PAL and module sources, which makes the test lane
Linux-only:

```bash
cmake --preset clang-x64-tests
cmake --build --preset clang-x64-tests
ctest --preset clang-x64-tests --output-on-failure
```

Source and test files are globbed. Drop a new `tests/unit/*_test.cpp` or a source under `src/` in
place and re-run the configure step; there is no list to edit. A test that needs a PAL or module
source the library does not already expose adds it to `target_sources` in `tests/CMakeLists.txt`.

`tests/unit/contract_test.cpp` checks the client's item and location IDs and `parse_slot_data` against `tests/fixtures/contract.json`, which holds the apworld's item and location tables and its `fill_slot_data()` output for the default options and one non-default set. After an ID or slot_data change in the world, regenerate it with `python scripts/gen_contract_fixture.py ../world` (Python 3.12 with `pip install -r ../world/requirements.txt`) and commit the diff.

### Windows mod (`mod.dll`)

The canonical Windows build uses native `clang-cl` with a static CRT (run on Windows or CI):

```bash
cmake --preset clang-cl-x64-release
cmake --build --preset clang-cl-x64-release
```

A LLVM-MinGW cross preset (`mingw-x64-debug`) builds the Windows module from a Linux box, so a
Windows compile error shows up before the change reaches CI. It expects llvm-mingw in
`/opt/llvm-mingw` (override with the `LLVM_MINGW_ROOT` CMake variable):

```bash
cmake --preset mingw-x64-debug
cmake --build --preset mingw-x64-debug
```

It is not the release build: CI and releases use clang-cl. The MinGW-ABI binary it produces is for local use and testing.

## Testing the Linux build

Copy `build/clang-x64-debug/mods/apclient/` (`mod.so` and `mod.yc`) into the game's mods
directory at `~/.local/share/Yacht Club Games/Mina the Hollower/mods/apclient/` (the SDL pref
path, not the install dir). Launch via Steam with the `-mod -mod-allow-code` options. The game loader
writes `~/.local/share/Yacht Club Games/Mina the Hollower/mod.log` with load diagnostics. The
mod's own runtime log is `~/.local/share/mth-apclient/mthap_*.log`.

## Reading a crash report

The mod logs a backtrace on a fatal signal (Linux) or an unhandled exception (Windows), appended to the runtime log above. Windows also writes a minidump next to it. Release builds carry their own debug info, so there is no separate symbol file to match up.

Windows frames come out already named: `mod.pdb` ships beside `mod.dll`, and the handler resolves them in-process.

Linux frames are raw `module(+0xRVA)`, because the handler has to stay async-signal-safe. Resolve them afterwards with `addr2line`, which reads the compressed DWARF in `mod.so` directly:

```bash
# one frame; the offset is the value in the parentheses
addr2line -e mods/apclient/mod.so -f -C 0x4a1c0

# every mod frame in a reported log
grep -o 'mod\.so(+0x[0-9a-f]*' report.log | cut -d+ -f2 |
    addr2line -e mods/apclient/mod.so -f -C
```

Use the `mod.so` from the release the reporter was running, or the offsets land on the wrong lines. The version is in the startup log and the release asset's filename. Game frames resolve the same way against the Linux game binary, which is unstripped.

## Formatting

Formatting is enforced (Allman style, `.clang-format`). Run it before committing:

```bash
bash format.sh
```

...or point git at the tracked hook once per clone, after which every commit is formatted:

```bash
git config core.hooksPath .githooks
```

CI gates merges on formatting with a pinned clang-format version.

## Code layout & the platform boundary

The codebase is split into these targets (see [docs/architecture.md](docs/architecture.md)):

- `mthap_core`: pure, cross-platform logic under `src/mth/core/` (grouped into `ap`, `save`,
  `game`, `rules` and `console`), plus the wrapper over the game's own mod API in `src/mod/`.
  **It must not include platform, OS, or hook-backend headers**, because the unit tests link it
  without the module or a backend. Portable third-party libraries are allowed.
- `mthap`: the final module. It composes core with the composition root (`src/mth/app/`), the
  per-feature game hooks (`src/mth/features/`), the Archipelago link (`src/mth/net/`), the UI
  (`src/mth/ui/`) and the platform abstraction layer (`src/pal/`, with the per-platform code
  under `src/pal/{linux,windows}/`).
- `mthap_imgui`: Dear ImGui, built as an object library and linked into `mthap`.
- `mthap_unit_tests`: the Catch2 tests under `tests/unit/`.

When you add platform-specific behavior, put it behind a PAL function in `src/pal/pal_*.hpp`
rather than `#ifdef`-ing it into the core or the higher-level logic. Per-platform struct offsets
go in `src/pal/pal_layout.hpp`.

See [docs/reverse-engineering.md](docs/reverse-engineering.md) for how game functions are
hooked and resolved on each platform. It covers the native mod hooks, the Frida/MinHook detour
seam, and the Windows signature-table workflow.

## Continuous integration

Pull requests run a formatting check, then the Linux unit tests and the Linux and Windows mod builds
(`ci.yml` calls the reusable `build.yml`). Keep them green. Tagged releases (`v*` on `master`) build
and publish artifacts automatically.

## Commit conventions

- The PR title must follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/),
  since it becomes the squash-merge subject and release-please reads it for the changelog. Types:
  `feat`, `fix`, `perf`, `refactor`, `build`, `ci`, `docs`, `test`, `style`, `chore`. No scope in
  parentheses.
- Commits inside a branch are free to say whatever describes them. Only the title is parsed.
- Keep commit bodies short and to the point.
- Keep history linear and rebase-friendly.

## License

By contributing, you agree that your contributions are licensed under the project's
[MIT License](LICENSE).
