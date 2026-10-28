# RabbitAAStore

RabbitAAStore is a C++17 library with Python bindings for storing and accessing
large protein sequence collections. It converts a protein FASTA file into a
compact database and retrieves individual sequences, arbitrary batches, or
consecutive ranges by sequence ID.

- **Compact storage:** 5-bit encoding with contiguous packing and a compact
  sequence index. Names and descriptions are stored in separate optional columns.
- **Batched access:** range merging, parallel I/O, and SIMD decoding, with results
  returned in the original request order, including repeated IDs.
- **Configurable memory use:** read encoded sequences on demand, preload a fixed
  prefix, or retain the entire encoded sequence collection in memory.
- **Reusable output:** access decoded batches through lightweight views or decode
  directly into caller-provided buffers.

The Python package, C++ namespace, and command-line tools use the name `aastore`.

## Installation

The supported platform is Linux x86_64 with glibc and a C++17-capable GCC or Clang
compiler. C++ builds require CMake 3.16 or later. The portable-wheel configuration
targets standard CPython 3.9–3.13; source builds require Python 3.9 or later.
See the [compatibility guide](docs/releasing.md#compatibility) for the validation
scope and kernel/backend details.

### C++ library and command-line tool

Run from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
cmake --install build --prefix "$HOME/.local"
```

Python is not required for a C++ build. If compatible liburing development files
are available, CMake enables the optional `io_uring` backend; otherwise it builds
with `pread`. To disable liburing detection, add
`-DAASTORE_ENABLE_IO_URING=OFF` to the configure command.

The default standalone build also runs small environment smoke checks. Full
regression tests and benchmarks are separate build options. See
[tests/README.md](tests/README.md) for test commands and
[docs/releasing.md](docs/releasing.md#selecting-tests-and-cross-compiling) for
build-time checks and cross-compilation.

Link the installed library from your own `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)
find_package(aastore 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

Configure your application with `-DCMAKE_PREFIX_PATH="$HOME/.local"`. The
imported target supplies the include paths, C++ standard, and link dependencies.
The [C++ guide](docs/cpp_api.md) also covers shared-library builds and embedding
with `add_subdirectory`.

### Python package

Install from the repository root:

```bash
python -m pip install .
```

A C++17 compiler and Python development headers are required; pip installs the
Python build dependencies in an isolated environment. NumPy is optional.

Python builds use `pread` by default. To enable the optional `io_uring` backend,
install the liburing development package for your system and run:

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

This local build may depend on a system liburing runtime. See the
[Python installation guide](docs/python_api.md#installation) for details.

## Quick start

The reading examples below assume at least three sequences and both text
columns, as provided by the bundled dataset and default construction options.

### Build a database

The repository includes a sample FASTA containing 100,000 protein sequences.
After building the C++ tool, run this command from the repository root:

```bash
./build/aastore_build examples/data/huge_100k.fa proteins.aastore --threads 4
```

Replace the input path with your own uncompressed protein FASTA file as needed.
Sequence IDs follow the input record order, starting at `0`. Names and
descriptions are included by default; `--sequences-only` omits both. Existing
databases are not overwritten unless `--overwrite` is supplied.

The builder normalizes sequences: lowercase letters become uppercase, whitespace
is removed, and characters other than `A`–`Z`, `*`, and `-` become `X`. Reads
return these normalized sequences; original FASTA line wrapping is not stored.

### C++ example

Save this as `main.cpp` and use the CMake configuration above:

```cpp
#include <aastore/store.hpp>
#include <iostream>

int main() {
    aastore::Store store("proteins.aastore");

    auto single = store.get(0);
    std::cout << single[0] << '\n';

    auto batch = store.get({2, 0, 2});
    for (size_t i = 0; i < batch.size(); ++i) {
        std::cout << batch[i] << '\n';
    }

    auto records = store.batch_get_records({0});
    std::cout << records.name(0) << ' '
              << records.description(0) << '\n';
}
```

`get()` always returns a `Batch`, even for a single ID. `batch[i]` returns a
string copy; `batch.view(i)` avoids that copy while the batch remains alive and
its storage is unchanged. `batch_get_records()` reads names, descriptions, and
sequences by default; its field mask can select only the columns you need.

The [standalone C++ example](examples/README.md) can also be built without
installing the library:

```bash
cd examples
make
make run
```

### Python example

Python can open the same database built by the C++ tool:

```python
import aastore

with aastore.Store("proteins.aastore") as store:
    print(store.get(0)[0])
    batch = store.get([2, 0, 2])
    for sequence in batch:
        print(sequence)

    record = store.batch_get_records([0])[0]
    print(record["name"], record["description"], record["sequence"])
```

To build a database entirely from Python, use a new output directory:

```python
import aastore

aastore.build_fasta(
    "examples/data/huge_100k.fa", "proteins-python.aastore", threads=4
)
```

Use `overwrite=True` only when replacing an existing database. Complete examples
for records, buffer views, and reusable output buffers are in
[examples/python](examples/python).

## Memory and I/O options

Set `StoreOptions.preload_bytes` to the maximum number of encoded sequence
bytes to preload. The store selects its residency mode automatically at opening:

| `preload_bytes` | Reported `StorageMode` | Behavior |
| --- | --- | --- |
| `0` (default) | `NoMemory` | Read requested encoded sequences on demand without preloading them. |
| Positive, below `packed_bytes` | `PartialMemory` | Preload a fixed prefix of the encoded sequences; read the rest on demand. |
| Positive, at least `packed_bytes` | `FullMemory` | Preload all encoded sequences and use the full-memory read path. |

Names and descriptions have independent residency options. For example:

```python
import aastore

opening = aastore.StoreOptions(
    preload_bytes=512 * 1024 * 1024,
    resident_names=True,
    resident_descriptions=True,
)
reads = aastore.ReadOptions(threads=4)

with aastore.Store("proteins.aastore", opening) as store:
    batch = store.get([2, 0, 2], reads)
    print(store.storage_mode, store.resident_bytes, store.resident_text_bytes)
```

`preload_bytes` is a limit on encoded sequence preloading, not total process
memory. The store loads at most `packed_bytes`, even with a larger budget.
Indexes, text columns, decoded output, and I/O buffers consume additional memory;
`NoMemory` can also use the operating system page cache. In `PartialMemory`, the
prefix remains fixed for the lifetime of the store. The selected mode is available
through the read-only `store.storage_mode` property; it is not an opening option.

Index loading is independent of sequence residency. The default
`IndexLoadMode.Mmap` maps sequence and text indexes read-only, allowing the
operating system to load pages on demand. `IndexLoadMode.Read` reads the complete
indexes into store-owned buffers when opening the database. Neither option
alone preloads sequence or text contents.

`StoreOptions.threads` controls preloading; `ReadOptions.threads` controls each
read call. Both default to `8`, whereas database construction defaults to one
thread. `IoBackend.Auto` uses `io_uring` when available and falls back to `pread`
when initialization or the required read operation is unsupported. Explicit
`IoBackend.IoUring` reports unavailability as an error. Actual I/O errors are
propagated to the caller. Sequential reads use the dedicated `pread` path.

## Documentation

Start with the [documentation index](docs/README.md), or choose a guide:

| Document | Contents |
| --- | --- |
| [C++ API guide](docs/cpp_api.md) | Construction, reads, field selection, memory settings, buffers, and statistics. |
| [Python API guide](docs/python_api.md) | Installation, options, sequence and record access, buffer views, and lifetimes. |
| [Examples](examples/README.md) | Standalone C++ and Python examples using the bundled dataset. |
| [Benchmarks](benchmarks/README.md) | Benchmark commands, cold-cache runs, and timing definitions. |
| [Tests](tests/README.md) | Build and run the regression suite. |
| [Release guide](docs/releasing.md) | Supported configurations, SDK validation, and version compatibility. |
| [Changelog](CHANGELOG.md) | Changes and API migration notes. |

The package version is defined in [VERSION](VERSION). Package versions and
database format versions are managed separately; an API update does not
necessarily require rebuilding databases.

## License

RabbitAAStore is distributed under the GNU General Public License, version 3
only (`GPL-3.0-only`). See [LICENSE](LICENSE).
