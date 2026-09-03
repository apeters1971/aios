# Contributing to AIOS

AIOS is experimental. Contributions are welcome, but expect the internals — wire formats,
on-disk layout, the POSIX ABI — to move. Open an issue before starting anything large.

<!-- TODO(owner): choose and add a LICENSE file. The kernel modules under kernel/ declare
     MODULE_LICENSE("GPL"), so the userspace license must be GPL-compatible; until a LICENSE
     exists, contributions are accepted under the assumption that the project owner will
     pick a GPL-compatible license and contributors agree to relicense under it. -->

## Build

Requirements and platform notes are in [README → Build](README.md#build). Short version:

```bash
# Linux
sudo apt-get install -y cmake ninja-build g++ pkg-config libboost-dev libssl-dev libsqlite3-dev libzstd-dev libfuse3-dev
# macOS
brew install cmake boost openssl@3 sqlite zstd isa-l pkg-config

cmake --preset default                 # build/, RelWithDebInfo; Homebrew paths auto-detected on macOS
cmake --build --preset default -j
ctest --preset default
```

`cmake/` holds the pieces that are not the build description itself: `AiosBuildOptions.cmake`
(`AIOS_WERROR`, `AIOS_SANITIZE`), `macos-homebrew.cmake` (prefix discovery) and
`aios_version.hpp.in` (generated into `<build>/generated/include/aios_version.hpp`).

Optional components (`libXrdAios`, `aios-fuse` / `aios-fusell`, ISA-L, zstd, cuObject) are auto-detected and
print one status line each; see the README for `-DXRootD_ROOT=…` and the `AIOS_WITH_*` switches.
Third-party sources are pinned to commit hashes in `CMakeLists.txt`; bump the hash and the
comment together.

## Tests

One GoogleTest binary, `aios_tests`, one CTest entry per `TEST` (serial: tests bind fixed loopback ports).

```bash
ctest --preset default                                     # all
ctest --preset default -R 'HttpApi|S3Iam'                  # regex on test names
./build/aios_tests --gtest_filter='SessionWireC6.*'        # gtest filter (fastest)
./build/aios_tests --gtest_filter='X.*' --gtest_repeat=30 --gtest_break_on_failure   # flakiness hunt
```

Every bug fix needs a regression test. Put it next to the existing tests for that area
(`tests/test_<area>.cpp`; the `test_review2_*.cpp` files hold the tests for the 2026-09 review
findings) and register new files in the `add_executable(aios_tests …)` list.

Timing-dependent tests: assert on the *outcome* (error code / message), keep any elapsed-time
check as a generous upper bound (seconds, not tens of milliseconds), and run the test in a loop
before submitting.

## Sanitizers and CI presets

| Preset | Purpose |
|--------|---------|
| `ci` | `-Werror` on AIOS targets (dependency headers are `SYSTEM`); this is what CI builds |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer, `halt_on_error=1` |
| `tsan` | ThreadSanitizer (currently advisory in CI — known races are being fixed) |

```bash
cmake --workflow --preset asan-ubsan            # configure + build + test
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan --timeout 600
```

On Linux run `sudo sysctl vm.mmap_rnd_bits=28` first (newer kernels' ASLR entropy breaks the
sanitizer runtimes). Each preset uses its own build directory (`build-ci/`, `build-asan/`,
`build-tsan/`), all ignored by git.

CI (`.github/workflows/ci.yml`) runs `linux` and `macos` with the `ci` preset, the two sanitizer
presets, and a `kernel` job that compiles `aios_http.ko` / `aiosfs.ko` / `aiosvd.ko` against
AlmaLinux 9 `kernel-devel` with `KCFLAGS=-Werror`. A PR should be green on `linux`, `macos` and
`asan-ubsan`; `tsan` and `kernel` are `continue-on-error` for now and will become blocking.

## Style

- C++20, `-Wall -Wextra -Wpedantic` clean (CI adds `-Werror`).
- Formatting follows `.clang-format` (Google style, 2-space indent, 100 columns). **Do not
  reformat files you are not otherwise changing** — format only the lines you touch
  (`git clang-format`, or your editor's format-modified-lines). A tree-wide reformat is a
  separate, owner-approved change.
- `.editorconfig` covers indentation for CMake, YAML, Markdown, shell and the kernel C sources
  (kernel code keeps the Linux kernel style: tabs, 80 columns).
- No `using namespace` in headers; keep the `aios::` namespace; prefer `std::string_view` /
  `std::span` over raw pointers on new APIs.
- Comments explain *why*, not *what*. No narrating comments.
- Kernel modules: follow Linux kernel coding style, keep stack frames under 2 KiB
  (`CONFIG_FRAME_WARN`), and build against el9 `kernel-devel` before pushing.
- Protocol or on-disk changes must update the matching `proto/*.md` in the same commit.

## Commit messages

The history uses one-line messages in the imperative that state **what changed and why**,
ending with a period, no type prefixes or scopes:

```
Reuse Session connections and cache POSIX metadata so FUSE I/O is not a new TCP hop and inode PUT on every call.
Keep aios_http auth and txn scratch off the stack so Alma builds stay under 2 KiB frames.
Parse only the digits of aiosvd header integers so remapping an existing volume is not EINVAL.
```

- One logical change per commit; build and tests pass at every commit.
- Reference review finding IDs (`OBJ-3`, `KRN-2`, …) or issue numbers in the body when applicable,
  and add a line to `CHANGELOG.md` under *Unreleased*.
- Do not mix reformatting with functional changes.

## Repository layout

| Path | Owner / content |
|------|-----------------|
| `src/` | Daemon, client, POSIX, S3, XRootD plugin |
| `tools/` | CLI binaries (`aios`, `aios-bench`, `aios-vd`, `aios-kbridge`, …) |
| `kernel/` | AlmaLinux 9 out-of-tree modules + DKMS (`kernel/README.md`) |
| `web/admin/` | Admin SPA (served by `aiosd`) |
| `proto/` | Wire / on-disk / API contracts |
| `tests/` | GoogleTest suite |
| `config/` | Example configs |
| `cmake/` | Build helpers |
| `docs/dev/` | Review audit trail and development statistics |
