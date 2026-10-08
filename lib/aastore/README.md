# AAStore

AAStore 是用于蛋白质 FASTA 的 C++17 / Python 存储库。将 FASTA 建成数据库后，
可以按序列 ID 读取单条、批量或连续区间，适合需要反复访问大量序列的程序。

- 序列使用紧凑的 5 位编码保存，可同时保留名称和描述。
- 批量读取保留请求顺序和重复 ID，支持多线程读取与解码。
- 可按需从磁盘读取，也可将部分或全部编码数据预载到内存。
- C++ 和 Python 共用同一套核心实现；x86-64 上按 CPU 能力选择 SIMD 或标量路径。

## 构建

| 项目 | 当前支持范围 |
| --- | --- |
| 系统 | Linux x86_64，64 位、小端、glibc |
| C++ | CMake 3.16+，支持 C++17 的 GCC / Clang |
| Python wheel | 普通 CPython 3.9–3.13，Linux x86_64 / glibc |

这些是当前支持范围的说明。其他平台尚未验证；源码包声明 `Python >=3.9`，
更新的 CPython 可尝试源码构建。

**内核建议：新部署优先使用发行版仍在维护的 Linux 6.1 或更新的 LTS 内核。**
这不是已验证的最低内核要求。Linux 5.4 的 READV 兼容路径仍保留，`pread` 不依赖
io_uring。维护状态见 [Linux 内核发布说明](https://www.kernel.org/category/releases.html)
及所用发行版的维护政策。

### C++ 库和命令行工具

在仓库根目录运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
cmake --install build --prefix "$HOME/.local"
```

普通 C++ 构建不依赖 Python。liburing 是可选依赖；找到可用的开发库时启用
io_uring，未找到时使用 `pread`。

默认的独立构建会自动运行几个环境小测试：临时 FASTA 建库、单/多线程 `pread`、
前缀/全内存读取、常驻文本、自动后端和 `io_uring`。启用 Python 构建时还检查扩展
导入与读写。测试自行生成并清理小数据，不需要下载数据或 root 权限，也不是冷缓存 benchmark。
失败会使构建返回非零；`io_uring` 不可用时该 case 显示 `Skipped` 和原因，
不代表通过了 io_uring 测试，`Auto` 的读取仍必须通过。

可用 `-DAASTORE_RUN_SMOKE_TESTS=OFF` 关闭编译后的自动检查。
`add_subdirectory` 接入和交叉编译默认关闭自动检查；交叉运行须配置 emulator。
只重跑小测试可用 `(cd build && ctest -L smoke -V)`。
自动检查属于默认构建目标；显式只编译 `--target aastore` 或 Python wheel 不触发它。

完整回归测试和 benchmark 默认不编译。需要测试时进入 `tests/`，直接运行：

```bash
cd tests
make       # 编译，可执行文件直接放在 tests/
make test  # 可选：运行测试
```

更多选项见 [测试说明](tests/README.md)。旧构建目录会保留原来的 CMake 开关设置。

在自己的 CMake 项目中链接 AAStore：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)
find_package(aastore 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE aastore::aastore)
```

配置该项目时添加 `-DCMAKE_PREFIX_PATH="$HOME/.local"`。编译标准、头文件和链接依赖
由 target 传递。仅安装库、构建共享库或直接嵌入源码的方法见 [C++ 指南](docs/cpp_api.md)。

### Python 包

从仓库源码安装，需要 C++17 编译器和 Python 开发头文件；其他构建依赖由 pip 准备，
无需安装 NumPy：

```bash
python -m pip install .
```

**Python 接口支持 io_uring，但默认构建和预编译 wheel 使用 `pread`。**
要启用 io_uring，先安装 liburing 开发包，再运行：

```bash
python -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
```

这种构建可能依赖系统 liburing 运行库，详见 [Python 安装说明](docs/python_api.md#1-安装与输入)。

## 快速开始

以下示例使用已有的、至少包含一条记录的 `proteins.fa`。

### 建库

```bash
./build/aastore_build proteins.fa proteins.aastore
```

序列 ID 按 FASTA 记录顺序从 `0` 开始。默认保存名称和描述；只需序列时加
`--sequences-only`。已有数据库默认不会被覆盖，需要重建时显式加 `--overwrite`。

### C++ 读取

将下面代码保存为自己项目的 `main.cpp`，配合上面的 CMake 配置使用：

```cpp
#include <aastore/store.hpp>
#include <iostream>

int main() {
    aastore::Store store("proteins.aastore");
    auto single = store.get(0);
    std::cout << single[0] << '\n';

    auto batch = store.get({0, 0});
    for (size_t i = 0; i < batch.size(); ++i) {
        std::cout << batch[i] << '\n';
    }
}
```

`batch[i]` 对应第 `i` 个请求，返回独立字符串。
完整示例和十万条 FASTA 数据集见 [examples](examples/README.md)，无需先安装 SDK：

```bash
cd examples
make
make run
```

### Python 读取

Python 可以直接打开上一步生成的同一份数据库：

```python
import aastore

with aastore.Store("proteins.aastore") as store:
    print(store.get(0)[0])
    batch = store.get([0, 0])
    print(list(batch))

    record = store.batch_get_records([0])[0]
    print(record["name"], record["description"], record["sequence"])
```

也可以直接在 Python 中建库：

```python
aastore.build_fasta("proteins.fa", "proteins-python.aastore", threads=4)
```

每次建库使用新的输出目录，或显式传入 `overwrite=True`。
不需要准备输入文件的完整示例见 [examples/python/basic.py](examples/python/basic.py)。

## 内存和读取选项

序列数据有三种打开模式：

| `storage_mode` | 行为 |
| --- | --- |
| `ByteRange`（默认） | 按需读取所请求的序列 |
| `PrefixMemory` | 预载固定大小的编码数据前缀，其余部分按需读取 |
| `FullMemory` | 预载全部编码数据，读取时从内存解码 |

预载的是编码数据。`PrefixMemory` 的大小由 `cache_bytes` 设置，前缀不会动态替换。
名称和描述通过独立选项控制，默认不预载；也可以同时开启：

```python
opening = aastore.StoreOptions(
    storage_mode=aastore.StorageMode.PrefixMemory,
    cache_bytes=512 * 1024 * 1024,
    resident_names=True,
    resident_descriptions=True,
)
with aastore.Store("proteins.aastore", opening) as store:
    print(store.resident_bytes, store.resident_text_bytes)
```

C++ 使用 `StoreOptions` 中的同名字段。启用后，打开库时将对应列的完整正文读入
`Store` 持有的内存缓冲，后续批读直接复制该缓冲，不再读取正文文件；索引仍使用只读映射。
`resident_text_bytes` 表示已载入的文本字节数，不含索引，也不是进程实时 RSS。

每次读取的线程数由 `ReadOptions.threads` 设置，例如 `aastore.ReadOptions(threads=4)`。
默认的 `IoBackend.Auto` 在构建及运行环境允许时使用 io_uring，后端初始化不可用时
回退到 `pread`；显式选择 `IoUring` 时，不可用会报错，实际 I/O 错误也会向调用方传播。

更多按字段读取、连续扫描、零复制视图、复用输出缓冲和诊断选项见下面的指南。

## 详细文档

- [C++ 使用指南](docs/cpp_api.md)：建库、读取接口、内存模式、线程及实现说明。
- [Python API 指南](docs/python_api.md)：完整参数、批次类型、缓冲区和生命周期。
- [基准测试](benchmarks/README.md)：命令行参数、冷缓存实验和指标含义。
- [版本与发布](docs/releasing.md)：SDK 验证、交叉编译和兼容约定。
- [变更记录](CHANGELOG.md)：版本变化及旧接口迁移说明。

项目版本由 [VERSION](VERSION) 统一管理。0.x 阶段的小版本可能调整 API；
包版本与磁盘格式版本分别管理，升级包不自动意味着需要重建数据库。

## 许可证

AAStore 采用 GNU General Public License version 3（`GPL-3.0-only`），见 [LICENSE](LICENSE)。
