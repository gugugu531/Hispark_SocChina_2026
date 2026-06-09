# board — 板端应用

海思 SS928 交叉编译（`aarch64-mix210-linux`）的板端应用，CMake 构建。

## 目录约定

采用组件式结构：**每个模块一个目录，内含 `include/ src/ tests/`**；模块编译成静态库（无 `main`），`tests/` 放该模块的测试入口（各自 `main`）。模块名用数据通路的实际语义。

```
board/
├── CMakeLists.txt
├── cmake/
│   ├── toolchain-aarch64-mix210-linux.cmake   # 交叉编译 toolchain file
│   └── helpers.cmake                          # add_board_module / add_board_test
├── common/include/                            # 公共: version/log/错误检查(header-only)
├── modules/                                   # 数据通路各模块(按需添加)
│   └── control/{include,src,tests}            # 示例: 场景自适应判决(SDK-free)
└── app/                                       # 生产主程序 socchina_app
```

后续模块（`capture/ isp/ vpss/ preproc/ infer/ postproc/ compose/ display/ stream`）由各 owner 照 `control/` 的样式添加：建 `modules/<x>/{include,src,tests}` + 一份 `CMakeLists.txt`，并在 `board/CMakeLists.txt` 登记 `add_subdirectory`。起步不预建空模块。

## 新增一个模块（模板）

`modules/<x>/CMakeLists.txt`：
```cmake
add_board_module(<x>)                  # src/*.c -> 库 mod_<x>，公开 include/
target_link_libraries(mod_<x> PUBLIC board_common)
add_board_test(<x> DEPS mod_<x>)       # tests/test_<x>.c -> 可执行 test_<x> + ctest
```
SDK-free（纯逻辑）模块在 `board/CMakeLists.txt` 顶部登记（可主机单测）；触 SDK/硬件 的模块登记在 `BOARD_HOST_TESTS` 之外的块（仅交叉构建）。

## 构建

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux
export ISL_LIB_DIR=/path/to/libisl          # 提示缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk  # 接入硬件后再设
scripts/build_board.sh                      # 产物 build/app/socchina_app (aarch64)
```

## 测试

- **主机单元测试**（SDK-free 模块，自动化）：`scripts/test_host.sh` → 原生编译 + `ctest`。
- **板端测试/驱动**（触硬件模块）：交叉编译出的 `test_<模块>` 部署到板上手动运行。

详见 `../docs/development-guide.md`（§4 构建、§10 测试）与 `../docs/architecture.md`。
