# RabbitAAStore version overview

## 0.1.0

RabbitAAStore provides C++ and Python interfaces for compact protein sequence
storage and random access. The C++ namespace and Python package are `aastore`.

- Continuous 5-bit sequence encoding with the compact `AAPCMP3` boundary index.
- Streaming FASTA construction with optional sequence names and descriptions,
  stored separately with `AATEXT1` byte-offset indexes.
- Automatic `NoMemory`, `PartialMemory`, or `FullMemory` residency selection
  from the encoded-sequence `preload_bytes` budget. Index loading and text-column
  residency are configured independently.
- Single and batched sequence access through `get`, consecutive ID access
  through `sequential_get`, and field selection through `batch_get_records`.
  Batched queries preserve request order and repeated IDs.
- Caller-provided output buffers through `get_into` and `sequential_get_into`,
  batch views without additional copying, and index-based sequence and text
  length queries.
- Independent thread budgets for construction, preloading, and retrieval,
  with runtime selection of scalar, AVX2/BMI2, and AVX-512 kernels.
- Batched disk reads with range merging, `pread` and `io_uring` backends,
  automatic I/O fallback, and optional access diagnostics.
- An installable C++17 SDK with the `aastore::aastore` CMake target and pkg-config
  metadata, plus Python bindings with buffer views and type declarations.

See the [documentation index](docs/README.md) for installation and API guides,
and the [compatibility guide](docs/releasing.md) for platform and version compatibility.
