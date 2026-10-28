# RabbitAAStore benchmarks

This directory provides a C++ sequence-read benchmark and a Python runner for
comparing memory residency, thread counts, and I/O backends. Both use an existing
RabbitAAStore database; database construction and text-column retrieval are
outside the measured workload.

## Build

Run from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAASTORE_BUILD_BENCHMARKS=ON
cmake --build build --target aastore_bench aastore_build -j 4
```

If needed, create an input database before benchmarking:

```sh
./build/aastore_build proteins.fa proteins.aastore --sequences-only
```

The io_uring backend requires usable liburing development files at build time
and kernel support at runtime. Explicit `uring` runs fail if the backend is
unavailable; `auto` permits fallback to `pread`.

## Individual runs

The following commands use an existing database. Replace `proteins.aastore`
with its path. These examples measure random requests, a 512 MiB preload
budget, full residency, and the consecutive-read interface:

```sh
./build/aastore_bench proteins.aastore \
  --requests 1000000 --seed 12345 --rounds 1 --threads 8 --drop-cache \
  --io-backend uring --queue-depth 64 --io-buffer-bytes 16777216

./build/aastore_bench proteins.aastore \
  --requests 1000000 --rounds 1 --threads 8 --drop-cache --memory-mb 512

./build/aastore_bench proteins.aastore \
  --requests 1000000 --rounds 1 --threads 8 --drop-cache --memory-mb -1

./build/aastore_bench proteins.aastore \
  --requests 1000000 --fast-sequential --rounds 1 --threads 8 --drop-cache
```

`--requests` generates random IDs with replacement unless a sequential mode is
selected. The default seed is `12345`. To sample without replacement and limit
batch output size, use:

```sh
./build/aastore_bench proteins.aastore \
  --sample-percent 1 --seed 12345 --batch-bytes 67108864 \
  --rounds 1 --threads 8 --io-backend pread --drop-cache
```

`--sample-percent P` selects `max(1, floor(N * P / 100))` distinct IDs, or zero
for an empty database, then shuffles them. It cannot be combined with
`--requests`, sequential modes, or `--ids-file`. For a fixed database, binary,
seed, and percentage, the ID order is identical across memory and thread
configurations. Sampling uses memory proportional to the number of selected IDs.

`--batch-bytes` targets decoded output bytes, including one NUL per record.
A sequence exceeding the target is processed alone and is never truncated.
Without this option, all requests are passed in one batch. The batch target and
the encoded I/O buffer budget are separate settings; neither limits total
process memory.

### Memory and I/O settings

| Option | Default | Meaning |
| --- | --- | --- |
| `--memory-mb` | `0` | Sequence preload budget in MiB; `0` disables preloading and `-1` requests full residency. |
| `--memory-bytes` | Unset | Exact sequence preload budget in bytes; zero disables preloading. |
| `--threads` | `8` | Internal worker budget for reads and initial sequence preloading. |
| `--io-backend` | `auto` | `auto`, `pread`, or `uring`. |
| `--queue-depth` | `64` | Target maximum outstanding requests per io_uring ring. |
| `--io-buffer-bytes` | `16777216` | Shared io_uring range-buffer budget; may reduce actual queue depth. |
| `--merge-gap` | `4096` | Maximum gap in bytes that range planning may bridge. |
| `--max-range` | `16384` | Maximum merged range size when bridging a nonzero gap. |
| `--max-exact-range` | `1048576` | Maximum range size for overlapping or adjacent merges. |

`--full-memory` is an alias for `--memory-mb -1`; both pass `UINT64_MAX` as
`StoreOptions::preload_bytes`. `--memory-bytes` cannot be combined with
`--memory-mb` or `--full-memory`. All forms pass a budget to the library, which
selects `NoMemory` for zero, `PartialMemory` for a positive budget below
`packed_bytes`, and `FullMemory` when a positive budget covers all encoded data.
Only the available encoded bytes are allocated when the budget is larger.
A partial prefix is fixed during a run; reads do not replace the resident
region. Indexes use the library's default read-only mmap mode.

The budget covers encoded sequences only. It excludes indexes, text columns,
decoded output, and I/O buffers. `NoMemory` means no explicit sequence preloading;
it does not disable the operating system page cache.

The caller issues batch reads serially, and RabbitAAStore distributes work
internally. Full residency decodes directly from memory without disk range
planning during retrieval. Partial residency decodes resident requests from
memory and plans disk reads for the remaining requests.

`--fast-sequential` calls `sequential_get()` for a prefix of consecutive IDs.
Its request count must fit within the database, and it cannot be combined with
`--batch-bytes`. The consecutive-read API uses `pread`.

## Cold-start conditions

`--drop-cache` requires `--rounds 1`. Each cold repeat must use a fresh process.
Before opening the database, the benchmark synchronizes and requests eviction
of **`packed.bin` and `meta.bin` only**, then uses `mincore` to verify that no
pages from either file remain resident. Failure to evict causes the run to fail.
Other workloads must not access the same database during measurement.

This establishes a cold file-page-cache start; it does not flush hardware or
controller caches, executable pages, or unrelated files. Pages can become
resident during opening, request preparation, and earlier batches. They are
not evicted again before each retrieval call.

## Timing and statistics

The two main timing fields are:

| Field | Scope |
| --- | --- |
| `fetch_ms` | Sum of timed library read calls; excludes opening, request generation, and output checks. |
| `service_ms` | `open_ms + fetch_ms`; includes opening and any sequence preloading. |

The benchmark enables `ReadOptions::collect_diagnostics`, so retrieval timings
include diagnostic collection overhead. The library disables these detailed
diagnostics by default. This distinction matters when comparing results from
other harnesses or reproducing a published experiment.

Additional fields include:

- `open_ms`: database opening, including sequence preloading.
- `preload_ms`: sequence preloading alone; already included in `open_ms`.
- `prepare_ms`: ID generation, metadata inspection, and batch preparation.
- `consume_ms`: output consumption, length checks, and hashes, outside the
  timed read calls.
- `end_to_end_ms`: opening, preparation, retrieval, output checks, and harness
  overhead; excludes cache eviction and process startup.
- `fetch_user_ms` and `fetch_system_ms`: process CPU time during read calls.
- `peak_rss_kib`: peak process RSS, including harness allocations; excludes
  unrelated system memory and the kernel's page cache.
- `resident_bytes`: resident encoded sequence bytes, excluding metadata.
- `preload_bytes`: requested encoded-sequence preload budget, which can exceed
  the actual resident bytes.
- `storage_mode`: automatically selected `no_memory`, `partial_memory`, or
  `full_memory`.
- `open_storage_read_bytes` and `fetch_storage_read_bytes`: Linux
  `/proc/self/io` device-read deltas, including attributable readahead. These
  differ from logical library `read_bytes` and whole-device counters.
- `trace_hash_fnv1a64`: order-sensitive hash of requested IDs.
- `output_hash_fnv1a64`: order-sensitive hash of IDs, lengths, and decoded bytes.

Hashes are noncryptographic consistency checks. The matrix runner requires them
to match across paired configurations. Counts and timings are summed across
batches and, for warm multi-round runs, across rounds. Worker counts, queue
depth, and maximum range size report maxima. Output hashes describe one round;
subsequent rounds must match it.

## Matrix runner

For example, this command runs 60 cases: five residency configurations,
three sampling percentages, and four thread counts:

```sh
python3 benchmarks/run_cold_matrix.py proteins.aastore results/cold-matrix \
  --label MyDataset --percents 1 5 10 --threads 1 4 16 64 \
  --prefix-percents 0 25 50 75 --backend uring --seed 12345
```

Use a new output directory. The runner adds a full-residency case to the
requested prefix percentages. Prefix budgets are
`floor(packed_bytes * percentage / 100)`: they represent encoded bytes, not
record fractions or guaranteed hit rates. Zero selects `NoMemory`; a positive
budget below the encoded data size selects `PartialMemory`. A positive budget
covering all encoded bytes selects `FullMemory`. For an empty encoded sequence
file, a zero budget still selects `NoMemory`; the full-residency request uses a
positive budget and reports `FullMemory`.

Without explicit options, the runner uses sampling percentages `0.01` and `0.1`,
thread counts `1`, `8`, and `64`, a zero-byte prefix plus full residency, one
repeat, seed `12345`, a 64 MiB decoded batch target, a 16 MiB I/O buffer budget,
queue depth `64`, explicit `uring`, and a 1,800-second timeout per case. Use
`--backend pread` for a matched backend experiment and `--repeats` to measure
run-to-run variation. Each repeat uses a new seed; paired configurations within
that repeat share the same requests. Case order is shuffled reproducibly.

The runner saves `manifest.json`, the binary SHA-256, input file sizes and
modification times, environment information, exact commands, raw logs, and
`results.csv`. It stops on timeouts, an insufficient estimated memory budget,
noncold input, backend errors, or inconsistent paired outputs. `COMPLETE` is
written only after every case succeeds. The memory estimate is a preliminary
check, not an enforced memory limit or a guarantee against exhaustion.

For reliable timings, avoid concurrent database construction, downloads, or
other storage benchmarks. Full-memory runs also require enough available RAM
to retain the encoded sequence file and benchmark allocations.

A separate regression checks the harness on small generated data:

```sh
python3 tests/benchmark_test.py build/aastore_build build/aastore_bench
```

Add `--uring` to exercise that backend when it is available. This regression
checks correctness and argument handling; it does not produce performance
results for a publication dataset.
