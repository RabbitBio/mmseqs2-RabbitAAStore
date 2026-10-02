// 文件职责：将现有 C++ 建库、读取和批次对象绑定到 Python，不重写核心算法。
// 长操作释放 GIL；缓冲视图保留所有者，直接输出在写入前校验容量和重叠。
#include "aastore/store.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;
using namespace aastore;

namespace {

// 校验原生长度可由 Python 的索引和缓冲协议表示。
py::ssize_t python_size(uint64_t size) {
    if (size > static_cast<uint64_t>(std::numeric_limits<py::ssize_t>::max())) {
        throw std::overflow_error("size exceeds the Python address space");
    }
    return static_cast<py::ssize_t>(size);
}

// 支持 Python 负索引，越界时抛出 IndexError。
size_t checked_index(py::ssize_t index, uint64_t size) {
    const py::ssize_t count = python_size(size);
    if (index < 0) index += count;
    if (index < 0 || index >= count) throw py::index_error("index out of range");
    return static_cast<size_t>(index);
}

// 返回 UTF-8 字符串副本，空视图无需访问数据指针。
py::str copy_text(std::string_view text) {
    return py::str(text.empty() ? "" : text.data(), text.size());
}

// 创建惰性的序列迭代器，由 Python 持有并延长批次对象的生命周期。
py::object sequence_iterator(py::object owner) {
    PyObject* iterator = PySeqIter_New(owner.ptr());
    if (iterator == nullptr) throw py::error_already_set();
    return py::reinterpret_steal<py::object>(iterator);
}

// 导出只读 arena；Python buffer 协议会延长批次对象的生命周期。
template <typename T>
py::buffer_info batch_buffer(T& batch) {
    return py::buffer_info(
        batch.storage.empty() ? const_cast<char*>("") : batch.storage.data(),
        1, "B", 1, {python_size(batch.storage.size())}, {1}, true);
}

// 按批次索引复制一条字符串，避免将整个 arena 自动转换为字符串列表。
template <typename T>
py::str batch_item(const T& batch, py::ssize_t index) {
    const size_t i = checked_index(index, batch.size());
    if (batch.lengths[i] == 0) return py::str("");
    return copy_text(batch.view(i));
}

// 以 Python memoryview 切片借用一条记录的字节，切片仍保留整个批次。
template <typename T>
py::object batch_view(py::object owner, py::ssize_t index) {
    const auto& batch = owner.cast<const T&>();
    const size_t i = checked_index(index, batch.size());
    const py::ssize_t offset = python_size(batch.offsets[i]);
    const py::ssize_t length = python_size(batch.lengths[i]);
    py::memoryview storage(owner);
    return storage[py::slice(offset, offset + length, 1)];
}

// 复制 Python 切片选择的条目，按切片顺序返回列表。
template <typename T>
py::list batch_slice(const T& batch, const py::slice& slice) {
    py::ssize_t begin, end, step, count;
    if (!slice.compute(python_size(batch.size()), &begin, &end, &step, &count)) {
        throw py::error_already_set();
    }
    py::list result;
    for (py::ssize_t i = 0; i < count; ++i) {
        result.append(batch_item(batch, begin));
        if (i + 1 < count) begin += step;
    }
    return result;
}

// 为序列和文本批次提供统一的索引、切片、统计和只读缓冲接口。
template <typename T>
void bind_batch(py::module_& module, const char* name) {
    py::class_<T>(module, name, py::buffer_protocol())
        .def("__len__", [](const T& batch) { return python_size(batch.size()); })
        .def("size", &T::size)
        .def("empty", &T::empty)
        .def("__getitem__", &batch_item<T>)
        .def("__getitem__", &batch_slice<T>)
        .def("__iter__", &sequence_iterator)
        .def("view", &batch_view<T>, py::arg("index"),
             "Return a read-only byte memoryview that keeps this batch alive.")
        .def_property_readonly("storage", [](py::object owner) {
            return py::memoryview(owner);
        })
        .def_property_readonly("stats", [](const T& batch) { return batch.stats; })
        .def_buffer(&batch_buffer<T>);
}

// 包装 Store 的关闭语义；在途读取持有独立 shared_ptr，不受同时 close 的影响。
class PythonStore {
public:
    // 接管已经打开的原生库。
    explicit PythonStore(std::shared_ptr<Store> store) : store_(std::move(store)) {}

    // 在持有 GIL 时取得本次操作的资源引用，拒绝关闭后的新读取。
    std::shared_ptr<Store> acquire() const {
        if (!store_) throw py::value_error("Store is closed");
        return store_;
    }

    // 关闭本对象；已经开始的读取在其资源引用释放后才完成真正回收。
    void close() { store_.reset(); }

    // 判断是否已关闭。
    bool closed() const { return !store_; }

private:
    std::shared_ptr<Store> store_;
};

// 从 ID 列表取得长度，同时在申请输出缓冲前验证全部 ID。
std::vector<uint64_t> request_lengths(const Store& store,
                                      const std::vector<SequenceId>& ids) {
    std::vector<uint64_t> lengths;
    lengths.reserve(ids.size());
    for (SequenceId id : ids) lengths.push_back(store.sequence_length(id));
    return lengths;
}

// 验证连续半开区间，并取得每条序列的长度。
std::vector<uint64_t> interval_lengths(const Store& store,
                                       SequenceId begin, SequenceId end) {
    if (begin > end || end > store.size()) throw py::index_error("invalid sequence interval");
    python_size(end - begin);
    std::vector<uint64_t> lengths;
    lengths.reserve(static_cast<size_t>(end - begin));
    for (SequenceId id = begin; id < end; ++id) lengths.push_back(store.sequence_length(id));
    return lengths;
}

struct WritableOutputs {
    std::vector<py::buffer_info> buffers;
    std::vector<char*> pointers;

    // 申请并固定可写缓冲；拒绝容量不足、非连续字节视图及相互重叠的写入区。
    WritableOutputs(const std::vector<uint64_t>& lengths, const py::iterable& outputs) {
        buffers.reserve(lengths.size());
        pointers.reserve(lengths.size());
        std::vector<std::pair<uintptr_t, uintptr_t>> intervals;
        intervals.reserve(lengths.size());
        for (py::handle output : outputs) {
            const size_t i = pointers.size();
            if (i >= lengths.size()) throw py::value_error("output count does not match requests");
            if (output.is_none()) {
                if (lengths[i] != 0) throw py::value_error("nonempty sequence needs an output buffer");
                pointers.push_back(nullptr);
                continue;
            }
            if (!PyObject_CheckBuffer(output.ptr())) {
                throw py::type_error("outputs must contain writable byte buffers or None");
            }
            auto info = py::reinterpret_borrow<py::buffer>(output).request(true);
            if (info.ndim != 1 || info.itemsize != 1 ||
                (info.size != 0 && info.strides[0] != 1)) {
                throw py::value_error("output must be a contiguous one-dimensional byte buffer");
            }
            if (info.size < 0 || static_cast<uint64_t>(info.size) < lengths[i]) {
                throw py::value_error("output buffer is too small");
            }
            if (lengths[i] != 0) {
                const uintptr_t begin = reinterpret_cast<uintptr_t>(info.ptr);
                if (begin == 0 || lengths[i] > std::numeric_limits<uintptr_t>::max() - begin) {
                    throw py::value_error("invalid output buffer address");
                }
                intervals.emplace_back(begin, begin + static_cast<uintptr_t>(lengths[i]));
            }
            pointers.push_back(static_cast<char*>(info.ptr));
            buffers.push_back(std::move(info));
        }
        if (pointers.size() != lengths.size()) throw py::value_error("output count does not match requests");
        std::sort(intervals.begin(), intervals.end());
        for (size_t i = 1; i < intervals.size(); ++i) {
            if (intervals[i].first < intervals[i - 1].second) {
                throw py::value_error("output buffers overlap");
            }
        }
    }
};

// 直接读取到固定的 Python 输出区；释放 GIL 前全部缓冲均已校验并取得引用。
BatchStats batch_into(const PythonStore& self, const std::vector<SequenceId>& ids,
                       const py::iterable& outputs, ReadOptions options) {
    const auto store = self.acquire();
    WritableOutputs prepared(request_lengths(*store, ids), outputs);
    py::gil_scoped_release release;
    return store->batch_get_into(ids, prepared.pointers, options);
}

// 将连续区间读取到 Python 输出区，保持与 C++ 接口相同的 pread 路径。
BatchStats sequential_into(const PythonStore& self, SequenceId begin, SequenceId end,
                            const py::iterable& outputs, ReadOptions options) {
    const auto store = self.acquire();
    WritableOutputs prepared(interval_lengths(*store, begin, end), outputs);
    py::gil_scoped_release release;
    return store->sequential_get_into(begin, end, prepared.pointers, options);
}

// 按字段掩码复制一条记录，未请求的字段不出现在字典中。
py::dict record_item(const RecordBatch& records, py::ssize_t index) {
    const size_t i = checked_index(index, records.size());
    py::dict result;
    if (has_field(records.fields(), RecordFields::Name)) result["name"] = copy_text(records.name_view(i));
    if (has_field(records.fields(), RecordFields::Description)) {
        result["description"] = copy_text(records.description_view(i));
    }
    if (has_field(records.fields(), RecordFields::Sequence)) {
        result["sequence"] = batch_item(records.sequences, static_cast<py::ssize_t>(i));
    }
    return result;
}

// 返回记录切片的字典列表。
py::list record_slice(const RecordBatch& records, const py::slice& slice) {
    py::ssize_t begin, end, step, count;
    if (!slice.compute(python_size(records.size()), &begin, &end, &step, &count)) {
        throw py::error_already_set();
    }
    py::list result;
    for (py::ssize_t i = 0; i < count; ++i) {
        result.append(record_item(records, begin));
        if (i + 1 < count) begin += step;
    }
    return result;
}

// 定义选项和枚举；Python 建库统一使用 threads，映射到 C++ 的 encode_threads。
void bind_options(py::module_& module) {
    py::enum_<StorageMode>(module, "StorageMode")
        .value("ByteRange", StorageMode::ByteRange)
        .value("PrefixMemory", StorageMode::PrefixMemory)
        .value("FullMemory", StorageMode::FullMemory);
    py::enum_<IoBackend>(module, "IoBackend")
        .value("Auto", IoBackend::Auto).value("Pread", IoBackend::Pread)
        .value("IoUring", IoBackend::IoUring);
    py::enum_<ReadStrategy>(module, "ReadStrategy")
        .value("Sparse", ReadStrategy::Sparse).value("Scan", ReadStrategy::Scan);
    py::enum_<RecordFields>(module, "RecordFields", py::arithmetic())
        .value("None_", RecordFields::None).value("Name", RecordFields::Name)
        .value("Description", RecordFields::Description).value("Sequence", RecordFields::Sequence)
        .value("All", RecordFields::All)
        .def("__or__", [](RecordFields left, RecordFields right) { return left | right; }, py::is_operator())
        .def("__and__", [](RecordFields left, RecordFields right) { return left & right; }, py::is_operator());

    py::class_<BuildOptions>(module, "BuildOptions")
        .def(py::init([](size_t threads, bool overwrite, bool names, bool descriptions) {
            return BuildOptions{overwrite, names, descriptions, threads};
        }), py::kw_only(), py::arg("threads") = 1, py::arg("overwrite") = false,
            py::arg("write_names") = true, py::arg("write_descriptions") = true)
        .def_readwrite("threads", &BuildOptions::encode_threads)
        .def_readwrite("overwrite", &BuildOptions::overwrite)
        .def_readwrite("write_names", &BuildOptions::write_names)
        .def_readwrite("write_descriptions", &BuildOptions::write_descriptions);
    py::class_<StoreOptions>(module, "StoreOptions")
        .def(py::init([](StorageMode mode, uint64_t cache, size_t threads, bool names, bool descriptions) {
            return StoreOptions{mode, cache, threads, names, descriptions};
        }), py::kw_only(), py::arg("storage_mode") = StorageMode::ByteRange,
            py::arg("cache_bytes") = 0, py::arg("threads") = 8,
            py::arg("resident_names") = false, py::arg("resident_descriptions") = false)
        .def_readwrite("storage_mode", &StoreOptions::storage_mode)
        .def_readwrite("cache_bytes", &StoreOptions::cache_bytes)
        .def_readwrite("threads", &StoreOptions::threads)
        .def_readwrite("resident_names", &StoreOptions::resident_names)
        .def_readwrite("resident_descriptions", &StoreOptions::resident_descriptions);
    py::class_<ReadOptions>(module, "ReadOptions")
        .def(py::init([](size_t threads, IoBackend backend, size_t depth, uint64_t buffer,
                         bool sorted, ReadStrategy strategy, uint64_t gap, uint64_t merged,
                         uint64_t exact, uint64_t scan, bool diagnostics) {
            return ReadOptions{threads, backend, depth, buffer, sorted, strategy,
                               gap, merged, exact, scan, diagnostics};
        }), py::kw_only(), py::arg("threads") = 8, py::arg("io_backend") = IoBackend::Auto,
            py::arg("io_queue_depth") = 64, py::arg("io_buffer_bytes") = kIoBufferBytes,
            py::arg("input_sorted") = false, py::arg("read_strategy") = ReadStrategy::Sparse,
            py::arg("merge_gap_bytes") = kByteRangeMergeGapBytes,
            py::arg("max_merged_range_bytes") = kMaxMergedRangeBytes,
            py::arg("max_exact_merged_range_bytes") = kMaxExactMergedRangeBytes,
            py::arg("scan_max_range_bytes") = kScanMaxRangeBytes,
            py::arg("collect_diagnostics") = false)
#define READ_OPTION(field) .def_readwrite(#field, &ReadOptions::field)
        READ_OPTION(threads) READ_OPTION(io_backend) READ_OPTION(io_queue_depth)
        READ_OPTION(io_buffer_bytes) READ_OPTION(input_sorted) READ_OPTION(read_strategy)
        READ_OPTION(merge_gap_bytes) READ_OPTION(max_merged_range_bytes)
        READ_OPTION(max_exact_merged_range_bytes) READ_OPTION(scan_max_range_bytes)
        READ_OPTION(collect_diagnostics);
#undef READ_OPTION
}

// 导出只读统计字段，避免 Python 修改批次中的计数。
void bind_stats(py::module_& module) {
    py::class_<BuildStats>(module, "BuildStats")
        .def_readonly("sequences", &BuildStats::sequences)
        .def_readonly("residues", &BuildStats::residues)
        .def_readonly("packed_bytes", &BuildStats::packed_bytes)
        .def_readonly("name_bytes", &BuildStats::name_bytes)
        .def_readonly("description_bytes", &BuildStats::description_bytes);
    py::class_<BatchStats>(module, "BatchStats")
#define BATCH_STAT(field) .def_readonly(#field, &BatchStats::field)
        BATCH_STAT(full_memory) BATCH_STAT(prefix_memory) BATCH_STAT(io_uring)
        BATCH_STAT(requested_sequences) BATCH_STAT(requested_residues)
        BATCH_STAT(cache_hits) BATCH_STAT(cache_misses)
        BATCH_STAT(cache_hit_residues) BATCH_STAT(cache_miss_residues)
        BATCH_STAT(unique_packed_bytes) BATCH_STAT(ranges) BATCH_STAT(sparse_ranges)
        BATCH_STAT(scan_ranges) BATCH_STAT(merged_gaps) BATCH_STAT(free_merges)
        BATCH_STAT(read_bytes) BATCH_STAT(overread_bytes) BATCH_STAT(max_range_bytes)
        BATCH_STAT(workers) BATCH_STAT(io_workers) BATCH_STAT(decode_workers)
        BATCH_STAT(io_queue_depth) BATCH_STAT(io_submissions) BATCH_STAT(io_completions)
        BATCH_STAT(io_submit_calls) BATCH_STAT(io_submit_ns) BATCH_STAT(io_wait_calls)
        BATCH_STAT(io_empty_waits) BATCH_STAT(layout_ns) BATCH_STAT(sort_ns)
        BATCH_STAT(range_plan_ns) BATCH_STAT(worker_wall_ns) BATCH_STAT(worker_max_busy_ns)
        BATCH_STAT(read_ns) BATCH_STAT(decode_ns) BATCH_STAT(cache_decode_ns)
        BATCH_STAT(diagnostics_collected);
#undef BATCH_STAT
    py::class_<TextBatchStats>(module, "TextBatchStats")
#define TEXT_STAT(field) .def_readonly(#field, &TextBatchStats::field)
        TEXT_STAT(resident) TEXT_STAT(io_uring) TEXT_STAT(requested_strings)
        TEXT_STAT(requested_bytes) TEXT_STAT(unique_bytes) TEXT_STAT(ranges)
        TEXT_STAT(merged_gaps) TEXT_STAT(read_bytes) TEXT_STAT(overread_bytes)
        TEXT_STAT(max_range_bytes);
#undef TEXT_STAT
}

// 绑定记录的按字段访问，子批次的引用策略确保父批次存活。
void bind_records(py::module_& module) {
    py::class_<RecordBatch>(module, "RecordBatch")
        .def("__len__", [](const RecordBatch& batch) { return python_size(batch.size()); })
        .def("size", &RecordBatch::size).def("empty", &RecordBatch::empty)
        .def_property_readonly("fields", &RecordBatch::fields)
        .def("__getitem__", &record_item).def("__getitem__", &record_slice)
        .def("__iter__", &sequence_iterator)
        .def("name", [](const RecordBatch& batch, py::ssize_t i) {
            return copy_text(batch.name_view(checked_index(i, batch.size())));
        })
        .def("description", [](const RecordBatch& batch, py::ssize_t i) {
            return copy_text(batch.description_view(checked_index(i, batch.size())));
        })
        .def("sequence", [](const RecordBatch& batch, py::ssize_t i) {
            const size_t index = checked_index(i, batch.size());
            if (!has_field(batch.fields(), RecordFields::Sequence)) {
                throw std::logic_error("record batch does not contain sequences");
            }
            return batch_item(batch.sequences, static_cast<py::ssize_t>(index));
        })
        .def_property_readonly("names", [](RecordBatch& batch) -> TextBatch& { return batch.names; },
                               py::return_value_policy::reference_internal)
        .def_property_readonly("descriptions", [](RecordBatch& batch) -> TextBatch& { return batch.descriptions; },
                               py::return_value_policy::reference_internal)
        .def_property_readonly("sequences", [](RecordBatch& batch) -> Batch& { return batch.sequences; },
                               py::return_value_policy::reference_internal);
}

// 打开库或执行读取时释放 GIL，选项按值复制，避免其他 Python 线程修改在途配置。
void bind_store(py::module_& module) {
    py::class_<PythonStore>(module, "Store")
        .def(py::init([](const std::filesystem::path& path, StoreOptions options) {
            py::gil_scoped_release release;
            return PythonStore(std::make_shared<Store>(path, options));
        }), py::arg("path"), py::arg("options") = StoreOptions{})
        .def("close", &PythonStore::close)
        .def_property_readonly("closed", &PythonStore::closed)
        .def("__enter__", [](PythonStore& self) -> PythonStore& {
            self.acquire();
            return self;
        }, py::return_value_policy::reference_internal)
        .def("__exit__", [](PythonStore& self, py::object, py::object, py::object) { self.close(); })
        .def("__len__", [](const PythonStore& self) { return python_size(self.acquire()->size()); })
        .def("size", [](const PythonStore& self) { return self.acquire()->size(); })
        .def_property_readonly("total_residues", [](const PythonStore& self) { return self.acquire()->total_residues(); })
        .def_property_readonly("packed_bytes", [](const PythonStore& self) { return self.acquire()->packed_bytes(); })
        .def_property_readonly("storage_mode", [](const PythonStore& self) { return self.acquire()->storage_mode(); })
        .def_property_readonly("resident_bytes", [](const PythonStore& self) { return self.acquire()->resident_bytes(); })
        .def_property_readonly("resident_text_bytes", [](const PythonStore& self) { return self.acquire()->resident_text_bytes(); })
        .def_property_readonly("preload_ns", [](const PythonStore& self) { return self.acquire()->preload_ns(); })
        .def_property_readonly("has_names", [](const PythonStore& self) { return self.acquire()->has_names(); })
        .def_property_readonly("has_descriptions", [](const PythonStore& self) { return self.acquire()->has_descriptions(); })
        .def_property_readonly("decode_kernel_name", [](const PythonStore& self) { return self.acquire()->decode_kernel_name(); })
        .def("sequence_length", [](const PythonStore& self, SequenceId id) { return self.acquire()->sequence_length(id); }, py::arg("id"))
        .def("get", [](const PythonStore& self, SequenceId id) {
            const auto store = self.acquire();
            py::gil_scoped_release release;
            return store->get(id);
        }, py::arg("id"))
        .def("__getitem__", [](const PythonStore& self, py::ssize_t index) {
            const auto store = self.acquire();
            const size_t id = checked_index(index, store->size());
            py::gil_scoped_release release;
            return store->get(id);
        })
        .def("batch_get", [](const PythonStore& self, const std::vector<SequenceId>& ids, ReadOptions options) {
            const auto store = self.acquire();
            py::gil_scoped_release release;
            return store->batch_get(ids, options);
        }, py::arg("ids"), py::arg("options") = ReadOptions{})
        .def("batch_get_into", &batch_into, py::arg("ids"), py::arg("outputs"), py::arg("options") = ReadOptions{},
             "Decode into nonoverlapping writable byte buffers without adding NUL bytes.")
        .def("sequential_get", [](const PythonStore& self, SequenceId begin, SequenceId end, ReadOptions options) {
            const auto store = self.acquire();
            py::gil_scoped_release release;
            return store->sequential_get(begin, end, options);
        }, py::arg("begin"), py::arg("end"), py::arg("options") = ReadOptions{})
        .def("sequential_get_into", &sequential_into, py::arg("begin"), py::arg("end"),
             py::arg("outputs"), py::arg("options") = ReadOptions{})
        .def("batch_get_records", [](const PythonStore& self, const std::vector<SequenceId>& ids,
                                     RecordFields fields, ReadOptions options) {
            const auto store = self.acquire();
            py::gil_scoped_release release;
            return store->batch_get_records(ids, fields, options);
        }, py::arg("ids"), py::arg("fields") = RecordFields::All, py::arg("options") = ReadOptions{});
}

}  // namespace

// 初始化 Python 扩展；建库和读取仍由原生 AAStore 实现。
PYBIND11_MODULE(_aastore, module) {
    module.doc() = "AAStore C++ bindings for compact FASTA storage";
    module.attr("__version__") = AASTORE_PYTHON_VERSION;
    bind_options(module);
    bind_stats(module);
    bind_batch<Batch>(module, "Batch");
    bind_batch<TextBatch>(module, "TextBatch");
    bind_records(module);
    bind_store(module);
    module.def("_build_fasta", [](const std::filesystem::path& fasta,
                                 const std::filesystem::path& path, BuildOptions options) {
        py::gil_scoped_release release;
        return build_fasta(fasta, path, options);
    }, py::arg("fasta"), py::arg("store"), py::arg("options") = BuildOptions{});
}
