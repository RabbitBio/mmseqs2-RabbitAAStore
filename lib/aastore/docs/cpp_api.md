# C++ API guide

RabbitAAStore builds a database from FASTA and provides sequence and record
access through `aastore::Store`. Three option types control different stages:
`BuildOptions` for construction, `StoreOptions` for opening a database, and
`ReadOptions` for each read operation. Public declarations are in
[store.hpp](../include/aastore/store.hpp),
[builder.hpp](../include/aastore/builder.hpp), and
[read_options.hpp](../include/aastore/read_options.hpp).

For a complete example, run `make run` in [examples/](../examples/README.md).
It uses the included 100,000-record FASTA file, demonstrates unordered and
duplicate requests and consecutive reads, and keeps the generated database.

Commands below run from the repository root. C++ snippets are intended for a
function body; reading examples assume that `proteins.aastore` contains at
least two records. The following headers cover all examples:

```cpp
#include <aastore/store.hpp>
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
```

## Build and link

Use CMake 3.16 or later and a GCC or Clang compiler with C++17 support. Python
is not required for a C++ build; liburing is optional. See the
[main README](../README.md) for a standard build. On x86-64, runtime dispatch
selects AVX-512, AVX2/BMI2, or scalar kernels according to CPU capabilities;
`-march=native` is not required.

| CMake option | Default behavior |
| --- | --- |
| `AASTORE_BUILD_TOOLS` | Enabled for a top-level build; disabled when embedded with `add_subdirectory`. |
| `AASTORE_BUILD_BENCHMARKS` | Disabled; enabled explicitly by the test Makefile and CI. |
| `AASTORE_BUILD_TESTS` | Full regression suite disabled; run `make` in `tests/` to build it. |
| `AASTORE_RUN_SMOKE_TESTS` | Run small environment checks after a default native, standalone build; disabled by default for embedded and cross builds. |
| `BUILD_SHARED_LIBS` | Disabled by default in a top-level build; enable to build a shared library. |
| `AASTORE_ENABLE_IO_URING` | Probe for liburing by default; use the `pread` backend if a usable library is unavailable. |
| `AASTORE_ENABLE_IPO` | Attempt interprocedural optimization for a top-level build, subject to compiler support; disabled by default for GCC before version 9. |
| `AASTORE_BUILD_PYTHON` | Disabled; see the [Python guide](python_api.md) for installation. |

To build and install only the C++ SDK:

```bash
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DAASTORE_BUILD_TOOLS=OFF -DAASTORE_BUILD_BENCHMARKS=OFF \
  -DAASTORE_BUILD_TESTS=OFF -DAASTORE_RUN_SMOKE_TESTS=OFF
cmake --build build-sdk -j 4
cmake --install build-sdk --prefix "$HOME/.local"
```

The installation includes public headers, the library, a CMake package, and
pkg-config metadata. Link the installed library from your `CMakeLists.txt`:

```cmake
find_package(aastore 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

Configure your project with `-DCMAKE_PREFIX_PATH="$HOME/.local"`, or specify
`-Daastore_DIR=/path/to/libdir/cmake/aastore` for a custom library directory.
The imported target supplies the C++ standard, include paths, and link
dependencies. With io_uring enabled, shared-library deployments need a
compatible liburing runtime; static consumers need the development library,
which the installed CMake package locates again.

Alternatively, embed the source directly:

```cmake
add_executable(my_app main.cpp)
add_subdirectory(path/to/RabbitAAStore)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

Each `AASTORE_BUILD_*` option can explicitly override its default. To build and
run the standalone example against an installed SDK:

```bash
cmake -S examples/cpp -B build-example -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build-example -j 4
(cd build-example && ctest --output-on-failure)
```

## Build a database

The command-line builder accepts a plain, uncompressed FASTA file and an output
directory. By default, it stores names and descriptions and refuses to overwrite an existing
database:

```bash
./build/aastore_build proteins.fa proteins.aastore --threads 4
```

Use `--no-names` or `--no-descriptions` to omit either text column, or
`--sequences-only` to omit both. Add `--overwrite` to replace an existing
database. The C++ equivalent is:

```cpp
aastore::BuildOptions options;
options.encode_threads = 4;
const auto stats = aastore::build_fasta("proteins.fa", "proteins.aastore", options);
std::cout << stats.sequences << " sequences, " << stats.residues << " residues\n";
```

| `BuildOptions` field | Default | Meaning |
| --- | --- | --- |
| `overwrite` | `false` | Allow replacement of an existing database. |
| `write_names` | `true` | Store the first whitespace-delimited field of each FASTA header. |
| `write_descriptions` | `true` | Store the remaining header text. |
| `encode_threads` | `1` | Total construction thread budget, including the FASTA parser; must be positive. |

A budget of `1` disables the background pipeline. Larger budgets enable
parallel encoding and output stages. Encoded chunks are written in input
order even if workers finish out of order, so sequence IDs and packed offsets
remain stable. IDs start at `0` and follow FASTA record order; an empty sequence
still receives an ID. Omitting a text column avoids its parsing, writing, and
index construction.

Sequence input is normalized before encoding: lowercase letters become
uppercase, sequence whitespace is removed, `A`–`Z`, `*`, and `-` are retained,
and other characters become `X`. Reads return these normalized sequences,
not the original FASTA line wrapping or letter case.

`BuildStats` reports sequence and residue counts and the sizes of the encoded
sequence, name, and description data. Before returning successfully, the
builder synchronizes temporary files, installs their final names, and
synchronizes the database directory and its parent. The CLI's `build_seconds`
includes these waits; `BuildStats` itself has no timing field.

## Open a database

Sequence data, text data, and indexes have independent loading options.

### Sequence residency

Set `StoreOptions::preload_bytes` to an encoded-sequence preload budget. The
store compares it with `packed_bytes()` at opening and selects the mode:

| `preload_bytes` | Reported `StorageMode` | Behavior |
| --- | --- | --- |
| `0` (default) | `NoMemory` | Plan and read requested encoded byte ranges on demand without preloading them. |
| Positive, below `packed_bytes()` | `PartialMemory` | Preload a fixed prefix of exactly the budgeted bytes. Random requests entirely inside this prefix decode from memory. |
| Positive, at least `packed_bytes()` | `FullMemory` | Preload all encoded sequences. Random batches bypass sorting, range planning, and sequence-file I/O. |

For example, allow up to 512 MiB of sequence preloading and preload the complete
name column:

```cpp
aastore::StoreOptions options;
options.preload_bytes = 512ULL * 1024 * 1024;
options.threads = 4;
options.resident_names = true;
options.resident_descriptions = false;
aastore::Store store("proteins.aastore", options);
std::cout << store.size() << " sequences, " << store.resident_bytes() << " packed bytes\n";
```

`store.storage_mode()` reports the selected mode; `StoreOptions` has no mode
selector. A budget equal to or larger than the encoded data automatically uses
`FullMemory` and allocates only `packed_bytes()` bytes. To request all encoded
data without first checking its size, use `UINT64_MAX` as the budget. Zero selects
`NoMemory` even for an empty database; a positive budget selects `FullMemory`
when there are no encoded bytes.

`preload_bytes` excludes indexes, resident text, decoded output, and I/O buffers.
`NoMemory` means no explicit sequence preloading; it does not disable the
operating system page cache or eliminate other memory use. A partial prefix is
fixed: there is no dynamic admission, eviction, or replacement, and any positive
budget is accepted without a minimum prefix size. `StoreOptions::threads`
defaults to `8` and limits sequence-preload workers, including the calling
thread; `0` is treated as `1`.

### Text residency

`resident_names` and `resident_descriptions` both default to `false`.
`FullMemory` does not automatically preload text. Enabling either option
reads the complete text column into a `Store`-owned buffer at opening;
subsequent requests copy from that buffer without reading its data file.

`resident_text_bytes()` reports loaded text bytes, and `resident_bytes()`
reports loaded encoded sequence bytes. Neither includes indexes or measures
process RSS. `preload_ns()` measures only encoded sequence preloading during
opening, excluding text loading and index initialization.

Opening a database validates and opens all existing text columns and loads
their indexes according to the index option below. Selecting record fields
controls which columns a read retrieves; it does not prevent existing columns
from being opened. To omit a column's data and index files altogether, disable
that column when building the database. Use `has_names()` and
`has_descriptions()` to check which columns exist.

### Index loading

`StoreOptions::index_load_mode` applies to the sequence index (`meta.bin`)
and every existing text index (`names.meta` and `descriptions.meta`):

| `IndexLoadMode` | Behavior |
| --- | --- |
| `Mmap` (default) | Create read-only mappings; the operating system loads index pages as they are accessed. |
| `Read` | Read each complete index into a `Store`-owned memory buffer during opening. |

```cpp
aastore::StoreOptions options;
options.index_load_mode = aastore::IndexLoadMode::Read;
aastore::Store store("proteins.aastore", options);
```

This choice does not change sequence or text residency. `Mmap` avoids an
explicit full-index read at opening, but later index access can incur page
faults. `Read` includes that read in opening time and allocates space for the
complete indexes. Neither mode locks pages in physical memory.

## Read sequences and record fields

| Call | Result |
| --- | --- |
| `get(id, options)` | A `Batch` containing one sequence. |
| `get(ids, options)` | A `Batch` in request order, including duplicate IDs. |
| `get_into(id, string)` | Resize and reuse the caller's string; return `void`. |
| `get_into(ids, outputs, options)` | Decode into caller-provided buffers; return `BatchStats`. |
| `batch_get_records(ids, fields, options)` | A `RecordBatch` containing the selected fields in request order. |
| `name_length(id)` | The stored name's byte count, queried from its index. |
| `description_length(id)` | The stored description's byte count, queried from its index. |

Single and random batch reads use the same `get()` interface:

```cpp
aastore::Store store("proteins.aastore");
const aastore::SequenceId id = 0;
auto single = store.get(id);
std::string scratch;
store.get_into(id, scratch);
std::cout << single.view(0) << '\n' << scratch << '\n';
```

`get(id, options)` and `get(ids, options)` share read options and statistics.
A single request does not need sorting or additional worker threads.
`get({})` explicitly requests an empty batch; `get({id})` requests a
one-element batch.

The single-string `get_into(id, string)` overload has no `ReadOptions`
argument. It avoids creating a `Batch` or background workers and decodes
resident data directly; otherwise, it uses `pread`.
Use `sequence_length(id)` to obtain a sequence's residue count. Valid IDs are
in `[0, store.size())`.

### Random batches and views

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
auto batch = store.get(ids);
for (size_t i = 0; i < batch.size(); ++i) {
    std::cout << ids[i] << '\t' << batch.view(i) << '\n';
}
std::string owned = batch[0];
std::string_view borrowed = batch.view(0);
```

`batch[i]` returns an independent `std::string`; `batch.view(i)` borrows the
batch's output storage without copying. A view remains valid only while that
storage exists and is not modified or reused. A returned batch owns its
output and remains usable after the `Store` is destroyed.

The public `storage`, `offsets`, and `lengths` members expose the buffer layout.
Offsets and lengths are byte counts in the decoded output. Random sequence
batches append a NUL after each sequence, but consecutive sequence batches
and `TextBatch` do not. Use lengths or `view()` consistently.

### Names and descriptions

Use `batch_get_records()` for any combination of name, description, and
sequence fields:

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
if (store.has_names()) {
    const auto fields = aastore::RecordFields::Name | aastore::RecordFields::Sequence;
    auto records = store.batch_get_records(ids, fields);
    for (size_t i = 0; i < records.size(); ++i) {
        std::cout << records.name_view(i) << '\t' << records.sequence_view(i) << '\n';
    }
}
```

Choose `RecordFields::Name`, `Description`, or `Sequence`, or combine flags
with `|`. The default, `All`, requests all three. A single name uses the same
API: `store.batch_get_records({id}, aastore::RecordFields::Name)`.
Unselected columns are not retrieved by the call. Requesting a text column
that was not stored raises an exception.

`RecordBatch::name()`, `description()`, and `sequence()` return copies;
the corresponding `*_view()` methods borrow each column's batch storage.
Column results and statistics are available as `names`, `descriptions`, and
`sequences`. Name and description lengths are measured in bytes.

Use `store.name_length(id)` and `store.description_length(id)` to obtain
`uint64_t` byte counts before reading record fields, for example when planning
a batch's memory budget. Both query the text indexes without reading text
bodies or allocating an output string, regardless of text residency. They
return zero for an empty field and count stored bytes, not Unicode characters.
A missing column raises `std::runtime_error`; an ID outside `[0, store.size())`
in a present column raises `std::out_of_range`.

## Read consecutive IDs

`sequential_get(first, end)` reads the half-open interval `[first, end)`.
For example, scan a database in blocks of at most 1,024 sequences:

```cpp
aastore::Store store("proteins.aastore");
for (aastore::SequenceId first = 0; first < store.size();) {
    const auto end = first + std::min<aastore::SequenceId>(1024, store.size() - first);
    auto block = store.sequential_get(first, end);
    for (size_t i = 0; i < block.size(); ++i) {
        std::cout << first + i << '\t' << block.view(i) << '\n';
    }
    first = end;
}
```

Consecutive reads lay out a contiguous decoded buffer and partition it among
workers. They do not create individual random requests, sort IDs, or invoke
the random range planner. File reads use `pread`, including when
`ReadOptions::io_backend` selects io_uring. An optional third argument sets
read options such as `threads`. Choosing the ID interval controls output
memory per call; applications may arrange their own prefetch of a later block.

## Reuse output buffers

To avoid allocating a library-owned output buffer, decode directly into
caller-provided memory. This random batch example allocates one string for
each request:

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
aastore::ReadOptions options;
options.threads = 4;
std::vector<std::string> sequences(ids.size());
std::vector<char*> outputs(ids.size());
for (size_t i = 0; i < ids.size(); ++i) {
    sequences[i].resize(store.sequence_length(ids[i]));
    outputs[i] = sequences[i].empty() ? nullptr : sequences[i].data();
}
const auto stats = store.get_into(ids, outputs, options);
std::cout << sequences[0] << '\n' << stats.read_bytes << " bytes read\n";
```

Consecutive reads also support caller-provided buffers. Here, `outputs[i]`
corresponds to sequence `first + i`:

```cpp
aastore::Store store("proteins.aastore");
const aastore::SequenceId first = 0;
const auto end = std::min<aastore::SequenceId>(1024, store.size());
std::vector<std::string> sequences(end - first);
std::vector<char*> outputs(sequences.size());
for (size_t i = 0; i < sequences.size(); ++i) {
    sequences[i].resize(store.sequence_length(first + i));
    outputs[i] = sequences[i].empty() ? nullptr : sequences[i].data();
}
const auto stats = store.sequential_get_into(first, end, outputs);
std::cout << stats.requested_sequences << " sequences\n";
```

Both batch `*_into()` methods also accept `outputs.data(), outputs.size()`
and return `BatchStats`. The output count must equal the request count.
Each nonempty output must have at least `sequence_length(id)` writable bytes
and must not overlap another output, even for duplicate IDs. Empty sequences
may use null pointers. These methods do not append NUL terminators and cannot
check the actual capacity behind a raw pointer.

When reusing strings, refresh their pointers after each `resize()` and keep
them valid throughout the read. Internal encoded-data buffers are still
needed. Consecutive reads reuse scratch buffers associated with the calling
thread, retaining up to 64 MiB; larger reads use temporary buffers.

### Concurrent callers

Multiple threads may share an open `Store` if each call has independent
output buffers. Each call has its own worker budget; concurrent calls do not
automatically share a global thread limit. Finish all active reads before
moving or destroying the `Store`. Applications must synchronize access to
shared mutable batches and caller-provided buffers.

## Read options and statistics

| `ReadOptions` field | Default | Meaning |
| --- | --- | --- |
| `threads` | `8` | Maximum user-space workers per call, including the calling thread; `0` means `1`, and small jobs may use fewer. |
| `io_backend` | `IoBackend::Auto` | Select `Auto`, `Pread`, or `IoUring` for random disk reads. |
| `input_sorted` | `false` | Skip sorting when the caller supplies nondecreasing IDs; duplicates are allowed. |
| `merge_gap_bytes` | `4096` | Largest physical gap that may be included when merging reads. |
| `max_merged_range_bytes` | `16384` | Maximum merged range size when combining ranges across a nonzero gap. |
| `max_exact_merged_range_bytes` | `1048576` | Maximum merged range size for overlapping or directly adjacent ranges. |
| `io_queue_depth` | `64` | Target depth per io_uring queue; `0` means `1`. |
| `io_buffer_bytes` | `16777216` | Total in-flight range-buffer budget across queues; the largest range determines a conservative depth limit. |
| `collect_diagnostics` | `false` | Collect read amplification, merge counts, hit/miss residue counts, phase timings, and detailed submit/wait counters. |

For the two merged-range limits and `io_buffer_bytes`, `0` means unlimited.
A merge limit does not split an individual oversized request. The in-flight
budget always permits the slots needed to make progress, so these settings
are not hard limits on process memory.

Sorting, planning, I/O, and decoding use the same `threads` budget; their
thread counts do not add together. Single-string reads, consecutive reads,
and fully resident paths use the behavior described above rather than every
backend and planning option. `BuildOptions`, `StoreOptions`, and `ReadOptions`
also expose `validate()` for explicit configuration checks; entry points
validate their options before performing work.

Enable diagnostics for a particular batch as follows:

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
aastore::ReadOptions options;
options.collect_diagnostics = true;
auto batch = store.get(ids, options);
std::cout << batch.stats.read_bytes << " bytes, " << batch.stats.ranges << " ranges\n";
```

Basic counters remain available with diagnostics disabled.
`requested_sequences` and `requested_residues` count requested output;
`cache_hits` and `cache_misses` describe prefix residency, and `ranges` counts
planned reads. `read_bytes` counts bytes read by the application, not physical
device I/O; the operating system page cache and readahead can affect the latter.

`workers`, `io_workers`, and `decode_workers` describe actual worker usage.
The io_uring backend uses the same workers for I/O and decoding, so those
counts must not be added. `io_submissions` and `io_completions` count submitted
and completed I/O entries. Detailed `io_submit_calls` and `io_wait_calls`
count API calls, while `io_empty_waits` counts waits entered with an empty
completion queue.

When diagnostics are disabled, `cache_hit_residues`, `cache_miss_residues`,
`unique_packed_bytes`, `merged_gaps`, `free_merges`, `overread_bytes`, all
`*_ns` fields in read statistics, and the three detailed API counters above
remain zero. Diagnostic clocks, worker timing arrays, and the extra scan for
unique-byte coverage are skipped. Check `diagnostics_collected` to distinguish
an uncollected metric from a measured zero.

With diagnostics enabled, `overread_bytes` measures bytes read beyond the
required encoded ranges. For io_uring, `read_ns` measures time in wait APIs,
not pure device latency. Summed worker times are not end-to-end elapsed time.
The separate `Store::preload_ns()` measurement is unaffected by the read
options. `TextBatchStats` reports text request sizes, read bytes, ranges,
residency, and backend use; its `unique_bytes`, `merged_gaps`, and
`overread_bytes` also require diagnostics.

## I/O behavior and database format

Random disk reads use sequence IDs to locate encoded ranges, group duplicate
requests, and merge nearby ranges within the configured limits. Reads may
execute in a different order, but decoding places results at their original
request positions. Fully resident random batches decode directly in parallel.

Each io_uring worker owns a queue and decodes completed ranges while other
requests remain outstanding. In `PartialMemory`, those same workers interleave
resident decoding with completion processing; there is no separate decoding
pool. A fully resident batch needs no I/O queue. With `pread`, each worker
synchronously reads and decodes its assigned ranges, and prefix hits are
handled in separate phases.

`Auto` falls back to `pread` if io_uring initialization or a usable read
operation is unavailable. Explicit `IoUring` raises an error instead. Actual
I/O, submission, and decode callback failures propagate to the caller.
The backend prefers `READ` and otherwise uses `READV`, keeping each request's
`iovec` alive until completion. The READV path supports environments without
opcode probing, including Linux 5.4 and older liburing APIs. RabbitAAStore does
not set or query kernel io-wq worker limits; queue depth limits outstanding
requests, not kernel threads.

The on-disk format is versioned independently of the library:

| File | Contents |
| --- | --- |
| `meta.bin` | Compact sequence-boundary index in `AAPCMP3` format, with offsets measured in residues. |
| `packed.bin` | Contiguous 5-bit encoded sequence data. |
| `names.bin`, `descriptions.bin` | Optional text columns without separators. |
| `names.meta`, `descriptions.meta` | Corresponding `AATEXT1` indexes, with offsets measured in bytes. |

Switching sequence residency or index-loading modes does not require rebuilding
the database. Source entry points include `builder.cpp` for construction,
`metadata.cpp` for sequence indexing, `codec.cpp` for runtime-dispatched
encoding and decoding, `byte_range_planner.cpp` for planning,
`io_uring_backend.cpp` for asynchronous reads, and `text_column.cpp` for text
access. `store.cpp`, `store_batch.cpp`, and `store_sequential.cpp` manage
resources, random batches, and consecutive reads, respectively.

See the [changelog](../CHANGELOG.md) for migration from removed interfaces and
the [release guide](releasing.md) for compatibility and release validation.
