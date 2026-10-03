# 文件职责：演示 Python 建库、选项、批次与按字段读取，所有文件仅写入临时目录。
"""运行：python examples/python/basic.py；先安装 aastore，无需其他依赖。"""

from pathlib import Path
from tempfile import TemporaryDirectory

import aastore


def main():
    with TemporaryDirectory(prefix="aastore-python-basic-") as temporary:
        root = Path(temporary)
        fasta = root / "input.fasta"
        fasta.write_text(
            ">alpha 第一条描述\nACDEFG\n"
            ">empty 空序列\n\n"
            ">beta 第二条描述\nWY\n",
            encoding="utf-8",
        )
        path = root / "store"
        build_options = aastore.BuildOptions(threads=2)
        built = aastore.build_fasta(fasta, path, build_options)
        print("建库：", built.sequences, "条序列，", built.residues, "个残基")

        opening = aastore.StoreOptions(threads=2, resident_names=True)
        reads = aastore.ReadOptions(
            threads=2,
            io_backend=aastore.IoBackend.Pread,
            collect_diagnostics=True,
        )
        with aastore.Store(path, opening) as store:
            print("版本：", aastore.__version__, "解码路径：", store.decode_kernel_name)
            print("单条：", store[0], "最后一条：", store[-1])
            batch = store.get([2, 0, 2, 1], reads)
            print("乱序、重复、空序列：", list(batch))
            print("反向切片：", batch[::-1])
            print("请求数：", batch.stats.requested_sequences)
            print("顺读 [0, 2)：", list(store.sequential_get(0, 2, reads)))

            fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
            records = store.batch_get_records([2, 0], fields, reads)
            print("所选字段：", list(records))
            print("名称子批次：", list(records.names))

            complete = store.batch_get_records([0, 1, 2], options=reads)
            print("描述子批次：", list(complete.descriptions))
            print("第一条记录：", complete[0])

        # 批次持有自己的输出；Store 关闭后仍可访问。
        print("Store 已关闭：", store.closed, "保留的批次：", list(batch))


if __name__ == "__main__":
    main()
