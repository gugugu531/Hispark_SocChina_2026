# board — 板端应用

海思 SS928 交叉编译（`aarch64-mix210-linux`）的板端应用，CMake 构建。常规嵌入式 Linux 工程结构。

## 结构

```
board/
├── CMakeLists.txt
├── cmake/toolchain-aarch64-mix210-linux.cmake   # 交叉编译 toolchain file
├── include/     # 头文件: log.h / version.h / control.h / pipeline.h(后续共享帧结构)
├── src/         # 源码, 按功能分文件: main.c / control.c (+ capture.c isp.c infer.c ... 后续)
└── tests/       # 单元/驱动测试: test_<名字>.c, 各编一个可执行 + ctest
```

- `src/` 平铺，每个功能一个 `.c`（+ `include/` 里对应 `.h`）；`main.c` 是主程序入口。
- `src/*.c`（除 `main.c`）编成静态库 `socchina`，主程序与测试共用。
- `tests/test_*.c` 每个自动编成可执行并注册到 `ctest`。
- 新增功能：往 `src/` 加 `.c`、`include/` 加 `.h`、（可选）`tests/` 加 `test_*.c`，无需改 CMake（glob 自动收集）。

## 构建

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux
export ISL_LIB_DIR=/path/to/libisl          # 提示缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk  # 接入硬件后再设
scripts/build_board.sh                      # 产物 build/socchina_app (aarch64)
```

## 测试

- **主机单元测试**（SDK-free 纯逻辑，如 control）：`scripts/test_host.sh`（本机 cc 直接编译运行）。
- **板端测试**（触硬件）：交叉编译出的 `test_<名字>` 部署到板上手动跑。

详见 `../docs/development-guide.md`。
