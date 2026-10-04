# 文件职责：检查当前构建的 Python 扩展能导入，并完成小规模建库与读取。
from pathlib import Path
import tempfile
import unittest

import aastore


class EnvironmentSmokeTest(unittest.TestCase):
    # 使用临时输入核对批次、调用方缓冲及常驻名称，退出时自动清理。
    def test_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix="aastore-python-smoke-") as temporary:
            root = Path(temporary)
            fasta = root / "input.fa"
            fasta.write_text(
                ">first example\nACDEFG\n>empty\n>last tail\nWY\n", encoding="ascii"
            )
            built = aastore.build_fasta(fasta, root / "store", threads=2)
            self.assertEqual((built.sequences, built.residues), (3, 8))
            opening = aastore.StoreOptions(threads=2, resident_names=True)
            reads = aastore.ReadOptions(threads=2, io_backend=aastore.IoBackend.Pread)
            with aastore.Store(root / "store", opening) as store:
                batch = store.get([2, 0, 2, 1], reads)
                self.assertEqual(list(batch), ["WY", "ACDEFG", "WY", ""])
                outputs = [bytearray(2), bytearray(6), bytearray()]
                store.get_into([2, 0, 1], outputs, reads)
                self.assertEqual(list(map(bytes, outputs)), [b"WY", b"ACDEFG", b""])
                records = store.batch_get_records([2, 0], options=reads)
                self.assertEqual(records[0]["name"], "last")
                self.assertEqual(records[1]["sequence"], "ACDEFG")
                self.assertTrue(records.names.stats.resident)
            self.assertEqual(batch[0], "WY")
            print("Python extension:", aastore.__version__)


if __name__ == "__main__":
    unittest.main(verbosity=2)
