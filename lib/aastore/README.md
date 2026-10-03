# AAStore Byte Range

AAStore 使用唯一的 AAPCMP3 连续编码格式。当前库文件为：

- `meta.bin`：`AAPCMP3` compact sequence offsets for a continuous 5-bit stream
- `packed.bin`：连续的 5-bit packed 数据
- `names.bin` / `names.meta`（可选）：无分隔符名称数据与 `AATEXT1` compact offsets
- `descriptions.bin` / `descriptions.meta`（可选）：无分隔符描述数据与 `AATEXT1` compact offsets

默认读取路径是 Sparse Byte-range：sequence ID 查 residue offset，按 ID/物理 offset
排序，生成 `4096` byte gap、`16384` byte cap 的 sparse ranges。
读取可分给多个 io_uring worker；每个 worker
独占一个 ring，并在自己的 completion 路径中用原版 SIMD kernel 解码，最后 scatter
回请求顺序。io_uring 不可用时自动回退到并行 `pread`。

Prefix-memory 模式在打开库时预装固定大小的连续 packed 前缀。完整落在前缀内的
序列直接从内存解码，其他请求保持物理顺序，经 Byte-range planner 合并后读取。
前缀不会动态准入、淘汰或迁移；文件读取仍可使用操作系统的页缓存。

Full-memory 模式在打开库时把 `packed.bin` 预载为一块连续 raw buffer，batch 跳过排序、
range planner 和磁盘 I/O。三种模式共用磁盘格式和解码 kernel。

`StoreOptions::resident_names` 和 `resident_descriptions` 独立控制文本预加载，默认关闭。
启用后在打开库时将对应列的完整正文读入 Store 持有的内存缓冲，后续批读不再读取
正文文件；文本索引仍使用只读映射。`resident_text_bytes()` 返回已载入的文本字节数。

x86-64 构建使用可移植基线指令集。运行时根据 CPU 能力选择 AVX-512、AVX2/BMI2 或
scalar 建库和解码路径，因此同一个二进制可部署到不同 SIMD 能力的机器，不需要使用
`-march=native` 重新编译。scalar 回退用于支持范围内的 x86-64 CPU，不代表支持其他架构。

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

当前支持与验证范围为 **Linux x86_64、64 位、小端、glibc**，编译器为支持 C++17 的
**GCC/Clang**。此说明用于提醒当前支持范围；范围之外的平台尚未验证，不承诺可用性。
普通 C++ 构建不依赖 Python；liburing 是可选依赖。
CMake 会实际编译并链接 `std::filesystem` 探测程序，按所选标准库决定是否需要
`stdc++fs` 或 `c++fs`，也支持 Clang 配旧 libstdc++；安装的 CMake target 和
pkg-config 元数据会传递所需链接库。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
(cd build && ctest --output-on-failure)
```

Store 测试按 `builder`、`random`、`sequential`、`text`、`concurrency` 分组，可单独
选择；`options` 检查配置校验。`compatibility` 检查旧内核、旧 liburing 读取路径和
CMake 交叉编译流程，即使宿主禁用 io_uring 也会运行。默认测试命令仍运行全部测试：

```bash
(cd build && ctest -L sequential --output-on-failure)
(cd build && ctest -L compatibility --output-on-failure)
./build/aastore_test --group text
```

交叉编译时通过 `CMAKE_TOOLCHAIN_FILE` 指定工具链。安装后的 SDK 测试会沿用工具链、
sysroot、编译参数和 `CMAKE_CROSSCOMPILING_EMULATOR`；有模拟器时运行读取测试，
无模拟器时只验证配置与构建，并禁用目标程序的运行测试。当前 Python 和 benchmark
脚本测试只在本机构建中注册。

顶层构建默认启用工具、基准和测试；通过 `add_subdirectory` 接入其他项目时，三者默认
关闭。可分别用 `AASTORE_BUILD_TOOLS`、`AASTORE_BUILD_BENCHMARKS`、`AASTORE_BUILD_TESTS`
覆盖默认值。仅构建并安装 C++ 库：

```bash
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DAASTORE_BUILD_TOOLS=OFF -DAASTORE_BUILD_BENCHMARKS=OFF \
  -DAASTORE_BUILD_TESTS=OFF
cmake --build build-sdk -j 4
cmake --install build-sdk --prefix "$HOME/.local"
```

默认构建静态库；`-DBUILD_SHARED_LIBS=ON` 构建共享库。安装内容包含公共头文件、库、
CMake 包配置和 pkg-config 元数据。liburing 启用时，共享库运行环境需要对应运行库，
静态库的下游链接需要对应开发库；安装的 CMake 包会为静态链接重新查找依赖。

### 在其他 C++ 项目中使用

安装后的项目通过 `find_package` 接入，编译标准、头文件路径和链接依赖由 target 传播：

```cmake
find_package(aastore 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

配置下游项目时通过 `-DCMAKE_PREFIX_PATH="$HOME/.local"` 指定安装前缀。
自定义了非标准库目录时，也可用 `-Daastore_DIR=/path/to/libdir/cmake/aastore` 指定包配置目录。
也可直接嵌入源码：

```cmake
add_subdirectory(path/to/AABinaryStoreV2)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

可独立编译和运行的示例位于 [examples/cpp](examples/cpp)：

```bash
cmake -S examples/cpp -B build-example -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build-example -j 4
(cd build-example && ctest --output-on-failure)
```

示例通过已安装的库建一个临时数据库、读取重复且乱序的 ID，并自动清理临时数据。

## Python 接口

预编译 wheel 支持上述 Linux x86_64/glibc 平台的普通 **CPython 3.9–3.13**。
源码包声明 `Python >=3.9`，可在相同平台上尝试更新的 CPython，但这些版本尚不在当前
wheel 支持与验证范围内；其他 Python 实现和特殊 ABI 同样尚未验证，不承诺可用性。
在仓库根目录从源码安装时，本机需要支持 C++17 的 GCC/Clang
和 Python 开发头文件，其他编译依赖由 pip 的隔离构建环境准备。无需安装 NumPy：

```bash
python -m pip install .
python examples/python/basic.py
python examples/python/buffer_layout.py
```

两个示例自行创建小 FASTA，在临时目录中建库并自动清理。完整的输入、选项、结果、
数据布局、输出复用、错误和生命周期说明见 [Python API 指南](docs/python_api.md)；
公开签名与类型见 [类型声明](python/aastore/__init__.pyi)。

Python 包和 portable wheel 默认使用 pread。需要在本机启用 io_uring 时，
先安装 liburing 开发包，再从源码显式构建：

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

Python wheel 包含扩展、模块和类型声明，C++ SDK 使用上述独立安装流程。
io_uring 源码构建可能依赖系统 `liburing`；重新分发时的要求和源码包重建步骤见
[发布指南](docs/releasing.md)。

Python 复用相同的 C++ 建库、编码和读取核心，不启动外部命令行程序：

```python
import aastore

stats = aastore.build_fasta("input.fasta", "store", threads=4)
reads = aastore.ReadOptions(threads=4)

with aastore.Store("store") as store:
    sequence = store.get(0)[0]               # 从单元素 Batch 取出 str，ID 从 0 开始
    batch = store.get([0, 0], reads)    # 保留请求顺序和重复 ID
    print(batch[0], list(batch))             # 字符串访问创建副本
    view = batch.view(0)                     # 只读字节视图，保留所属批次
    raw = batch.storage[
        batch.offsets[0]:batch.offsets[0] + batch.lengths[0]
    ]                                       # 与 view 指向同一条目的字节

    fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
    records = store.batch_get_records([0], fields, reads)
    print(records[0]["name"], records[0]["sequence"])
```

`Batch`、`TextBatch` 和 `RecordBatch` 支持 `len()`、负索引、迭代与切片；切片返回
字符串或记录字典列表。`Batch` 与 `TextBatch` 的 `storage` 为只读字节视图，
`offsets`、`lengths` 为只读的一维本机字节序 `uint64` 视图，均保留所属批次。
文本长度以 UTF-8 字节计；按偏移与长度取条目，不通过 NUL 猜测边界。

`BuildOptions`、`StoreOptions`、`ReadOptions` 分别控制建库、驻留模式和每次读取，
统计字段只读。已有输出区时，可用 `get_into()` 或 `sequential_get_into()`
直接写入并复用 `bytearray`；原生工作释放 GIL，输出缓冲不可同时被其他线程读写。
Store 关闭后已有结果与视图仍有效，关闭后的新读取会被拒绝。

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

auto batch = store.get(ids);
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
因此聚类等只需要序列的路径仍使用 `get(ids)`，不承担 name/description 的内存和
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

已有输出缓冲时可使用 `get_into()` 直接解码，连续批次可复用这些缓冲：

```cpp
std::vector<std::string> sequences(ids.size());
std::vector<char*> outputs(ids.size());
for (size_t i = 0; i < ids.size(); ++i) {
    sequences[i].resize(store.sequence_length(ids[i]));
    outputs[i] = sequences[i].data();
}
auto stats = store.get_into(ids, outputs, options);
```

也可传入 `outputs.data(), outputs.size()`。输出数量必须与 ID 数量一致，
每个非空序列的输出区不得重叠，容量至少为其序列长度；空序列允许空指针。
库不分配解码输出 arena、不追加 NUL，也无法从裸指针检查实际容量。
它仍会按当前读取模式规划范围和使用 packed 暂存缓冲，返回 `BatchStats`。

单条和随机批量统一使用 `get`，返回 `Batch`；单条结果只有一个元素：

```cpp
auto single = store.get(id, options);
std::string sequence = single[0];

std::string scratch;
store.get_into(id, scratch);  // Reuse this string for subsequent requests.
```

`get(id, options)` 与批量读取使用相同的选项和统计语义。
`get_into(id, scratch)` 保留轻量的字符串复用路径，不构造 Batch、不启动工作线程：
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
已有输出缓冲时使用 `sequential_get_into()`，与 `get_into()` 采用相同的指针数组约定：

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
read_options.threads = 8;            // total workers, including the caller
read_options.io_queue_depth = 64;     // per ring
auto tuned_batch = store.get(ids, read_options);

aastore::StoreOptions full_options;
full_options.storage_mode = aastore::StorageMode::FullMemory;
full_options.threads = 8;
aastore::Store full_store("proteins.aastore", full_options);
auto resident_batch = full_store.get({9, 2, 100, 2});

aastore::StoreOptions prefix_options;
prefix_options.storage_mode = aastore::StorageMode::PrefixMemory;
prefix_options.cache_bytes = 512ULL * 1024 * 1024;
aastore::Store prefix_store("proteins.aastore", prefix_options);
auto mixed_batch = prefix_store.get({9, 2, 100, 2});

```

`PrefixMemory` 在启动时顺序预装指定大小的连续 packed 前缀。完整落在前缀内的
sequence 直接从内存解码，其他 sequence 仍由 Byte-range planner 合并并读取；前缀
不会发生准入、淘汰或迁移。`FullMemory` 则预装整个 packed 文件。

动态序列缓存模式及其准入、淘汰统计已移除。旧调用方需改用上述三种模式之一并
重新编译；已有 AAPCMP3 数据库无需重建。

范围规划使用 `merge_gap_bytes` 和 `max_merged_range_bytes`，默认分别为
`4096` 和 `16384`；无额外读取的合并由 `max_exact_merged_range_bytes` 限制，默认 1 MiB。
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
初始化 ring 时优先使用内核支持的 `READ`，仅支持 `READV` 时使用单个 `iovec`；
Linux 5.4 等缺少操作码探测接口的内核直接使用 `READV`，并保持 `iovec` 到读取完成。
旧 liburing 缺少 `READ` 或探测 API 时也保留 `READV` 路径。后端初始化失败或探测确认
没有可用读取操作时，`Auto` 回退到 `pread`，显式 `IoUring` 报错；实际读取、提交或
解码回调中的错误仍向调用方传播。
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
auto batch = store.get(ids, options);
```

开启后额外记录去重字节、额外读取量、合并次数、命中/未命中残基数，以及各阶段耗时、
submit/wait API 调用次数及提交耗时、等待前 CQ 为空的次数。
关闭时，`unique_packed_bytes`、`overread_bytes`、`merged_gaps`、`free_merges`、
`cache_hit_residues`、`cache_miss_residues`、所有 `*_ns` 和 `io_submit_calls`、
`io_wait_calls`、`io_empty_waits` 为零。不会执行去重字节统计的额外并行扫描、调用读取
诊断时钟或分配线程计时数组；`diagnostics_collected` 标明本次是否开启。
文本列的 `unique_bytes`、`merged_gaps`、`overread_bytes` 也由同一开关控制。
`io_submissions` 统计 SQE 请求数，不是系统调用次数；`read_ns` 在 io_uring 路径上
统计等待 API 耗时，不等同于纯设备等待。benchmark 显式开启诊断，日常调用默认关闭。
`preload_ns()` 是一次性打开库的计时，不受读取诊断开关影响。

## 基准

```bash
./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --drop-cache \
  --io-backend uring \
  --queue-depth 64 \
  --io-buffer-bytes 16777216

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
可在相同 workload 下比较不同合并阈值；输出中的
`ranges`、`read_bytes` 和 `overread_bytes` 用于检查 planner 决策。

## 版本与维护

当前版本由仓库根目录的 [VERSION](VERSION) 统一提供给 CMake 和 Python 包。
0.x 阶段的 minor 更新可能修改公开 API；共享库 SONAME 包含 major 和 minor 版本。
包版本与磁盘格式版本分别管理，更新包不自动意味着需要重建数据库。

[CHANGELOG.md](CHANGELOG.md) 记录变更；[docs/releasing.md](docs/releasing.md) 说明兼容约定、
安装后验证和发布检查。仓库 CI 检查静态/共享库、io_uring/pread 构建与 Python 源码包重建。
portable wheels 工作流需要手动触发，只生成构建产物，不自动发布。

## 许可证

AAStore 采用 GNU General Public License version 3（`GPL-3.0-only`）。
完整条款见 [LICENSE](LICENSE)。
