# 文件职责：用临时 FASTA 验证 Python 建库、读取、缓冲生命周期、GIL 和并发关闭。
import array
from concurrent.futures import ThreadPoolExecutor
import gc
from pathlib import Path
import sys
import tempfile
import threading
import unittest

import aastore


class PythonInterfaceTest(unittest.TestCase):
    # 创建含空序列、长序列和 UTF-8 文本的可复现小库。
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="aastore-python-test-")
        cls.root = Path(cls.temporary.name)
        cls.fasta = cls.root / "input-\u03b1.fasta"
        cls.path = cls.root / "store"
        cls.sequences = ["", "ACDEFG", "WY" * (1024 * 1024), "A", ""]
        cls.sequences.extend("ACDEFGHIKLMNPQRSTVWY" * (8 + i % 13) for i in range(128))
        cls.names = ["name{}".format(i) for i in range(len(cls.sequences))]
        cls.descriptions = ["description{} \u6d4b\u8bd5".format(i) for i in range(len(cls.sequences))]
        cls.fasta.write_text("".join(
            ">{} {}\n{}\n".format(name, description, sequence)
            for name, description, sequence in zip(cls.names, cls.descriptions, cls.sequences)
        ), encoding="utf-8")
        cls.build_stats = aastore.build_fasta(cls.fasta, cls.path, threads=4)
        cls.backends = [aastore.IoBackend.Pread, aastore.IoBackend.Auto]
        cls.uring_reason = None
        with aastore.Store(cls.path) as store:
            try:
                store.batch_get([1], aastore.ReadOptions(io_backend=aastore.IoBackend.IoUring))
                cls.backends.append(aastore.IoBackend.IoUring)
            except RuntimeError as error:
                if "io_uring unavailable" not in str(error):
                    raise
                cls.uring_reason = str(error)

    # 释放临时测试数据库。
    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    # 按指定驻留模式打开同一份数据库。
    def open_store(self, mode):
        return aastore.Store(self.path, aastore.StoreOptions(
            storage_mode=mode, cache_bytes=self.build_stats.packed_bytes // 2,
            threads=4, resident_names=True, resident_descriptions=True,
        ))

    # 检查默认诊断关闭，启用后可以观测解码阶段。
    def check_stats(self, stats, diagnostics):
        self.assertEqual(stats.diagnostics_collected, diagnostics)
        if diagnostics and stats.requested_residues:
            self.assertGreater(stats.decode_ns, 0)
        if not diagnostics:
            for field in ("layout_ns", "sort_ns", "range_plan_ns", "worker_wall_ns",
                          "worker_max_busy_ns", "read_ns", "decode_ns", "cache_decode_ns",
                          "io_submit_calls", "io_submit_ns", "io_wait_calls", "io_empty_waits"):
                self.assertEqual(getattr(stats, field), 0, field)

    # 核对建库统计、选项覆盖、文本列开关及覆盖已有库的行为。
    def test_build_and_options(self):
        self.assertEqual(self.build_stats.sequences, len(self.sequences))
        self.assertEqual(self.build_stats.residues, sum(map(len, self.sequences)))
        self.assertEqual(self.build_stats.packed_bytes, (self.build_stats.residues * 5 + 7) // 8)
        self.assertGreater(self.build_stats.name_bytes, 0)
        with self.assertRaises(AttributeError):
            self.build_stats.sequences = 0
        path = self.root / "sequence_only"
        options = aastore.BuildOptions(threads=2)
        stats = aastore.build_fasta(self.fasta, path, options, threads=1,
                                    write_names=False, write_descriptions=False)
        self.assertEqual(options.threads, 2)
        self.assertTrue(options.write_names)
        self.assertEqual((stats.name_bytes, stats.description_bytes), (0, 0))
        with aastore.Store(path) as store:
            self.assertFalse(store.has_names)
            self.assertFalse(store.has_descriptions)
            self.assertEqual(store.get(1), self.sequences[1])
            with self.assertRaises(RuntimeError):
                store.batch_get_records([1])
        with self.assertRaises(RuntimeError):
            aastore.build_fasta(self.fasta, path)
        aastore.build_fasta(self.fasta, path, overwrite=True)
        with aastore.Store(path) as store:
            self.assertTrue(store.has_names)
        with self.assertRaises(TypeError):
            aastore.build_fasta(self.fasta, path, options=object())
        self.assertFalse(aastore.ReadOptions().collect_diagnostics)
        for options_type in (aastore.ReadOptions, aastore.StoreOptions, aastore.BuildOptions):
            with self.assertRaises(TypeError):
                options_type(threads=-1)
            with self.assertRaises(TypeError):
                options_type(unknown_option=True)

    # 覆盖各驻留模式、后端与线程预算，比较索引、切片、顺读和诊断结果。
    def test_reads(self):
        ids = [130, 1, 0, 6, 2, 6, 4, 11]
        expected = [self.sequences[i] for i in ids]
        for mode in (aastore.StorageMode.ByteRange, aastore.StorageMode.PrefixMemory,
                     aastore.StorageMode.FullMemory):
            with self.open_store(mode) as store:
                self.assertEqual(len(store), len(self.sequences))
                self.assertEqual(store.size(), len(self.sequences))
                self.assertEqual(store.total_residues, self.build_stats.residues)
                self.assertEqual(store.packed_bytes, self.build_stats.packed_bytes)
                self.assertEqual(store.storage_mode, mode)
                self.assertIsInstance(store.decode_kernel_name, str)
                self.assertGreater(store.resident_text_bytes, 0)
                for i, value in enumerate(self.sequences):
                    self.assertEqual(store.get(i), value)
                    self.assertEqual(store.sequence_length(i), len(value))
                self.assertEqual(store[-1], self.sequences[-1])
                for backend in self.backends:
                    for threads in (0, 1, 4):
                        for diagnostics in (False, True):
                            with self.subTest(mode=mode, backend=backend, threads=threads, diagnostics=diagnostics):
                                options = aastore.ReadOptions(
                                    threads=threads, io_backend=backend, merge_gap_bytes=0,
                                    io_queue_depth=2, collect_diagnostics=diagnostics,
                                )
                                batch = store.batch_get((i for i in ids), options)
                                self.assertIsInstance(batch, aastore.Batch)
                                self.assertEqual(len(batch), len(ids))
                                self.assertEqual(batch.size(), len(ids))
                                self.assertFalse(batch.empty())
                                self.assertEqual(list(batch), expected)
                                self.assertEqual(batch[-1], expected[-1])
                                self.assertEqual(batch[::-1], expected[::-1])
                                self.assertEqual(batch[1::2], expected[1::2])
                                self.assertEqual(batch[1:1], [])
                                self.assertEqual(bytes(batch.view(4)), expected[4].encode())
                                self.assertTrue(batch.view(4).readonly)
                                self.assertTrue(memoryview(batch).readonly)
                                self.assertTrue(batch.storage.readonly)
                                self.assertEqual(batch.stats.requested_sequences, len(ids))
                                self.assertEqual(batch.stats.requested_residues, sum(map(len, expected)))
                                self.assertLessEqual(batch.stats.workers, max(1, threads))
                                self.check_stats(batch.stats, diagnostics)
                                for begin, end in ((0, len(store)), (1, 6), (0, 1), (3, 5), (5, 5)):
                                    sequential = store.sequential_get(begin, end, options)
                                    self.assertEqual(list(sequential), self.sequences[begin:end])
                                    self.assertEqual(len(sequential.storage), sum(map(len, self.sequences[begin:end])))
                                    self.assertFalse(sequential.stats.io_uring)
                                    self.check_stats(sequential.stats, diagnostics)
                                empty = store.batch_get([], options)
                                self.assertTrue(empty.empty())
                                self.assertEqual(list(empty), [])
                                self.assertEqual(bytes(memoryview(empty)), b"")
                                self.check_stats(empty.stats, diagnostics)
                with self.assertRaises(IndexError):
                    store.get(len(store))
                with self.assertRaises(IndexError):
                    store[-len(store) - 1]
                with self.assertRaises(IndexError):
                    batch[len(batch)]
                with self.assertRaises(IndexError):
                    batch.view(-len(batch) - 1)
                with self.assertRaises(ValueError):
                    batch[::0]
                with self.assertRaises(IndexError):
                    store.sequential_get(4, 3)

    # 检查字段掩码、UTF-8 文本、记录切片及缺失字段的报错。
    def test_records(self):
        with aastore.Store(self.path) as store:
            records = store.batch_get_records([1, 0, 1])
            expected = [dict(name=self.names[i], description=self.descriptions[i], sequence=self.sequences[i])
                        for i in (1, 0, 1)]
            self.assertEqual(list(records), expected)
            self.assertEqual(records[::-1], expected[::-1])
            self.assertEqual(records[-1], expected[-1])
            self.assertEqual(records.name(0), self.names[1])
            self.assertEqual(records.description(0), self.descriptions[1])
            self.assertEqual(records.sequence(1), "")
            self.assertEqual(list(records.names), [row["name"] for row in expected])
            self.assertEqual(list(records.descriptions), [row["description"] for row in expected])
            self.assertEqual(list(records.sequences), [row["sequence"] for row in expected])
            self.assertEqual(records.names.stats.requested_strings, 3)
            fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
            selected = store.batch_get_records([1], fields)
            self.assertEqual(selected[0], dict(name=self.names[1], sequence=self.sequences[1]))
            self.assertEqual(fields & aastore.RecordFields.Name, aastore.RecordFields.Name)
            with self.assertRaises(RuntimeError):
                selected.description(0)
            names = store.batch_get_records([1], aastore.RecordFields.Name)
            with self.assertRaises(RuntimeError):
                names.sequence(0)
            self.assertEqual(list(store.batch_get_records([1], aastore.RecordFields.None_)), [{}])

    # 保留临时批次或记录子批次的视图，确认父对象销毁后数据仍有效且不可写。
    def test_view_lifetime(self):
        store = aastore.Store(self.path)
        view = store.batch_get([1]).view(0)
        whole = store.batch_get([1]).storage
        iterator = iter(store.batch_get([1, 3]))
        records = store.batch_get_records([1])
        child = records.descriptions
        text_view = child.view(0)
        del records, child
        store.close()
        del store
        gc.collect()
        self.assertEqual(bytes(view), self.sequences[1].encode())
        self.assertEqual(bytes(whole), self.sequences[1].encode() + b"\0")
        self.assertEqual(bytes(text_view), self.descriptions[1].encode("utf-8"))
        self.assertEqual(list(iterator), [self.sequences[1], self.sequences[3]])
        with self.assertRaises(TypeError):
            view[0] = 0

    # 重用带保护字节的缓冲，覆盖直接批读和直接顺读两条路径。
    def test_direct_outputs(self):
        ids = [5, 1, 0, 7, 1]
        for mode in (aastore.StorageMode.ByteRange, aastore.StorageMode.PrefixMemory,
                     aastore.StorageMode.FullMemory):
            with self.open_store(mode) as store:
                for backend in self.backends:
                    options = aastore.ReadOptions(threads=4, io_backend=backend)
                    cases = [(ids, lambda outputs: store.batch_get_into(ids, outputs, options)),
                             (list(range(6)), lambda outputs: store.sequential_get_into(0, 6, outputs, options))]
                    for requested, read in cases:
                        buffers = [bytearray(b"#" * (len(self.sequences[i]) + 2)) for i in requested]
                        outputs = [memoryview(buffer)[1:-1] if self.sequences[i] else None
                                   for i, buffer in zip(requested, buffers)]
                        for _ in range(2):
                            stats = read(iter(outputs))
                            self.assertEqual(stats.requested_sequences, len(requested))
                            self.check_stats(stats, False)
                            for i, buffer in zip(requested, buffers):
                                self.assertEqual(buffer[1:-1], self.sequences[i].encode())
                                self.assertEqual((buffer[0], buffer[-1]), (35, 35))
                output = bytearray(len(self.sequences[1]))
                store.batch_get_into([1], [output])
                output.append(35)
                self.assertEqual(output[:-1], self.sequences[1].encode())
                typed = array.array("B", [0] * len(self.sequences[1]))
                store.batch_get_into([1], [typed])
                self.assertEqual(typed.tobytes(), self.sequences[1].encode())
                self.assertEqual(store.batch_get_into([], []).requested_sequences, 0)
                self.assertEqual(store.batch_get_into([0], [None]).requested_residues, 0)

    # 拒绝非法输出，在全部校验完成前不写入任何合法输出区。
    def test_invalid_outputs(self):
        with aastore.Store(self.path) as store:
            valid = bytearray(b"#" * len(self.sequences[1]))
            invalid = [[], [None], [bytearray(1)], [b"readonly"], [object()],
                       [memoryview(bytearray(24))[::2]], [array.array("H", [0] * 8)],
                       [memoryview(bytearray(12)).cast("B", (2, 6))], [valid, valid]]
            for outputs in invalid:
                with self.subTest(outputs=repr(outputs)):
                    with self.assertRaises((ValueError, TypeError, BufferError)):
                        store.batch_get_into([1], outputs)
                    self.assertEqual(valid, b"#" * len(valid))
            with self.assertRaises(IndexError):
                store.batch_get_into([len(store)], [valid])
            for outputs in ([valid, bytearray(1)], [valid, None], [valid, b"readonly"]):
                with self.assertRaises((ValueError, BufferError)):
                    store.batch_get_into([1, 1], outputs)
                self.assertEqual(valid, b"#" * len(valid))
            arena = bytearray(b"#" * 12)
            with self.assertRaises(ValueError):
                store.batch_get_into([1, 1], [memoryview(arena)[:6], memoryview(arena)[5:11]])
            with self.assertRaises(ValueError):
                store.batch_get_into([1, 1], [valid, valid])
            store.batch_get_into([1, 1], [memoryview(arena)[:6], memoryview(arena)[6:]])
            self.assertEqual(arena, 2 * self.sequences[1].encode())
            with self.assertRaises(ValueError):
                store.sequential_get_into(1, 2, [])
            with self.assertRaises(IndexError):
                store.sequential_get_into(2, 1, [])

    # 从多个 Python 线程共享只读 Store，核对结果不被并发调用覆盖。
    def test_parallel_callers(self):
        with aastore.Store(self.path) as store:
            options = aastore.ReadOptions(threads=2)
            ids = [1, 12, 13, 14, 1, 0]
            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(store.batch_get, ids, options) for _ in range(24)]
                for future in futures:
                    self.assertEqual(list(future.result()), [self.sequences[i] for i in ids])

    # 在缓冲生成器暂停时关闭 Store 并修改选项，已经开始的读取仍使用自己的资源和配置。
    def test_close_during_read(self):
        store = aastore.Store(self.path)
        options = aastore.ReadOptions(threads=1, io_backend=aastore.IoBackend.Pread)
        entered, resume = threading.Event(), threading.Event()
        output = bytearray(len(self.sequences[1]))

        # 在 C++ 已取得库引用后暂停，创建可重复的关闭时序。
        def outputs():
            entered.set()
            if not resume.wait(10):
                raise RuntimeError("test coordination timed out")
            yield output

        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(store.batch_get_into, [1], outputs(), options)
            try:
                self.assertTrue(entered.wait(10))
                store.close()
                options.io_backend = aastore.IoBackend.IoUring
            finally:
                resume.set()
            stats = future.result(timeout=10)
        self.assertEqual(output, self.sequences[1].encode())
        self.assertFalse(stats.io_uring)
        self.assertTrue(store.closed)
        store.close()
        with self.assertRaises(ValueError):
            store.get(1)
        with self.assertRaises(ValueError):
            store.batch_get([1])
        with self.assertRaises(ValueError):
            store.__enter__()

    # 长解码调用期间其他 Python 线程能执行，证明原生工作没有长期占用 GIL。
    def test_gil_release(self):
        with self.open_store(aastore.StorageMode.FullMemory) as store:
            active, stop = threading.Event(), threading.Event()
            progress = [0]

            # 周期性让出 GIL，避免观察线程阻塞调用线程返回。
            def observe():
                while not stop.wait(0.001):
                    if active.is_set():
                        progress[0] += 1

            worker = threading.Thread(target=observe)
            worker.start()
            previous = sys.getswitchinterval()
            try:
                sys.setswitchinterval(1.0)
                active.set()
                batch = store.batch_get([2] * 32, aastore.ReadOptions(threads=1))
                active.clear()
                self.assertEqual(batch[0], self.sequences[2])
                self.assertGreater(progress[0], 0)
            finally:
                active.clear()
                sys.setswitchinterval(previous)
                stop.set()
                worker.join(timeout=10)


if __name__ == "__main__":
    unittest.main(verbosity=2)
