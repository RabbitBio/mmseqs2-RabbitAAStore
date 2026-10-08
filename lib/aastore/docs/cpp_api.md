# C++ API 使用指南

AAStore 从 FASTA 建库，再通过 `Store` 按 ID 读取序列或所选记录字段。`BuildOptions`
控制建库，`StoreOptions` 控制打开库时的预加载，`ReadOptions` 控制每次批量读取。
公开接口见 [store.hpp](../include/aastore/store.hpp)、
[builder.hpp](../include/aastore/builder.hpp) 和 [read_options.hpp](../include/aastore/read_options.hpp)。

建议先进入 [examples](../examples/README.md) 执行 `make run`：它使用随项目提供的
十万条 FASTA 数据集建库，展示乱序、重复 ID 和连续读取，并保留生成的数据库。
本文命令从仓库根目录运行；
C++ 片段用于函数体内，读取示例假设 `proteins.aastore` 已包含至少两条记录，统一使用：

```cpp
#include <aastore/store.hpp>
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
```

## 构建与接入项目

使用支持 C++17 的 GCC 或 Clang 构建；CMake 最低版本为 3.16。
普通 C++ 构建无需 Python，liburing 为可选依赖。基本命令见 [README 构建说明](../README.md#构建)。x86-64 上使用可移植基线指令集，运行时选择
AVX-512、AVX2/BMI2 或 scalar 编解码路径，无需为不同 SIMD 能力使用 `-march=native`。

| CMake 选项 | 默认行为 |
| --- | --- |
| `AASTORE_BUILD_TOOLS` | 顶层构建开启，`add_subdirectory` 接入时关闭 |
| `AASTORE_BUILD_BENCHMARKS` | 默认关闭，测试 Makefile 和 CI 显式开启 |
| `AASTORE_BUILD_TESTS` | 完整回归默认关闭；进入 `tests/` 执行 `make` 编译 |
| `AASTORE_RUN_SMOKE_TESTS` | 独立本机默认构建后自动运行环境小测试；嵌入和交叉编译默认关闭 |
| `BUILD_SHARED_LIBS` | 顶层默认关闭，开启后构建共享库 |
| `AASTORE_ENABLE_IO_URING` | 默认开启探测；找不到可用 liburing 时构建 pread 后端 |
| `AASTORE_ENABLE_IPO` | 顶层默认尝试开启；GCC 9 以前默认关闭；实际启用取决于探测 |
| `AASTORE_BUILD_PYTHON` | 默认关闭，绑定的安装方法见 [Python 指南](python_api.md) |

仅构建并安装 C++ SDK：

```bash
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release \
  -DAASTORE_BUILD_TOOLS=OFF -DAASTORE_BUILD_BENCHMARKS=OFF \
  -DAASTORE_BUILD_TESTS=OFF -DAASTORE_RUN_SMOKE_TESTS=OFF
cmake --build build-sdk -j 4
cmake --install build-sdk --prefix "$HOME/.local"
```

安装内容包括公共头文件、库、CMake 包和 pkg-config 元数据。在下游 `CMakeLists.txt` 中：

```cmake
find_package(aastore 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

配置下游项目时传入 `-DCMAKE_PREFIX_PATH="$HOME/.local"`；非标准库目录也可通过
`-Daastore_DIR=/path/to/libdir/cmake/aastore` 指定。target 会传播 C++ 标准、头文件
路径及链接依赖。liburing 启用时，共享库部署需要对应运行库；静态链接需要开发库，
安装的 CMake 包会重新查找依赖。也可在源码项目中直接接入：

```cmake
add_executable(my_app main.cpp)
add_subdirectory(path/to/AABinaryStoreV2)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

各 `AASTORE_BUILD_*` 开关可显式覆盖默认值。安装后运行独立示例：

```bash
cmake -S examples/cpp -B build-example -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build-example -j 4
(cd build-example && ctest --output-on-failure)
```

## 从 FASTA 建库

建库工具接收输入文件和输出目录，默认保留名称及描述，并拒绝覆盖已有库：

```bash
./build/aastore_build proteins.fa proteins.aastore --threads 4
```

`--no-names` 和 `--no-descriptions` 分别省略一列，`--sequences-only` 同时省略两列；
需要重建已有目录时显式添加 `--overwrite`。对应 C++ 代码如下，假设输入文件已存在：

```cpp
aastore::BuildOptions options;
options.write_names = true;
options.write_descriptions = false;
options.encode_threads = 4;
const auto stats = aastore::build_fasta("proteins.fa", "proteins.aastore", options);
std::cout << stats.sequences << " sequences, " << stats.residues << " residues\n";
```

`overwrite` 默认 `false`，`write_names`、`write_descriptions` 默认 `true`。
`encode_threads`（CLI 的 `--threads`）默认 `1`，表示包含 FASTA 解析线程的总线程预算；
`1` 关闭后台流水线，`0` 无效。更大的值按预算启用编码和输出流水线，编码任务虽然
可乱序完成，输出仍按原 chunk 顺序写入，因此 ID 和 packed offset 保持稳定。

sequence ID 按 FASTA 记录顺序从 `0` 开始，空序列也占一个 ID。header 的第一个
空白分隔字段保存为 name，剩余文本保存为 description；省略某列可避免其解析、
写出和索引。`BuildStats` 返回序列数、残基数和各数据列的字节数。
返回成功前，构建器会同步临时文件、切换正式文件名并同步库目录及父目录；
CLI 的 `build_seconds` 包含这些同步等待，C++ `BuildStats` 本身不包含计时字段。

## 打开库与驻留方式

| `StorageMode` | 打开和读取行为 |
| --- | --- |
| `ByteRange`（默认） | 按请求规划 packed byte ranges 并读取，可利用操作系统页缓存 |
| `PrefixMemory` | 打开时预装最多 `cache_bytes` 字节的连续 packed 前缀，完整落在前缀内的序列从内存解码 |
| `FullMemory` | 打开时把全部 packed 数据读入连续 buffer，随机 batch 跳过排序、range planner 和文件 I/O |

例如，预装 512 MiB 的 packed 前缀，并独立选择名称列的驻留方式：

```cpp
aastore::StoreOptions options;
options.storage_mode = aastore::StorageMode::PrefixMemory;
options.cache_bytes = 512ULL * 1024 * 1024;
options.threads = 4;
options.resident_names = true;
options.resident_descriptions = false;
aastore::Store store("proteins.aastore", options);
std::cout << store.size() << " sequences, " << store.resident_bytes() << " packed bytes\n";
```

`cache_bytes` 只控制 PrefixMemory，且必须大于 `0`；超过 packed 大小时只预装实际数据。
前缀固定，不动态准入、淘汰或迁移。改用 FullMemory 只需设置 `storage_mode`，无需前缀预算。
`StoreOptions::threads` 默认 `8`，控制 packed 预加载线程上限，包含调用线程；`0` 按 `1` 处理。

`resident_names`、`resident_descriptions` 默认均为 `false`，与序列模式互相独立：
FullMemory 不会自动预载文本。启用文本驻留后，打开库时将对应列的完整正文读入
`Store` 持有的内存缓冲，后续批读直接复制该缓冲，不再读取正文文件；文本索引仍使用只读映射。
`resident_text_bytes()` 返回已载入文本缓冲的大小，`resident_bytes()` 返回已预装
packed buffer 的大小；两者都不含元数据，
也不是进程实时 RSS。`preload_ns()` 只记录打开库时 packed 数据的预加载耗时。

打开库时会检查已有的文本列、打开数据文件并映射文本元数据。后续按字段读取只控制
本次结果的数据读取和复制；若希望连文本文件及索引都不存在，应在建库时省略相应列。
`has_names()`、`has_descriptions()` 可分别检查列是否存在。

## 单条、随机批量与记录字段

| 调用 | 返回值与输出位置 |
| --- | --- |
| `get(id, options)` | 含一条序列的 `Batch` |
| `get(ids, options)` | 按输入顺序返回多条序列的 `Batch` |
| `get_into(id, string)` | 返回 `void`，调整并复用用户字符串 |
| `get_into(ids, outputs, options)` | 返回 `BatchStats`，直接写入用户指针数组所指内存 |

单条和随机批量读取统一使用 `get()`，均返回 `Batch`；单条结果包含一个元素。
也可通过 `get_into()` 复用调用方字符串：

```cpp
aastore::Store store("proteins.aastore");
const aastore::SequenceId id = 0;
auto single = store.get(id);
std::string scratch;
store.get_into(id, scratch);
std::cout << single.view(0) << '\n' << scratch << '\n';
```

`get(id, options)` 与 `get(ids, options)` 使用相同的 `ReadOptions` 和统计语义，
单条请求不会排序或启动额外工作线程。`get({})` 明确表示空批次，`get({id})` 表示单元素列表。
`get_into(id, string)` 保留单条轻量路径，不构造 Batch、不启动后台线程，
也不接收 `ReadOptions`：FullMemory 及 PrefixMemory 命中直接解码，否则使用 `pread`。
`sequence_length(id)` 可查询长度，合法 ID 范围为 `[0, store.size())`。

随机批量读取保留输入顺序和重复 ID：

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
auto batch = store.get(ids);
for (size_t i = 0; i < batch.size(); ++i) {
    std::cout << ids[i] << '\t' << batch.view(i) << '\n';
}
std::string owned = batch[0];
std::string_view borrowed = batch.view(0);
```

`batch[i]` 返回独立 `std::string`，`batch.view(i)` 借用批次的输出存储。
视图只在所属存储仍存活且未被修改或复用时有效；普通批次拥有自身输出，Store 销毁后
结果仍有效。`storage`、`offsets`、`lengths` 提供底层布局，按偏移和长度定位条目。
随机序列 Batch 在每条序列后附加 NUL，但顺序 Batch 和 TextBatch 没有逐条 NUL，
统一使用长度或 `view()` 访问。

需要 header 时选择字段，下面的片段会在名称列存在时读取名称及序列：

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
if (store.has_names()) {
    const auto fields = aastore::RecordFields::Name | aastore::RecordFields::Sequence;
    auto records = store.batch_get_records(ids, fields);
    for (size_t i = 0; i < records.size(); ++i) {
        std::cout << records.name_view(i) << '\t' << records.sequence_view(i) << '\n';
    }
}
```

仅名称可用 `RecordFields::Name`，描述为 `Description`，默认 `All` 请求三列。
请求库中不存在的列会抛出异常；未选中的列不执行本次批读。`RecordBatch` 的
`name()`、`description()`、`sequence()` 返回副本，对应 `*_view()` 借用各列的批次存储。
名称和描述的长度按字节计算；各列结果及统计位于 `names`、`descriptions`、`sequences`。

## 连续扫描与输出缓冲复用

连续 ID 使用 `sequential_get(first, end)`，区间为左闭右开。下面按每块最多 1024 条扫描：

```cpp
aastore::Store store("proteins.aastore");
aastore::ReadOptions options;
options.threads = 4;
for (aastore::SequenceId first = 0; first < store.size();) {
    const auto end = first + std::min<aastore::SequenceId>(1024, store.size() - first);
    auto block = store.sequential_get(first, end, options);
    for (size_t i = 0; i < block.size(); ++i) {
        std::cout << first + i << '\t' << block.view(i) << '\n';
    }
    first = end;
}
```

顺序读取把区间布局为连续 residue arena，分片读取并解码，不生成逐序列随机请求、
不排序或做 range planning。文件读取使用 `pread`，不使用 io_uring；控制区间大小即可
限制每批输出内存。可在处理当前块时由调用方安排下一块预取。

已有输出区可直接解码，省去库分配的输出 arena。以下随机批读示例为每个请求建立独立缓冲：

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
aastore::ReadOptions options;
options.threads = 4;
std::vector<std::string> sequences(ids.size());
std::vector<char*> outputs(ids.size());
for (size_t i = 0; i < ids.size(); ++i) {
    sequences[i].resize(store.sequence_length(ids[i]));
    outputs[i] = sequences[i].empty() ? nullptr : sequences[i].data();
}
const auto stats = store.get_into(ids, outputs, options);
std::cout << sequences[0] << '\n' << stats.read_bytes << " bytes read\n";
```

顺序读取也支持直接输出，`outputs[i]` 对应 `first + i`：

```cpp
aastore::Store store("proteins.aastore");
const aastore::SequenceId first = 0;
const auto end = std::min<aastore::SequenceId>(1024, store.size());
aastore::ReadOptions options;
std::vector<std::string> sequences(end - first);
std::vector<char*> outputs(sequences.size());
for (size_t i = 0; i < sequences.size(); ++i) {
    sequences[i].resize(store.sequence_length(first + i));
    outputs[i] = sequences[i].empty() ? nullptr : sequences[i].data();
}
const auto stats = store.sequential_get_into(first, end, outputs, options);
std::cout << stats.requested_sequences << " sequences\n";
```

两个 `*_into()` 接口也接受 `outputs.data(), outputs.size()`，均返回 `BatchStats`。
输出数量必须等于请求数量；每个非空序列的输出区不得重叠，容量至少为序列长度，
空序列可用空指针。接口不追加 NUL，也无法从裸指针校验实际容量。复用字符串时，
在每次 `resize()` 后重新取得指针；读取期间不得并发修改缓冲或使地址失效。
内部仍需 packed 暂存缓冲；顺读暂存按调用线程复用，最多保留 64 MiB，更大读取使用临时缓冲。

多个线程可以共享已打开的 Store，各自使用独立输出缓冲；每次读取的工作线程预算
独立生效，并发调用不会自动共享一个总预算。销毁或移动 Store 前需等待相关读取结束。
视图不拥有数据，共享可修改的 Batch 或调用方缓冲仍需由调用方同步。

## ReadOptions 与统计

| 字段 | 默认值与含义 |
| --- | --- |
| `threads` | `8`；单次读取用户态线程上限，包含调用线程；`0` 按 `1`，小任务可使用更少线程 |
| `io_backend` | `IoBackend::Auto`；也可选 `Pread` 或 `IoUring` |
| `input_sorted` | `false`；设为 `true` 时调用方须保证 ID 非递减，允许重复，内部跳过排序 |
| `merge_gap_bytes` | `4096`；合并可跨越的最大物理间隙 |
| `max_merged_range_bytes` | `16384`；跨非零间隙合并时的范围上限 |
| `max_exact_merged_range_bytes` | `1048576`；仅重叠或首尾相接时的合并上限 |
| `io_queue_depth` | `64`；每个 ring 的目标深度，`0` 按 `1` |
| `io_buffer_bytes` | `16777216`；所有 ring 的在途 range buffer 预算，用最大范围保守估算并降低实际深度 |
| `collect_diagnostics` | `false`；开启后收集读取放大、合并次数、命中残基数、阶段耗时及 submit/wait 诊断 |

两个范围大小上限及 `io_buffer_bytes` 的 `0` 表示不限制。合并上限不拆分单条超长
请求；在途预算也至少允许必要的读取槽位，因此这些参数不是进程内存硬上限。
排序、规划、读取等阶段使用同一 `threads` 预算，不叠加各阶段线程数。
单条、顺序及完全内存命中路径按前述规则执行，不使用全部后端或范围参数。

需要诊断某次随机批读时：

```cpp
aastore::Store store("proteins.aastore");
const std::vector<aastore::SequenceId> ids = {1, 0, 1};
aastore::ReadOptions options;
options.collect_diagnostics = true;
auto batch = store.get(ids, options);
std::cout << batch.stats.read_bytes << " bytes, " << batch.stats.ranges << " ranges\n";
```

基础统计始终保留：`requested_sequences`、`requested_residues` 是请求量，`cache_hits`、
`cache_misses` 描述前缀命中，`ranges` 描述规划后的读取范围数量。
`read_bytes` 是应用读取字节数，不等于设备的物理 I/O；后者还受到操作系统页缓存和预读等影响。
`workers`、`io_workers`、`decode_workers` 表示实际工作线程；io_uring 的 I/O 和解码
由同一组线程执行，不能把这两个计数相加。`io_submissions/completions` 计数 SQE 请求，
`io_submit_calls`、`io_wait_calls` 才计数 API 调用；`io_empty_waits` 记录等待前 CQ 为空的次数。

诊断关闭时，`cache_hit_residues`、`cache_miss_residues`、`unique_packed_bytes`、
`merged_gaps`、`free_merges`、`overread_bytes`、全部 `*_ns` 及上述三个详细 API
计数字段为 `0`。不会调用读取诊断时钟、分配线程计时数组，或执行去重字节统计的额外
并行扫描；用 `diagnostics_collected` 区分未采集和实测为零。开启时，
`overread_bytes` 表示相对于所需 packed 字节的额外读取量。
io_uring 下的 `read_ns` 是等待 API 耗时，不等于纯设备等待；各线程累计耗时也不能
直接当作端到端延迟。`preload_ns()` 不受读取诊断开关影响。文本列使用 `TextBatchStats`，
提供请求量、读取字节、范围数量、驻留和后端信息；其中 `unique_bytes`、
`merged_gaps`、`overread_bytes` 同样只在 `collect_diagnostics=true` 时采集。

## 实现与兼容说明

随机读取按 ID 查 residue offset，去重及物理排序后按间隙和大小上限合并范围，读取并解码，
最后恢复请求顺序。FullMemory 随机 batch 直接并行解码。
io_uring 按读取字节量划分有序范围，每个 worker 独占一个 ring，并在自己的 completion
路径解码。PrefixMemory 命中工作由相同线程在 I/O 提交后分批处理并穿插检查完成事件，
没有额外解码池；全命中时不创建 ring。pread 线程各自读取并解码，前缀命中分阶段处理。

`Auto` 在后端初始化失败或确认没有可用读取操作时回退到 pread，显式 `IoUring` 会报错；
实际读取、提交及解码回调错误仍向调用方传播。初始化优先选 `READ`，只有 `READV`
可用时使用单个 iovec，并保持其存活到完成；Linux 5.4 等缺少操作码探测接口的内核，
以及缺少 READ 或探测 API 的旧 liburing，仍保留 READV 路径。
AAStore 不设置或查询内核 io-wq 线程上限，队列深度限制在途请求数，不代表内核线程数。

磁盘格式独立于软件包版本：`meta.bin` 使用 `AAPCMP3` 紧凑 residue offsets，
`packed.bin` 保存连续 5-bit stream；可选的 `names.bin`、`descriptions.bin` 保存
无分隔符文本，各自的 `.meta` 使用 `AATEXT1` 紧凑 offsets。三种模式不需要重建数据库。

源码入口：`builder.cpp` 负责建库，`metadata.cpp` 负责序列元数据，`codec.cpp` 负责编解码及 SIMD dispatch，
`byte_range_planner.cpp` 负责范围规划，
`io_uring_backend.cpp` 负责异步读取，`text_column.cpp` 负责文本索引及批读；
`store.cpp`、`store_batch.cpp`、`store_sequential.cpp` 分别组织资源、随机批读及顺读。
旧动态缓存、独立 I/O/解码线程参数和 `sequential_get_into(..., Batch&, ...)` 重载已移除，
调用方应更新并重新编译。版本和发布约定见 [发布指南](releasing.md)。
