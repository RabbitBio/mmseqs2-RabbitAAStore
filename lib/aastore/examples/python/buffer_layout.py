# 文件职责：读取示例数据集，演示只读布局、UTF-8 字节长度、输出复用与视图生命周期。
"""运行：python examples/python/buffer_layout.py；读取 examples/data/huge_100k.fa，无需 NumPy。"""

from pathlib import Path
from tempfile import TemporaryDirectory

import aastore


def layout_bytes(storage, offsets, lengths):
    """按字节偏移和长度恢复每条数据；bytes() 在这里显式创建副本。"""
    return [bytes(storage[offset:offset + length])
            for offset, length in zip(offsets, lengths)]


def main():
    fasta = Path(__file__).resolve().parents[1] / "data" / "huge_100k.fa"
    print("数据集：", fasta)
    with TemporaryDirectory(prefix="aastore-python-buffer-") as temporary:
        root = Path(temporary)
        path = root / "store"
        built = aastore.build_fasta(fasta, path, threads=2)
        print("建库：", built.sequences, "条序列，", built.residues, "个残基")
        reads = aastore.ReadOptions(threads=2, io_backend=aastore.IoBackend.Auto)

        with aastore.Store(path) as store:
            sequential_end = 4
            ids = [2, 0, 1, 2]
            batch = store.get(ids, reads)
            storage, offsets, lengths = batch.storage, batch.offsets, batch.lengths
            print("布局只读：", storage.readonly, offsets.readonly, lengths.readonly)
            print("偏移：", list(offsets), "字节长度：", list(lengths))
            print("按布局恢复：", layout_bytes(storage, offsets, lengths))
            print("随机批读完整字节区（逐项追加 NUL）：", bytes(storage))
            saved_view = batch.view(0)

            sequential = store.sequential_get(0, sequential_end)
            print("顺读前 4 条的完整字节区（无 NUL）：", bytes(sequential.storage))

            fields = aastore.RecordFields.Description
            records = store.batch_get_records([0, 2], fields, reads)
            descriptions = records.descriptions
            text_storage = descriptions.storage
            text_offsets = descriptions.offsets
            text_lengths = descriptions.lengths
            print("UTF-8 描述：", descriptions[0])
            print("字符数与字节数：", len(descriptions[0]), text_lengths[0])

            # 为每个槽位预留最大容量，同一组 bytearray 可接收不同长度的请求。
            capacity = max(store.sequence_length(i) for i in range(sequential_end))
            outputs = [bytearray(capacity) for _ in ids]
            for requested in (ids, [0, 2, 1, 3]):
                stats = store.get_into(requested, outputs, reads)
                values = [bytes(output[:store.sequence_length(i)]).decode("ascii")
                          for i, output in zip(requested, outputs)]
                print("复用输出：", values, "请求数：", stats.requested_sequences)

            # 顺读使用相同缓冲约定，结果有效范围由本次序列长度决定。
            store.sequential_get_into(0, sequential_end, outputs, reads)
            values = [bytes(output[:store.sequence_length(i)]).decode("ascii")
                      for i, output in enumerate(outputs)]
            print("直接顺读：", values)

            # 删除父对象变量；下面的视图仍保留其所属批次和记录对象。
            del batch, records, descriptions, sequential

        print("Store 已关闭：", store.closed)
        print("保留的序列视图：", bytes(saved_view))
        print("保留的布局：", layout_bytes(storage, offsets, lengths))
        print("保留的文本布局：", layout_bytes(text_storage, text_offsets, text_lengths))


if __name__ == "__main__":
    main()
