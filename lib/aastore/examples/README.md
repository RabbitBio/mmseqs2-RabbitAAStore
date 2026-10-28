# RabbitAAStore examples

These examples demonstrate database construction, individual and batched reads,
consecutive reads, and access to sequence names and descriptions. They use the
same bundled FASTA file in C++ and Python.

## Example dataset

[`data/huge_100k.fa`](data/huge_100k.fa) contains the first **100,000 complete
records** from `huge.fa`, in their original order. It contains **35,975,590
residues** and occupies **51,154,661 bytes**. Original headers and sequence line
wrapping are preserved. Extraction details are recorded in
[`data/huge_100k.json`](data/huge_100k.json).

## C++

Build and run from the repository root:

```sh
cd examples
make       # Build ./aastore_example with up to four compiler jobs
make run   # Build a database from data/huge_100k.fa and demonstrate reads
```

The Makefile compiles the repository's C++17 sources directly; no SDK installation
or test build is needed. Object files are placed in `.objects/`, and the
executable is placed in this directory.

The first run creates `output/huge_100k.aastore/`. Later runs report that the
database already exists and reuse it. The example preserves duplicate IDs and
request order in batch reads. It prints only the first 60 characters of each
sequence; the database retains each complete sequence.

Run the executable directly or supply another dataset:

```sh
./aastore_example data/huge_100k.fa output/huge_100k.aastore 4
make run DATA=/path/to/proteins.fa STORE=output/my_dataset.aastore THREADS=8
make JOBS=8
```

Use a new `STORE` path when changing datasets: an existing database is reused
without checking whether it matches the supplied FASTA. `THREADS` controls
encoding and the example's random and record reads; `JOBS` controls compilation.
The consecutive-read example omits read options and uses the library defaults.

Random and record reads use `IoBackend::Auto`, which selects io_uring when
available and otherwise falls back to `pread`. The consecutive-read API uses
`pread`. The Makefile accepts three build settings:

| Setting | Behavior |
| --- | --- |
| `IO_URING=AUTO` (default) | Detect usable liburing development files; otherwise build with `pread` only. |
| `IO_URING=ON` | Require usable liburing development files. |
| `IO_URING=OFF` | Build without the io_uring backend. |

`make clean` removes the executable, `.objects/`, and the default
`output/huge_100k.aastore/` database. It preserves the dataset, source files, and
databases created at custom `STORE` paths.

For integration through an installed SDK, use
[`cpp/CMakeLists.txt`](cpp/CMakeLists.txt), which links the `aastore::aastore`
target through `find_package`.

## Python

Install the package and run these commands from the repository root:

```sh
python3 -m pip install .
python3 examples/python/basic.py
python3 examples/python/buffer_layout.py
```

The default Python build uses `pread`. To enable liburing detection during a
source build, install the liburing development files and use:

```sh
python3 -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

Both examples select `IoBackend.Auto`, so they can use either backend. See the
[Python installation guide](../docs/python_api.md#installation) for requirements
and runtime behavior.

- [`python/basic.py`](python/basic.py) demonstrates construction, read options,
  batch ordering, consecutive reads, and selected record fields.
- [`python/buffer_layout.py`](python/buffer_layout.py) demonstrates read-only
  buffer views, byte offsets, reusable output buffers, and view lifetimes.

Each Python example builds a database from the bundled dataset in a temporary
directory and removes it on exit. Neither requires NumPy or additional input
files. The buffer example uses the first four records and sizes output buffers
with `sequence_length()`.
