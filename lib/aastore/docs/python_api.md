# Python API 使用指南

AAStore 的 Python 包直接调用 C++ 建库和读取核心。通常先用 `build_fasta()`
生成库目录，再通过 `Store` 读取序列或记录；批次结果同时提供 Python 字符串访问和
只读字节视图。包内包含 `.pyi` 和 `py.typed`，供编辑器与类型检查器识别公开接口。

两个独立示例读取仓库自带的 `examples/data/huge_100k.fa`（100,000 条完整 FASTA 记录），
每次在临时目录建库，结束时自动清理，无需另行准备输入或安装 NumPy。
示例只展示少量记录；`buffer_layout.py` 展示首 4 条和包含重复 ID 的 4 条乱序批次，
并按 `sequence_length()` 计算可复用输出缓冲的容量。
在仓库根目录安装本地包并运行：

```bash
python3 -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
python3 examples/python/basic.py
python3 examples/python/buffer_layout.py
```

安装命令开启 liburing 探测；开发库可用时编译 io_uring 后端，供示例的 `Auto` 选择，
构建或运行环境不支持时回退到 `pread`。
详细安装要求见下节，数据集信息见 [示例说明](../examples/README.md)。

## 1. 安装与输入

当前支持与验证范围为 Linux x86_64、64 位、小端、glibc，使用支持 C++17 的 GCC/Clang。
预编译 wheel 覆盖普通 CPython 3.9–3.13。这些说明用于提醒当前支持范围；范围之外的
平台、Python 实现和特殊 ABI 尚未验证，不承诺可用性。
源码包声明 `Python >=3.9`，可在相同平台上尝试更新的 CPython，但这些版本尚不在
当前 wheel 支持与验证范围内。在仓库根目录从源码安装：

```bash
python -m pip install .
```

本机需要支持 C++17 的 GCC/Clang 和 Python 开发头文件，其他编译依赖由 pip 的
隔离构建环境准备。
NumPy 是可选的使用工具，不是 AAStore 的运行依赖。

Python 包默认禁用 io_uring，使用 pread；portable wheels 工作流也采用此配置。
新部署的内核建议见 [README 构建说明](../README.md#构建)；默认的 pread 后端不要求
内核提供 io_uring。
需要在本机启用 io_uring 时，先安装 liburing 开发包，再从源码构建：

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

这种本机构建可能依赖系统 `liburing`。目标机器需要相应运行库；读取时的 Auto 回退
不能处理扩展导入之前就发生的动态库缺失。wheel 只包含 Python 包，不包含 C++ SDK。
源码包重建和发布流程见 [releasing.md](releasing.md)。

也可手动通过 CMake 构建绑定。普通 C++ 构建默认不需要 Python 或 pybind11；
显式启用绑定时可运行：

```bash
python -m pip install 'pybind11>=3.0,<4'
cmake -S . -B build-python -DAASTORE_BUILD_PYTHON=ON \
  -DPython_EXECUTABLE="$(python -c 'import sys; print(sys.executable)')" \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build-python -j 8
(cd build-python && ctest --output-on-failure)
```

直接构建支持 CMake 3.16 及以上。3.16/3.17 使用 `Development` 查找组件，
需要 Python 开发头文件和开发库；3.18 及以上使用 `Development.Module`，
只要求扩展模块所需的开发文件。旧 CMake 未设置 `Python_ROOT_DIR` 时，会用所选
解释器的 `sys.base_prefix` 查找开发文件，也支持缺少 `distutils` 的新版本 Python
虚拟环境。非标准安装仍可显式指定 `Python_ROOT_DIR` 或 `Python_INCLUDE_DIR`、
`Python_LIBRARY`。

`build_fasta(fasta, store, ...)` 接受路径字符串或 `pathlib.Path`，输入为 FASTA 文件，
输出为 AAStore 库目录。序列 ID 按输入记录顺序从 `0` 开始分配，空序列也占一个 ID。
名称是 FASTA header 中的第一个字段，描述是其后的文本。名称与描述可独立选择是否保存。

```python
from pathlib import Path
from tempfile import TemporaryDirectory
import aastore

with TemporaryDirectory(prefix="aastore-guide-") as temporary:
    root = Path(temporary)
    fasta = root / "input.fasta"
    fasta.write_text(
        ">alpha 示例描述\nACDEFG\n>empty 空序列\n\n>beta 第二条\nWY\n",
        encoding="utf-8",
    )
    built = aastore.build_fasta(fasta, root / "store", threads=2)
    with aastore.Store(root / "store") as store:
        print(built.sequences, store[0])  # 3 ACDEFG
```

## 2. 选择建库、打开和读取选项

选项对象的构造参数均为关键字参数，字段可修改。建库和读取调用会取得自己的选项副本；
修改原对象不会改变已经开始的原生调用。

三个选项类均提供 `validate()`，可在使用前主动检查；成功返回 `None`，无效配置抛出
`ValueError`，且不会修改选项。选项字段可分步配置；建库、开库和读取入口会重新校验，
因此修改后无需依赖之前的校验结果，空请求和空区间也会校验。
未知的存储模式、I/O 后端或读取策略会被拒绝；建库线程须大于零，
`PrefixMemory` 的 `cache_bytes` 须大于零。读取参数中已有的零值约定保持有效。

### `BuildOptions`：哪些数据写入库

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `threads` | `1` | 建库总线程预算，对应 C++ 的 `encode_threads` |
| `overwrite` | `False` | 是否允许重建已有库 |
| `write_names` | `True` | 保存名称列 |
| `write_descriptions` | `True` | 保存描述列 |

```python
options = aastore.BuildOptions(threads=2, write_descriptions=False)
built = aastore.build_fasta(fasta, path, options, threads=4)
# 本次使用 threads=4；options.threads 仍为 2。
```

`build_fasta()` 的 `threads`、`overwrite`、`write_names`、`write_descriptions`
关键字可覆盖选项对象中的对应字段，不修改原对象。默认拒绝覆盖已有库。
返回的 `BuildStats` 提供只读的 `sequences`、`residues`、`packed_bytes`、
`name_bytes` 和 `description_bytes`。
统计与批次类型由建库、读取调用返回，不提供 Python 构造器。

### `StoreOptions`：打开时驻留哪些数据

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `storage_mode` | `StorageMode.ByteRange` | 按字节范围读取；也可选 `PrefixMemory` 或 `FullMemory` |
| `cache_bytes` | `0` | `PrefixMemory` 的固定 packed 前缀预算，以字节计 |
| `threads` | `8` | 打开与预载阶段的线程预算 |
| `resident_names` | `False` | 将名称数据预载入内存 |
| `resident_descriptions` | `False` | 将描述数据预载入内存 |
| `index_load_mode` | `IndexLoadMode.Mmap` | 只读映射索引；可选 `Read` 将完整索引读入自有内存 |

启用 `resident_names` 或 `resident_descriptions` 后，打开库时将对应列的完整正文
读入 `Store` 持有的内存缓冲。后续批读直接从该缓冲复制，不再读取正文文件。
`resident_text_bytes` 返回这些正文缓冲的总字节数，不含索引，也不是进程实时 RSS。

`index_load_mode` 统一控制序列索引 `meta.bin` 及存在的 `names.meta`、
`descriptions.meta`。默认 `Mmap` 使用只读文件映射；`Read` 在打开时完整读取这些
索引，保存在 `Store` 自有内存中。它与序列、名称和描述正文的驻留选项独立。

```python
opening = aastore.StoreOptions(index_load_mode=aastore.IndexLoadMode.Read)
with aastore.Store(path, opening) as store:
    print(store.index_load_mode)
```

`PrefixMemory` 的前缀固定，不会动态淘汰或更换内容；完全命中前缀的序列从内存解码，
其他序列仍按范围读取。`FullMemory` 在打开时预载全部 packed 序列数据。
这两个模式驻留的是编码数据，不是预先生成的 Python 字符串。

```python
opening = aastore.StoreOptions(
    storage_mode=aastore.StorageMode.PrefixMemory,
    cache_bytes=64 * 1024 * 1024,
    threads=2,
    resident_names=True,
)
with aastore.Store(path, opening) as store:
    print(store.storage_mode, store.resident_bytes, store.resident_text_bytes)
```

### `ReadOptions`：每次读取的调度与诊断

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `threads` | `8` | 本次读取的用户态工作线程上限，包含调用线程 |
| `io_backend` | `IoBackend.Auto` | 可选 `Auto`、`Pread`、`IoUring` |
| `input_sorted` | `False` | 调用方承诺请求 ID 已非递减排列，可重复；开启后跳过内部排序 |
| `merge_gap_bytes` | `4096` | 合并允许跨过的字节间隙 |
| `max_merged_range_bytes` | `16384` | 跨非零间隙合并的范围上限 |
| `max_exact_merged_range_bytes` | `1048576` | 无额外读取的连续范围合并上限 |
| `io_queue_depth` | `64` | 每个 io_uring worker 的队列深度 |
| `io_buffer_bytes` | `16777216` | 所有 io_uring ring 合计的在途读取缓冲预算 |
| `collect_diagnostics` | `False` | 收集读取放大、合并次数、命中残基数、阶段耗时及调用计数 |

`input_sorted=True` 是调用方的承诺，库不会验证乱序输入。只有 ID 非递减排列时才能开启，
例如 `[0, 0, 2]` 合法，`[2, 0]` 应保持默认排序。
`max_merged_range_bytes`、`max_exact_merged_range_bytes` 为 `0`
时分别取消对应的合并大小限制；`io_buffer_bytes=0` 取消由缓冲预算推导的队列深度限制，
仍受 `io_queue_depth` 等条件约束。`merge_gap_bytes=0` 则表示不跨越额外物理间隙，
并不取消 gap 限制。合并大小限制不会将单条超长序列拆开。
`threads=0` 和 `io_queue_depth=0` 均按 `1` 处理。

`Auto` 在当前构建和运行环境允许时使用 io_uring，否则回退到 pread；显式指定
`IoUring` 时，不可用会抛出异常。FullMemory 命中内存的路径无需磁盘 I/O。
io_uring 初始化时优先使用支持的 `READ`，仅支持 `READV` 或缺少操作码探测接口
时使用 `READV`，兼容 Linux 5.4。后端初始化失败或探测确认没有可用读取操作时
才按上述规则回退；真实读取或回调错误会抛出异常。
`sequential_get()` 和 `sequential_get_into()` 使用 pread 顺读路径，
不会因为 `io_backend=IoUring` 改成 io_uring。

```python
reads = aastore.ReadOptions(
    threads=2,
    io_backend=aastore.IoBackend.Pread,
    collect_diagnostics=True,
)
```

## 3. 读取结果：字符串、批次和记录

`Store` 支持 `with`、`len(store)` 和整数索引。`store[-1]` 使用 Python 负索引；
`get(id)`、`sequence_length(id)` 及批读 ID 列表使用非负序列 ID。

| 调用 | 返回值 | 顺序与用途 |
| --- | --- | --- |
| `store.get(id, reads)` | `Batch` | 包含一条序列，使用 `batch[0]` 或 `batch.view(0)` 访问 |
| `store[id]` | `str` | 单条序列副本，保留整数索引的便利用法 |
| `store.sequence_length(id)` | `int` | 序列长度，便于预分配输出 |
| `store.get(ids, reads)` | `Batch` | 按请求顺序返回，保留重复 ID |
| `store.sequential_get(begin, end, reads)` | `Batch` | 连续半开区间 `[begin, end)` |
| `store.batch_get_records(ids, fields, reads)` | `RecordBatch` | 按请求顺序取得所选记录字段 |

### `Batch` 与 `TextBatch`

`Batch` 保存解码后的序列；`TextBatch` 保存名称或描述。两者均支持：

- `len(batch)`、`batch.size()`、`batch.empty()`；
- 整数索引与负索引，返回 `str` 副本；
- 迭代，按访问顺序逐条创建字符串；
- 切片，返回字符串列表，例如 `batch[::-1]`；
- `view(i)`，返回该条目的只读字节 `memoryview`；
- `storage`、`offsets`、`lengths`，提供整个批次的数据布局；
- `stats`，返回只读统计对象。

```python
with aastore.Store(path) as store:
    batch = store.get([2, 0, 2], reads)
    print(batch[0], list(batch), batch[-2:])
    raw = batch.view(0)
    print(raw.readonly, bytes(raw))
```

序列是 ASCII 数据。名称与描述转为 `str` 时按 UTF-8 解码；如果输入文本不符合 UTF-8，
字符串访问会报错，仍可用 `TextBatch.view(i)` 取得原始字节。
`view(i)` 自身不复制数据；`bytes(view)` 和字符串访问会创建副本。

### `RecordBatch`：按字段读取

`RecordFields.Name`、`Description` 和 `Sequence` 可用 `|` 组合，默认 `All`。
仅请求名称时不必解码序列。请求的名称或描述列必须在建库时保存。
枚举由 pybind11 导出，可访问 `.name`、`.value` 和 `__members__`；
枚举类不支持 Python `enum.Enum` 的迭代或按字符串下标访问。

```python
with aastore.Store(path) as store:
    fields = aastore.RecordFields.Name | aastore.RecordFields.Sequence
    records = store.batch_get_records([2, 0], fields, reads)
    print(records[0])  # 字典只含 name 和 sequence
    print(records.name(0), records.sequence(0))
    names = records.names          # TextBatch
    sequences = records.sequences # Batch
```

`RecordBatch` 支持长度、负索引、迭代和切片；条目是字典，切片是字典列表。
`fields` 标识实际请求的字段。`names`、`descriptions` 和 `sequences`
是各列的子批次，可继续使用字节视图。访问未请求字段的 `name()`、`description()`
或 `sequence()` 会抛出异常；代码应先根据 `fields` 选择列。

## 4. 使用数据布局与可选 NumPy

`Batch` 与 `TextBatch` 的三个布局属性均为只读的一维 `memoryview`：

| 属性 | 元素类型 | 含义 |
| --- | --- | --- |
| `storage` | 无符号字节 | 本批次的完整连续字节区 |
| `offsets` | 本机字节序的 `uint64` | 每个条目在 `storage` 中的起始字节偏移 |
| `lengths` | 本机字节序的 `uint64` | 每个条目的字节长度，不含 NUL |

`len(offsets) == len(lengths) == len(batch)`。对第 `i` 条记录，可直接用布局恢复字节：

```python
storage = batch.storage
offsets = batch.offsets
lengths = batch.lengths
i = 0
raw = storage[offsets[i]:offsets[i] + lengths[i]]
assert bytes(raw) == bytes(batch.view(i))
```

偏移单位始终是字节。文本长度也是 UTF-8 **字节数**，不是 `len(text)` 的字符数。
`get()` 以及 `batch_get_records()` 中的序列批次，在每条序列后追加一个 NUL，
三种驻留模式一致；`sequential_get()` 的输出连续拼接序列，不追加 NUL。
文本批次也不追加 NUL。应始终按 `offsets + lengths`
定位，避免把整块 `storage` 当成一个字符串或通过 NUL 猜测记录边界。
这些布局描述的是批次中的解码字节，不是数据库中 5-bit packed 数据的布局。

如果应用已经安装 NumPy，可直接借用布局与字节区：

```python
import numpy as np  # 可选依赖，仅此示例需要

offsets = np.frombuffer(batch.offsets, dtype=np.uint64)
lengths = np.frombuffer(batch.lengths, dtype=np.uint64)
storage = np.frombuffer(batch.storage, dtype=np.uint8)
first = storage[int(offsets[0]):int(offsets[0] + lengths[0])]
```

这里 `np.uint64` 使用本机字节序，与属性导出的 native `uint64` 一致。
三个数组借用原存储且不可写；需要独立、可修改数组时显式调用 `.copy()`。
不要把布局视图当成跨平台的磁盘序列化格式。

## 5. 直接写入并复用输出缓冲

Python 用可写缓冲对象承接 C++ 的直接输出接口，例如 `bytearray` 或连续的可写
`memoryview`。C++ 的裸 `char*` 和可变 `std::string` 不直接暴露为 Python 参数。
单条写法是 `store.get_into(id, output, reads)`，返回 `BatchStats`；Python 的 `str`
不可修改，因此这里的 `output` 同样是预先分配的可写字节缓冲，不会自动调整大小。

```python
with aastore.Store(path) as store:
    output = bytearray(store.sequence_length(1))
    store.get_into(1, output, reads)
    print(output.decode("ascii"))

    ids = [2, 0, 2]
    outputs = [bytearray(store.sequence_length(i)) for i in ids]
    stats = store.get_into(ids, outputs, reads)
    print(outputs[0].decode("ascii"), stats.requested_sequences)
    # 下一次请求容量足够时，可重用同一组 bytearray。
    store.get_into(ids, outputs, reads)
```

`sequential_get_into(begin, end, outputs, reads)` 使用相同约定，输出数量为 `end - begin`。
两个方法返回 `BatchStats`，直接写入调用方内存，不生成解码输出 `Batch`，也不追加 NUL。
每项输出必须满足以下条件：

- 数量与请求数量一致；空序列允许使用 `None` 或空缓冲；
- 一维、连续、可写，元素大小为一个字节；
- 容量至少等于对应序列长度；各实际写入区不重叠。

缓冲较大时，只覆盖前 `sequence_length(id)` 个字节，其余内容保持原值。
复用不同长度的请求后，按新长度截取有效结果；直接解码整个旧缓冲可能读到上次的尾部。
接口先检查 ID、输出数量、容量、布局和重叠，再开始写入。

原生调用会释放 GIL。调用期间不要从其他线程读写同一输出区；缓冲协议固定了内存，
可以阻止扩容，但不会阻止其他线程修改其中的字节。普通批读结果与用户输出缓冲互相独立。

## 6. 统计、错误与生命周期

### 只读统计与运行信息

`BuildStats` 描述建库规模；`Batch.stats` 或直接输出方法的返回值是 `BatchStats`，
`TextBatch.stats` 是 `TextBatchStats`。常用字段包括：

| 对象 | 常用字段 |
| --- | --- |
| `BuildStats` | `sequences`、`residues`、`packed_bytes`、`name_bytes`、`description_bytes` |
| `BatchStats` | `requested_sequences`、`requested_residues`、`cache_hits`、`cache_misses`、`read_bytes`、`ranges`、`io_uring` |
| `TextBatchStats` | `requested_strings`、`requested_bytes`、`read_bytes`、`resident`、`io_uring` |

`BatchStats.diagnostics_collected` 标识是否开启额外诊断；默认关闭时，
`cache_hit_residues`、`cache_miss_residues`、`unique_packed_bytes`、`merged_gaps`、
`free_merges`、`overread_bytes`、阶段耗时和 submit/wait 调用诊断字段保持零。
文本列的 `unique_bytes`、`merged_gaps`、`overread_bytes` 也由同一开关控制。
这些零表示未采集，不能当成实测结果；关闭时跳过去重字节统计的额外扫描和线程启动。
请求量、命中条数、读取字节数、范围/线程/队列及 SQE/CQE 计数仍然保留。
开启后还可查看 `layout_ns`、`range_plan_ns`、`read_ns`、`decode_ns` 等纳秒字段。
io_uring 的 `io_workers` 和 `decode_workers` 描述同一组线程，不应相加。

`Store` 还提供只读的 `total_residues`、`packed_bytes`、`storage_mode`、`index_load_mode`、`resident_bytes`、
`resident_text_bytes`、`preload_ns`、`has_names`、`has_descriptions` 和
`decode_kernel_name`。包版本通过 `aastore.__version__` 取得。
完整字段和签名见 [类型声明](../python/aastore/__init__.pyi)。

### 常见错误

| 异常 | 常见原因 |
| --- | --- |
| `IndexError` | ID 或索引越界，连续读取区间非法 |
| `ValueError` | 已关闭的 Store、输出数量/容量/布局不符或写入区重叠 |
| `TypeError` / `BufferError` | 参数类型不符、输出不支持所需可写缓冲协议 |
| `RuntimeError` | 库文件或建库失败、缺少文本列/请求字段、显式 io_uring 不可用 |
| `UnicodeDecodeError` | 名称或描述不是有效 UTF-8；可改用字节视图 |

不要依赖完整错误消息作为稳定接口。参数检查失败后可修正并重新调用；
实际 I/O 失败不承诺回滚已经写入用户缓冲的部分字节。

### Store 与结果的生命周期

优先使用 `with aastore.Store(path) as store:`，退出时自动关闭。`close()` 可重复调用，
`closed` 可查询状态；关闭后的新读取会抛出 `ValueError`。已经开始的调用持有自己的
库资源引用，可继续完成。建库、打开、读取和解码中的主要原生工作释放 GIL，
多个 Python 调用线程可以共享只读 Store。

`Batch` 和 `TextBatch` 拥有自己的解码输出，关闭 Store 不会使已有批次失效。
`view(i)`、`storage`、`offsets`、`lengths` 的视图会保留所属批次；`RecordBatch` 的
子批次会保留父记录对象。即使删除局部批次变量，仍被视图引用的数据继续有效：

```python
with aastore.Store(path) as store:
    view = store.get([0]).view(0)
    offsets = store.get([0]).offsets
# 两个临时批次仍由各自视图保留，Store 已关闭。
print(bytes(view), offsets[0])
```

小切片也会保留整个批次的内存。需要尽快释放大批次时，可先用 `bytes(view)`
复制所需内容，再释放所有相关视图。输出缓冲由调用方持有，后续复用会覆盖旧数据。

## 7. 与 RabbitSketch Python 接口的对应关系

此对照固定到官方 RabbitSketch `lib` 分支提交
[`8db783ea75f597068b72705272ae207c94194e83`](https://github.com/RabbitBio/RabbitSketch/tree/8db783ea75f597068b72705272ae207c94194e83)，
不代表其旧版 PyPI 包一定包含相同接口。

| 使用维度 | AAStore | RabbitSketch |
| --- | --- | --- |
| 配置与统计 | `BuildOptions`、`StoreOptions`、`ReadOptions` 和建库/读取统计 | `SketchConfig`、`BuildStats`，以及算法能力与运行信息 |
| 自定义结果 | `Batch`、`TextBatch`、`RecordBatch` | `BuildResult`、`BuiltSketch`、`QueryResult` |
| 主要调用 | 建库、打开、单读/批读/顺读 | FASTX 构建、流式 builder、sketch 查询及各算法原生接口 |
| 长调用与 GIL | 主要建库/读取原生工作释放 GIL | 高层 build、builder 与 query 绑定也释放 GIL |
| 批次数据交换 | 连续字节区及只读布局视图；可直接写入用户缓冲 | 高层构建返回结果对象列表；部分原生数组转为 Python 列表 |
| 类型提示 | 包含 `.pyi` 和 `py.typed` | 此提交中未提供这两类文件 |

共同点是将 C++ 配置、结果对象与统计作为 Python API 公开，而非只返回一个裸数值。
AAStore 的布局视图服务于批量序列读取；RabbitSketch 的多种原生算法接口服务于 sketch
计算。接口数量体现职责，不能直接作为 Python 工程完备性的评分。
对照源码见 RabbitSketch 的
[Python 绑定](https://github.com/RabbitBio/RabbitSketch/blob/8db783ea75f597068b72705272ae207c94194e83/src/pybind.h#L769)、
[构建结果定义](https://github.com/RabbitBio/RabbitSketch/blob/8db783ea75f597068b72705272ae207c94194e83/src/api/SketchBuilder.h#L28)
和[Python 使用测试](https://github.com/RabbitBio/RabbitSketch/blob/8db783ea75f597068b72705272ae207c94194e83/tests/test_fastx_python.py#L39)。
