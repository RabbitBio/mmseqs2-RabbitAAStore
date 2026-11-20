# RabbitAAStore tests

This directory's Makefile enables the full test suite through the root CMake
configuration.

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

Tests requiring host io_uring support report a skip when it is unavailable.

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

For an existing CMake build, enable the suite with
`-DAASTORE_BUILD_TESTS=ON`, then run `ctest --output-on-failure` in the build
directory. Set `-DAASTORE_BUILD_TESTS=OFF` to disable it again.
