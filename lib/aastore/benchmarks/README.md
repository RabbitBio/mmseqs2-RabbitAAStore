# Cold Random-Access Pilot

This suite measures AAStore only. The initial publication dataset is the full
UniRef100 2026_03 FASTA, not NR and not a smaller database made from sampled records.
Record sampling changes the workload, not the underlying database.

## Initial Matrix

| Variable | Initial values |
| --- | --- |
| Storage | FullMemory; NonFull with 0%, 25%, 50%, 75% resident packed prefixes |
| Requested records | 1%, 5%, 10% of the full database |
| AAStore internal threads | 1, 4, 16, 64 |
| Repeats | 1 for the pilot; use at least 3 for final measurements |
| Output batch target | 64 MiB decoded bytes, including one NUL per record |
| NonFull backend | Explicit io_uring; no silent fallback |
| I/O buffer setting | 16 MiB per batch call, shared among rings |
| Queue depth setting | 64, subject to the library's range and buffer limits |

The caller invokes `batch_get()` serially with bounded batches in both modes.
AAStore distributes work internally using `ReadOptions::threads`.
FullMemory takes the resident decode path, bypassing range planning and disk reads
during fetch. NonFull plans ranges and reads/decodes using its internal workers.
The same thread setting applies to FullMemory and PrefixMemory initial preload.
No outer pool is added to multiply the configured thread count.

This matrix has 60 cases per repeat: four NonFull budgets plus one FullMemory
case, crossed with three request fractions and four thread counts. Prefix budget
is `floor(packed_bytes * prefix_percent / 100)`, passed using `--memory-bytes`.
Zero budget selects ByteRange; a positive budget selects PrefixMemory. FullMemory
is the separate 100% resident case, not repeated for every prefix setting.
Packed byte percentage is not record-count percentage or a promised hit rate.
The runner records requested percentage, exact budget, actual resident bytes and
request/residue hit counts. Paired correctness checks also span prefix budgets.
Hit/miss counters describe NonFull prefix routing; FullMemory takes a direct
resident decode path and does not populate those counters.

`--sample-percent` selects `max(1, floor(N * percent / 100))` distinct IDs
(zero for an empty store), then shuffles their order. The same seed, percentage,
and binary produce identical ID order across modes and thread counts. Selection
uses O(sample size) memory. Case execution order is also seeded and shuffled.
A sequence exceeding the batch target is processed alone, never truncated.
The output batch target is separate from the compressed I/O buffer setting;
neither is a hard bound on total process memory.

## Cold Definition

Every case uses a fresh process and `--rounds 1 --drop-cache`. The benchmark
syncs and evicts **only `packed.bin` and `meta.bin`**, then verifies zero resident
pages with `mincore` before opening the Store. Failure to evict is a failed case,
not a warm measurement. Other workloads must not access the same store.
This is a cold file-page-cache start, not a hardware/controller cache flush.
Pages can naturally warm across batches within a case; they are not evicted
between batches. OS code pages and unrelated files are not flushed.

## Metrics

The benchmark explicitly enables `ReadOptions::collect_diagnostics` and reports
`collect_diagnostics=1`. The library leaves detailed read timings and submit/wait
API counters off by default; basic byte, range, request and SQE counts remain
available. Benchmark fetch timings include the enabled diagnostic overhead.

- `open_ms`: Store construction, including FullMemory or PrefixMemory preload.
- `fetch_ms`: sum of timed AAStore calls, excluding request generation and hashes.
- `service_ms`: `open_ms + fetch_ms`, the primary cold service comparison.
- `end_to_end_ms`: also includes trace preparation, output consumption and harness
  overhead; excludes the cache-eviction setup and process startup.
- `prepare_ms`, `consume_ms`: trace preparation and output validation separately.
- `peak_rss_kib`: whole-process high-water RSS, including benchmark allocations;
  **not** machine memory usage or kernel page-cache size.
- `resident_bytes`: AAStore's resident packed bytes, excluding metadata.
- `open_storage_read_bytes`, `fetch_storage_read_bytes`: Linux process-attributed
  device reads; distinct from logical AAStore `read_bytes` and range counts.
- `trace_hash_fnv1a64`, `output_hash_fnv1a64`: order-sensitive noncryptographic
  checks, including ID, length and every output byte. These must match paired cases.

Counts and timings are summed across batches (and rounds for legacy warm runs).
Worker counts, queue depth and maximum range size report maxima, not sums.
The output digest describes one round; repeated rounds must match.
Record inspection and batch sizing touch metadata before fetch in both modes;
the experiment starts cold **before open**, not immediately before every fetch.

## Run

```sh
cmake --build build --target aastore_bench aastore_build -j 8
python3 benchmarks/run_cold_matrix.py /path/to/full/store /path/to/new/results \
  --label UniRef100_2026_03 --percents 1 5 10 --threads 1 4 16 64 \
  --prefix-percents 0 25 50 75
```

The runner records the binary SHA-256, input file sizes/mtimes, environment,
randomized case order, exact commands, raw logs, and `results.csv`. It stops on
timeouts, insufficient estimated available memory, noncold input, backend errors,
or mismatched paired outputs. `COMPLETE` appears only after every case succeeds.
The memory guard is conservative, not a cgroup limit or a guarantee against OOM.
Do not run the suite concurrently with dataset downloads/builds or other storage
benchmarks when collecting publishable measurements.

For a quick generated-data regression (not a performance result):

```sh
python3 tests/benchmark_test.py build/aastore_build build/aastore_bench --uring
```

Use `--backend pread` for a separate matched backend experiment. Omitting
`--prefix-percents` retains the two-case FullMemory/zero-prefix comparison.
The runner defaults to smaller request fractions for quick trials; the command
above explicitly selects the current publication pilot. Batch-size sweeps and
additional repeats can follow the pilot.
