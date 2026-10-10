# CHG-002 lint 门禁覆盖面扩面（后端 / 能力宏 + Windows 口径 + 增量口径对齐）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-002 |
| 提出日期 | 2026-10-10 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.QUALITY.CORE.MEMORY-SAFETY.001 |
| 影响面 | CI 的三道 clang-tidy 作业（`.github/workflows/ci.yml` 作业 8 `lint` / 8w `lint-wasm` / 8i `lint-incremental`）的 configure 口径与矩阵形状；新增 Windows 口径 lint 作业；`tools/check/lint_db.py` 的生成物排除与 C 语言 TU 处理；`cmake/AuroraBackends.cmake` 的 Wayland 协议生成目标；`.clang-tidy` 的命名类 `CheckOptions` 与若干就地 `NOLINT`；按处置阶梯修改的运行时代码（`media/audio_alsa.cpp`、`storage/sqlite_backend.cpp`、`window/detail/atspi_bridge.cpp`、`window/detail/atspi_protocol.{h,cpp}`、`inspector/inspector_server.cpp`、`window/x11_surface.cpp`、`window/wayland_surface.cpp`、`tools/verify/*`），其中两处带**可观察行为修正**（`SqliteBackend::close()` 之后 `is_open()` 如实为 false、AT-SPI `Component.GrabFocus` 的「尽力而为受理」口径落成类型化单点），各配单测（`utest_sqlite_backend.close_releases_connection_without_virtual_call_in_destructor`、`utest_atspi_protocol.model_grab_focus_is_best_effort`）。公共 API 仅两处签名变化，且同源于 `performance-unnecessary-value-param`：`SqliteBackend` 构造形参按值 → `const SqliteOptions &`（`storage/sqlite_backend.h`）与 `Storage::create(SqliteOptions)` 重载 → `Storage::create(const SqliteOptions &)`（`storage/storage.h`），后者是前者落地后的调用侧连带（按值收参再 `std::move` 反而多一次整结构体拷贝）；调用方源码零改动、ABI 变化记入 CHANGELOG Breaking；不改信号/属性键、不改分层边界与后端支持矩阵本身 |

## 动机

clang-tidy 门禁当前的三道作业都按「最小后端」口径 configure：`AURORA_BACKEND_X11` / `AURORA_BACKEND_WAYLAND` / `AURORA_BACKEND_GLFW` / `AURORA_ENABLE_AUDIO`（含 `AURORA_ENABLE_AUDIO_ALSA`）/ `AURORA_ENABLE_STORAGE_SQLITE` / `AURORA_ENABLE_IMAGE_*` / `AURORA_BUILD_INSPECTOR_SERVER` / `AURORA_BUILD_VERIFY_TOOLS` 全部留在默认 OFF。后果是这些宏门控的代码在门禁眼里**不是「干净」，而是「不存在」**：TU 能编译成功，函数体却是空的，于是 `unique_findings = 0` 被读成「查过了」。`broken_tus` 只兜「编译失败」这一种失能，兜不住这种「编译成功但体内无代码」的塌陷。

在本机把 Linux 可开的宏全部打开、跑同一套 runner（最高危条目另用 CI 钉的 clang-tidy 22.1.2 单独跑相应 TU 逐字复验，本机默认是 21.1.8）后，量化的盲区如下：

| 口径 | 非 third_party TU | 去重后项目内告警 | 门禁当前口径可见 |
|:---|:---|:---|:---|
| 默认（等于 CI 作业 8 的口径） | 558（全量复扫实测） | **0**（复扫实测，与逐条核对结论一致） | — |
| 宏全开（native） | 569（另有 3 个 Wayland 生成 `.c` TU 编译失败） | 292；崩溃 / UB 一档处置完按同配置复扫降到 **235**（15 个 check 整项归零，无一项变多） | 5（仅 DEBUG 极性差集） |
| 宏全开（浏览器口径） | 471 | 60，其中 59 条与 native 全开口径逐条重合 | — |

盲区规模：**30 个文件、8,722 行**不参与门禁分析，其中 8,102 行在本机可开、620 行是 Windows 专属（`media/audio_wasapi.cpp` 一处即 617 行，文件全长 632 行）。按归属分：库侧 215 条、`tools/verify/*` 真机探针 52 条、测试 20 条。

值得按缺陷看待的是其中的**崩溃 / UB 形态 45 处站点**（口径并集，可复算：按 `clang-analyzer-core.CallAndMessage`、`clang-analyzer-optin.cplusplus.VirtualCall`、`clang-analyzer-unix.StdCLibraryFunctions`、`clang-analyzer-optin.performance.Padding`、`bugprone-bitwise-pointer-cast`、`bugprone-multi-level-implicit-pointer-conversion`、`bugprone-exception-escape`、`bugprone-narrowing-conversions` / `cppcoreguidelines-narrowing-conversions`、`cppcoreguidelines-pro-type-{const-cast,union-access,static-cast-downcast,cstyle-cast}`、`performance-no-int-to-ptr` 这组 check 名过滤后按 `(file, line, check)` 去重——宏全开 native 遍 50 行 / 41 处，浏览器口径独立多报 1 处，GLFW 后端闭包遍独立多报 3 处；反极性两遍（DEBUG OFF 与四项缓存 / SIMD 全关）报出的站点全部落在前两者之内、无新增）。逐处归属：`audio_alsa.cpp` 的 4 处 `dlopen` 绑定函数指针未判空即调用（`Called function pointer is null`，已就地核对源码确认非假告警）、`~SqliteBackend()` 内 `(void)close();` 绕过虚派发（1 处）、`atspi_bridge.cpp` 的 `memcpy` 指针转换与 `void **` → `const void *` 隐式多级转换及 1 处去 const（4 处）、`inspector_server.cpp` 与 `itest_inspector_robustness.cpp` 的 `ssize_t` → `int` 收包长度窄化（4 处，扫描初稿漏计测试侧 1 处）、`x11_surface.cpp` DPI/缩放换算链上的 `int` → `float` 窄化（6 处）与联合体活跃成员读取（2 处）、9 个真机探针 `main()` 可能抛出即 `terminate`（宏全开遍 7 处 + GLFW 遍 2 处）、`atspi_live_probe.cpp` 的 2 处无保护下行转型、浏览器口径独立报出的 `utest_inspector_server.cpp` 里 `connect` 首参为 `-1`（socket 创建失败未判返回值，1 处）、`sqlite_backend.cpp` 的 `SQLITE_TRANSIENT` 逐处 C 风格整型→指针转换（8 处，每处两条 check）、探针回调与 `glfw_surface.cpp` 的 `native_handle()` 里 int→指针（3 处，其中 GLFW 那处只在 GLFW 口径报出）。另有 1 处 `WaylandSurface::Impl` 多余填充 78 字节影响每窗口的缓存友好性。

**开发分支的增量口径带同一个缺陷**：作业 8i 的三套编译库同样不开后端宏，于是「改了 `x11_surface.cpp`」这类推送在增量门禁里选出一个 TU、跑完、报 0 告警——覆盖面看起来到位，实际是空壳。扩面必须同步落进 8i，否则分支侧的假绿比全量侧更常发生。

成本侧已实测，结论与直觉相反（这也是「lint 非常耗时」这一顾虑需要数据校正的地方）：

| 跑法 | TU | 并发 | 墙钟 | 折算核时单价 |
|:---|:---|:---|:---|:---|
| 默认口径单遍（既有记录，见 `codespec/BUILD_OPTIONS.md` §4.5） | 498 | 4 vCPU | 5,172 s | 41.5 core-s/TU |
| 宏全开单遍（本机实测） | 569 | 16 | 1,535 s | 43.2 core-s/TU |

宏全开相对默认只多 3 个 TU、单价高 4%——**因为它填的是同一批 TU 内部的空区域，而不是新增翻译单元**。把现有两遍 native 的口径打开，每片墙钟从约 22 分钟涨到约 26 分钟量级，不需要新增作业。

真正贵的是**反极性补扫**（把架构优化开关、`AURORA_ENABLE_TEST_HOOKS` / `PROFILING` / `TRACING` 关到另一极）：按 `select_lint_tus.py --changed <宏命中文件>` 实测的 TU 闭包，三个架构优化开关 OFF 牵动 326 个 TU，TEST_HOOKS/PROFILING/TRACING OFF 牵动 469 个，并集 490 个（分母 569）。按 43.2 core-s/TU、runner 4 vCPU 折墙钟：分别约 59 / 85 / 88 分钟（单 job 不切片）。`toggles` 作业的形状是「一个 matrix 项 = 一个 runner，串行 configure→build→ctest」，把 lint 塞进去省下的只是一次 configure（约 1–3 分钟），省不掉那约一小时核时；后果是 `toggles` 里那一格从「构建 + 测试」变成全流程最长的作业，而且它天生无分片，`timeout-minutes` 只能往 120 以上抬。因此本提案把反极性覆盖落成 **lint 矩阵的第三个 pass 行 × 既有 4 片分片**（每片约 22 分钟、整门墙钟基本不动，代价是 +4 个 runner 的核时），并把「塞进 `toggles`」保留为备选（见「变更内容」相应条目）。

最后一条动机关于节奏：`codespec/BUILD_OPTIONS.md` §4.5 已写下「一个永远红的必过位不等于门禁」与「日后在此转红按同一阶梯清理，不得再次回退 report-only」。287 条存量此刻还挂着，所以**扩面与转红必须等存量清零**，否则第一遍扩面口径的 PR 就撞上 287 条与自身无关的红灯。本提案据此把交付切成两批：本批只落方案与存量处置清单，`ci.yml` 不动；存量清零后一次性把扩面 + 必过切上去。

## 变更内容

1. **native `lint`（作业 8）configure 扩面**，把默认 OFF 的后端 / 能力开关显式打开，并补上其 apt 依赖（与作业 2 `backends` 的 linux 项同源，ALSA 因 `cmake/AuroraBackends.cmake` 走 `dlopen("libasound.so.2")` 零构建依赖、不需 dev 包）：

   ```yaml
   - name: Install desktop backend dev dependencies
     run: sudo apt-get update -y && sudo apt-get install -y
          libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev
          libgl1-mesa-dev libwayland-dev wayland-protocols libxkbcommon-dev

   - name: Configure (debug ${{ matrix.pass.debug }}, export compile_commands.json)
     run: cmake -S . -B build-lint -G Ninja
          -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DAURORA_ENABLE_CLANG_TIDY=ON
          -DAURORA_ENABLE_DEBUG=${{ matrix.pass.debug }}
          -DAURORA_LINT_SHARD=${{ matrix.shard }}/${{ matrix.total }}
          -DAURORA_BACKEND_X11=ON -DAURORA_BACKEND_WAYLAND=ON -DAURORA_BACKEND_GLFW=ON
          -DAURORA_ENABLE_AUDIO=ON
          -DAURORA_ENABLE_STORAGE_SQLITE=ON
          -DAURORA_ENABLE_IMAGE_JPEG=ON -DAURORA_ENABLE_IMAGE_WEBP=ON -DAURORA_ENABLE_IMAGE_PNG=ON
          -DAURORA_BUILD_INSPECTOR_SERVER=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
   ```

   `AURORA_BUILD_TESTS` / `AURORA_BUILD_E2E` / `AURORA_BUILD_DEMOS` 已是默认 ON，口径不变；SQLite / libjpeg-turbo / libwebp / wuffs / GLFW 均在 `third_party/` 已 vendored，不新增外部依赖。

2. **Wayland 生成物前置**：`add_custom_target(lint ...)`（`cmake/AuroraLint.cmake`）没有 `DEPENDS`，而 `window/wayland_surface.cpp` include 的 `xdg-shell-client-protocol.h` 等只在 configure 后由 `wayland-scanner` 生成。为扩面作业新增一个轻量聚合目标（暂名 `wayland-gen`，在 `cmake/AuroraBackends.cmake` 里把现有 `add_custom_command` 的产物列进 `add_custom_target`），lint 作业在 configure 后、lint 前跑 `cmake --build build-lint --target wayland-gen`（秒级）。不采用「先 `--target aurora`」的写法：那会把整个库编一遍，是扩面成本的主要来源，而 tidy 只需要那三个头。

3. **生成物排除与 C TU 处理**（与条目 2 必须同批落地，否则新出现的三个 `.c` TU 直接进 `broken_tus` 把门禁推红）：`tools/check/lint_db.py` 的 `DEFAULT_EXCLUDE` 增补构建目录生成物（覆盖 `<build>/*-gen/`、`<build>/gen/` 形态，本次实测为 211 条 `xdg-shell-protocol.c` / `text-input-unstable-v3-protocol.c` / `xdg-decoration-unstable-v1-protocol.c` 噪声 + 3 个 `broken_tus`），并在 `run_clang_tidy.py` 的汇总 JSON 里自述 `generated_skipped` 计数——排除要留痕，不能静默。同时按语言分流 `.clang-tidy` 的 `ExtraArgs`：对 C TU 不再附加 `-std=c++20`（当前症状是 `invalid argument '-std=c++20' not allowed with 'C'`，即这些 TU 零覆盖）。命名规范类检查在生成 C 协议文件上本就不适用，排除不构成盲点（判据见 §4.5「整文件排除是永久盲点」的口径核对：这些文件由上游 XML 生成、不受本仓风格约束）。

4. **`lint-wasm`（作业 8w）configure 扩面**：补 `-DAURORA_ENABLE_AUDIO=ON`（浏览器口径的 Web Audio 后端 `media/audio_webaudio.cpp` 只在 wasm 编译库里出现，本次该遍零告警，属「跑到了且干净」）、`-DAURORA_ENABLE_STORAGE_SQLITE=ON` 与三个 `AURORA_ENABLE_IMAGE_*=ON`、`-DAURORA_BUILD_INSPECTOR_SERVER=ON`。该作业仍只 configure 不构建，扩面后 TU 单价与墙钟沿用其现有分片预算。

5. **`lint-incremental`（作业 8i）三套编译库同步扩面，选集口径保持不变**：native DEBUG=OFF / DEBUG=ON 两套与 wasm 一套的 configure 都补上条目 1 / 4 的同一组 `-D`，加同一条 apt 依赖步骤与 `wayland-gen` 前置。**继续用 `select_lint_tus.py --changed-only`**——分支推送只 lint「本次改动涉及的文件本身」，不展开 include 闭包；闭包级覆盖仍只属于全量口径（master 推送与面向 master 的 PR）。触发边界不动：全量矩阵的 `if` 仍限定 `pull_request` / `schedule` / `workflow_dispatch` / `refs/heads{master,main}`，8i 仍只在非 master 分支推送时跑。

6. **收口作业 8 / 8w 里的失效增量分支**：这两处各有一个 `Decide lint scope` + `Select affected translation units` 的分支，走 `--changed`（展开闭包），但按各自的 `if` 与实际 scope 判定，能进入该作业的事件一律被判为全量——即那段闭包选集在生产里不可达。把它改成 `--changed-only`（与「不为闭包扩大范围」的策略一致，防止日后有人放开触发条件时静默变成闭包扫描），或整段删除并让 8i 作为唯一增量入口；本批取**改为 `--changed-only` 并在注释里标明唯一增量入口是 8i**，删除会连带牵动 `Annotate findings with shard scope` 步骤的形状。

7. **新增 `lint-windows` 作业，按「Windows 相关文件」定向选集而非全量编译库**：`runs-on: windows-latest`，走 MSYS2 mingw-w64 的 gcc/g++ 编译数据库（与作业 1 的 `windows-mingw` 项同款工具链，clang-tidy 消费 gcc 形态 `compile_commands.json` 的路径已在 Linux 侧验证），configure 加 `-DAURORA_BACKEND_WIN32=ON -DAURORA_BACKEND_D3D11=ON -DAURORA_ENABLE_AUDIO=ON -DAURORA_ENABLE_AUDIO_WASAPI=ON -DAURORA_BUILD_VERIFY_TOOLS=ON`，clang-tidy 用 LLVM 22（与 Linux 侧同一钉版本，避免检查集漂移）。

   选集口径（实测规模见下）：CI 现场生成清单，不落硬编码文件表——

   ```bash
   git -c core.quotepath=off grep -lE \
     'AURORA_PLATFORM_WINDOWS|AURORA_BACKEND_WIN32|AURORA_ENABLE_AUDIO_WASAPI|AURORA_BACKEND_D3D11|AURORA_UIA|_WIN32|WINAPI|HWND|HMODULE|windows\.h|winnt\.h|shell32' \
     -- src tools tests examples include | sort > win-tus.txt
   python3 tools/check/select_lint_tus.py --build-dir build-lint-win --changed win-tus.txt \
     --changed-only --out-dir build-lint-win/sel --shards 1
   ```

   本仓当前命中 **79 个 `.cpp` / 32,208 行**（库 25、工具 14、测试 36、示例 4）。其中 **11 个是 `tools/verify/{win32_*,wasapi_audio,x11_wgpu,glfw_dpi}_live_probe.cpp`，合计 4,916 行，在 Linux / wasm 任何口径的编译库里根本不存在**（体内无 `#ifdef`、靠 CMake 选源，只有 Windows 编译库能给这些 TU）；其余 68 个文件（27,292 行）在 Linux 库里是 TU、但 Windows 分支被宏关成死代码。按 43.2 core-s/TU 估：79 TU ≈ 57 分钟核时，Windows runner 4 核约 14 分钟墙钟 + configure 约 5 分钟；对照「Windows 全量编译库约 570 TU」的 ≈ 100 分钟墙钟，**定向选集把它降到约 1/7**——这是采取选集而非全量的直接理由。

   头文件不单独成 TU，覆盖靠包含者：19 个含 Windows 门控区的头（`src/aurora/window/detail/win32_ua.h` 344 行、`include/aurora/window/d3d11_surface.h` 257 行、`win32_surface.h` 292 行等）的直接包含者全部落在这 79 个文件里，故定向选集不牺牲头侧覆盖（核对方法写进「验收判据」）。

   兜底与触发：定向选集跑在作业 8 的同一触发判据上（master 推送、面向 master 的 PR、每周定时、手动）；额外让**每周定时**多跑一遍 Windows 全量编译库——未命中 token 的共享代码也可能经 `aurora/aurora.h` 伞头间接看到 `windows.h`，这类间接面用周度全量收口，不值得每个 PR 付 100 分钟。Windows 侧不设 `pass`×`shard` 矩阵（79 TU 一片即可）；Windows-only 代码里的 DEBUG 门控区同样在这一遍里被解析，DEBUG 两极的系统性覆盖仍由 Linux 的 pass 矩阵承担。

   **诚实标注本条在本机不可验证**：Windows SDK 头不在 Linux 主机上，`d3d11.h` 在 mingw-w64 下的可解析性也未经实测；若 mingw 腿在 D3D11 头处编译失败，退化为「mingw 腿跑 win32 + WASAPI 子集，D3D11 单列一条 clang-cl 腿」，并在 §4.5 记录该腿的实际覆盖面与选集命中数。

8. **反极性覆盖落成 lint 矩阵的第三个 pass 行**（推荐形状，量化对比见「动机」）：`pass` 维度增加一项 `label: optimizations-off`，`cmake-flags` 取 `-DAURORA_ENABLE_LAYOUT_CACHE=OFF -DAURORA_ENABLE_OCCLUSION_CULLING=OFF -DAURORA_ENABLE_DISPLAY_LIST=OFF -DAURORA_ENABLE_SIMD=OFF -DAURORA_ENABLE_TEST_HOOKS=OFF`，沿用同一份 `shard: [0,1,2,3]` 与 `total: [4]`（改片数仍需同步两处、后果不对称的既有约束不变）。矩阵从 2×4=8 作业变 3×4=12 native 作业（整门绿 = 8 + 12 + 若干），每片墙钟约 22 分钟，`timeout-minutes: 40` 余量不变。**备选**：把同一组 `-D` 塞进 `toggles` 作业的 matrix 项并追加 lint 步骤——省一次 configure，但那约 88 分钟核时全压在一个无分片的 runner 上，且 `toggles` 现有 6 项里已有 2 项 `run-tests: false` 的构建-only 口径，混进去会让「构建绿」与「lint 绿」两类失败原因纠缠。本条的最终形状在提案评审时定，两种都能满足覆盖面目标，差别在墙钟与作业可读性。

9. **覆盖面自述**：`run_clang_tidy.py` 的汇总 JSON 增补 `enabled_options` 字段（列出本遍显式打开的后端 / 能力开关），使「跑到了但没活儿」与「跑过且干净」在产物里可区分。分片设计已经防缺片（`shard` / `tu_total`），但没防「宏没开」——本次的塌陷正是这么发生的。

10. **存量处置（先清后切的「清」，与扩面解耦、可并行推进）**：
    - 崩溃 / UB 形态 45 处逐条处置，不当 lint 噪声处理；**真改代码的**与**只在 C ABI 边界 / 仓内既定口径下就地豁免的**必须分清：ALSA 四处符号绑定后判空再调用（`audio_alsa.cpp`）、`~SqliteBackend()` 不再调虚函数（析构只做资源释放终态，`close()` 语义交调用方）、`atspi_bridge.cpp` 的 `dlsym` 落值改显式 `memcpy` 到函数指针类型别名并就地写明 POSIX ABI 理由、「id → 可写控件」的去 const 收口到 `AtspiModel::widget_of` 单点（桥侧不再自行 `const_cast`，理由是该快照指针的常量性只来自 `flatten_snapshot` 的只读遍历签名）、`inspector_server.cpp` 与 `itest_inspector_robustness.cpp` 的收包长度改用 `std::ptrdiff_t` 承接后再显式折算、`x11_surface.cpp` 的 DPI/缩放换算链补显式 `static_cast<float>` 与 2²⁴ / `scale ∈ [0.5, 4.0]` 边界注释、XIM 联合体读取就地写明判别式配对、`sqlite_backend.cpp` 的 `SQLITE_TRANSIENT` 收成单点命名常量转换（8 处→1 处，两条 check 就地豁免）、`utest_inspector_server.cpp` 判 `socket()` 返回值、`WaylandSurface::Impl` 的 78 字节填充按子系统分组的既有排布保留并写明与 `Window` 同口径不作优化目标（每窗口一份实例，收益小于一次分配的零头）、`atspi_live_probe.cpp` 的 2 处下行转型用区间豁免包住并注 dlopen 类型来源。
    - **9 个探针 `main()` 不包 `try/catch`**（此处修正本提案初稿的处置建议）：本仓真机探针口径是「入口不吞异常」——未捕获异常以非零退出码 / `terminate` 呈现失败，捕获后返回 0 反而把真机排查现场压成成功，与 `tools/verify/glfw_cursor_live_probe.cpp` 的既有写法、`examples/` 各 demo 入口一致，故这一类走入口就地 `NOLINTNEXTLINE(bugprone-exception-escape)` + 理由；三个尚未被任何口径扫到的 wgpu 探针入口（`wayland_wgpu` / `x11_wgpu` / `win32_wgpu`）按同一写法统一标注，属预防性一致而非观测告警。`glfw_surface.cpp` 的 `native_handle()` Linux 腿把「整数 XID → `void *`」的折算并成单行，两条指针形态 check 同行名指。
    - 命名规范 80 条经逐条核对源码后**分两类处置，不是一律豁免**：真正词汇绑定的只有 `inspector_server.cpp` 的 7 条 Winsock→POSIX 兼容层声明（`INVALID_SOCKET` / `SOCKET_ERROR` / `SD_BOTH` 三个全局常量、`WSAStartup` / `WSACleanup` / `WSAGetLastError` 三个函数、`MAKEWORD` 一个宏，见该文件顶部的垫片段）——这些名字就是读者检索 Winsock 文档的锚点，改名即切断可追溯性，走区间 `NOLINTBEGIN/NOLINTEND` + 理由（受 `check_nolint_layout` 的排版纪律约束：指令必须落在相邻物理行、`(check list)` 必须同行闭合，历史教训是跨行写法让豁免退化成「全 check 免检」、修好后一次暴露 795 条）。其余 73 条是**本仓命名约定的偏离而非上游绑定**：`atspi_bridge.cpp` 的 44 条（`Fn_*` 5 个 dlopen 符号类型别名 + `mt_method_call` / `ty_bool` 一类 39 个 libdbus 常量简写，其上游 `DBus*` 原名已在相邻 `///<` 注释里逐条保留）、`wayland_surface.cpp` 的 13 条（`BUFFER_LISTENER` 等缺 `AURORA_` 前缀的 `wl_listener` 对象、局部常量 `H`）、`x11_modifiers.h` 的 7 条（`kShift` / `kMod1` 等，X11 原名 `Mod1` 在注释里）、探针 5 条、其余 4 条——这些按 `codespec/CODING_STANDARDS.md` §2 命名约定改名到位（机械、可批量），保住门禁对全部文件的命名效力。**这一条修正了扫描报告的初稿建议**（初稿按「外部 C API 桥一律走配置层豁免」论断，核对后仅 Winsock 段成立）：`.clang-tidy` 的 `GlobalConstantIgnoredRegexp` 是仓库级整串匹配，为这 73 条放宽会把 `mt_.*` / `k.*` 形态的命名逃逸开放给整个代码库，代价大于收益。
    - 风格面约 160 条走 `lint-fix` 目标批量应用后人工审 diff（`.clang-tidy` 已因 clang-tidy 22 的错误 fix-it 关掉 `modernize-use-ranges` 与 `readability-convert-member-functions-to-static`）。
    - 逐条清单与文件分布已按 `(file, line, check)` 去重成表，随本批在 PR 描述里附出（构建目录里的临时报告不可作仓库引用，见「回写落点」的口径说明）。

11. **切换批次**：上述存量在三道（扩面后含 Windows 共四道）口径下 `unique_findings` 归零，才把条目 1–9 的 `ci.yml` 改动一次性合入并转必过。本批不动 `ci.yml`。

## 验收判据

1. 扩面口径的 native 编译库中，`window/x11_surface.cpp`、`window/wayland_surface.cpp`、`window/detail/atspi_bridge.cpp`、`media/audio_alsa.cpp`、`storage/sqlite_backend.cpp`、`inspector/inspector_server.cpp` 与 `tools/verify/*_live_probe.cpp` 各自被分析到时**有实际代码贡献**：判据是这些 TU 在 `broken_tus` 之外、且构造一次「函数体内植入一处已知形态告警」（例如空函数指针调用）的试点 PR 时，全量与增量两道门禁都报出该条——而不是只比对 TU 计数。
2. 门禁产物自述覆盖面：每片 `lint-findings.json` 的 `enabled_options` 列出 X11 / WAYLAND / GLFW / AUDIO(ALSA) / STORAGE_SQLITE / IMAGE_{JPEG,WEBP,PNG} / BUILD_INSPECTOR_SERVER 为 ON，`generated_skipped` ≥ 3 且 `broken_tus` 为空（Wayland 生成 `.c` 不再以 `-std=c++20` 失败）。
3. 墙钟不失控：扩面后 native 单片 ≤ 30 分钟（现约 22 分钟，43.2 core-s/TU × 569 TU ÷ 4 vCPU ÷ 4 片的算术值约 26 分钟，余量给编译失败重试与 apt）；反极性 pass 行单片 ≤ 30 分钟；`lint-incremental` 单 job ≤ 90 分钟（现值不变）。
4. 触发边界逐条核对（用 PR 与分支推送各造一次验证，不读注释）：master 推送与面向 master 的 PR → 全量矩阵（含新增 pass 行与 `lint-windows`）；非 master 分支推送 → 只有 `lint-incremental`（及其 Windows 对应物，若为 8i 加 Windows 腿）跑，且选集 = 改动文件里本身是 TU 的那些，**不含 include 闭包**（用一个「只改公共头」的推送验证：该 job 选中的 TU 数应为 0 并留下 idle 记录，而不是重跑所有包含者）。
5. 存量清零口径：宏全开 native 遍的项目内告警数 = 0（处置前 292 条，其中盲区 287 条），其中崩溃 / UB 形态 45 处（「动机」给出可复算的 check 名集合与跨口径并集）逐条给出修复提交或就地豁免 + 理由——**这一档已达成**：宏全开 native 遍按同配置全量复扫（569 TU），该 check 集合 **50 行 / 41 处 → 0 行 / 0 处**，项目内告警随之 **292 → 235**（17 类 check 只降不升，其中 15 类整项归零，附带 `cppcoreguidelines-pro-type-reinterpret-cast` 30→27、`readability-identifier-naming` 80→79）；19 个受影响 TU 的定向清单另跑在 GLFW（按其 5 TU 清单，3 → 0 行）、DEBUG 反极性、优化开关反极性三道口径上，第一档在每一道里都是 0；浏览器口径对 `utest_inspector_server.cpp` 复扫 **0 条**。各遍均未引入任何基线里没有的 check 项。**第二档（命名）亦已达成**：台账按「文件 × 标识符」去重共 85 处站点（宏全开 native 遍 72 处 + GLFW 口径独有 13 处）落地为 Winsock 7 处区间豁免（处置形态即条目 10 所述）+ 78 处改名（ATSPI 桥 `Fn_*` / libdbus 常量 / `L` → `dbus`、Wayland listener 补 `AURORA_` 前缀并修 `RETISTERY_` 拼写、X11 掩码 `k*` → `AURORA_*_MASK`、ALSA `strerror_` → `str_error`、探针与测试常量对齐）；`readability-identifier-naming` 单项定向复扫在宏全开 19-TU 清单与 GLFW 3-TU 清单上均为 **0 条**，宏全开全量复扫（569 TU，产物 `lint-findings-max-tier2.json`）该 check 同样归零；`ctest --preset ninja-test` 全绿（含 `check_nolint_layout`、`check_platform_macros`、`check_codespec_xref`、`check_change_proposals`）。**第三档（风格面）亦已达成**：机械 fix 批（20 TU / 23 个 check 的 fix-it，就地应用后逐 diff 人工审）+ 判断型 10 类逐条处置（真改 / 就地豁免 + 理由）后，宏全开 native 全量复扫（569 TU，产物 `lint-findings-max-tier3.json`）**项目侧 0 条**（仅剩 Wayland 生成物 211，3 broken TU）；GLFW 口径补齐 Wayland 生成物后的完整编译库终扫（572 TU）暴露 10 条第三档改码的连带回落（删除特-member 挪 public 的连锁 4、SQLite 选项改 const 引用的调用侧 3、`make-member-function-const` 2、探针全局 1），逐条修毕后 6 TU 定向复扫归零（`lint-findings-glfw-fixcheck.json`）。本条前半后半的「= 0」就此整体达成。口径注记：wgpu 后端闭包遍（rustup 装好后本机可开，12 TU）另扫出 214 条独立口径存量（主体是 `wgpu_rhi.cpp` FFI 层），三道门禁口径均不开启该宏，**不计入本判据**，作为独立台账在把 `AURORA_BACKEND_GPU_WGPU` 加进 lint 矩阵之前清完。
6. 极性/闭包补扫的覆盖增量可核对：反极性 pass 行跑出的 TU 数 ≈ 490（并集算术值），且该 pass 在 `AURORA_ENABLE_SIMD=OFF` 下确实解析到标量黄金路径代码（以条目 1 的植入法验证一处）。
7. Windows 口径的诚实边界与选集有效性：`lint-windows` 的 `selection.json` 自述 `selected_count` ≥ 79（本仓现值），且这 11 个 Windows 专属探针 TU（`win32_ua_live_probe.cpp` 795 行等）确实被解析到（以「植入法」验证一处）；19 个含 Windows 门控区的头文件各自至少有一个直接包含者入选，该包含关系由脚本核对而非人工断言。每周定时的全量 Windows 腿自述其 TU 数与告警数，作为「定向选集没有漏面」的周期对照。若 mingw 腿无法解析 D3D11 头，验收改为「win32 + WASAPI 覆盖达成，D3D11 单列腿状态标计划/待建」，并把选集命中数按实际可达子集下调、在文档里写明差额，不得把该口径称已覆盖。
8. 定向选集的维护性：Windows 文件清单在 CI 现场由 token 检索生成，不含硬编码文件表；新增一个含 `AURORA_PLATFORM_WINDOWS` 分支的源文件后，无需改作业即被纳入（用一个试点 PR 验证）。选集为空或命中数明显低于上一遍（例如骤降）时作业以非零码拒跑并留 warning，绝不退化成「0 告警 = 干净」。

## 回写落点

- `codespec/BUILD_OPTIONS.md` §4.5（权威 lint 口径文档：三道→四道作业的 configure `-D` 全集、矩阵形状与片数、墙钟单价表、`wayland-gen` 前置、生成物排除与 `generated_skipped` / `enabled_options` 自述字段、反极性覆盖的落点决定、`lint-windows` 的实际覆盖面）
- `codespec/BUILD_OPTIONS.md` §4 开头的开关速查表（`AURORA_ENABLE_CLANG_TIDY` / `AURORA_LINT_SHARD` 两行）与 §3.2、§4.6 的对应条目：补一句「CI 的 lint 口径显式打开这些后端 / 能力开关，最小后端口径不构成静态分析覆盖」，避免文档里复制即用的命令只覆盖默认后端
- `codespec/ARCHITECTURE.md` §14.4（CI 执行层作业清单：新增 `lint-windows`、lint 矩阵 pass 行数变化）与 §14.6（按改动类型的门禁矩阵：`window/`、`media/`、`storage/`、`inspector/`、`tools/verify/` 的必跑口径从「lint」细化为「扩面后的 lint」）
- `codespec/CODING_STANDARDS.md` §5.2（`NOLINT` 排版纪律处补「上游词汇绑定」这一类豁免的写法范例，与 Winsock 兼容层的区间豁免实例互指）
- `codespec/specification/08-tooling.md`（真机探针与 inspector server 的门禁可见性：这些工具此前只被 CI 的构建作业编译、不被静态分析）
- `codespec/specification/06-app-platform.md` §6.4（AT-SPI 桥：`Component.GrabFocus` 的「尽力而为受理」口径、`AtspiModel::grab_focus` / `widget_of` 的解引用与去 const 单点约束）
- `codespec/specification/03-layout-render.md`（音频后端一节：ALSA 逐符号判空的绑定契约——`dlopen` 成功但个别符号缺失时，`drop`/`close`/`recover` 这类不经 `start()` 前置校验的收尾路径同样不得空指针调用）
- `codespec/changes/widened-lint-coverage/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）。两条口径说明：其一，CI 门禁口径变更属交付纪律变化，按 `codespec/CODING_STANDARDS.md` §7 的版本与变更管理约定在根变更记录里落一条（根数据文件不作为本表的目标路径，见本仓文档落点规则）；其二，本次扫描的逐条告警明细与「宏 → 盲区行数」矩阵产在本机临时构建目录，构建目录不入版控、不可作仓库引用，其结论已以数字形式内联进本提案；扩面落地时若需长期留存明细，改写进 `codespec/` 正文而非引用构建产物
