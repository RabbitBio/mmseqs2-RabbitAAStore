# MMseqs2 with RabbitAAStore

This repository integrates [RabbitAAStore](https://github.com/RabbitBio/RabbitAAStore)
into Linclust based on [MMseqs2 release 18](https://github.com/soedinglab/MMseqs2/tree/18-8cc5c).
It combines compact protein sequence storage with automatic memory preloading
and batched disk reads for clustering large sequence collections.

RabbitAAStore supplies query and target sequences during diagonal rescoring,
alignment, and realignment, and supplies sequence names, descriptions, and
sequences for TSV and FASTA export. Linclust's k-mer candidate generation,
alignment scoring, filtering criteria, and clustering algorithm are preserved.

## Installation

### Prebuilt binaries

Download a Linux x86-64 package from this repository's
[Releases](https://github.com/RabbitBio/mmseqs2-RabbitAAStore/releases).
Each package contains `bin/mmseqs`, a small protein FASTA, and `run_example.sh`.
The binaries are statically linked; no compiler, Python, or separate
RabbitAAStore or liburing installation is needed.

| Package | MMseqs2 baseline | CPU requirements |
| --- | --- | --- |
| AVX2 (recommended) | AVX2 | x86-64 with AVX2, FMA, and CMPXCHG16B |
| SSE2 (compatibility) | SSE2 | x86-64 with SSE2 |

Both packages build RabbitAAStore with `AUTO`, selecting scalar, AVX2/BMI2,
or AVX-512 kernels at runtime according to CPU capabilities. The package name
refers to the MMseqs2 baseline; an AVX-512-capable machine can use the AVX2 package
and still use RabbitAAStore's AVX-512 kernels.

For example, download the AVX2 package, verify its checksum, and run the included example:

```bash
curl -fLO https://github.com/RabbitBio/mmseqs2-RabbitAAStore/releases/download/rabbitaastore-v0.1.0/mmseqs2-rabbitaastore-v0.1.0-linux-x86_64-avx2.tar.gz
curl -fLO https://github.com/RabbitBio/mmseqs2-RabbitAAStore/releases/download/rabbitaastore-v0.1.0/SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
tar -xzf mmseqs2-rabbitaastore-v0.1.0-linux-x86_64-avx2.tar.gz
cd mmseqs2-rabbitaastore-v0.1.0-linux-x86_64-avx2
./run_example.sh
```

The example uses four threads and creates `example-output/`. To run it again,
provide a new output directory: `./run_example.sh example-output-2`.

Both packages use `Auto` for nonresident sequence reads: they prefer `io_uring`
and fall back to `pread` when it is unavailable. The small example may fit
entirely in memory and does not check disk I/O support.

### Build from source

Use Linux with a C++17 compiler supporting OpenMP, CMake 3.16 or newer, and
liburing development files. Install zlib and bzip2 development files to enable
compressed input support. RabbitAAStore sources are included in `lib/aastore`.

```bash
git clone --branch rabbitaastore-release18 https://github.com/RabbitBio/mmseqs2-RabbitAAStore.git
cd mmseqs2-RabbitAAStore
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mmseqs -j 4
```

The executable is `./build/src/mmseqs`. The default source build targets the
build machine's CPU. For explicit SIMD profiles and portable builds, see
[build configuration](AASTORE_LINCLUST.md#simd-profiles-for-prebuilt-binaries).

## Run Linclust with RabbitAAStore

From an extracted binary package, cluster a protein FASTA with:

```bash
./bin/mmseqs easy-linclust proteins.fasta clusters tmp \
  --threads 64 --min-seq-id 0.9 -c 0.8 --cluster-mode 3 --aastore 1
```

For a source build, use `./build/src/mmseqs` in place of `./bin/mmseqs`.
Adjust the thread count and clustering thresholds for your dataset and machine.
The command constructs the database, clusters the sequences, and exports:

| Output | Contents |
| --- | --- |
| `clusters_cluster.tsv` | Representative and member names, one pair per line. |
| `clusters_rep_seq.fasta` | Cluster representative sequences. |
| `clusters_all_seqs.fasta` | Clustered sequences grouped by representative. |

RabbitAAStore is enabled explicitly with `--aastore 1`; the shorter `--aastore`
form is also accepted. `--aastore 0` selects native MMseqs2 sequence access.

To retain a database for reuse, run database construction and clustering separately:

```bash
./bin/mmseqs createdb proteins.fasta target --threads 64 --aastore 1
./bin/mmseqs linclust target clusters tmp \
  --threads 64 --min-seq-id 0.9 -c 0.8 --cluster-mode 3 --aastore 1
```

Here `linclust` produces a cluster database. Export commands and the supported
`easy-cluster` workflow are described in the [integration guide](AASTORE_LINCLUST.md).

## Memory and access configuration

The integration automatically calculates the sequence preload budget when it
opens RabbitAAStore, using available memory and reserving space for indexes,
candidate data, working buffers, and a safety margin. Depending on the budget,
it preloads all encoded sequences, a prefix, or none. Nonresident sequences are
retrieved through batched reads. The integration uses `Auto` I/O selection,
preferring `io_uring` and falling back to `pread` if `io_uring` is unavailable.
The selected preload size stays fixed until the database is closed. Details are
in the [memory policy](AASTORE_LINCLUST.md#memory-policy).

| Option | Purpose |
| --- | --- |
| `--aastore 1` | Build or reuse RabbitAAStore and enable it for supported commands. |
| `--threads N` | Set the compute thread count. RabbitAAStore preloading uses a separate budget of eight threads. |
| `--aastore-index-mode mmap` | Access sequence and text indexes through memory mappings; the default. |
| `--aastore-index-mode read` | Load complete sequence and text indexes into allocated memory. |

Sequence preloading is automatic in this integration; it has no separate
`preload_bytes` command-line option. Index loading is independent of sequence
preloading.

`createdb --aastore 1` writes `target.aastore` alongside the native sequence
database, with encoded sequences, names, descriptions, and their indexes.
The complete workflow still uses the native sequence database for k-mer
matching and native databases for candidates and clustering results. Retain
these files together when reusing a database. Supported inputs are protein
sequences; nucleotide and GPU sequence databases are not supported by this integration.

## Documentation and support

- [Integration guide](AASTORE_LINCLUST.md): database reuse, export commands, memory budgeting, and tests.
- [RabbitAAStore](https://github.com/RabbitBio/RabbitAAStore): standalone C++ and Python library.
- [MMseqs2 documentation](https://github.com/soedinglab/MMseqs2/wiki): general clustering parameters and upstream functionality.
- [Issue tracker](https://github.com/RabbitBio/mmseqs2-RabbitAAStore/issues): questions and problems with this integration.

## References and licenses

This integration builds on MMseqs2 and Linclust:

- Steinegger M and Söding J. *MMseqs2 enables sensitive protein sequence searching for the analysis of massive data sets.* Nature Biotechnology (2017). [doi:10.1038/nbt.3988](https://doi.org/10.1038/nbt.3988).
- Steinegger M and Söding J. *Clustering huge protein sequence sets in linear time.* Nature Communications (2018). [doi:10.1038/s41467-018-04964-5](https://doi.org/10.1038/s41467-018-04964-5).

The [MMseqs2 license](LICENSE.md) and [RabbitAAStore license](lib/aastore/LICENSE)
are included in the repository. Third-party license notices are retained in
their respective library directories.
