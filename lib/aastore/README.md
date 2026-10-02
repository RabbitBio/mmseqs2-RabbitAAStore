# AAStore Byte Range

AAStore 使用唯一的 AAPCMP3 连续编码格式。当前库文件为：

- `meta.bin`：`AAPCMP3` compact sequence offsets for a continuous 5-bit stream
- `packed.bin`：连续的 5-bit packed 数据
- `names.bin` / `names.meta`（可选）：无分隔符名称数据与 `AATEXT1` compact offsets
- `descriptions.bin` / `descriptions.meta`（可选）：无分隔符描述数据与 `AATEXT1` compact offsets

默认读取路径是 Sparse Byte-range：sequence ID 查 residue offset，按 ID/物理 offset
排序，生成 `4096` byte gap、`16384` byte cap 的 sparse ranges。显式选择 Scan 时，
相邻请求会在配置的最大跨度内合并成连续范围。读取可分给多个 io_uring worker；每个 worker
独占一个 ring，并在自己的 completion 路径中用原版 SIMD kernel 解码，最后 scatter
回请求顺序。io_uring 不可用时自动回退到并行 `pread`。

Prefix-memory 模式在打开库时预装固定大小的连续 packed 前缀。完整落在前缀内的
序列直接从内存解码，其他请求保持物理顺序，经 Byte-range planner 合并后读取。
前缀不会动态准入、淘汰或迁移；文件读取仍可使用操作系统的页缓存。

Full-memory 模式在打开库时把 `packed.bin` 预载为一块连续 raw buffer，batch 跳过排序、
range planner 和磁盘 I/O。三种模式共用磁盘格式和解码 kernel。

x86-64 构建使用可移植基线指令集。运行时根据 CPU 能力选择 AVX-512、AVX2/BMI2 或
scalar 建库和解码路径，因此同一个二进制可部署到不同 SIMD 能力的机器，不需要使用
`-march=native` 重新编译。非 x86 平台使用 scalar 路径。

## 文件职责

```text
include/aastore/store.hpp      公共建库和读取 API
src/builder.cpp                FASTA -> meta.bin/packed.bin
src/metadata.cpp               AAPCMP3 紧凑元数据映射和校验
src/codec.cpp                  原版 stop-aware 5-bit 编解码和 SIMD dispatch
src/byte_range_planner.cpp     radix sort + sparse/scan planner
src/io_uring_backend.cpp       多 ring、有界异步 range submit/completion
src/text_column.cpp            名称/描述的紧凑 offset 索引与 batch range read
src/store.cpp                  三种模式的 batch 编排、并行解码和 scatter
```

## 构建

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

## Python 接口

在仓库根目录安装，编译依赖由 pip 的隔离构建环境准备；本机仍需 C++17 编译器
和 Python 开发头文件。无需安装 NumPy：

```bash
python -m pip install .
python tests/python_test.py
```

如果安装了 liburing 开发包，扩展会支持 io_uring；否则 Auto 回退到 pread。
需要明确禁用 io_uring 构建时：

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=OFF
```

Python 复用相同的 C++ 建库、编码和读取核心，不启动外部命令行程序：

```python
import aastore

stats = aastore.build_fasta("input.fasta", "store", threads=4)
options = aastore.ReadOptions(threads=4)

with aastore.Store("store") as store:
    sequence = store.get(5)                  # str，ID 从 0 开始
    batch = store.batch_get([1, 5, 10], options)
    print(batch[0])                          # 按请求顺序访问，返回 str 副本
    for sequence in batch:
        consume(sequence)
    view = batch.view(0)                    # 只读 memoryview，不复制序列字节
    block = store.sequential_get(0, 100, options)  # [begin, end)，使用 pread

    fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
    records = store.batch_get_records([1, 5], fields, options)
    print(records[0]["name"], records[0]["sequence"])
```

`Batch`、`TextBatch` 和 `RecordBatch` 支持 `len()`、负索引、迭代与切片；切片返回
字符串或记录字典列表，逐条访问才创建 Python 对象。`batch.view(i)` 是原始字节视图，
会保留整个批次；即使批次变量删除或 Store 关闭，视图仍有效。`batch.storage` 是完整
只读 arena，随机批读包含逐条 NUL，顺读不包含；通常应使用 `view(i)` 而不是自行解析 arena。
名称和描述转为 `str` 时要求有效 UTF-8，原始字节可通过文本批次的 `view(i)` 访问。

驻留模式与前缀预算通过 `StoreOptions` 设置；统计字段只读，诊断默认关闭：

```python
store_options = aastore.StoreOptions(
    storage_mode=aastore.StorageMode.PrefixMemory,
    cache_bytes=1024 * 1024 * 1024,
    threads=4,
)
with aastore.Store("store", store_options) as store:
    reads = aastore.ReadOptions(threads=4, io_backend=aastore.IoBackend.IoUring)
    batch = store.batch_get([1, 5, 10], reads)
    print(batch.stats.read_bytes)
```

FullMemory 使用 `StorageMode.FullMemory`。建库参数也可通过 `BuildOptions` 提供，Python
中的 `threads` 对应 C++ 的 `encode_threads`；`build_fasta()` 的显式关键字覆盖选项但不修改原对象。

已有缓冲时可以直接写入；输出列表每项必须是容量足够、连续一维且可写的字节缓冲，
空序列允许 `None`。在写入前验证数量、容量和写入区不重叠：

```python
with aastore.Store("store") as store:
    ids = [1, 5, 10]
    outputs = [bytearray(store.sequence_length(i)) for i in ids]
    stats = store.batch_get_into(ids, outputs, options)
    print(outputs[0].decode("ascii"))
    # outputs 可以在下一批读取时复用；不会追加 NUL。
```

`sequential_get_into(begin, end, outputs, options)` 使用同样的缓冲约定。可写 `memoryview`
切片和符合缓冲协议的字节数组也可使用，Python 不暴露裸 `char*` 或可变 `std::string`。
建库、打开库、读取和解码释放 GIL，允许多个 Python 调用线程共享只读 Store，但不可在
原生写入期间修改或读取同一输出区；缓冲协议阻止的是扩容，不是对字节内容的并发访问。
在途调用保留自己的库资源引用；`close()` 后拒绝新的读取，现有 Batch 不受影响。

普通 C++ 构建默认不需要 Python 或 pybind11。手动构建绑定时可使用：

```bash
python -m pip install 'pybind11>=3.0,<4'
cmake -S . -B build-python -DAASTORE_BUILD_PYTHON=ON \
  -DPython_EXECUTABLE="$(python -c 'import sys; print(sys.executable)')" \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build-python -j 8
ctest --test-dir build-python --output-on-failure
```

## 建库

编译后使用独立的 `aastore_build`，输入 FASTA 和输出库目录：

```bash
./build/aastore_build proteins.fa proteins.aastore
```

默认拒绝覆盖已有库。确认需要重建时显式传入：

```bash
./build/aastore_build proteins.fa proteins.aastore --overwrite
```

name 与 description 是彼此独立的可选列。默认保留两列以兼容现有脚本；不需要时可避免
解析、写出和索引这些 header 数据：

```bash
./build/aastore_build proteins.fa proteins.aastore --no-descriptions
./build/aastore_build proteins.fa proteins.aastore --no-names
./build/aastore_build proteins.fa proteins.aastore --sequences-only
./build/aastore_build proteins.fa proteins.aastore --sequences-only --threads 4
```

对应的 C++ 接口为：

```cpp
aastore::BuildOptions options;
options.write_names = true;
options.write_descriptions = false;
options.encode_threads = 4;
aastore::build_fasta("proteins.fa", "proteins.aastore", options);
```

`encode_threads`（CLI 为 `--threads`）表示建库阶段的总线程预算，包含 FASTA
解析线程。设置为 `1` 时会关闭后台流水线，严格使用单线程；更大的值才会按预算启用
编码和输出流水线。编码任务可乱序完成，但始终按原 chunk 编号写入，因此 sequence ID
和 packed offset 不变。

sequence ID 按 FASTA 中的记录顺序从 `0` 开始分配。建库程序会打印序列数、
残基数、各数据列大小和总耗时。返回成功前，构建器会同步所有临时文件，再切换正式
文件名并同步库目录；因此 `build_seconds` 包含等待数据持久化到底层存储的时间。

MMseqs2 默认 `createdb --shuffle 1` 使用固定的 32 路轮转顺序。需要让物理 ID 与
shuffle 后的 MMseqs2 本地 ID 一致时，先建临时顺序库，再执行：

```bash
./build/aastore_reorder_createdb \
  proteins.aastore.input-order proteins.aastore \
  --threads 8 --remove-source
```

工具顺序扫描源库，按原 ID 的 `id % 32` 分流后依次合并 32 个分片。最终库完整同步
并提交后才删除源库；失败时源库保留。该工具只生成序列列，且只适配普通
`createdb` 的默认 32 路 shuffle，不适用于 GPU/按长度重排。

## 使用

```cpp
#include <aastore/store.hpp>

#include <string_view>
#include <vector>

aastore::build_fasta("proteins.fa", "proteins.aastore");

aastore::Store store("proteins.aastore");
std::vector<aastore::SequenceId> ids = {9, 2, 100, 2};

auto batch = store.batch_get(ids);
for (size_t i = 0; i < batch.size(); ++i) {
    std::string sequence = batch[i];
    consume(ids[i], sequence);
}
```

需要 FASTA header 时按列选择，未选中的列不会读取：

```cpp
auto records = store.batch_get_records(
    ids,
    aastore::RecordFields::Name | aastore::RecordFields::Sequence);

for (size_t i = 0; i < records.size(); ++i) {
    consume(records.name_view(i), records.sequence_view(i));
}
```

FASTA 的第一个空白分隔字段写入 name，剩余 header 写入 description。两列分别保存，
因此聚类等只需要序列的路径仍使用 `batch_get(ids)`，不承担 name/description 的内存和
I/O；仅需名称时也可只请求 `RecordFields::Name`。`RecordBatch` 的 `*_view()` 为零复制
视图，`name()`、`description()` 和 `sequence()` 返回独立的 `std::string`。
打开库后可用 `has_names()` 和 `has_descriptions()` 分别检查两列。

`batch[i]` 是默认接口，返回独立的 `std::string`；即使 `batch` 已经销毁，
该字符串仍然有效。AAStore 在内部完成去重、物理 offset 排序、range 合并、
并行读取、解码和原顺序恢复，因此 `batch[i]` 始终对应 `ids[i]`。

性能敏感路径可以显式使用零复制 `view`：

```cpp
std::string_view sequence = batch.view(0);
```

`view` 只在所属 `batch` 存活期间有效。

已有输出缓冲时可使用 `batch_get_into()` 直接解码，连续批次可复用这些缓冲：

```cpp
std::vector<std::string> sequences(ids.size());
std::vector<char*> outputs(ids.size());
for (size_t i = 0; i < ids.size(); ++i) {
    sequences[i].resize(store.sequence_length(ids[i]));
    outputs[i] = sequences[i].data();
}
auto stats = store.batch_get_into(ids, outputs, options);
```

也可传入 `outputs.data(), outputs.size()`。输出数量必须与 ID 数量一致，
每个非空序列的输出区不得重叠，容量至少为其序列长度；空序列允许空指针。
库不分配解码输出 arena、不追加 NUL，也无法从裸指针检查实际容量。
它仍会按当前读取模式规划范围和使用 packed 暂存缓冲，返回 `BatchStats`。

单条访问使用独立的 `get` 接口，不构造 `Batch`、不启动后台线程：

```cpp
std::string sequence = store.get(id);

std::string scratch;
store.get_into(id, scratch);  // Reuse this string for subsequent requests.
```

`FullMemory` 直接从驻留的 packed 数据解码，`PrefixMemory`
命中时从已有内存解码，否则只 `pread` 当前序列需要的 packed 范围。
多个线程可以共享已打开的 `Store`，各自使用独立的输出缓冲；全内存路径没有
共享解码缓冲或 batch 互斥锁。输出由调用方持有，不依赖临时 `Batch` 的生命周期。

连续 ID 扫描不要走通用的随机 batch planner，直接按块调用顺序接口：

```cpp
aastore::ReadOptions scan_options;
scan_options.threads = 8;

auto block = store.sequential_get(first_id, end_id, scan_options);
for (size_t i = 0; i < block.size(); ++i) {
    consume(first_id + i, block.view(i));
}
```

`sequential_get()` 将整个 ID 区间布局成无分隔符的连续 residue arena，并把区间切给
少量工作线程直接执行顺序 `pread + SIMD decode`，不生成逐序列请求、不排序，也不做
range planning。调用方通过控制 `[first_id, end_id)` 大小来限制驻留内存，并可在消费
当前块时预取下一块。
已有输出缓冲时使用 `sequential_get_into()`，与 `batch_get_into()` 采用相同的指针数组约定：

```cpp
std::vector<std::string> sequences(end_id - first_id);
std::vector<char*> outputs(sequences.size());
for (size_t i = 0; i < sequences.size(); ++i) {
    sequences[i].resize(store.sequence_length(first_id + i));
    outputs[i] = sequences[i].empty() ? nullptr : sequences[i].data();
}
auto stats = store.sequential_get_into(first_id, end_id, outputs, scan_options);
```

也可传入 `outputs.data(), outputs.size()`。`outputs[i]` 对应 `first_id + i`，
输出数量必须等于 `end_id - first_id`；非空序列输出区不得重叠，容量至少为序列长度。
该接口直接解码到调用方内存、不追加 NUL，返回 `BatchStats`，不分配解码输出 arena。
内部仍按连续 packed 范围分片读取，不做随机请求排序或逐序列 `pread`。
顺读 packed 暂存缓冲由调用线程内部持有，各工作线程使用独立槽位；每个调用线程
最多保留 64 MiB 容量，更大的读取使用临时缓冲。输出缓冲由调用方跨调用复用。
旧的 `sequential_get_into(..., Batch&, ...)` 重载已移除，调用方需更新并重新编译。

需要调整读取策略时，再传入 `ReadOptions`：

```cpp
aastore::ReadOptions read_options;
read_options.read_strategy = aastore::ReadStrategy::Sparse;
read_options.threads = 8;            // total workers, including the caller
read_options.io_queue_depth = 64;     // per ring
auto tuned_batch = store.batch_get(ids, read_options);

aastore::StoreOptions full_options;
full_options.storage_mode = aastore::StorageMode::FullMemory;
full_options.threads = 8;
aastore::Store full_store("proteins.aastore", full_options);
auto resident_batch = full_store.batch_get({9, 2, 100, 2});

aastore::StoreOptions prefix_options;
prefix_options.storage_mode = aastore::StorageMode::PrefixMemory;
prefix_options.cache_bytes = 512ULL * 1024 * 1024;
aastore::Store prefix_store("proteins.aastore", prefix_options);
auto mixed_batch = prefix_store.batch_get({9, 2, 100, 2});

```

`PrefixMemory` 在启动时顺序预装指定大小的连续 packed 前缀。完整落在前缀内的
sequence 直接从内存解码，其他 sequence 仍由 Byte-range planner 合并并读取；前缀
不会发生准入、淘汰或迁移。`FullMemory` 则预装整个 packed 文件。

动态序列缓存模式及其准入、淘汰统计已移除。旧调用方需改用上述三种模式之一并
重新编译；已有 AAPCMP3 数据库无需重建。

`ReadStrategy::Sparse` 使用 `merge_gap_bytes` 和 `max_merged_range_bytes`，默认分别为
`4096` 和 `16384`。`Scan` 强制在 `scan_max_range_bytes` 限定的跨度内尽量合并。
`ReadOptions::threads` 是每次读取调用的用户态工作线程上限，默认 `8`，包含调用线程；
`0` 按 `1` 处理，任务较少时会减少实际线程数。布局、排序和读取等阶段分别使用该预算，
不会把各阶段的线程数叠加。`StoreOptions::threads` 控制打开库时的预加载线程，默认也为 `8`。
大 batch 的 packed extent 计算、hard-gap 检测和 sparse range 统计也使用
`threads` 并行执行；`threads=1` 保留串行 planner 作为等价参考路径。
io_uring 按累计读取字节数把有序范围划分给至多 `threads` 个 ring owner；每个线程
管理自己的 ring，并直接解码自己的完成事件，不创建独立解码池。
PrefixMemory 的前缀命中由这些相同的线程在各自首次提交 I/O 后共同领取并解码，
每轮处理约 256 KiB 输出或最多 256 个命中组，然后非阻塞检查 CQE，解码完成范围并
补发读取；单个命中组不会拆分，因此超大组可以超过字节粒度。前缀任务耗尽后才
阻塞等待 I/O，不额外启动前缀线程。
全命中批次直接并行解码，无需创建 ring。
`pread` 工作线程各自执行读取和解码；前缀命中与文件读取分阶段执行。
旧读取参数 `io_threads`、`io_uring_workers`、`io_decode_threads` 已合并为 `threads`，
打开库的 `preload_threads` 也更名为 `threads`；调用方需要更新配置并重新编译。
`BatchStats::io_workers` 和 `decode_workers` 在 io_uring 路径描述的是同一组线程，不能相加。
`io_queue_depth` 是每个 ring 的目标深度，`io_buffer_bytes` 仍限制整个 batch 的在途
range buffer 保守上界，默认 16 MiB，必要时会自动降低每个 ring 的实际深度。

AAStore 不设置或查询内核 io-wq 线程上限；队列深度限制在途读取数量，不等于限制
内核线程数。原有 `io_uring_max_bounded_workers` 参数及对应统计已移除，调用方需
删除相关设置并重新编译。高并发工作负载应重新测量 CPU 和 I/O 开销。
`BuildStats` 保留建库总数和字节数等基本统计。`BatchStats` 默认保留请求量、前缀命中、
读取字节、范围数量、实际线程数及 SQE 提交/完成数，不收集详细耗时。
需要诊断时显式开启：

```cpp
aastore::ReadOptions options;
options.collect_diagnostics = true;
auto batch = store.batch_get(ids, options);
```

开启后额外记录各阶段耗时、submit/wait API 调用次数及提交耗时、等待前 CQ 为空的次数。
关闭时，所有 `*_ns` 和 `io_submit_calls`、`io_wait_calls`、`io_empty_waits` 为零，
不会调用读取诊断时钟或分配线程计时数组；`diagnostics_collected` 标明本次是否开启。
`io_submissions` 统计 SQE 请求数，不是系统调用次数；`read_ns` 在 io_uring 路径上
统计等待 API 耗时，不等同于纯设备等待。benchmark 显式开启诊断，日常调用默认关闭。
`preload_ns()` 是一次性打开库的计时，不受读取诊断开关影响。

## 基准

```bash
./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --drop-cache \
  --io-backend uring \
  --queue-depth 64 \
  --read-strategy scan \
  --scan-max-range 262144 --io-buffer-bytes 16777216

./build/aastore_bench /path/to/original/store \
  --requests 1000000 --rounds 1 --threads 8 --drop-cache --memory-mb 512

./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --drop-cache --memory-mb -1

./build/aastore_bench /path/to/original/store \
  --requests 1000000 --fast-sequential \
  --threads 8


```

`--memory-mb 0`、正数和 `-1` 分别选择 Byte-range、Prefix-memory 和 Full-memory；
`--full-memory` 是 `-1` 的别名。正数预算预装固定前缀，后续读取不会改变常驻区域。
`preload_ms` 报告 Prefix-memory 或 Full-memory 的预装时间，
`fetch_ms` 是所有 rounds 的总和。
原有 `--prefix-memory` 参数已移除，固定前缀只需设置正数 `--memory-mb`。
`fetch_user_ms` / `fetch_system_ms` 统计 fetch 阶段的进程 CPU 时间，
`fetch_storage_read_bytes` 是 Linux `/proc/self/io` 在 fetch 前后的 `read_bytes` 差值，
包含归因于该进程的存储预读，不是 planner 的应用请求字节数，也不是整台设备的计数。
`--drop-cache` 在打开库前同步、驱逐该库的 packed/meta 文件页缓存，并用 `mincore`
确认驻留页为零；无法驱逐时直接报错，不清理全局缓存。它只允许 `--rounds 1`，
冷缓存重复实验须分别启动新进程。同一轮内的后续 batch 允许自然利用页缓存。
`--sample-percent P` 从完整数据库无放回随机抽取指定百分比的记录，
`--batch-bytes N` 按解码输出字节数分批；超大单条记录独立成批。
`--memory-bytes N` 可精确指定前缀预算（0 表示 ByteRange），不能与 `--memory-mb`
或 `--full-memory` 同时使用。矩阵脚本的 `--prefix-percents 0 25 50 75`
按 packed 字节比例设置 NonFull 前缀，另跑独立的 FullMemory 对照。
FullMemory/NonFull 冷启动矩阵、计时口径和执行脚本见
[benchmarks/README.md](benchmarks/README.md)。
`--io-backend auto` 默认优先使用 io_uring；`pread` 可用于公平对照，显式 `uring` 在
运行环境不支持时直接报错。读取线程统一由 `--threads` 设置，`--queue-depth` 默认 `64`。
`--read-strategy sparse|scan` 可做同一 workload 的公平对照；输出中的
`sparse_ranges`、`scan_ranges`、`read_bytes` 和 `overread_bytes` 用于检查 planner 决策。
