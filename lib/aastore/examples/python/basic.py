
from pathlib import Path
from tempfile import TemporaryDirectory

import aastore


def main():
    fasta = Path(__file__).resolve().parents[1] / "data" / "huge_100k.fa"
    print("Dataset:", fasta)
    with TemporaryDirectory(prefix="aastore-python-basic-") as temporary:
        root = Path(temporary)
        path = root / "store"
        build_options = aastore.BuildOptions(threads=2)
        built = aastore.build_fasta(fasta, path, build_options)
        print("Database built:", built.sequences, "sequences,", built.residues, "residues")

        opening = aastore.StoreOptions(threads=2, resident_names=True)
        reads = aastore.ReadOptions(
            threads=2,
            io_backend=aastore.IoBackend.Auto,
            collect_diagnostics=True,
        )
        with aastore.Store(path, opening) as store:
            print("Version:", aastore.__version__, "Decoding kernel:", store.decode_kernel_name)
            print("First sequence:", store[0], "Last sequence:", store[-1])
            batch = store.get([2, 0, 2, 1], reads)
            print("Unordered sequences with duplicates:", list(batch))
            print("Reverse slice:", batch[::-1])
            print("Request count:", batch.stats.requested_sequences)
            print("Sequential retrieval [0, 2):", list(store.sequential_get(0, 2)))

            fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
            records = store.batch_get_records([2, 0], fields, reads)
            print("Selected fields:", list(records))
            print("Name batch:", list(records.names))

            complete = store.batch_get_records([0, 1, 2], options=reads)
            print("Description batch:", list(complete.descriptions))
            print("First record:", complete[0])

        print("Store closed:", store.closed, "Retained batch:", list(batch))


if __name__ == "__main__":
    main()
