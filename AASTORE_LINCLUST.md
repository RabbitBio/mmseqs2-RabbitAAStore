# Minimal AAStore integration for Linclust

This branch uses AAStore only as an optional random-access sequence reader.
MMseqs2 continues to own database creation, shuffle, k-mer matching, result
databases, exports, and the native sequence payload. `--aastore` automatically
adds sequence-only AAStore construction after native database creation.

The integration has six parts:

1. `AAStoreTargetReader`: storage selection, memory budgeting, ID mapping,
   FullMemory reads, and thread-local random batches.
2. `rescorediagonal`: AAStore query/target access for same-database rescoring.
3. `Alignment`: AAStore target access and same-database query access.
4. `createsubdb`: propagation of the small `.aastore.path` association.
5. `--aastore`: automatic construction in final MMseqs ID order and workflow
   parameter forwarding.
6. CMake wiring and focused unit/integration tests.

There is no AAStore read path in `kmermatcher`, `createseqfiledb`, or
`result2repseq`. AAStore does not replace the native MMseqs2 sequence file.

Linclust's workflow and clustering parameters are unchanged. The native
`--cluster-mode` option applies to both clustering passes. Nonresident AAStore
reads use candidate-byte batches, while native and FullMemory rescoring retain the
original `schedule(dynamic, 1)` query scheduling.

## Build

AAStore sources are included in `lib/aastore` and built together with MMseqs2:

```bash
cmake -S . -B build-aastore
cmake --build build-aastore -j8
```

The build always uses this project's `lib/aastore` sources; no external
checkout or prebuilt archive is selected. Linux `liburing` is required.
The AAStore library is linked into `mmseqs`. Standalone tools are not part of
the default build or MMseqs2 installation, but remain available as explicit
build targets for tests or independent use.

## Automatic workflows

Use the usual native command with one additional option:

```bash
./build-aastore/src/mmseqs easy-linclust proteins.fasta clusters tmp \
  --threads 64 --aastore

# Cascaded clustering is supported as well.
./build-aastore/src/mmseqs easy-cluster proteins.fasta clusters tmp \
  --threads 64 --aastore
```

`createdb` and workflows over an existing native database also accept the flag:

```bash
./build-aastore/src/mmseqs createdb proteins.fasta target --aastore
./build-aastore/src/mmseqs linclust target clusters tmp --threads 64 --aastore
```

Native `createdb` completes its usual shuffle/renumbering first. AAStore then
encodes those records directly in final key order: AAStore ID `i` is native
key `i`. This avoids building, decoding, and rewriting a separate input-order
AAStore, and does not create an intermediate FASTA. Names and descriptions
remain in the native header database. Compressed/multiple FASTA inputs use
the native parser. Shuffle and createdb-mode defaults are not changed; native
soft mode disables shuffle even when `--shuffle 1` was requested.

An existing associated store is reused; otherwise `linclust`/`cluster` builds
`target.aastore` automatically. A local store older than the native index is
rebuilt. Subsets retain the parent association. Easy
workflows remove their own temporary store with `--remove-tmp-files 1`.
Automatic construction currently requires a non-GPU amino-acid database
with dense keys starting at zero (`--id-offset 0`).

`--aastore` and `--aastore 1` enable automatic preparation and AAStore reads.
Without the option, or with `--aastore 0`, sequence reads always use native
MMseqs2, even when a store is associated. The flag is forwarded to the
`rescorediagonal` and `align` subprocesses. AAStore has no environment-variable
configuration in the MMseqs2 adapter.

## Database associations

Automatic preparation writes the store to `target.aastore`. A `createsubdb`
operation records the parent store's absolute path in the subset's
`.aastore.path` file, so stable keys continue to address the same physical
AAStore IDs. The native data link or payload is retained. Associations only
locate storage; they cannot enable AAStore reads.

Without `--aastore`, both alignment stages use the unchanged native MMseqs2
path. Direct `rescorediagonal` and `align` commands also accept `--aastore`
to read an already prepared, associated store.

## Random-access pipeline

For nonresident modes, every compute thread collects complete queries until
their candidate sequences account for approximately 64 MiB. It then:

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

The adapter automatically selects storage mode by sampling host and cgroup
memory before opening the sequence index:

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

## Read configuration

Enable the adapter with `--aastore` and set compute threads with the native
`--threads` option. Storage mode and prefix size follow the memory policy
above. The adapter uses `io_uring`, eight preload threads, and the AAStore
library's default range-merging limits: 4 KiB gaps, 16 KiB normal ranges,
and 1 MiB exact ranges. These settings are not separate command-line or
environment-variable options. The standalone AAStore library retains its
own `StoreOptions` and `ReadOptions` APIs.

## Tests

```bash
cmake --build build-aastore -j8 --target test_aastore_reader aastore_build
./build-aastore/src/test_aastore_reader

python3 src/test/test_aastore_cli.py \
  --mmseqs build-aastore/src/mmseqs \
  --builder build-aastore/lib/aastore/aastore_build \
  --workdir /tmp/aastore-cli-test

python3 src/test/test_aastore_linclust.py \
  --mmseqs build-aastore/src/mmseqs \
  --workdir /tmp/aastore-linclust-test
```

The unit test covers memory budgeting, all three storage modes, both I/O
backends, ID mapping, and output-arena reuse. The integration test compares
native and automatically configured AAStore results for gapped and ungapped
Linclust with one and four compute threads, using `createdb --aastore` for
preparation. It also covers direct `Alignment` over a
reordered subset, and `createsubdb` association propagation. It also verifies
that `--cluster-mode 3` reaches both native clustering passes.

The automatic CLI test compares encoded stores with the public FASTA builder
after native export, including shuffled, unshuffled, soft, compressed and
multiple-input databases. It compares native and AAStore workflow results,
tests reuse/rebuild behavior and verifies easy-workflow cleanup.
