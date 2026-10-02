# Minimal AAStore integration for Linclust

This branch uses AAStore only as an optional random-access sequence reader.
MMseqs2 continues to own database creation, k-mer matching, result databases,
exports, and the native sequence payload.

The integration has five parts:

1. `AAStoreTargetReader`: storage selection, memory budgeting, ID mapping,
   FullMemory reads, and thread-local random batches.
2. `rescorediagonal`: AAStore query/target access for same-database rescoring.
3. `Alignment`: AAStore target access and same-database query access.
4. `createsubdb`: propagation of the small `.aastore.path` association.
5. CMake wiring and focused unit/integration tests.

There is no AAStore path in `createdb`, `kmermatcher`,
`createseqfiledb`, or `result2repseq`. AAStore does not replace the native
MMseqs2 sequence file.

Linclust's workflow and clustering parameters are unchanged. The native
`--cluster-mode` option applies to both clustering passes; the integration
does not interpret `MMSEQS_FINAL_CLUSTER_MODE`. Nonresident AAStore reads use
candidate-byte batches, while native and FullMemory rescoring retain the
original `schedule(dynamic, 1)` query scheduling.

## Build

AAStore sources are included in `lib/aastore` and built together with MMseqs2:

```bash
cmake -S . -B build-aastore
cmake --build build-aastore -j --target \
  mmseqs aastore_build aastore_reorder_createdb test_aastore_reader
```

The default `AASTORE_ROOT` is this project's `lib/aastore` directory. No
sibling checkout or prebuilt `libaastore.a` is required. AAStore tools are
written to `build-aastore/lib/aastore`. Linux `liburing` is required.

An external source checkout can still be selected with
`-DAASTORE_ROOT=/path/to/AABinaryStoreV2`. To link a prebuilt library instead,
also pass `-DAASTORE_LIBRARY=/path/to/libaastore.a`. When updating an existing
build that cached the old sibling paths, clear those settings once:

```bash
cmake -S . -B build-aastore -UAASTORE_ROOT -UAASTORE_LIBRARY
```

## Prepare data

Build a temporary input-order AAStore, then apply the fixed 32-way permutation
used by the default `createdb --shuffle 1`. The reorder scans its source
sequentially, commits the shuffled store, and only then removes the source.

```bash
mmseqs createdb proteins.fasta target

./build-aastore/lib/aastore/aastore_build \
  proteins.fasta target.aastore.input-order --sequences-only --threads 8

./build-aastore/lib/aastore/aastore_reorder_createdb \
  target.aastore.input-order target.aastore \
  --threads 8 --remove-source
```

AAStore can be selected explicitly:

```bash
MMSEQS_AASTORE="$PWD/target.aastore" \
mmseqs linclust target clusters tmp --threads 16
```

Alternatively, place the absolute store path and a newline in
`target.aastore.path`. A soft-mode `createsubdb` operation copies this
association to the resulting view while retaining the usual native data link.

When neither `MMSEQS_AASTORE` nor a valid association is present, both
alignment stages use the unchanged native MMseqs2 path.

## Random-access pipeline

For nonresident modes, every compute thread collects complete queries until
their candidate sequences account for approximately 64 MiB by default. It then:

1. removes duplicate local IDs within the batch;
2. translates local IDs to AAStore physical IDs;
3. submits one `batch_get_into` call;
4. scores all queries covered by that batch;
5. reuses the same output arena for its next batch.

The batch never splits one query. A query with more than 64 MiB of candidates
therefore forms one larger batch.

Each outer compute thread uses one AAStore I/O owner:

- `threads = 1`
- process-wide queue-slot budget = 512
- per-thread queue depth = `floor(512 / compute_threads)`

With `io_uring`, packed reads remain in flight while the calling compute
thread handles already completed ranges. PrefixMemory can also decode its
resident prefix after the initial asynchronous submissions. FullMemory skips
batch I/O and decodes each requested sequence into a reusable string;
Alignment then calls `Sequence::mapSequence` to fill its existing numeric buffer.

## Memory policy

Unless storage mode is explicitly selected, the adapter samples host and
cgroup memory before opening the sequence index:

```text
remaining pool =
    available memory
  - safety reserve
  - target index bytes
  - per-thread batch buffers
  - current stage working-set reserve

AAStore prefix       = floor_to_2_MiB(remaining pool * 90%)
page-cache/headroom  = remaining pool - AAStore prefix
```

The safety reserve is `max(4 GiB, memory limit / 5)`. The remaining roughly
10% absorbs transient allocations and file-page reclaim/writeback
fluctuations. The selected prefix is clamped to `packed.bin`; complete
coverage selects FullMemory, at least 64 MiB selects PrefixMemory, and a
smaller budget selects ByteRange.

## Supported settings

| Variable | Meaning | Default |
| --- | --- | --- |
| `MMSEQS_AASTORE` | Explicit AAStore directory | database association |
| `MMSEQS_AASTORE_FULL_MEMORY` | `1`: FullMemory, `0`: nonresident | automatic |
| `MMSEQS_AASTORE_CACHE_BYTES` | Fixed PrefixMemory bytes; `0`: ByteRange | automatic |
| `MMSEQS_AASTORE_THREAD_BATCH_BYTES` | Candidate bytes collected per compute thread | 64 MiB |
| `MMSEQS_AASTORE_IO_BACKEND` | `io_uring` or `pread` | `io_uring` |
| `MMSEQS_AASTORE_PRELOAD_THREADS` | Prefix/full preload threads | 8 |
| `MMSEQS_AASTORE_MERGE_GAP_BYTES` | Maximum gap merged into one read | 4096 |
| `MMSEQS_AASTORE_MAX_RANGE_BYTES` | Normal merged-range cap | 16 KiB |
| `MMSEQS_AASTORE_MAX_EXACT_RANGE_BYTES` | Exact-range cap | 1 MiB |

No Linclust-specific build, scan, stream-block, ID-map, nested-worker, or
diagnostic switches are part of this interface.

## Tests

```bash
./build-aastore/src/test_aastore_reader

python3 src/test/test_aastore_linclust.py \
  --mmseqs build-aastore/src/mmseqs \
  --builder build-aastore/lib/aastore/aastore_build \
  --reorder build-aastore/lib/aastore/aastore_reorder_createdb \
  --workdir /tmp/aastore-linclust-test
```

The integration test compares native and AAStore results for gapped and
ungapped Linclust, all three storage modes, both I/O backends, direct
`Alignment` over a reordered subset, and `createsubdb` association
propagation. It also verifies that `--cluster-mode 3` reaches both native
clustering passes without the removed environment override.
