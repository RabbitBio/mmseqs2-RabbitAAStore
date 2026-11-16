
from pathlib import Path
from tempfile import TemporaryDirectory

import aastore


def layout_bytes(storage, offsets, lengths):
    return [bytes(storage[offset:offset + length])
            for offset, length in zip(offsets, lengths)]


def main():
    fasta = Path(__file__).resolve().parents[1] / "data" / "huge_100k.fa"
    print("Dataset:", fasta)
    with TemporaryDirectory(prefix="aastore-python-buffer-") as temporary:
        root = Path(temporary)
        path = root / "store"
        built = aastore.build_fasta(fasta, path, threads=2)
        print("Database built:", built.sequences, "sequences,", built.residues, "residues")
        reads = aastore.ReadOptions(threads=2, io_backend=aastore.IoBackend.Auto)

        with aastore.Store(path) as store:
            sequential_end = 4
            ids = [2, 0, 1, 2]
            batch = store.get(ids, reads)
            storage, offsets, lengths = batch.storage, batch.offsets, batch.lengths
            print("Read-only layout:", storage.readonly, offsets.readonly, lengths.readonly)
            print("Offsets:", list(offsets), "Byte lengths:", list(lengths))
            print("Reconstructed from layout:", layout_bytes(storage, offsets, lengths))
            print("Full batch buffer (NUL after each sequence):", bytes(storage))
            saved_view = batch.view(0)

            sequential = store.sequential_get(0, sequential_end)
            print("Full buffer for the first 4 sequential sequences (no NUL):", bytes(sequential.storage))

            fields = aastore.RecordFields.Description
            records = store.batch_get_records([0, 2], fields, reads)
            descriptions = records.descriptions
            text_storage = descriptions.storage
            text_offsets = descriptions.offsets
            text_lengths = descriptions.lengths
            print("UTF-8 description:", descriptions[0])
            print("Character count and byte length:", len(descriptions[0]), text_lengths[0])

            capacity = max(store.sequence_length(i) for i in range(sequential_end))
            outputs = [bytearray(capacity) for _ in ids]
            for requested in (ids, [0, 2, 1, 3]):
                stats = store.get_into(requested, outputs, reads)
                values = [bytes(output[:store.sequence_length(i)]).decode("ascii")
                          for i, output in zip(requested, outputs)]
                print("Reused output buffers:", values, "Request count:", stats.requested_sequences)

            store.sequential_get_into(0, sequential_end, outputs, reads)
            values = [bytes(output[:store.sequence_length(i)]).decode("ascii")
                      for i, output in enumerate(outputs)]
            print("Sequential retrieval into output buffers:", values)

            del batch, records, descriptions, sequential

        print("Store closed:", store.closed)
        print("Retained sequence view:", bytes(saved_view))
        print("Retained layout:", layout_bytes(storage, offsets, lengths))
        print("Retained text layout:", layout_bytes(text_storage, text_offsets, text_lengths))


if __name__ == "__main__":
    main()
