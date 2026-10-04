# 测试

普通独立构建默认只运行 `smoke.cmake` 注册的环境自检。其余回归测试由
`regression.cmake` 注册，默认不编译；`Makefile` 复用根目录的 CMake 配置，
显式开启完整测试，不另写编译器参数或维护测试源文件清单。

## 编译和运行

进入 `tests/` 直接编译：

```sh
cd tests
make                           # 仅编译，默认最多 4 个编译任务
make test                      # 编译后运行全部已注册测试
make test CTEST_ARGS='-L sequential'
make clean                     # 清理编译产物，保留配置和测试报告
```

可执行文件和中间文件都放在 `tests/build/`，不会修改普通的 `build/`。
可通过 `BUILD_DIR=/absolute/path`、`BUILD_TYPE=Debug`、`JOBS=8` 覆盖。

测试构建中关闭构建后自动自检，避免 `make test` 重复执行；自检仍会作为普通 CTest
条目参与完整测试。任何测试失败都会使 `make test` 返回非零，io_uring 不可用仍明确跳过。
旧构建目录中的 CMake 缓存不会随默认值变化自动重置；可在普通构建中显式传入
`-DAASTORE_BUILD_TESTS=OFF -DAASTORE_BUILD_BENCHMARKS=OFF`。

## 可选部分

- `IO_URING=OFF`：不链接 liburing，验证 pread 路径和模拟的兼容性测试。
- `BENCHMARKS=OFF`：不构建 `aastore_bench`，也不注册依赖它的 benchmark 回归测试。
- `PYTHON=ON`：编译 Python 扩展并注册 Python 测试，需要 Python 开发文件和 pybind11。
- `CMAKE_ARGS='...'`：附加 CMake 参数，例如指定编译器、工具链或 pybind11 路径。
- `CTEST_ARGS='...'`：附加 CTest 参数，例如 `-L smoke` 或 `-R aastore_options_test`。

```sh
make test PYTHON=ON \
  CMAKE_ARGS='-DPython_EXECUTABLE=/path/to/python -Dpybind11_DIR=/path/to/pybind11/cmake'
```

完整测试覆盖建库、读取、编解码边界、参数校验、并发、io_uring 异常恢复、安装后链接
以及 CMake 兼容性。benchmark 源码和实验脚本保留在 `benchmarks/`；此处的
`benchmark_test.py` 只用自动生成的小数据检查它们，不启动论文数据集实验。
CI 仍可直接传入 `-DAASTORE_BUILD_TESTS=ON -DAASTORE_BUILD_BENCHMARKS=ON`，然后运行 CTest。
