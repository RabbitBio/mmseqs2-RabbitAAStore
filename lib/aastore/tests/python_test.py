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
                store.get([1], aastore.ReadOptions(io_backend=aastore.IoBackend.IoUring))
                cls.backends.append(aastore.IoBackend.IoUring)
            except RuntimeError as error:
                if "io_uring unavailable" not in str(error):
                    raise
                cls.uring_reason = str(error)

    # 释放临时测试数据库。
    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    # 用预算触发待测驻留路径，模式本身不再是开库选项。
    @staticmethod
    def preload_for_mode(mode, packed_bytes, partial_bytes=None):
        return {
            aastore.StorageMode.NoMemory: 0,
            aastore.StorageMode.PartialMemory: (max(1, packed_bytes // 2)
                                               if partial_bytes is None else partial_bytes),
            aastore.StorageMode.FullMemory: packed_bytes,
        }[mode]

    def open_store(self, mode):
        return aastore.Store(self.path, aastore.StoreOptions(
            preload_bytes=self.preload_for_mode(mode, self.build_stats.packed_bytes),
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
                          "io_submit_calls", "io_submit_ns", "io_wait_calls", "io_empty_waits",
                          "cache_hit_residues", "cache_miss_residues", "unique_packed_bytes",
                          "merged_gaps", "free_merges", "overread_bytes"):
                self.assertEqual(getattr(stats, field), 0, field)

    # 用布局视图直接切分 arena，长度按原始字节计算，不依赖字符串索引接口。
    def check_layout(self, batch, values, terminated=False):
        offsets, lengths = batch.offsets, batch.lengths
        encoded = [value.encode("utf-8") for value in values]
        for view in (offsets, lengths):
            self.assertIsInstance(view, memoryview)
            self.assertEqual(view.ndim, 1)
            self.assertEqual(view.itemsize, 8)
            self.assertIn(view.format, ("Q", "L", "@Q", "@L"))
            self.assertEqual(view.shape, (len(values),))
            self.assertEqual(view.strides, (8,))
            self.assertEqual(view.nbytes, 8 * len(values))
            self.assertTrue(view.readonly)
            self.assertTrue(view.c_contiguous)
            sliced = view[::2]
            self.assertEqual(sliced.tolist(), view.tolist()[::2])
            self.assertTrue(sliced.readonly)
            with self.assertRaises(TypeError):
                view[:] = view
            with self.assertRaises(TypeError):
                sliced[:] = sliced
        self.assertEqual(lengths.tolist(), list(map(len, encoded)))
        storage = batch.storage
        self.assertEqual(
            [bytes(storage[offset:offset + length])
             for offset, length in zip(offsets, lengths)], encoded,
        )
        self.assertEqual(storage.nbytes, sum(map(len, encoded)) +
                         (len(values) if terminated else 0))
        if terminated:
            for offset, length in zip(offsets, lengths):
                self.assertEqual(storage[offset + length], 0)
        for field in ("offsets", "lengths"):
            with self.assertRaises(AttributeError):
                setattr(batch, field, [])

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
            self.assertEqual(store.get(1)[0], self.sequences[1])
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
        self.assertEqual(aastore.StoreOptions().index_load_mode, aastore.IndexLoadMode.Mmap)
        for options_type in (aastore.ReadOptions, aastore.StoreOptions, aastore.BuildOptions):
            with self.assertRaises(TypeError):
                options_type(threads=-1)
            with self.assertRaises(TypeError):
                options_type(unknown_option=True)

    # 两种索引加载方式保持记录顺序、重复 ID 和顺读结果一致，且 Read 不映射索引文件。
    def test_index_load_modes(self):
        ids = [130, 1, 0, 2, 1, 4]
        expected = [dict(name=self.names[i], description=self.descriptions[i],
                         sequence=self.sequences[i]) for i in ids]
        index_paths = {str((self.path / name).resolve())
                       for name in ("meta.bin", "names.meta", "descriptions.meta")}
        maps = Path("/proc/self/maps")
        reads = aastore.ReadOptions(threads=2, io_backend=aastore.IoBackend.Pread)
        for mode in (aastore.IndexLoadMode.Mmap, aastore.IndexLoadMode.Read):
            with self.subTest(index_load_mode=mode):
                with aastore.Store(self.path, aastore.StoreOptions(index_load_mode=mode)) as store:
                    self.assertEqual(store.index_load_mode, mode)
                    with self.assertRaises(AttributeError):
                        store.index_load_mode = mode
                    self.assertEqual(store.resident_bytes, 0)
                    self.assertEqual(store.resident_text_bytes, 0)
                    self.assertEqual(list(store.batch_get_records(ids, options=reads)), expected)
                    self.assertEqual([store.sequence_length(i) for i in ids],
                                     [len(self.sequences[i]) for i in ids])
                    self.assertEqual(list(store.sequential_get(0, 5, reads)), self.sequences[:5])
                    if maps.is_file():
                        mapped = {fields[5].strip()
                                  for line in maps.read_text().splitlines()
                                  if len(fields := line.split(maxsplit=5)) == 6}
                        self.assertEqual(index_paths & mapped,
                                         index_paths if mode == aastore.IndexLoadMode.Mmap else set())

    # 无效配置在空请求和各读取路径上都被拒绝，且不创建库或修改输出。
    def test_option_validation(self):
        for options_type in (aastore.BuildOptions, aastore.StoreOptions, aastore.ReadOptions):
            self.assertIsNone(options_type().validate())

        build = aastore.BuildOptions()
        build.threads = 0
        with self.assertRaises(ValueError):
            build.validate()
        output_path = self.root / "invalid-options-store"
        with self.assertRaises(ValueError):
            aastore.build_fasta(self.root / "missing.fasta", output_path, build)
        self.assertFalse(output_path.exists())
        with self.assertRaises(ValueError):
            aastore.build_fasta(self.fasta, output_path, threads=0)
        self.assertFalse(output_path.exists())

        for mode in (aastore.IndexLoadMode(99), aastore.IndexLoadMode(255)):
            options = aastore.StoreOptions(index_load_mode=mode)
            with self.subTest(invalid_index_load_mode=mode):
                with self.assertRaises(ValueError):
                    options.validate()
                for path in (self.path, self.root / "missing-store"):
                    with self.assertRaises(ValueError):
                        aastore.Store(path, options)

        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
                     aastore.StorageMode.FullMemory):
            with self.open_store(mode) as store:
                for backend in (aastore.IoBackend(99), aastore.IoBackend(255)):
                    options = aastore.ReadOptions(io_backend=backend)
                    with self.subTest(mode=mode, invalid_backend=backend):
                        with self.assertRaises(ValueError):
                            options.validate()
                        with self.assertRaises(ValueError):
                            store.get(1, options)
                        output = bytearray(b"######")
                        with self.assertRaises(ValueError):
                            store.get_into(1, output, options)
                        self.assertEqual(output, b"######")
                        for ids in ([1], []):
                            with self.assertRaises(ValueError):
                                store.get(ids, options)
                            for fields in (aastore.RecordFields.None_, aastore.RecordFields.Name,
                                           aastore.RecordFields.Description, aastore.RecordFields.Sequence,
                                           aastore.RecordFields.All):
                                with self.assertRaises(ValueError):
                                    store.batch_get_records(ids, fields, options)
                            buffers = [bytearray(b"#" * len(self.sequences[i])) for i in ids]
                            with self.assertRaises(ValueError):
                                store.get_into(ids, buffers, options)
                            self.assertEqual(buffers, [b"#" * len(self.sequences[i]) for i in ids])
                        for end in (1, 2):
                            with self.assertRaises(ValueError):
                                store.sequential_get(1, end, options)
                            buffers = [bytearray(b"#" * len(self.sequences[i])) for i in range(1, end)]
                            with self.assertRaises(ValueError):
                                store.sequential_get_into(1, end, buffers, options)
                            self.assertEqual(buffers, [b"#" * len(self.sequences[i]) for i in range(1, end)])

    # 单条和批量 get 都返回 Batch；直接输出复用调用方内存而不修改尾部。
    def test_unified_get(self):
        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
                     aastore.StorageMode.FullMemory):
            with self.open_store(mode) as store:
                self.assertFalse(hasattr(store, "batch_get"))
                self.assertFalse(hasattr(store, "batch_get_into"))
                for backend in self.backends:
                    for diagnostics in (False, True):
                        reads = aastore.ReadOptions(threads=8, io_backend=backend,
                                                   collect_diagnostics=diagnostics)
                        for id in (0, 1, 2, 3):
                            with self.subTest(mode=mode, backend=backend, id=id,
                                              diagnostics=diagnostics):
                                batch = store.get(id=id, options=reads)
                                self.assertIsInstance(batch, aastore.Batch)
                                self.assertEqual(list(batch), [self.sequences[id]])
                                self.assertEqual(list(batch), list(store.get([id], reads)))
                                self.assertEqual(batch.stats.requested_sequences, 1)
                                self.assertLessEqual(batch.stats.workers, 1)
                                self.check_stats(batch.stats, diagnostics)
                                self.check_layout(batch, [self.sequences[id]], terminated=True)
                                output = bytearray(b"#" * (len(self.sequences[id]) + 1))
                                stats = store.get_into(id=id, output=output, options=reads)
                                self.assertEqual(output, self.sequences[id].encode() + b"#")
                                self.assertEqual(stats.requested_sequences, 1)
                                self.check_stats(stats, diagnostics)
                self.assertEqual(len(store.get([])), 0)
                self.assertIsInstance(store[1], str)
                self.assertEqual(store.get_into(0, None).requested_residues, 0)
                output = bytearray(b"######")
                for invalid in (None, b"######", "######", bytearray(5),
                                memoryview(bytearray(12))[::2]):
                    with self.assertRaises((ValueError, TypeError, BufferError)):
                        store.get_into(1, invalid)
                with self.assertRaises(IndexError):
                    store.get_into(len(store), output)
                self.assertEqual(output, b"######")
                view = store.get(1).view(0)
            gc.collect()
            self.assertEqual(bytes(view), self.sequences[1].encode())

    # 保留零线程、零队列和零预算的既有语义，校验不负责探测后端可用性。
    def test_zero_option_budgets(self):
        options = aastore.ReadOptions(
            threads=0, io_queue_depth=0, io_buffer_bytes=0, input_sorted=True,
            merge_gap_bytes=0, max_merged_range_bytes=0,
            max_exact_merged_range_bytes=0,
        )
        for backend in (aastore.IoBackend.Auto, aastore.IoBackend.Pread, aastore.IoBackend.IoUring):
            options.io_backend = backend
            self.assertIsNone(options.validate())
        options.io_backend = aastore.IoBackend.Pread
        ids = [1, 1, 3, 5]
        expected = [self.sequences[i] for i in ids]
        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
                     aastore.StorageMode.FullMemory):
            store_options = aastore.StoreOptions(
                preload_bytes=self.preload_for_mode(mode, self.build_stats.packed_bytes, 1),
                threads=0,
            )
            self.assertIsNone(store_options.validate())
            with aastore.Store(self.path, store_options) as store:
                self.assertEqual(list(store.get(ids, options)), expected)
                self.assertEqual(list(store.sequential_get(3, 6, options)), self.sequences[3:6])
                buffers = [bytearray(len(sequence)) for sequence in expected]
                store.get_into(ids, buffers, options)
                self.assertEqual(buffers, [sequence.encode() for sequence in expected])

    # 预算边界决定实际驻留方式，达到全量时走无序请求的全内存快速路径。
    def test_preload_budget(self):
        packed = self.build_stats.packed_bytes
        ids = [5, 1, 2, 0, 1]
        expected = [self.sequences[i] for i in ids]
        reads = aastore.ReadOptions(io_backend=aastore.IoBackend.Pread)
        for budget, mode in (
            (0, aastore.StorageMode.NoMemory),
            (1, aastore.StorageMode.PartialMemory),
            (packed - 1, aastore.StorageMode.PartialMemory),
            (packed, aastore.StorageMode.FullMemory),
            (packed + 1, aastore.StorageMode.FullMemory),
            ((1 << 64) - 1, aastore.StorageMode.FullMemory),
        ):
            with self.subTest(budget=budget), aastore.Store(
                    self.path, aastore.StoreOptions(preload_bytes=budget)) as store:
                self.assertEqual(store.storage_mode, mode)
                self.assertEqual(store.resident_bytes, min(budget, packed))
                batch = store.get(ids, reads)
                self.assertEqual(list(batch), expected)
                self.assertEqual(batch.stats.full_memory, mode == aastore.StorageMode.FullMemory)
                self.assertEqual(batch.stats.prefix_memory, mode == aastore.StorageMode.PartialMemory)
                if mode == aastore.StorageMode.FullMemory:
                    self.assertEqual(batch.stats.read_bytes, 0)
                    self.assertEqual(batch.stats.ranges, 0)
                with self.assertRaises(AttributeError):
                    store.storage_mode = aastore.StorageMode.NoMemory

        for budget in (-1, 1 << 64):
            with self.subTest(invalid_budget=budget):
                with self.assertRaises(TypeError):
                    aastore.StoreOptions(preload_bytes=budget)
                with self.assertRaises(TypeError):
                    aastore.StoreOptions().preload_bytes = budget
        for field in ("storage_mode", "cache_bytes"):
            with self.subTest(removed_option=field):
                with self.assertRaises(TypeError):
                    aastore.StoreOptions(**{field: 0})
                self.assertFalse(hasattr(aastore.StoreOptions(), field))
                with self.assertRaises(AttributeError):
                    setattr(aastore.StoreOptions(), field, 0)
        self.assertFalse(hasattr(aastore.StorageMode, "ByteRange"))
        self.assertFalse(hasattr(aastore.StorageMode, "PrefixMemory"))

    # 空编码文件在零预算时保持按需模式，正预算时全驻留且不分配序列字节。
    def test_empty_preload_budget(self):
        for label, contents, expected in (("empty", "", []),
                                           ("empty-sequence", ">empty\n", [""])):
            fasta, path = self.root / (label + ".fasta"), self.root / (label + "-store")
            fasta.write_text(contents)
            stats = aastore.build_fasta(fasta, path)
            self.assertEqual(stats.packed_bytes, 0)
            for budget in (0, 1, (1 << 64) - 1):
                with self.subTest(label=label, budget=budget), aastore.Store(
                        path, aastore.StoreOptions(preload_bytes=budget)) as store:
                    self.assertEqual(store.storage_mode, aastore.StorageMode.NoMemory if budget == 0
                                     else aastore.StorageMode.FullMemory)
                    self.assertEqual(store.resident_bytes, 0)
                    self.assertEqual(list(store.get(list(range(len(expected))))), expected)
                    self.assertEqual(list(store.sequential_get(0, len(expected))), expected)

    # 已删除的策略选项必须报错，不能被静默接受。
    def test_removed_scan_options(self):
        self.assertFalse(hasattr(aastore, "ReadStrategy"))
        for field, value in (("read_strategy", 0), ("scan_max_range_bytes", 0)):
            with self.subTest(field=field):
                with self.assertRaises(TypeError):
                    aastore.ReadOptions(**{field: value})
                self.assertFalse(hasattr(aastore.ReadOptions(), field))
        with aastore.Store(self.path) as store:
            stats = store.get([]).stats
            for field in ("sparse_ranges", "scan_ranges"):
                self.assertFalse(hasattr(stats, field))

    # Cover residency, backends, thread budgets, batch views, and diagnostics.
    def test_reads(self):
        ids = [130, 1, 0, 6, 2, 6, 4, 11]
        expected = [self.sequences[i] for i in ids]
        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
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
                    self.assertEqual(store.get(i)[0], value)
                    self.assertEqual(store.sequence_length(i), len(value))
                self.assertEqual(store[-1], self.sequences[-1])
                for backend in self.backends:
                    # Sequential reads use pread regardless of the backend option.
                    with self.subTest(mode=mode, backend=backend, access="sequential"):
                        sequential = store.sequential_get(
                            1, 2, aastore.ReadOptions(threads=2, io_backend=backend)
                        )
                        self.assertEqual(list(sequential), self.sequences[1:2])
                        self.assertFalse(sequential.stats.io_uring)
                    for threads in (0, 1, 4):
                        for diagnostics in (False, True):
                            with self.subTest(mode=mode, backend=backend, threads=threads, diagnostics=diagnostics):
                                options = aastore.ReadOptions(
                                    threads=threads, io_backend=backend, merge_gap_bytes=0,
                                    io_queue_depth=2, collect_diagnostics=diagnostics,
                                )
                                batch = store.get((i for i in ids), options)
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
                                empty = store.get([], options)
                                self.assertTrue(empty.empty())
                                self.assertEqual(list(empty), [])
                                self.assertEqual(bytes(memoryview(empty)), b"")
                                self.check_stats(empty.stats, diagnostics)
                # Exercise each interval once per residency/thread/diagnostic setting.
                for threads in (0, 1, 4):
                    for diagnostics in (False, True):
                        with self.subTest(mode=mode, threads=threads, diagnostics=diagnostics,
                                          access="sequential intervals"):
                            options = aastore.ReadOptions(
                                threads=threads, io_backend=aastore.IoBackend.Pread,
                                collect_diagnostics=diagnostics,
                            )
                            for begin, end in ((0, len(store)), (1, 6), (0, 1), (3, 5), (5, 5)):
                                sequential = store.sequential_get(begin, end, options)
                                self.assertEqual(list(sequential), self.sequences[begin:end])
                                self.assertEqual(len(sequential.storage), sum(map(len, self.sequences[begin:end])))
                                self.assertFalse(sequential.stats.io_uring)
                                self.check_stats(sequential.stats, diagnostics)
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
            self.assertEqual(list(names), [dict(name=self.names[1])])
            descriptions = store.batch_get_records([1], aastore.RecordFields.Description)
            self.assertEqual(list(descriptions), [dict(description=self.descriptions[1])])
            for text_only in (names, descriptions):
                self.assertEqual(text_only.sequences.stats.requested_sequences, 0)
                self.assertEqual(text_only.sequences.stats.read_bytes, 0)
            with self.assertRaises(RuntimeError):
                names.sequence(0)
            self.assertEqual(list(store.batch_get_records([1], aastore.RecordFields.None_)), [{}])

    # 长度查询使用文本索引，返回 UTF-8 字节数并遵守列存在性、ID 和关闭规则。
    def test_text_lengths(self):
        fasta, path = self.root / "text-lengths.fasta", self.root / "text-lengths-store"
        names, descriptions = ["", "na\u00efve", "tail"], ["", "\u63cf\u8ff0 \u00b5", ""]
        fasta.write_text("".join(
            ">{}{}\nACD\n".format(name, " " + description if description else "")
            for name, description in zip(names, descriptions)
        ), encoding="utf-8")
        aastore.build_fasta(fasta, path)
        for index_mode in (aastore.IndexLoadMode.Mmap, aastore.IndexLoadMode.Read):
            for resident in (False, True):
                with self.subTest(index_mode=index_mode, resident=resident), aastore.Store(
                        path, aastore.StoreOptions(index_load_mode=index_mode,
                                                  resident_names=resident,
                                                  resident_descriptions=resident)) as store:
                    records = store.batch_get_records(range(len(names)))
                    for i, (name, description) in enumerate(zip(names, descriptions)):
                        self.assertEqual(store.name_length(i), len(name.encode("utf-8")))
                        self.assertEqual(store.description_length(i), len(description.encode("utf-8")))
                        self.assertEqual(store.name_length(i), records.names.lengths[i])
                        self.assertEqual(store.description_length(i), records.descriptions.lengths[i])
                    self.assertGreater(store.name_length(1), len(names[1]))
                    self.assertGreater(store.description_length(1), len(descriptions[1]))
                    for method in (store.name_length, store.description_length):
                        for invalid in (len(names), (1 << 64) - 1):
                            with self.assertRaises(IndexError):
                                method(invalid)
                        for invalid in (-1, 1 << 64):
                            with self.assertRaises(TypeError):
                                method(invalid)
                for method in (store.name_length, store.description_length):
                    with self.assertRaises(ValueError):
                        method(0)

        for write_names, write_descriptions in ((False, True), (True, False), (False, False)):
            missing_path = self.root / f"text-lengths-{write_names}-{write_descriptions}"
            aastore.build_fasta(fasta, missing_path,
                               write_names=write_names, write_descriptions=write_descriptions)
            with aastore.Store(missing_path) as store:
                for present, method in ((write_names, store.name_length),
                                        (write_descriptions, store.description_length)):
                    if not present:
                        with self.assertRaises(RuntimeError):
                            method(0)

        # Once open, querying lengths must not depend on reading either text file.
        with aastore.Store(path, aastore.StoreOptions(index_load_mode=aastore.IndexLoadMode.Read)) as store:
            (path / "names.bin").write_bytes(b"")
            (path / "descriptions.bin").write_bytes(b"")
            self.assertEqual(store.name_length(1), len(names[1].encode("utf-8")))
            self.assertEqual(store.description_length(1), len(descriptions[1].encode("utf-8")))

    # 覆盖乱序及重复请求、空序列和空文本；UTF-8 边界以字节而非字符计量。
    def test_layout_views(self):
        fasta = self.root / "layout.fasta"
        path = self.root / "layout-store"
        sequences = ["", "ACDEFG", "A", ""]
        names = ["empty", "na\u00efve", "tail", "empty2"]
        descriptions = ["", "\u63cf\u8ff0 \u00b5", "", "\u5c3e\u58f0"]
        fasta.write_text("".join(
            ">{}{}\n{}\n".format(name, " " + description if description else "", sequence)
            for name, description, sequence in zip(names, descriptions, sequences)
        ), encoding="utf-8")
        stats = aastore.build_fasta(fasta, path)
        ids = [2, 1, 0, 1, 3]
        options = aastore.ReadOptions(threads=2, io_backend=aastore.IoBackend.Pread)
        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
                     aastore.StorageMode.FullMemory):
            with self.subTest(mode=mode), aastore.Store(path, aastore.StoreOptions(
                    preload_bytes=self.preload_for_mode(mode, stats.packed_bytes))) as store:
                self.check_layout(store.get(ids, options),
                                  [sequences[i] for i in ids], terminated=True)
                self.check_layout(store.sequential_get(0, len(sequences), options), sequences)
                self.check_layout(store.get([], options), [], terminated=True)
                self.check_layout(store.sequential_get(2, 2, options), [])
                records = store.batch_get_records(ids, options=options)
                self.check_layout(records.sequences, [sequences[i] for i in ids], terminated=True)
                self.check_layout(records.names, [names[i] for i in ids])
                self.check_layout(records.descriptions, [descriptions[i] for i in ids])
                self.assertGreater(records.names.lengths[1], len(names[1]))
                self.assertGreater(records.descriptions.lengths[1], len(descriptions[1]))
                self.assertEqual(records.descriptions.lengths[2], 0)
                empty = store.batch_get_records([], options=options)
                for column in (empty.names, empty.descriptions, empty.sequences):
                    self.check_layout(column, [])
                with self.assertRaises(AttributeError):
                    records.names.stats.requested_bytes = 0

    # 每条序列占 50 packed 字节，按已知文件区间检验零/非零合并预算。
    def test_range_budgets(self):
        fasta = self.root / "range-budget.fasta"
        path = self.root / "range-budget-store"
        alphabet = "ACDEFGHIKLMNPQRSTVWY"
        sequences = [(alphabet[i:] + alphabet[:i]) * 4 for i in range(8)]
        fasta.write_text("".join(
            ">sequence{}\n{}\n".format(i, value) for i, value in enumerate(sequences)
        ), encoding="ascii")
        aastore.build_fasta(fasta, path, write_names=False, write_descriptions=False)
        sparse_ids, adjacent_ids = [0, 0, 2, 4, 6], [0, 0, 1, 2, 3]
        # 预算只限制合并结果，零取消限额；单条读取本身仍必须完整执行。
        cases = [
            ("sparse-gap-limited", sparse_ids,
             50, 100, 0, 256, (4, 200, 50, 0)),
            ("sparse-gap-unlimited", sparse_ids,
             50, 0, 0, 0, (1, 350, 350, 3)),
            ("sparse-zero-gap", sparse_ids,
             0, 0, 0, 0, (4, 200, 50, 0)),
            ("exact-limited", adjacent_ids,
             0, 1, 75, 256, (4, 200, 50, 0)),
            ("exact-unlimited", adjacent_ids,
             0, 1, 0, 0, (1, 200, 200, 0)),
        ]
        with aastore.Store(path) as store:
            for label, ids, gap, merged, exact, buffer_bytes, expected in cases:
                for backend in self.backends:
                    with self.subTest(case=label, backend=backend):
                        # input_sorted 是调用方承诺，当前实现不会验证非法顺序。
                        options = aastore.ReadOptions(
                            threads=2, io_backend=backend, input_sorted=True,
                            merge_gap_bytes=gap,
                            max_merged_range_bytes=merged, max_exact_merged_range_bytes=exact,
                            io_buffer_bytes=buffer_bytes,
                        )
                        batch = store.get(ids, options)
                        self.assertEqual(list(batch), [sequences[i] for i in ids])
                        stats = batch.stats
                        self.assertEqual(stats.requested_sequences, 5)
                        self.assertEqual(stats.requested_residues, 400)
                        fields = ("ranges", "read_bytes", "max_range_bytes")
                        self.assertEqual(tuple(getattr(stats, field) for field in fields), expected[:3])
                        self.check_stats(stats, False)
                        options.collect_diagnostics = True
                        measured = store.get(ids, options)
                        self.assertEqual(list(measured), list(batch))
                        self.assertEqual(measured.stats.unique_packed_bytes, 200)
                        self.assertEqual(measured.stats.overread_bytes, stats.read_bytes - 200)
                        self.assertEqual(measured.stats.merged_gaps, expected[3])
                        self.assertEqual(tuple(getattr(measured.stats, field) for field in fields),
                                         expected[:3])
                        self.check_stats(measured.stats, True)
                        with self.assertRaises(AttributeError):
                            stats.read_bytes = 0

    # 保留临时批次或记录子批次的视图，确认父对象销毁后数据仍有效且不可写。
    def test_view_lifetime(self):
        store = aastore.Store(self.path)
        view = store.get([1]).view(0)
        whole = store.get([1]).storage
        iterator = iter(store.get([1, 3]))
        records = store.batch_get_records([1])
        child = records.descriptions
        text_view = child.view(0)
        offsets = store.get([1, 0, 1]).offsets[::2]
        batch = store.get([1, 0, 1])
        lengths = batch.lengths[::-1]
        expected_offsets = offsets.tolist()
        sequential_lengths = store.sequential_get(0, 2).lengths
        layout_records = store.batch_get_records([1])
        record_offsets = layout_records.descriptions.offsets
        record_lengths = store.batch_get_records([1]).descriptions.lengths
        name_lengths = store.batch_get_records([1]).names.lengths
        sequence_lengths = store.batch_get_records([1]).sequences.lengths
        del batch, layout_records
        del records, child
        store.close()
        del store
        gc.collect()
        self.assertEqual(bytes(view), self.sequences[1].encode())
        self.assertEqual(bytes(whole), self.sequences[1].encode() + b"\0")
        self.assertEqual(bytes(text_view), self.descriptions[1].encode("utf-8"))
        self.assertEqual(list(iterator), [self.sequences[1], self.sequences[3]])
        self.assertEqual(offsets.tolist(), expected_offsets)
        self.assertEqual(lengths.tolist(), [6, 0, 6])
        self.assertEqual(sequential_lengths.tolist(), [0, 6])
        self.assertEqual(record_offsets.tolist(), [0])
        self.assertEqual(record_lengths.tolist(), [len(self.descriptions[1].encode("utf-8"))])
        self.assertEqual(name_lengths.tolist(), [len(self.names[1].encode("utf-8"))])
        self.assertEqual(sequence_lengths.tolist(), [6])
        for layout in (offsets, lengths, sequential_lengths, record_offsets, record_lengths,
                       name_lengths, sequence_lengths):
            self.assertTrue(layout.readonly)
            with self.assertRaises(TypeError):
                layout[0] = 0
        with self.assertRaises(TypeError):
            view[0] = 0

    # 重用带保护字节的缓冲，覆盖直接批读和直接顺读两条路径。
    def test_direct_outputs(self):
        ids = [5, 1, 0, 7, 1]
        for mode in (aastore.StorageMode.NoMemory, aastore.StorageMode.PartialMemory,
                     aastore.StorageMode.FullMemory):
            with self.open_store(mode) as store:
                for backend in self.backends:
                    options = aastore.ReadOptions(threads=4, io_backend=backend)
                    cases = [(ids, lambda outputs: store.get_into(ids, outputs, options)),
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
                store.get_into([1], [output])
                output.append(35)
                self.assertEqual(output[:-1], self.sequences[1].encode())
                typed = array.array("B", [0] * len(self.sequences[1]))
                store.get_into([1], [typed])
                self.assertEqual(typed.tobytes(), self.sequences[1].encode())
                self.assertEqual(store.get_into([], []).requested_sequences, 0)
                self.assertEqual(store.get_into([0], [None]).requested_residues, 0)

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
                        store.get_into([1], outputs)
                    self.assertEqual(valid, b"#" * len(valid))
            with self.assertRaises(IndexError):
                store.get_into([len(store)], [valid])
            for outputs in ([valid, bytearray(1)], [valid, None], [valid, b"readonly"]):
                with self.assertRaises((ValueError, BufferError)):
                    store.get_into([1, 1], outputs)
                self.assertEqual(valid, b"#" * len(valid))
            arena = bytearray(b"#" * 12)
            with self.assertRaises(ValueError):
                store.get_into([1, 1], [memoryview(arena)[:6], memoryview(arena)[5:11]])
            with self.assertRaises(ValueError):
                store.get_into([1, 1], [valid, valid])
            store.get_into([1, 1], [memoryview(arena)[:6], memoryview(arena)[6:]])
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
                futures = [pool.submit(store.get, ids, options) for _ in range(24)]
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
            future = pool.submit(store.get_into, [1], outputs(), options)
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
            store.get([1])
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
                batch = store.get([2] * 32, aastore.ReadOptions(threads=1))
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
