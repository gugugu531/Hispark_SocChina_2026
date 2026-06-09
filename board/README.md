# board — 板端应用

海思 SS928 交叉编译（`aarch64-mix210-linux`）的板端应用，CMake 构建。

## 构建

统一入口（仓库根目录执行）：

```sh
export CROSS_COMPILE_ROOT=/path/to/aarch64-mix210-linux
export ISL_LIB_DIR=/path/to/libisl          # 提示缺 libisl.so.19 时设置
# export SS928_SDK_ROOT=/path/to/ss928_sdk  # 接入硬件后再设
../scripts/build_board.sh                   # 或在根目录: scripts/build_board.sh
```

产物：`build/socchina_app`（aarch64 ELF）。未设 `SS928_SDK_ROOT` 时构建不链接 SDK 的最小程序，用于验证工具链。

## 布局

| 路径 | 用途 |
| --- | --- |
| `CMakeLists.txt` | 构建配置 |
| `cmake/` | 交叉编译 toolchain file |
| `src/` | 源码（起步仅 `main.c` 骨架，随数据通路阶段扩展） |
| `include/` | 头文件 |

代码按数据通路阶段（采集→ISP→缩放→预处理→推理→后处理→合成→显示/串流 + 控制）组织，模块增多后再拆分子目录。详见 `../docs/development-guide.md` 与 `../docs/architecture.md`。
