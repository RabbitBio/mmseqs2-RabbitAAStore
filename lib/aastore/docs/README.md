# RabbitAAStore documentation

RabbitAAStore provides C++ and Python interfaces for building compact protein
sequence databases and accessing their contents. The package and namespace are
named `aastore`. Start with the [project README](../README.md) for installation
and a minimal example.

## Guides

| Document | Purpose |
| --- | --- |
| [Project README](../README.md) | Overview, installation, quick start, and memory/I/O configuration. |
| [C++ API](cpp_api.md) | Build databases, retrieve sequences and records, reuse output buffers, and inspect statistics. |
| [Python API](python_api.md) | Install the bindings and use options, batches, record fields, and buffer views. |
| [Examples](../examples/README.md) | Run standalone examples with the bundled 100,000-sequence FASTA. |
| [Benchmarks](../benchmarks/README.md) | Configure read benchmarks and interpret their timing and I/O metrics. |
| [Tests](../tests/README.md) | Build tests, select test groups, and check optional backends. |
| [Release guide](releasing.md) | Validate builds and installed SDKs; understand platform, API, and format compatibility. |
| [Changelog](../CHANGELOG.md) | Review changes and migrate callers of earlier APIs. |

## API overview

Both languages expose the same core operations. See the language guides for
complete signatures and examples.

| Operation | Interface | Result |
| --- | --- | --- |
| Build from FASTA | `build_fasta(...)` | A database directory and construction statistics. |
| Open a database | `Store(path, options)` | A handle configured for sequence, text, and index loading. |
| Read one sequence or arbitrary IDs | `get(id_or_ids, options)` | A `Batch`, preserving request order and duplicate IDs. |
| Read consecutive IDs | `sequential_get(begin, end, options)` | A `Batch` for the half-open interval `[begin, end)`. |
| Select record columns | `batch_get_records(ids, fields, options)` | A `RecordBatch` containing names, descriptions, sequences, or a selected combination. |
| Reuse output buffers | `get_into(...)`, `sequential_get_into(...)` | Decoded sequences written into caller-provided buffers. |
| Inspect sequence size | `sequence_length(id)` | The number of residues, useful for allocating output buffers. |

IDs are zero-based positions in the input FASTA, not FASTA names. Names and
descriptions use the same IDs as sequences. They are optional at construction;
requesting a missing column raises an error.

## Configuration overview

| Configuration | Controls |
| --- | --- |
| `BuildOptions` | Database replacement, optional text columns, and construction threads. Python also accepts these as `build_fasta()` keyword arguments. |
| `StoreOptions` | Sequence residency, text-column residency, index loading, and preloading threads. |
| `ReadOptions` | Per-call threads, I/O backend, range merging, queue/buffer limits, and optional diagnostics. |

These settings are independent: opening an index with `mmap` does not preload
the encoded sequences, and requesting only names does not decode sequences.
The storage modes share one database format, so switching modes does not
require rebuilding the database.

## Source references

Public C++ declarations are in
[`builder.hpp`](../include/aastore/builder.hpp),
[`store.hpp`](../include/aastore/store.hpp), and
[`read_options.hpp`](../include/aastore/read_options.hpp).
Python signatures are declared in
[`__init__.pyi`](../python/aastore/__init__.pyi), with bindings implemented in
[`bindings.cpp`](../python/bindings.cpp).

For database format details and implementation entry points, see the
[C++ API guide](cpp_api.md). For changes between revisions, see the
[changelog](../CHANGELOG.md).
