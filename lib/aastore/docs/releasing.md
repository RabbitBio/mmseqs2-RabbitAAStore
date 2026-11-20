# Versioning and compatibility

This guide covers platform requirements and version compatibility for RabbitAAStore.
For installation and API examples, start with the
[documentation index](README.md).

[`VERSION`](../VERSION) is the single source for the CMake project version,
Python package version, and Python extension version. The
[version overview](../CHANGELOG.md) summarizes the current feature set.

## Compatibility

RabbitAAStore supports Linux x86_64 with 64-bit
pointers, little-endian byte order, glibc, and a GCC or Clang toolchain supporting
C++17. Other platforms are unverified. Scalar fallback and runtime SIMD
dispatch cover instruction-set differences within supported x86_64 CPUs.

For new deployments, prefer a distribution-maintained LTS kernel based on
Linux 6.1 or later; this is a recommendation, not a minimum version requirement.
The pread backend does not require io_uring,
and the Linux 5.4 READV compatibility path is retained. Consult the
[kernel release information](https://www.kernel.org/category/releases.html)
and the distribution's maintenance policy when choosing a kernel.

Portable wheels target ordinary CPython 3.9–3.13 on Linux x86_64/glibc.
The source package retains `requires-python = ">=3.9"`; newer CPython versions
can be tried from source on the same platform but are outside the current
wheel support range. Other Python implementations and special ABIs are unverified.

At configuration time, CMake compiles and links a C++17 filesystem probe using
the selected toolchain, without running it. It first tries the standard library alone,
then stdc++fs and c++fs. Any required companion library is propagated by the
installed CMake target and pkg-config metadata, including Clang builds using
older libstdc++ versions.

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

## Build settings

Standalone builds run small environment checks automatically; disable them with
`-DAASTORE_RUN_SMOKE_TESTS=OFF`. They are disabled by default for embedded and
cross builds. See the [test README](../tests/README.md) to run the full suite.

From the repository root, select a cross-compilation toolchain with `CMAKE_TOOLCHAIN_FILE`:

```sh
cmake -S . -B build-cross -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake
cmake --build build-cross -j 4
```

Cross-compilation uses compile-and-link capability checks without running target
executables. To run target tests, configure `CMAKE_CROSSCOMPILING_EMULATOR` in
the toolchain; otherwise target runtime tests are disabled.
