# Changelog

This file records user-visible changes to RabbitAAStore. Package and API names
remain `aastore`.

## Unreleased

- Use RabbitAAStore consistently as the project and display name. Keep the
  `aastore` package, APIs, commands, build options, and database formats unchanged.
- Add `Store::name_length(id)` and `description_length(id)`, also available in
  Python, to query stored text byte counts from indexes without reading text
  bodies. Empty fields return zero; missing columns and out-of-range IDs raise
  exceptions. Existing databases need no conversion.
- Replace `StoreOptions::cache_bytes` with `preload_bytes` and remove its
  `storage_mode` selector in C++ and Python. The store now selects `NoMemory`
  for zero, `PartialMemory` for a positive budget below the encoded sequence
  size, and `FullMemory` when a positive budget covers all encoded bytes.
  Rename the `StorageMode` values `ByteRange` and `PrefixMemory` to `NoMemory`
  and `PartialMemory`; `store.storage_mode` remains a read-only result.
  Update callers to supply only the preload budget and rebuild. Existing
  databases need no conversion. Benchmark mode labels become `no_memory`,
  `partial_memory`, and `full_memory`, and `prefix_budget_bytes` becomes
  `preload_bytes`; existing memory command-line options remain supported.
- Rewrite the user guides and README files in English, add a documentation
  index, and align examples and option descriptions with the current APIs.
- Collect packed-byte coverage, overread, merge counts and prefix hit/miss
  residue counts only when `ReadOptions::collect_diagnostics` is enabled.
  Text coverage, overread and merge counts follow the same switch. Disabled
  fields remain zero and the parallel coverage-statistics pass is skipped.
  Keep basic request, hit/miss, read-byte, range, worker and SQE/CQE counts,
  along with the maximum range size needed for buffer budgeting.
- Unify single and random batch sequence reads as `get(id_or_ids, options)`,
  always returning `Batch`. Rename `batch_get_into` to `get_into` while keeping
  caller-owned output buffers and the C++ single-string overload. Python
  single `get_into` accepts a writable byte buffer; `store[index]` still returns
  a string. Remove the old batch method names; sequential and record reads
  are unchanged. Update callers and rebuild; existing databases remain valid.
- Load resident name and description columns into Store-owned buffers instead
  of pre-touching file mappings. Keep on-demand reads and mapped text indexes
  unchanged; `resident_text_bytes` reports the loaded text buffers.
- Remove the Scan strategy, `ReadStrategy`, `read_strategy`, and
  `scan_max_range_bytes` from C++ and Python, together with the benchmark's
  `--read-strategy` and `--scan-max-range` flags. Keep the existing Sparse
  gap/size-based merging and sequential read APIs. Use `ranges` instead of
  the removed `sparse_ranges` and `scan_ranges` counters; rebuild callers.
- Consolidate API migration notes: update callers and rebuild after removing
  dynamic sequence-cache modes and their admission/eviction statistics; set
  a fixed sequence preload budget instead. Existing AAPCMP3 databases
  do not need rebuilding for these API changes.
- Replace read options `io_threads`, `io_uring_workers`, and `io_decode_threads`
  with the shared `threads` budget; rename the Store option `preload_threads`
  to `threads`. Remove `io_uring_max_bounded_workers` and its statistics;
  RabbitAAStore does not configure or query kernel io-wq worker limits. Remove old
  settings and remeasure CPU/I/O costs for highly concurrent workloads.
- Remove the `sequential_get_into(..., Batch&, ...)` overload; use caller-owned
  output buffers with the pointer-array overload and consume the returned
  `BatchStats`.
- Remove the benchmark's `--prefix-memory` switch; set an encoded-sequence
  preload budget with `--memory-mb` or `--memory-bytes`.
- Consolidate engineering test fixtures in their runners and remove redundant
  probe cases and detailed toolchain snapshot checks while retaining legacy
  io_uring, linked dependency and installed SDK coverage.
- Document the current support and validation scope as a reminder: Linux x86_64, 64-bit,
  little-endian, glibc, with a C++17 GCC/Clang toolchain and ordinary CPython
  3.9–3.13 wheels, while retaining Python >=3.9 source-package metadata.
  Targets outside this scope remain unverified, with no compatibility guarantee.
- Link liburing and thread capability probes during cross-compilation, replacing
  stale compile-only results. Preserve toolchain settings and emulator lists in
  installed SDK tests, and skip target execution when no emulator is configured.
- Detect filesystem link requirements with the selected C++ toolchain instead
  of compiler versions, including Clang with older standard libraries, and
  propagate any required companion library to SDK consumers.
- Select io_uring READ/READV by kernel capabilities, using READV when opcode
  probing is unavailable and preserving iovec lifetime for Linux 5.4.
- Detect baseline and optional liburing APIs at configure time so older
  READV-only libraries remain usable; check static SDK probe dependencies and
  liburing structure layouts before linking a consumer.
- Centralize option validation and reject unknown index-loading modes and I/O backends
  before file access or output writes, including empty reads.
- Expose optional `validate()` checks for C++ and Python configuration objects
  while preserving documented zero-valued read budgets and thread settings.
- Select Python development components compatible with CMake 3.16/3.17.
- Split Store tests into independently selectable CTest groups.
- Expose owner-preserving, read-only `offsets` and `lengths` memoryviews for
  Python sequence and text batches, using native uint64 byte coordinates.
- Match Python statistics type declarations to their read-only runtime fields.
- Describe pybind11 enums, positional record accessors, and nonconstructible
  result types accurately in Python type declarations.
- Add a practical Python API guide and self-contained examples for records,
  batch buffer layouts, and reusable output buffers; run the examples in CI.
- Cover Python batch layout lifetimes, UTF-8 byte boundaries, random reads,
  sorted requests, and range budgets with installed-package tests.
- Add an installable C++ SDK with the `aastore::aastore` CMake target,
  relocatable package configuration, package version checks, and pkg-config metadata.
- Separate library, command-line tool, benchmark, and test build options;
  disable auxiliary targets by default when embedded as a subdirectory.
- Add an installed-library C++ example and tests for external consumers.
- Add Linux static/shared and I/O backend CI coverage, Python source-package
  rebuild tests, and a manually triggered portable-wheel build.
- Use `VERSION` as the C++ and Python version source; document compatibility
  expectations and release validation.
- Limit Python wheels to the Python installation component.
- Anchor source-package inclusion rules to exclude local build trees, paper
  assets, and Python bytecode; check the archive contents in CI.
- Read unaligned metadata block bases safely without changing AAPCMP3 files.
- Join active read workers when thread startup or the calling worker fails.
- Add the GPL-3.0-only license and matching package metadata.
- Detect runtime io_uring availability in integration tests so restricted
  kernels still validate the pread and automatic fallback paths.

## 0.1.0 source baseline

- Contiguous 5-bit sequence storage with AAPCMP3 compact boundary metadata.
- Disk, fixed-prefix memory, and full-memory read modes; SIMD runtime dispatch.
- Random batch reads, sequential reads, caller-provided buffers, and optional
  name and description columns.
- Python bindings with buffer views, type declarations, and concurrent read support.

The repository also contains the earlier `v0.0.1` tag. This file records the
current source baseline and subsequent changes; it does not assert that a
0.1.0 binary release has already been published.
