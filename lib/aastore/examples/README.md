# 示例

这个目录提供真实 FASTA 数据和可以独立编译运行的 C++ 示例。
不用先安装 AAStore，也不用先编译 `tests/`。

```sh
cd examples
make       # 编译当前目录的 aastore_example，默认最多 4 个任务
make run   # 从 data/huge_100k.fa 建库，再演示读取
```

`data/huge_100k.fa` 是从 `huge.fa` 按原顺序提取的前 **100,000 条完整记录**，
共 **35,975,590 个残基**，文件 **51,154,661 字节**。原始标题和序列换行保留，
提取信息见 `data/huge_100k.json`。

首次运行会在 `output/huge_100k.aastore/` 建库。之后运行会明确提示并复用已有库。
程序展示单条读取、包含重复 ID 的乱序批量读取、连续读取，以及名称和描述。
只打印序列前 60 个字符，数据库保留全部序列。

可以直接运行编译好的程序，或换成自己的数据：

```sh
./aastore_example data/huge_100k.fa output/huge_100k.aastore 4
make run DATA=/path/to/proteins.fa STORE=output/my_dataset.aastore THREADS=8
make JOBS=8
```

换数据时使用新的 `STORE`，避免复用旧库。`make clean` 删除生成的可执行文件、
`.objects/` 和默认的 `output/huge_100k.aastore/`，保留数据集和源码；自定义 `STORE`
不会被清理。

Makefile 直接编译本仓库的 C++17 源码，编译中间文件放在 `.objects/`。
示例的普通读取使用 `Auto`：优先尝试 io_uring，不可用时回退到 `pread`；
顺序读取接口固定使用 `pread`。Makefile 默认 `IO_URING=AUTO`，自动探测 liburing
开发文件，可用时编译 io_uring 后端，否则正常编译并使用 `pread`。
可以用 `make IO_URING=ON` 要求编译 io_uring 后端（缺少能力时报错），
或用 `make IO_URING=OFF` 关闭探测和该后端。

`cpp/CMakeLists.txt` 另外保留通过已安装 SDK 构建的方式，供验证 `find_package` 接入。

`python/basic.py` 和 `python/buffer_layout.py` 也读取同一份 `data/huge_100k.fa`，
每次运行在临时目录建库，结束时自动清理。示例只展示少量记录；`buffer_layout.py`
展示首 4 条和包含重复 ID 的 4 条乱序批次，并按 `sequence_length()` 计算输出缓冲容量。
无需另行准备输入或安装 NumPy。

在仓库根目录安装本地 Python 包并运行：

```sh
python3 -m pip install . -Ccmake.define.AASTORE_ENABLE_IO_URING=ON
python3 examples/python/basic.py
python3 examples/python/buffer_layout.py
```

安装命令开启 liburing 探测；开发库可用时编译 io_uring 后端，供示例的 `Auto` 选择，
构建或运行环境不支持时回退到 `pread`。
详细安装要求见 [Python API 指南](../docs/python_api.md#1-安装与输入)。
