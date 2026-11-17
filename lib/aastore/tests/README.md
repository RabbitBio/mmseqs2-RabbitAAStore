# RabbitAAStore tests

A normal standalone build runs the small environment checks registered in
`smoke.cmake`. The full regression suite is registered in `regression.cmake`
and is disabled by default. This directory's Makefile enables the full suite
through the root CMake configuration.

## Build and run

From the repository root:

```sh
cd tests
make                           # Compile with up to four jobs
make test                      # Build and run all registered tests
make test CTEST_ARGS='-L sequential'
make clean                     # Remove build outputs; retain configuration and reports
```

Executables are placed directly in `tests/`, including `./aastore_test` and
`./aastore_build`. The CMake cache, libraries, and intermediate files are placed
in `tests/build/`. Override `BUILD_DIR` with an absolute path to move those
intermediate files; executable output remains in `tests/`.

This build disables automatic post-build smoke checks to avoid running them
twice. They remain registered with CTest and run as part of `make test`. Test
failures produce a nonzero exit status. Tests requiring host io_uring support
report a skip when it is unavailable; compatibility tests use a private shim
and do not require host io_uring support.

## Build options

| Option | Default | Purpose |
| --- | --- | --- |
| `JOBS` | `4` | Maximum parallel compiler jobs. |
| `BUILD_TYPE` | `Release` | CMake build configuration. |
| `BUILD_DIR` | `tests/build/` | CMake cache, libraries, and intermediate files. |
| `IO_URING` | `ON` | Detect and enable liburing; set `OFF` to test a `pread`-only build. |
| `PYTHON` | `OFF` | Build the Python extension and register Python tests. |
| `CMAKE_ARGS` | Empty | Additional CMake configuration arguments. |
| `CTEST_ARGS` | Empty | Additional CTest arguments, such as `-L smoke`. |

For example, to enable Python tests with a selected interpreter and pybind11
installation:

```sh
make test PYTHON=ON \
  CMAKE_ARGS='-DPython_EXECUTABLE=/path/to/python -Dpybind11_DIR=/path/to/pybind11/cmake'
```

Python tests require the Python development files and pybind11.

## Coverage

The suite covers database construction, random and consecutive reads, text
columns, codec boundaries, option validation, concurrency, io_uring error
handling, installed SDK use, and CMake compatibility. Store tests can be selected
with the `builder`, `random`, `sequential`, `text`, and `concurrency` CTest labels.

CI can enable the suite directly with `-DAASTORE_BUILD_TESTS=ON`, then run
CTest. Existing build directories retain their cached CMake settings. To disable
the full suite in a normal build, configure explicitly with
`-DAASTORE_BUILD_TESTS=OFF`.
