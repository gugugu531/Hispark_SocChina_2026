# Agent Prompt: 板端 LVGL UI 改走"合成进视频帧"路线(方案 C,绕开 GFBG)

## 任务

把板端 LVGL 触摸 UI 从 **GFBG 图形层叠加(方案 A)** 改为 **离屏渲染 + 合成进 VO 视频帧(方案 C)**:
LVGL 渲染到一块内存缓冲(1024×600 ARGB8888),显示循环里把 UI 转成 NV21 并合成进 VPSS 输出帧,
经既有 `VI→ISP→VPSS→VO→HDMI` 视频通路上屏。**不再使用 GFBG `/dev/fb0`。**

## 为什么放弃方案 A(已板端实测证伪,不要重走)

GFBG G0(`/dev/fb0`)路线经逐层实测,存在**根本性、无法从 userspace 绕过**的障碍。以下为已坐实事实,
**直接采信,不要重新验证**:

- `/dev/fb0` 由 `gfbg.ko` 在 module-load 时**固定**为 `3840×2160 ARGB1555`,并由硬件缩放到 1024×600 面板
  (`/opt/ko/load_ss928v100` 注释:`fb0:argb1555,3840x2160,2buf`;`/proc/umap/gfbg0` 印证)。
- **运行时改不动几何/格式**:`FBIOPUT_VSCREENINFO`(即便带 `activate=FB_ACTIVATE_NOW` +
  `FBIOPUT_SCREEN_ORIGIN_GFBG` + `FBIOPUT_SCREEN_SIZE`)返回成功、同一 fd 回读还 echo 请求值,
  但 sysfs(`/sys/class/graphics/fb0`)与 `/proc/umap/gfbg0` 的真实硬件态纹丝不动。
- fb0 是**双缓冲**:`FBIOGET_FSCREENINFO` 的 `smem_len = 33 177 600 = 2 × (7680×2160)`,mmap 覆盖两个显示 buffer。
- **均匀填充正常,结构化内容必现扫描线**:`memset(fbp, 0xFF, smem_len)` → 整屏纯白;但任何结构化内容
  (逐行/逐块不同色)——即便写满每一行、写满两个 buffer——都显示为逐行黑的**扫描线**。说明缩放+双缓冲的
  显示 buffer 的真实扫描布局与线性 mmap 写入不匹配。
- GFBG 文档(`Reference/.../GFBG 开发指南.pdf` §1.2.6、"三种分辨率"一节):图形层是
  **"用户绘制 buffer → 显示 buffer"两级 + 缩放 + 双缓冲/压缩**,结构化内容正确上屏依赖**扩展刷新模式 +
  `FBIO_REFRESH`**;而 `FBIO_REFRESH` 板端实测返回 **EPERM**。
- 结论:userspace 线性 mmap 写入喂不对这个"缩放+双缓冲"的显示 buffer;方案 A 从一开始(最初 LVGL 即横条纹)
  就没真正工作过。

背景记忆:项目记忆 `lvgl-gfbg-fb0-fixed-3840` 记录了同一结论。

## 为什么方案 C 可行

主视频通路 `VI→ISP→VPSS→VO→HDMI` 在 **1024×600 NV21** 下**已验证干净**(`socchina-stream` 服务长期
稳定显示相机画面,无扫描线/花屏)。把 UI 合成进这条通路的帧里,即可 native 1024×600 上屏,无缩放、无
ARGB1555、无双缓冲扫描线。仓库已有 `board/src/menu_render.c` 作为"往 NV21 帧里画 UI"的现成模板。

## 现有代码资产

| 文件 | 作用 | 方案 C 中如何用 |
| --- | --- | --- |
| `board/ui/ui_lvgl.c` | 可移植 LVGL UI(顶栏 + 视频占位区 + 右侧参数侧栏)。**与显示后端无关**。 | **基本不改**,继续用;overlay 语义改为"视频区留空,合成时不覆盖"。 |
| `board/ui/ui_port_board.c` | 板端 port(当前:GFBG/fbdev/evdev/worker)。 | **重写**:去掉 GFBG/fbdev,改为 LVGL 离屏 display + 把渲染结果暴露给显示循环。 |
| `board/ui/lv_conf_board.h` | LVGL 配置(32bpp、LINUX_FBDEV on)。 | 关掉 `LV_USE_LINUX_FBDEV`(或保留但不用);离屏用自定义 flush。 |
| `board/src/menu_render.c` | 把菜单画进 NV21 帧(1024×600,Y/UV 基本图元:`set_pixel`/`fill_rect`/字模)。 | **NV21 写入与 RGB→YUV 的模板**;合成器复用其 Y/UV 布局与色彩常量。 |
| `board/src/main.c` | 显示循环;已 mmap VPSS NV21 帧、有"同步写回路径"(apply→写回 NV21→`display_send_frame`)。 | 合成 hook 加在这里:`display_send_frame` 前把 UI 叠加进帧。 |
| `board/src/display.c` | VO/HDMI @ 1024×600 NV21。 | 不改。 |
| `board/src/touch.c` | evdev 触摸(旧菜单用)。 | LVGL 已用 `lv_evdev`(`/dev/input/event0`,1:1);沿用。 |

## 实现步骤

1. **LVGL 离屏 display**
   - `lv_display_create(1024, 600)`,`lv_display_set_buffers` 指向 malloc 的 ARGB8888 缓冲;
     render mode 用 **FULL**(整屏一次 flush,便于合成器取整帧)。
   - `flush_cb` 不写 fb,而是把渲染缓冲(或其脏区)拷进一块**共享 UI 帧缓冲**(ARGB8888,1024×600),
     用互斥/双缓冲 swap 供显示线程读取,然后 `lv_display_flush_ready`。
   - UI 逻辑分辨率 = 1024×600 → 触摸坐标 1:1(面板即 1024×600);`lv_evdev_create(POINTER, "/dev/input/event0")`
     标定 `(0,0,1023,599)`。
   - 保留 worker 线程跑 `lv_timer_handler` + `ui_lvgl_update`(~5Hz),命令经 `app_ctrl_push`(与 web 同队列)。

2. **合成器(ARGB8888 → NV21 叠加)**
   - 在 `main.c` 显示循环,`display_send_frame` 之前:取最新共享 UI 帧,逐像素把 UI 叠加进 VPSS NV21 帧的
     Y/UV 平面(用帧的真实 `stride`,不是 width)。
   - **Alpha 语义**:UI 像素 `a==0` → 保留视频(不写);`a==255` → 覆盖为 UI 颜色;`0<a<255` → 按 alpha 与
     底层视频像素混合(抗锯齿边缘)。视频占位区在 UI 里 alpha=0 → 视频透出;侧栏/顶栏不透明 → 覆盖。
   - RGB→YUV 用 BT.601 full-range(与 `menu_render.c` 的 YUV 常量/`set_pixel` 一致;NV21 = Y 平面 +
     交织 VU 半高)。
   - **性能**:只有不透明区(侧栏 388×600 + 顶栏)需要写;UI ~5Hz 变化 → 缓存"UI 的 NV21 版本 + alpha 掩码",
     仅 UI 变化时重算,每帧只做掩码混合。确保 30fps 不掉帧(先有限帧冒烟测,再长跑)。

3. **线程/所有权**
   - LVGL worker 渲染 → 共享 UI 帧(带 `dirty`/版本号);显示线程读最新版本合成。
   - 帧数据仍走 VB 物理地址;UI 合成在 CPU 侧对已 mmap 的 Y/UV 虚拟地址写入(`main.c` 已有该 mmap 路径)。

4. **清理方案 A 的遗留(本会话已引入的临时/试验代码)**
   > 当前工作树(未提交)包含上一轮方案 A 的改动与**临时测试代码**,方案 C 落地时应清理:
   - `board/sim/vendor/lvgl/src/drivers/display/fb/lv_linux_fbdev.c`:删除临时栅格/memset 测试块、
     `[lvgl-fb]` 调试 `fprintf`、`scale_mode`/`convert_line_argb8888_to_argb1555`/放大分支
     (方案 C 不经 fbdev,应把此文件**还原为 vendored 原版**:`git checkout` 该文件或重新 `scripts/fetch_lvgl.sh`)。
   - `board/ui/lv_conf_board.h`:还原 `LV_LINUX_FBDEV_FORCE_W/H` 及相关新增。
   - `board/ui/ui_port_board.c`:`gfbg_setup` 整体删除,由离屏+合成 port 取代。
   - 方案 A 中"不强设 32bpp""ARGB1555 转换"逻辑在 C 下不再需要。

## 构建 / 部署 / 运行(**必读:含本轮踩过的坑**)

```sh
# 构建(env 变量取值见项目记忆 hispark-build-env)
export CROSS_COMPILE_ROOT=.../aarch64-mix210-linux
export SS928_SDK_ROOT=.../SS928V100_SDK_V2.0.2.2_MPP_Sample-master
export ISL_LIB_DIR=.../isl-0.19-0/lib
export PATH="$CROSS_COMPILE_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$ISL_LIB_DIR:$LD_LIBRARY_PATH"
cmake -S board -B build-lvgl \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/board/cmake/toolchain-aarch64-mix210-linux.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_SDK=ON -DSS928_SDK_ROOT="$SS928_SDK_ROOT" -DENABLE_LVGL=ON
cmake --build build-lvgl --target socchina_app -j"$(nproc)"   # 产物 build-lvgl/socchina_app (aarch64)
```

板端(SSH 别名 `hispark-direct`,直连以太网):

- **运行必须带 `LD_LIBRARY_PATH=/opt/lib/npu`**,否则缺 `libascendcl.so` 秒退。
- systemd 服务 `socchina-stream` 起的是**纯 stream `socchina_app`(无 `--ui`)**;LVGL UI 在 `ENABLE_LVGL`
  编入 + 显示开启时**自动**起(`main.c` 的 `ui_port_board_start`)。核验时手动跑:

```sh
# 部署(先停旧实例,否则 scp 报 Text file busy)
scp build-lvgl/socchina_app hispark-direct:/root/socchina-2026/socchina_app_lvgl.new
ssh hispark-direct 'systemctl stop socchina-stream; sleep 2;
  LD_LIBRARY_PATH=/opt/lib/npu nohup /root/socchina-2026/socchina_app_lvgl.new \
    --stream --sensor 1 --fps 30 --no-nn --rtsp-port 8556 --rtsp-bind 127.0.0.1 \
    --stream-path internal --ctrl-sock /run/socchina/lvgltest.sock >/root/socchina-2026/lvgl-test.log 2>&1 &'
# 核验后恢复：SIGTERM 干净停 + 恢复服务
ssh hispark-direct 'pkill -TERM -f socchina_app_lvgl.new; sleep 5; systemctl start socchina-stream'
```

**媒体栈纪律(违反会浪费大量时间,本轮已踩)**:

- **停实例只用 `kill -TERM`,绝不用 `kill -9`**。`-9` 不让 app deinit → 残留 VB → 下次
  `ss_mpi_vb_set_conf failed! / sample_comm_sys_init_with_vb_supplement failed`,stream 服务也一起起不来。
- VB 脏了的恢复:`ssh hispark-direct 'cd /opt/ko && ./load_ss928v100 -a'`(rmmod+insmod 全部媒体 ko),
  然后 `systemctl restart socchina-stream`。
- 每次核验前确认无遗留 `socchina_app*` 进程;长跑前先有限帧冒烟。

## 验收标准

- [ ] HDMI 1024×600:视频(左)+ 不透明 UI 面板(顶栏 + 右侧栏),**无扫描线/横条纹/花屏**,尺寸正确、颜色正确。
- [ ] 视频占位区透出相机画面(alpha 合成);顶栏/侧栏卡片不透明覆盖。
- [ ] 触摸可用(`event0`),滑块/预设按钮响应;UI 操作经 `app_ctrl_push` → 控制线程生效(与 web 同队列)。
- [ ] 合成不掉帧:30fps 满帧稳定(有限帧冒烟 + 10 分钟长跑目视无撕裂/闪烁)。
- [ ] host sim(`board/sim`)仍能构建(UI 布局离线迭代;sim 用 SDL,不经合成器)。
- [ ] 方案 A 临时/试验代码已清理,vendored LVGL fbdev 还原原版。

## 关键约束 / 提醒

- 不要在相机/增强链运行时另起厂商 sample(争用 SYS/VB)。
- 合成写入用帧真实 `stride`(非 width);NV21 = Y 平面 + 交织 VU(半高)。
- RGB→YUV 口径与 `menu_render.c` 一致(BT.601 full range),避免与视频色域不一致。
- 不要把一次性 DHCP 地址写死;板端访问用别名 `hispark-direct`(直连)/受管网络。
- 文档/提交遵循仓库 `AGENTS.md`:不写死机器绝对路径、不泄身份/网络信息、仓库内链接。

## 参考

- `docs/prompts/agent-prompt-lvgl-board-bringup.md` —— 方案 A 上板收尾(已被本文替代;其"白线"归因不准,以本文实测为准)。
- `docs/architecture.md` —— 数据通路与 VO/HDMI 接口。
- `board/README.md` —— 板端代码组织与构建。
- 项目记忆 `lvgl-gfbg-fb0-fixed-3840` —— GFBG fb0 固定 3840×2160 ARGB1555 的实测结论。
