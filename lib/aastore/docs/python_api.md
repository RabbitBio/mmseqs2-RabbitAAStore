# Python API guide

RabbitAAStore's `aastore` package uses the same C++ core as the C++ API for
building databases and reading sequences. It supports individual IDs, arbitrary
batches, consecutive ID ranges, and optional name and description fields.
Results offer both Python strings and read-only byte views. The package includes
`.pyi` declarations and `py.typed` for editors and type checkers.

- [Installation](#installation)
- [Quick start](#quick-start)
- [Options](#options)
- [Reading sequences and records](#reading-sequences-and-records)
- [Buffer layout and NumPy](#buffer-layout-and-numpy)
- [Reusing output buffers](#reusing-output-buffers)
- [Statistics, errors, and lifetimes](#statistics-errors-and-lifetimes)

## Installation

From the repository root, install the package with:

```bash
python -m pip install .
```

A source build requires a C++17 GCC or Clang compiler and Python development
headers. Pip supplies the remaining build dependencies in an isolated build
environment. NumPy is not required.

The current supported platform is Linux x86_64 with 64-bit pointers,
little-endian byte order, and glibc. The portable-wheel configuration targets
standard CPython 3.9–3.13. The source package declares Python 3.9 or later;
newer versions can be tried from source but are outside the current wheel
validation scope. Other platforms, Python implementations, and special ABIs
have not been validated.

### Optional io_uring backend

Python builds use `pread` by default, including the portable-wheel workflow.
To enable io_uring in a local source build, install the liburing development
package and run:

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

This enables liburing detection. If a usable development library is found,
the build includes io_uring; otherwise it uses `pread`. At runtime,
`IoBackend.Auto` selects io_uring when available and falls back to `pread`
when backend initialization is unavailable.

A build linked to a shared system liburing also needs a compatible runtime
library on the destination machine. Runtime backend fallback cannot handle a
missing shared library that prevents the Python extension from importing.
Python wheels contain the Python package, not the C++ SDK. See the
[release guide](releasing.md) for packaging and compatibility details.

### Building the bindings with CMake

A normal C++ build does not require Python or pybind11. To enable the bindings
explicitly, run:

```bash
python -m pip install 'pybind11>=3.0,<4'
cmake -S . -B build-python -DAASTORE_BUILD_PYTHON=ON \
  -DPython_EXECUTABLE="$(python -c 'import sys; print(sys.executable)')" \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build-python -j 8
(cd build-python && ctest --output-on-failure)
```

CMake 3.16 or later is supported. Versions 3.16 and 3.17 use the Python
`Development` component and require development headers and libraries;
newer versions use `Development.Module`. For nonstandard Python installations,
set `Python_ROOT_DIR`, or supply `Python_INCLUDE_DIR` and `Python_LIBRARY`
as needed.

## Quick start

The following example creates a small FASTA file, builds a database, and reads
sequences and complete records. Its temporary files are removed on exit.

```python
from pathlib import Path
from tempfile import TemporaryDirectory
import aastore

with TemporaryDirectory(prefix="aastore-guide-") as temporary:
    root = Path(temporary)
    fasta = root / "input.fasta"
    fasta.write_text(
        ">alpha Example protein\nACDEFG\n"
        ">empty Empty sequence\n\n"
        ">beta Another protein\nWY\n",
        encoding="utf-8",
    )
    built = aastore.build_fasta(fasta, root / "store", threads=2)
    with aastore.Store(root / "store") as store:
        print(built.sequences, store[0])  # 3 ACDEFG
        print(list(store.get([2, 0, 2])))  # ['WY', 'ACDEFG', 'WY']
        print(store.batch_get_records([0])[0])
```

`build_fasta(fasta, store, ...)` accepts strings or `pathlib.Path` objects.
Its input is an uncompressed FASTA file and its output is a database directory.
Sequence IDs start at zero and follow FASTA record order; empty sequences also receive an
ID. The name is the first field of the FASTA header, and the description is
the text that follows it. Both columns are saved by default and can be
omitted independently.

Sequence input is normalized during construction: lowercase letters become
uppercase, ASCII whitespace is removed, `A`–`Z`, `*`, and `-` are retained,
and other characters become `X`. Reads return these normalized sequences;
the database does not preserve the original FASTA line wrapping or sequence
letter case.

For longer examples, run the scripts provided with the repository:

```bash
python examples/python/basic.py
python examples/python/buffer_layout.py
```

Both use the bundled [100,000-record FASTA file](../examples/README.md),
build a database in a temporary directory, and clean up afterward.
`basic.py` covers sequence and record access. `buffer_layout.py` demonstrates
byte views, a batch with repeated IDs, and reusable output buffers. Neither
requires NumPy.

The snippets below assume `import aastore`, an existing database directory
named `path`, and at least three records where IDs `0`, `1`, and `2` are used.
Arguments named `reads` are optional `ReadOptions` objects; omit them to use
the defaults.

## Options

The three option classes have keyword-only constructors and mutable fields.
Build, open, and read operations copy their options, so changing an option
object does not change a native call already in progress.

Each class provides `validate()`. It returns `None` for valid settings and
raises `ValueError` for invalid settings without changing the object.
Public operations validate again, including empty requests and empty ranges.
Unknown index-loading modes and I/O backends are rejected. Build threads must
be positive. `preload_bytes` accepts integers from `0` through `2**64 - 1`;
the binding rejects values outside this range.

### BuildOptions

| Field | Default | Description |
| --- | --- | --- |
| `threads` | `1` | Total build thread budget; corresponds to C++ `encode_threads`. |
| `overwrite` | `False` | Allow rebuilding an existing database. |
| `write_names` | `True` | Store sequence names. |
| `write_descriptions` | `True` | Store sequence descriptions. |

```python
options = aastore.BuildOptions(threads=2, write_descriptions=False)
built = aastore.build_fasta(fasta, path, options, threads=4)
# This build uses four threads; options.threads remains 2.
```

The `threads`, `overwrite`, `write_names`, and `write_descriptions` keyword
arguments to `build_fasta()` override the corresponding fields without
modifying the supplied `BuildOptions`. Existing databases are not overwritten
unless explicitly requested. The returned `BuildStats` has read-only
`sequences`, `residues`, `packed_bytes`, `name_bytes`, and `description_bytes`
fields. Result and statistics classes are returned by library calls and
cannot be constructed directly in Python.

### StoreOptions

| Field | Default | Description |
| --- | --- | --- |
| `preload_bytes` | `0` | Maximum encoded sequence bytes to preload; the store selects its residency mode automatically. |
| `threads` | `8` | Thread budget for opening and preloading; zero is treated as one. |
| `resident_names` | `False` | Preload the complete name column. |
| `resident_descriptions` | `False` | Preload the complete description column. |
| `index_load_mode` | `IndexLoadMode.Mmap` | Map indexes read-only; `Read` loads them completely into owned memory. |

The store compares `preload_bytes` with its encoded sequence size (`packed_bytes`)
at opening. The resulting mode is exposed as the read-only `store.storage_mode`:

| Budget | Reported `StorageMode` | Behavior |
| --- | --- | --- |
| `0` | `NoMemory` | No explicit sequence preloading; read requested bytes on demand. |
| Positive, below `packed_bytes` | `PartialMemory` | Preload a fixed prefix of the budgeted size and read the rest on demand. |
| Positive, at least `packed_bytes` | `FullMemory` | Preload all encoded bytes and use the full-memory read path. |

The budget limits encoded sequence bytes, not total process memory. Indexes,
text columns, decoded output, and I/O buffers are separate. `NoMemory` still
allows operating system page caching. A partial prefix is not replaced or
evicted dynamically, and there is no minimum positive budget. The resident data
remains encoded, not decoded Python strings.

A larger budget loads only `packed_bytes` bytes; use `preload_bytes=2**64 - 1`
to request all encoded data without first querying its size. Zero selects
`NoMemory` even for an empty database, while a positive budget selects
`FullMemory` when there are no encoded bytes. `StoreOptions` no longer accepts
`storage_mode` or `cache_bytes`; update callers to pass `preload_bytes`.

```python
opening = aastore.StoreOptions(
    preload_bytes=64 * 1024 * 1024,
    threads=2,
    resident_names=True,
)
with aastore.Store(path, opening) as store:
    print(store.storage_mode, store.resident_bytes, store.resident_text_bytes)
```

Enabling either text residency option loads that column's complete data file
into a buffer owned by `Store`. Later reads copy from the buffer without
reading the text data file again. `resident_text_bytes` reports the combined
size of these buffers; it excludes indexes and does not represent process RSS.

Index loading is independent of sequence and text residency.
`index_load_mode` controls `meta.bin` and any `names.meta` and
`descriptions.meta` files. `Mmap` creates read-only file mappings whose pages
are accessed on demand; `Read` reads the complete indexes into buffers owned
by `Store` during opening.

```python
opening = aastore.StoreOptions(index_load_mode=aastore.IndexLoadMode.Read)
with aastore.Store(path, opening) as store:
    print(store.index_load_mode)
```

### ReadOptions

| Field | Default | Description |
| --- | --- | --- |
| `threads` | `8` | Maximum user-space worker count for this read, including the calling thread. |
| `io_backend` | `IoBackend.Auto` | Select `Auto`, `Pread`, or `IoUring`. |
| `input_sorted` | `False` | Promise that request IDs are in nondecreasing order, allowing internal sorting to be skipped. |
| `merge_gap_bytes` | `4096` | Maximum gap in bytes that a merged read may span. |
| `max_merged_range_bytes` | `16384` | Maximum merged range size when spanning a nonzero gap. |
| `max_exact_merged_range_bytes` | `1048576` | Maximum range size when merging without reading extra bytes. |
| `io_queue_depth` | `64` | Maximum queue depth per io_uring worker. |
| `io_buffer_bytes` | `16777216` | Aggregate in-flight read-buffer budget across io_uring rings. |
| `collect_diagnostics` | `False` | Collect additional coverage, merge, timing, and submit/wait statistics. |

Set `input_sorted=True` only when IDs are nondecreasing, for example
`[0, 0, 2]`. The library does not check this promise; leave it disabled for
requests such as `[2, 0]`.

A zero value for either merged-range limit removes that size limit.
`io_buffer_bytes=0` removes the buffer-budget limit on queue depth, while
`io_queue_depth` and the number of requests still apply. In contrast,
`merge_gap_bytes=0` permits no extra gap between merged ranges. Range-size
limits do not split a single long sequence. Zero `threads` or
`io_queue_depth` is treated as one.

`Auto` uses io_uring when supported by the build and runtime environment,
otherwise falling back to `pread` when backend initialization or supported
read operations are unavailable. Explicit `IoUring` requests raise an
exception if unavailable. Actual I/O and callback errors propagate to the
caller rather than triggering fallback. The io_uring backend selects `READ`
when supported and otherwise uses `READV`, including the compatibility path
for kernels without opcode probing. Reads served from resident sequence data
do not need disk I/O.

`sequential_get()` and `sequential_get_into()` use the sequential `pread`
path for disk reads regardless of `io_backend`.

```python
reads = aastore.ReadOptions(threads=2, collect_diagnostics=True)
```

## Reading sequences and records

`Store` supports context managers, `len(store)`, and integer indexing.
`store[-1]` uses Python's negative-index convention. In contrast, sequence IDs
passed to `get()`, `sequence_length()`, `name_length()`, `description_length()`,
and batch methods must be nonnegative.

`store[id]` uses the direct single-string path, decoding resident data from
memory or reading nonresident data with `pread`. Use `store.get(id, reads)`
to select read options, including the I/O backend, for a single request.

| Call | Result | Purpose |
| --- | --- | --- |
| `store.get(id, reads)` | `Batch` | One sequence, accessed with `batch[0]` or `batch.view(0)`. |
| `store[id]` | `str` | A string copy of one sequence. |
| `store.sequence_length(id)` | `int` | Sequence length, useful for allocating output buffers. |
| `store.name_length(id)` | `int` | Stored name byte count, queried from its index without reading text. |
| `store.description_length(id)` | `int` | Stored description byte count, queried from its index without reading text. |
| `store.get(ids, reads)` | `Batch` | Arbitrary requests, preserving order and repeated IDs. |
| `store.sequential_get(begin, end, reads)` | `Batch` | The half-open ID interval `[begin, end)`. |
| `store.batch_get_records(ids, fields, reads)` | `RecordBatch` | Selected record fields in request order. |

`name_length()` and `description_length()` can help budget a record batch before
reading its fields. They query the text indexes without reading or decoding
the text bodies, whether or not those bodies are resident in memory. Empty
fields return zero. The result counts stored bytes, so it can differ from the
number of characters in a decoded Python string. A missing column raises
`RuntimeError`. For a present column, an ID representable as `uint64` but at
least `len(store)` raises `IndexError`; negative IDs and values above
`2**64 - 1` are rejected by the binding with `TypeError`. A closed store raises
`ValueError`, as with `sequence_length()`.

### Batch and TextBatch

`Batch` holds decoded sequences; `TextBatch` holds names or descriptions.
Both support:

- `len(batch)`, `batch.size()`, and `batch.empty()`;
- integer indexing, including negative indices, returning a `str` copy;
- iteration, creating strings as entries are visited;
- slicing, returning a list of strings, for example `batch[::-1]`;
- `view(i)`, returning a read-only byte `memoryview` without copying;
- `storage`, `offsets`, and `lengths`, exposing the complete batch layout;
- `stats`, exposing read-only statistics.

```python
with aastore.Store(path) as store:
    batch = store.get([2, 0, 2])
    print(batch[0], list(batch), batch[-2:])
    raw = batch.view(0)
    print(raw.readonly, bytes(raw))
```

Sequences contain ASCII bytes. Names and descriptions are decoded as UTF-8
when accessed as strings. Invalid UTF-8 raises `UnicodeDecodeError`; raw
bytes remain accessible through `TextBatch.view(i)`. Unlike `view(i)`,
`bytes(view)` and string access create copies.

### Selecting record fields

Combine `RecordFields.Name`, `Description`, and `Sequence` with `|`.
The default is `RecordFields.All`. Requesting only names or descriptions
avoids sequence decoding. Requested text columns must have been stored when
the database was built.

```python
with aastore.Store(path) as store:
    fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
    records = store.batch_get_records([2, 0], fields)
    print(records[0])  # A dictionary containing only name and sequence.
    print(records.name(0), records.sequence(0))
    names = records.names          # TextBatch
    sequences = records.sequences  # Batch
```

For name-only access, use the same interface:

```python
with aastore.Store(path) as store:
    names = store.batch_get_records([2, 0], aastore.RecordFields.Name).names
    print(list(names))
```

`RecordBatch` supports length, negative indexing, iteration, and slicing.
Items are dictionaries; slices are lists of dictionaries. `fields` records
the requested columns. The `names`, `descriptions`, and `sequences`
sub-batches also provide byte views. Calling `name()`, `description()`, or
`sequence()` for an unrequested field raises an exception; check `fields`
before choosing a column.

The enums are exported by pybind11. They expose `.name`, `.value`, and
`__members__`, but do not support Python `enum.Enum` iteration or class
subscription by a string name.

## Buffer layout and NumPy

`Batch` and `TextBatch` expose three read-only, one-dimensional memoryviews:

| Property | Element type | Description |
| --- | --- | --- |
| `storage` | Unsigned byte | The batch's contiguous output buffer. |
| `offsets` | Native-endian `uint64` | Each entry's starting byte offset in `storage`. |
| `lengths` | Native-endian `uint64` | Each entry's byte length, excluding any NUL terminator. |

`len(offsets) == len(lengths) == len(batch)`. Recover entry `i` with:

```python
storage = batch.storage
offsets = batch.offsets
lengths = batch.lengths
i = 0
raw = storage[offsets[i]:offsets[i] + lengths[i]]
assert bytes(raw) == bytes(batch.view(i))
```

Offsets and lengths are always measured in bytes. Text lengths count UTF-8
bytes, not Python characters. `get()` and the sequence column returned by
`batch_get_records()` append a NUL byte after each sequence in all residency
modes. `sequential_get()` concatenates sequences without terminators, and
text batches also omit terminators. Use `offsets` and `lengths` to locate
entries rather than interpreting the entire buffer as one string or
inferring boundaries from NUL bytes. These properties describe decoded
batch data, not the database's 5-bit storage format.

If NumPy is already installed, it can borrow these buffers directly:

```python
import numpy as np  # Optional; required only for this example.

offsets = np.frombuffer(batch.offsets, dtype=np.uint64)
lengths = np.frombuffer(batch.lengths, dtype=np.uint64)
storage = np.frombuffer(batch.storage, dtype=np.uint8)
first = storage[int(offsets[0]):int(offsets[0] + lengths[0])]
```

The native byte order of `np.uint64` matches the layout views. These arrays
share the original read-only storage. Call `.copy()` to obtain an independent,
writable array. Layout views are an in-memory interface, not a portable disk
serialization format.

## Reusing output buffers

`get_into()` decodes directly into caller-provided writable byte buffers,
such as `bytearray` objects or contiguous writable memoryviews. Python strings
are immutable, and the C++ pointer and mutable-string overloads are not
exposed directly. The output buffers must already be large enough; the
library does not resize them.

```python
with aastore.Store(path) as store:
    output = bytearray(store.sequence_length(1))
    store.get_into(1, output)
    print(output.decode("ascii"))

    ids = [2, 0, 2]
    outputs = [bytearray(store.sequence_length(i)) for i in ids]
    stats = store.get_into(ids, outputs)
    print(outputs[0].decode("ascii"), stats.requested_sequences)
    # Reuse the buffers for another request that fits their capacities.
    store.get_into(ids, outputs)
```

`sequential_get_into(begin, end, outputs, reads)` follows the same conventions,
with one output for each ID in `[begin, end)`. Both direct-output methods
return `BatchStats`, avoid constructing an output `Batch`, and do not append
NUL bytes.

Each output must satisfy the following requirements:

- Supply one output per request. Empty sequences may use `None` or an empty buffer.
- Buffers must be writable, one-dimensional, contiguous, and have one-byte elements.
- Capacity must be at least the corresponding sequence length, and the regions
  actually written must not overlap.

Only the first `sequence_length(id)` bytes are overwritten. If a larger buffer
is reused for a shorter sequence, slice to the new length before consuming the
result. The library checks IDs, output counts, capacities, layouts, and overlap
before starting writes.

Native reads release the GIL. Do not access the same output region from another
thread during a call. The buffer protocol pins the memory against resizing but
does not prevent concurrent writes to its contents. Ordinary batch results own
separate storage from these caller-provided buffers.

## Statistics, errors, and lifetimes

### Statistics and runtime information

`BuildStats` describes a completed build. `Batch.stats` and the direct-output
methods provide `BatchStats`; `TextBatch.stats` provides `TextBatchStats`.
These statistics are read-only.

| Object | Common fields |
| --- | --- |
| `BuildStats` | `sequences`, `residues`, `packed_bytes`, `name_bytes`, `description_bytes` |
| `BatchStats` | `requested_sequences`, `requested_residues`, `cache_hits`, `cache_misses`, `read_bytes`, `ranges`, `io_uring` |
| `TextBatchStats` | `requested_strings`, `requested_bytes`, `read_bytes`, `resident`, `io_uring` |

`BatchStats.diagnostics_collected` indicates whether extra diagnostics were
enabled. With the default `collect_diagnostics=False`, residue-level hit/miss
counts, `unique_packed_bytes`, `merged_gaps`, `free_merges`, `overread_bytes`,
phase timings, and submit/wait diagnostic counters remain zero. Text-column
`unique_bytes`, `merged_gaps`, and `overread_bytes` follow the same switch.
These zeros mean "not collected," not measured zero cost. Disabling diagnostics
also skips the extra coverage-counting pass and its worker startup.

Basic request counts, sequence hit/miss counts, read bytes, range/worker/queue
counts, and SQE/CQE counts are still recorded. With diagnostics enabled,
`layout_ns`, `range_plan_ns`, `read_ns`, `decode_ns`, and related fields report
nanoseconds. For io_uring, `io_workers` and `decode_workers` describe the same
worker group and must not be added together.

`Store` also exposes read-only `total_residues`, `packed_bytes`, `storage_mode`,
`index_load_mode`, `resident_bytes`, `resident_text_bytes`, `preload_ns`,
`has_names`, `has_descriptions`, and `decode_kernel_name` properties.
Use `aastore.__version__` for the package version. See the
[type declarations](../python/aastore/__init__.pyi) for complete signatures
and statistics fields.

### Errors

| Exception | Common causes |
| --- | --- |
| `IndexError` | Out-of-range IDs or indices; invalid sequential intervals. |
| `ValueError` | Invalid options, a closed Store, or incorrect output counts, capacities, layouts, or overlapping writes. |
| `TypeError` / `BufferError` | Invalid argument types or buffers that do not support writable byte access. |
| `RuntimeError` | Database or build failures, missing text columns or requested fields, or unavailable explicit io_uring requests. |
| `UnicodeDecodeError` | A name or description is not valid UTF-8; use a byte view to access it. |

Do not treat complete exception messages as a stable API. Correct invalid
arguments before retrying. An actual I/O failure does not guarantee rollback
of bytes already written into caller-provided buffers.

### Store and result lifetimes

Prefer `with aastore.Store(path) as store:` to close the database automatically.
`close()` is idempotent, and `closed` reports its state. New reads after closure
raise `ValueError`. Calls already in progress retain their own resource
references and can finish. The main native work in building, opening, reading,
and decoding releases the GIL, allowing concurrent read-only calls to a shared
Store from Python threads.

`Batch` and `TextBatch` own their decoded output and remain valid after the
Store closes. Views from `view(i)`, `storage`, `offsets`, and `lengths` retain
their owning batch. A `RecordBatch` sub-batch retains the parent record object.
Deleting a local batch variable therefore does not invalidate a live view:

```python
with aastore.Store(path) as store:
    view = store.get([0]).view(0)
    offsets = store.get([0]).offsets
# Each view retains its temporary batch, although the Store is now closed.
print(bytes(view), offsets[0])
```

Even a small slice retains the entire batch allocation. To release a large
batch sooner, copy the required bytes with `bytes(view)` and drop all related
views. Caller-provided output buffers remain owned by the caller; reusing them
overwrites their previous contents.
