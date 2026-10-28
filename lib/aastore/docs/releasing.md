# Versioning and release validation

This guide covers compatibility and release validation for RabbitAAStore.
For installation and API examples, start with the
[documentation index](README.md).

[`VERSION`](../VERSION) is the single source for the CMake project version,
Python package version, and Python extension version. Update it when preparing
a release and record user-visible changes in the [changelog](../CHANGELOG.md).

## Compatibility

RabbitAAStore's current support and validation scope is Linux x86_64 with 64-bit
pointers, little-endian byte order, glibc, and a GCC or Clang toolchain supporting
C++17. This is an advisory support statement. Targets outside this scope remain
unverified, with no compatibility guarantee. Scalar fallback and runtime SIMD
dispatch cover instruction-set differences within supported x86_64 CPUs.

For new deployments, prefer a distribution-maintained LTS kernel based on
Linux 6.1 or later. This is deployment guidance; RabbitAAStore has not established
a tested minimum kernel version. The pread backend does not require io_uring,
and the Linux 5.4 READV compatibility path is retained. Consult the
[kernel release information](https://www.kernel.org/category/releases.html)
and the distribution's maintenance policy when choosing a kernel.

Portable wheels target ordinary CPython 3.9–3.13 on Linux x86_64/glibc.
The source package retains `requires-python = ">=3.9"`; newer CPython versions
can be tried from source on the same platform but are outside the current
wheel support and validation range. Other Python implementations and special
ABIs also remain unverified, with no compatibility guarantee. CI records the
combinations actually checked within this support policy.

At configuration time, CMake compiles and links a C++17 filesystem probe using
the selected toolchain, without running it. It first tries the standard library alone,
then stdc++fs and c++fs. Any required companion library is propagated by the
installed CMake target and pkg-config metadata, including Clang builds using
older libstdc++ versions.

Cross-compiling capability checks link target executables without running them,
including liburing and pthread checks when a toolchain normally uses
STATIC_LIBRARY for try_compile. The relocated SDK test preserves toolchain,
sysroot, compiler flags and emulator lists. Target tests run through the emulator
when configured; otherwise they are disabled and SDK validation covers only
configure/build. Python and benchmark script tests are registered for native
builds only.

The io_uring backend selects READ when supported and otherwise uses READV.
Kernels without opcode probing, including Linux 5.4, use READV and keep each
request's iovec valid until completion. Configure checks distinguish the
baseline READV APIs from optional READ/probe APIs in older liburing releases.
Auto falls back to pread on unavailable initialization or known unsupported
opcodes; actual I/O and callback errors propagate. A static SDK requires a
matching liburing structure layout on the consumer host, and the probe API
when used by the SDK. CMake rejects incompatible layouts before linking;
use a matching liburing build or rebuild the SDK for the destination.
Redistributed shared binaries still need a compatible liburing runtime;
the static CMake SDK checks do not inspect a shared library replaced later
on the destination machine. Portable Python wheels avoid this dependency
by building with io_uring disabled.

During the 0.x series, minor versions may change the public API. Patch versions
are intended to preserve source compatibility within the same minor series.
The installed CMake package accepts compatible versions from that minor series.
The shared-library SONAME includes both the major and minor version, so a new
minor version does not silently replace the previous ABI.

RabbitAAStore does not promise binary interchangeability across different C++
standard libraries, compiler ABIs, or build options. Rebuild applications when
changing one of these. Changes to exposed option, statistics, or batch layouts
must be reviewed as ABI changes even if the code still compiles.

Database formats have their own identifiers: `AAPCMP3` for sequence metadata
and `AATEXT1` for optional text columns. Package version changes do not by
themselves require a database rebuild. A format change must document whether
existing data remains readable and how to migrate it.

## Selecting tests and cross-compiling

Normal builds register only the environment smoke checks. The default
standalone build runs them automatically using small temporary datasets. They
cover construction, single- and multithreaded reads, residency modes, text
columns, and backend selection. Python-enabled builds also check importing and
using the extension. These checks need neither external datasets nor root
access. An unavailable io_uring backend is explicitly reported as skipped;
the automatic fallback path must still pass.

Set `-DAASTORE_RUN_SMOKE_TESTS=OFF` to disable automatic post-build checks.
They are disabled by default for embedded builds and cross-compilation.
Building only `--target aastore` or building a Python wheel does not run them.
To rerun registered smoke checks, use `(cd build && ctest -L smoke -V)`.

To compile the full suite, enter `tests/` and run `make`; executables go directly into `tests/`,
while intermediate files and the CMake cache stay in `tests/build/`.
`make test` then runs all registered tests. See the
[test README](../tests/README.md) for Makefile options. Store tests have separate
`builder`, `random`, `sequential`, `text`, and `concurrency` labels; `options`
covers configuration validation. The `compatibility` label covers legacy kernel
and liburing paths through a private shim, plus CMake cross-compilation checks,
without requiring host io_uring support.

```sh
cd tests
make
make test
make test CTEST_ARGS='-L sequential'
make test CTEST_ARGS='-L compatibility'
./aastore_test --group text
```

From the repository root, select a cross-compilation toolchain with `CMAKE_TOOLCHAIN_FILE`:

```sh
cmake -S . -B build-cross -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake -DAASTORE_BUILD_TESTS=ON
cmake --build build-cross -j 4
(cd build-cross && ctest --output-on-failure)
```

Set `CMAKE_CROSSCOMPILING_EMULATOR` in the toolchain when target execution is
available. As described above, without an emulator the target runtime tests
are disabled, while the installed SDK still receives configure/build validation.

## Before a release

1. Run the C++ CI matrix for static/shared libraries with io_uring enabled and
   disabled. An io_uring test may explicitly skip when the host kernel blocks
   the API; the pread path must still pass. The `compatibility` tests use a
   private shim and must pass without needing host io_uring support.
2. Install the SDK into a fresh prefix, move that prefix, and build and run
   `tests/install_consumer` and `examples/cpp` through `find_package`.
3. Build an sdist, unpack it outside the checkout, build a wheel from that
   source tree, install the wheel into a clean environment, and run the Python
   tests. Check that type declarations are present.
4. Use the portable-wheel workflow for redistributable Linux wheels. It
   disables io_uring, uses pread, and repairs the wheel through auditwheel.
   A local source build can enable io_uring; that wheel may require liburing
   on the destination machine and must not be presented as a portable wheel.
5. Review the changelog, version, license metadata, and artifact contents.
   The source distribution must exclude local build trees and paper assets.
   Create release tags and publish artifacts only as an explicitly authorized
   release action.

The workflows validate and upload build artifacts. They do not publish to
PyPI or create releases automatically.
