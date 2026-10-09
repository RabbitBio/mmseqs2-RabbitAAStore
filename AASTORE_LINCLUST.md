# Minimal AAStore integration for Linclust

This branch uses AAStore as an optional random-access sequence and record reader.
MMseqs2 continues to own database creation, shuffle, k-mer matching, result
databases and the native sequence payload. AAStore alignment and rescoring use
AAStore sequence metadata without opening the native sequence `.index`.
`createdb --aastore` captures names
and descriptions while parsing, then constructs all three AAStore fields in
the final native ID order without a native header database.

The integration has seven parts:

1. `AAStoreTargetReader`: storage selection, memory budgeting, full-store keys
   and compact subset membership, FullMemory reads, and thread-local random batches.
2. `rescorediagonal`: AAStore query/target access for same-database rescoring.
3. `Alignment`: AAStore target access and same-database query access.
4. `aastoreexportfasta`: complete and representative FASTA output through
   `batch_get_records(All)`. `AAStoreTsvAdapter` supplies AAStore text fields
   to same-source `createtsv --aastore`, sharing native TSV formatting.
5. `createsubdb`: propagation of the small `.aastore.path` association.
6. `--aastore`: automatic construction in final MMseqs ID order and workflow
   parameter forwarding.
7. CMake wiring and focused unit/integration tests.

There is no AAStore read path in `kmermatcher` or `result2flat`.
AAStore does not replace the native MMseqs2 sequence file. Native k-mer matching,
candidate/result database access and clustering still use their usual `.index`
files; keep these files for the complete Linclust workflow.

Linclust's clustering stages and parameters are unchanged. AAStore easy
workflows bypass ASCII output databases and `result2flat`. The native
`--cluster-mode` option applies to both clustering passes. Nonresident AAStore
reads collect candidate-byte batches behind a shared sequence-access interface.
The shared adapter preserves AAStore's candidate-byte scheduling while scoring
keeps native candidate order and rules.

## Build

AAStore sources are included in `lib/aastore` and built together with MMseqs2:

```bash
cmake -S . -B build-aastore
cmake --build build-aastore -j8
```

The bundled source is based on
[`RabbitBio/AABinaryStore` at `ab94e4d3a0306c78d29ffe2182c3839fe803073d`](https://github.com/RabbitBio/AABinaryStore/tree/ab94e4d3a0306c78d29ffe2182c3839fe803073d).
The Scan strategy and its public options/counters have been removed in sync
with the standalone library; existing Sparse range merging is unchanged.
Resident name and description columns likewise use Store-owned buffers
instead of pre-touched file mappings, matching the standalone library.
Single and random batch sequence reads share `get`, returning `Batch`;
caller-owned output buffers use `get_into`. The adapter keeps its reusable
output arena and the single-string FullMemory path.
Its build dependencies, SDK files, tests, and examples are included alongside
the library; no external checkout is required.

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
AAStore, and does not create an intermediate FASTA. During parsing, names and
descriptions are written into text-column shards matching native shuffle
partitions, using AAStore's standard `TextBuildWriter`. The shards are read
with AAStore's text reader and joined in final ID order by the same writer;
`target_h`, `target_h.index` and `target_h.dbtype` are never generated in this
mode. Names and descriptions follow AAStore's default field semantics;
exports insert one space before a nonempty description. Original separator
whitespace is not retained for new builds.
Compressed/multiple FASTA inputs use
the native parser. Shuffle and createdb-mode defaults are not changed; native
soft mode disables shuffle even when `--shuffle 1` was requested.

An existing associated store is reused; otherwise `linclust`/`cluster` builds
sequence-only `target.aastore` automatically from the native sequence database.
This path does not add names or descriptions. Direct AAStore FASTA export
requires the record columns produced by `createdb --aastore`; for an existing
sequence-only store, rebuild from its original FASTA with that command.
A local store older than the native index is rebuilt. Subsets retain the parent
association. Easy workflows remove their own temporary store with
`--remove-tmp-files 1`.
Automatic construction currently requires a non-GPU amino-acid database
with dense keys starting at zero (`--id-offset 0`).

`--aastore` and `--aastore 1` enable automatic preparation and AAStore reads.
Without the option, or with `--aastore 0`, sequence reads always use native
MMseqs2, even when a store is associated. The flag is forwarded to the
`rescorediagonal` and `align` subprocesses.
Easy workflows additionally forward it to `createtsv` and directly export both
FASTA files with `aastoreexportfasta`, using `--representative-only 1` for the
representative output. `--representative-only` belongs to `aastoreexportfasta`
and controls which sequences that command writes; it does not change cluster
assignment, scoring, or the clustering parameters. Native formatters that
require `_h` are not used for new AAStore databases. The native
`result2repseq` and `createseqfiledb` commands retain their original
implementations and do not read AAStore.
AAStore has no environment-variable
configuration in the MMseqs2 adapter.

## Database associations

Automatic preparation writes the store to `target.aastore`. The full database
uses AAStore ID = native key and needs no extra member table. A `createsubdb`
operation records the parent store's absolute path in the subset's
`.aastore.path` file and writes `.aastore.ids`, a compact list of the subset's
original sequence keys with its residue-count and maximum-length metadata.
Scoring reads this membership directly instead of the subset's native sequence
index. A key outside the subset is rejected even when the sequence exists in
the shared full store. Missing or invalid subset membership is an error.

The native data link or payload is retained. Associations only locate storage;
they cannot enable AAStore reads. Full and subset associations distinguish their
view type. An older single-line association can be upgraded once by streaming
its existing native sequence index; subsequent scoring uses the saved member
table. New associations require no such conversion.
`cpdb`, `lndb` and `mvdb` carry the association and member table with a subset;
`rmdb` removes those subset artifacts without deleting the shared AAStore.
Reusing a subset output for an unassociated native source clears its old
association and member table. If the output owns a local `.aastore` directory,
`createsubdb` requires a fresh output path instead of overwriting that store.

Without `--aastore`, both alignment stages use the unchanged native MMseqs2
path. Direct `rescorediagonal` and `align` commands also accept `--aastore`
to read an already prepared, associated store.

## Random-access pipeline

`AAStoreSequenceAccess` owns each compute thread's sequence buffers and delegates
ID lookup and size metadata to AAStore when enabled. It does not keep a native
sequence `DBReader` in this mode. Native reads retain the original `DBReader`.
`AAStoreQueryBatches` shares the existing candidate collector and assignment
cursor between alignment and rescoring. In nonresident modes, each worker
claims complete queries until their candidate sequence lengths plus terminators
reach approximately 64 MiB, then calls `get_into` once for the batch. Multiple
small queries share a batch. Repeated candidate occurrences count toward the
threshold; query sequences and ID bookkeeping do not.

The threshold retains the previous complete-query boundary: a single query
larger than 64 MiB is not split, so decoded memory can exceed the threshold.
Candidate scanning and query assignment hold one shared lock; sequence I/O
and scoring run outside that lock. Batch views stay valid for all assigned
queries and realignment. Early stopping keeps the native scoring rule, while
the candidates in that batch have already been fetched. Native and FullMemory
paths retain their original OpenMP scheduling. Candidate order, acceptance and
rejection limits, and scoring logic remain native.

Each outer compute thread uses one AAStore I/O owner:

- `threads = 1`
- process-wide queue-slot budget = 512
- per-thread queue depth = `floor(512 / compute_threads)`

With `io_uring`, packed reads remain in flight while the calling compute
thread handles already completed ranges. PrefixMemory can also decode its
resident prefix after the initial asynchronous submissions. During rescoring
and alignment, FullMemory skips batch I/O and decodes each requested sequence into a reusable string;
Alignment then calls `Sequence::mapSequence` to fill its existing numeric buffer.

## Direct record output

New `createdb --aastore` stores contain sequences, names and descriptions in
verified native ID order. Export both cluster FASTA and representative FASTA
without creating an intermediate text database:

```bash
./build-aastore/src/mmseqs aastoreexportfasta target clusters all_seqs.fasta \
  --aastore 1 --threads 8
./build-aastore/src/mmseqs aastoreexportfasta target clusters representatives.fasta \
  --aastore 1 --threads 8 --representative-only 1
./build-aastore/src/mmseqs createtsv target target clusters clusters.tsv \
  --aastore 1
```

`aastoreexportfasta` calls
`Store::batch_get_records(..., RecordFields::All)` and writes the result directly
through one ordered writer. It preserves cluster/member order, repeated members,
cluster filtering and HH-format behavior. Small clusters share batches; large
clusters span batches of approximately 64 MiB. Individual records are not split,
so this threshold is not a hard memory cap. A cluster key absent from its member
batch is fetched as text fields only.

Names and descriptions use the default AAStore field semantics: one space is
inserted before a nonempty description. Original header separator whitespace
is not retained for newly created stores. Sequence content follows AAStore's
normalization rules; it does not restore original letter case or unsupported
characters discarded during construction.

Verified dense source IDs let direct FASTA output avoid both the native header
reader and native sequence index. New record stores use V2 certification bound
to `meta.bin`, `names.meta`, `descriptions.meta`, `packed.bin`, `names.bin` and
`descriptions.bin`. Validation checks each file's size, inode and modification
time with `stat`, without scanning its contents. Replacing or touching a
certified AAStore file invalidates record export. The source's native sequence
payload, sequence `.index` and `_h` database are not required; cluster/result
data and `.index` remain native.

Dense record output requires a full-store association with no `.aastore.ids`.
An external store must have an explicit `FULL` association; the local
`database.aastore` layout without a locator represents the full store directly.
A normal subset, or an old single-line subset association without its saved
member table, cannot inherit the parent store's dense-output authorization.

An existing V1 record certificate is upgraded when record output first verifies it.
The original native sequence `.index` must pass its old size/inode/modification
time checks once; its contents are not loaded. The reader establishes the full
association before atomically publishing V2 certification and retains the
existing header reconstruction convention, including original whitespace in
older record columns. Subsequent outputs no longer depend on that native index.

The adapter keeps names and descriptions
resident together when their complete data fit after metadata, workspaces and
safety reserves. Otherwise both columns use batched reads. Only the remaining
pool is assigned to packed sequence data. AAStore supplies `name_length` and
`description_length` alongside `sequence_length` for batch sizing.

`--representative-only 1` writes the first cluster member's sequence under the
cluster key's header, matching the native representative-output convention.
It does not affect clustering or the all-sequence export. Representative-only
output does not support HH format.

`AAStoreTsvAdapter` supplies accession and header fields to same-source
`createtsv --aastore`; `TsvRowFormatter` keeps the native column and
first-representative rules shared. Reading an accession does not copy the full
description. Native `result2repseq`, `createseqfiledb` and `result2flat` remain
available for native databases with native headers; the AAStore easy workflows
use direct record output instead.

## Memory policy

The adapter automatically selects storage mode by sampling host and cgroup
memory before opening AAStore:

```text
remaining pool =
    available memory
  - safety reserve
  - current-stage metadata and index bytes
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

The direct `aastoreexportfasta` command uses an 8 GiB safety reserve for its
single writer and bounded records workspace. It opens the cluster index before
sampling available memory, reserves the complete cluster data and three compact
metadata mappings, and then decides whether both text columns fit in memory.
After reserving resident text, the same 90%/10% rule assigns the remaining pool
to packed data and transient headroom. Other stages retain the default safety
reserve above.

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
cmake --build build-aastore -j8 --target test_aastore_reader test_aastore_sequence_access aastore_build
./build-aastore/src/test_aastore_reader
./build-aastore/src/test_aastore_sequence_access

python3 src/test/test_aastore_sequence_batches.py \
  --mmseqs build-aastore/src/mmseqs \
  --workdir "$HOME/.cache/aastore-validation/sequence-batches-test"

python3 src/test/test_aastore_cli.py \
  --mmseqs build-aastore/src/mmseqs \
  --builder build-aastore/lib/aastore/aastore_build \
  --workdir "$HOME/.cache/aastore-validation/cli-test"

python3 src/test/test_aastore_linclust.py \
  --mmseqs build-aastore/src/mmseqs \
  --workdir "$HOME/.cache/aastore-validation/linclust-test"

python3 src/test/test_aastore_exports.py \
  --mmseqs build-aastore/src/mmseqs \
  --workdir "$HOME/.cache/aastore-validation/export-test"

python3 src/test/test_aastore_createdb_records.py \
  --mmseqs build-aastore/src/mmseqs \
  --builder build-aastore/lib/aastore/aastore_build \
  --workdir "$HOME/.cache/aastore-validation/createdb-records-test"
```

The unit test covers memory budgeting, all three storage modes, both I/O
backends, ID mapping, and output-arena reuse. The integration test compares
native and automatically configured AAStore results for gapped and ungapped
Linclust with one and four compute threads, using the automatic sequence-only
adapter for an existing native database. It also covers direct `Alignment` over a
reordered subset, and `createsubdb` association and membership propagation.
Fresh main/subset scoring tests hide both native sequence indexes before running
AAStore align/rescore with one and four threads and compare keyed result rows
with native scoring, including realignment and alternative alignments. They
also compare a separate native query database against an AAStore subset target
while its native target index is hidden. They
reject globally valid keys excluded from a subset and check that missing AAStore
storage cannot trigger a native fallback. They also verify that
copied, linked and moved subsets retain their index-independent scoring view,
and removing those artifacts leaves their source intact. Native subset reuse
checks clear old associations, while a local AAStore output is preserved when
reuse is rejected. `--cluster-mode 3` is checked on both native clustering passes.

The sequence-access test checks candidate-byte collection, repeated IDs,
complete-query batch boundaries, concurrent query assignment, and stable batch
views in all three storage modes. The batch-scoring integration test forces
disk-backed reads with a sparse candidate file and compares native align/rescore
results for multiple queries sharing a batch and a single query exceeding the
threshold, including early stopping and realignment. It also checks that
missing query/target IDs fail before scoring and empty inputs stay empty.

The automatic CLI test compares encoded stores with the public FASTA builder
after native sequence reads, including shuffled, unshuffled, soft, compressed and
multiple-input databases. It compares native and AAStore workflow results,
tests reuse/rebuild behavior and verifies easy-workflow cleanup.
It also compares representative/all-sequence FASTA outputs and checks export
flag propagation. Direct export tests compare FASTA records and TSV rows with
native output for stores constructed by `createdb --aastore`, including member
order, repeated members and representative selection. They also verify that
all/representative FASTA and TSV outputs remain identical after removing the
native source sequence index and all payload shards from header-free stores.
Certificate tests cover V1 automatic upgrade, changed AAStore index identity or
mtime, and rejection of both explicit and old single-line subset views.

The records construction test compares all six column files against the
default public FASTA builder in final ID order, and verifies header-free easy
workflows and rebuilding an existing native database with `createdb --aastore`.
Soft-createdb records also export all/representative FASTA and TSV with their
native payload links and sequence index unavailable.
