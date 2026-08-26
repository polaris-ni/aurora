# BUILD_OPTIONS_ENABLE

> 本文件由 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) 划分而出（AURORA_ENABLE_* 插桩 / 分析 / 能力开关）。
> 返回主线见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**本文包含章节：**
- [3. `AURORA_ENABLE_*` —— 插桩/分析/能力开关](#3-aurora_enable_--插桩分析能力开关)

## 3. `AURORA_ENABLE_*` —— 插桩/分析/能力开关

不影响库功能，只改工具链参数，用于开发期质量保障。本组除插桩/分析外，亦含构建加速（`LLD` / `CCACHE`）与内部能力（`SIMD` / `DEBUG`）等开关，均按 `AURORA_ENABLE_*` 命名组归类。

| 选项                      | 默认值 | 含义                                                                                                                                                                                                                           | 注入内容                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
|---------------------------|--------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `AURORA_ENABLE_COVERAGE`  | `OFF`  | 行覆盖率（终端摘要，不生成 HTML；按编译器分流）                                                                                                                                                                                | GCC：`--coverage -O0 -g`（gcov）；Clang：`-fprofile-instr-generate -fcoverage-mapping -O0 -g`（LLVM 原生 source-based）。均清除默认 `-O3/-Os/-DNDEBUG`、关闭 PCH，提供 `coverage` custom target                                                                                                                                                                                                                                                               |
| `AURORA_ENABLE_ASAN`      | `OFF`  | AddressSanitizer + UndefinedBehaviorSanitizer                                                                                                                                                                                  | 对所有目标注入 `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0`；仅 GNU/Clang 生效                                                                                                                                                                                                                                                                                                                                                               |
| `AURORA_ENABLE_PROFILING` | `AUTO` | 渲染性能插桩（作用域计时 + 渲染计数器）                                                                                                                                                                                        | 三态：`AUTO`（Debug/RelWithDebInfo 注入 `AURORA_ENABLE_PROFILING`，Release/MinSizeRel 不注入）/ `ON`（全配置注入，PUBLIC 传播 + 安装导出）/ `OFF`（任何配置都不注入）                                                                                                                                                                                                                                                                                         |
| `AURORA_ENABLE_TRACING`   | `OFF`  | Chrome Trace Event 时间线落盘                                                                                                                                                                                                  | 注入 `AURORA_ENABLE_TRACING`（PUBLIC 传播 + 安装导出）， **并强制** 打开 `AURORA_ENABLE_PROFILING`（tracing 依赖 profiling 的 zone 数据）                                                                                                                                                                                                                                                                                                                     |
| `AURORA_ENABLE_DEBUG`     | `AUTO` | 真实后端 DEBUG 能力（帧缓冲/真实窗口截图、Widget 树、性能快照、可视化调试叠层、控件拾取；API 契约见 `SUBSYSTEM_APP_WINDOW.md` §H.10c 与 `GUIDELINE_PITFALLS.md` §22，设计取舍见 `../architecture/ARCHITECTURE_PERF.md` §10.7） | 三态：`AUTO`（Debug/RelWithDebInfo 注入 `AURORA_ENABLE_DEBUG`，Release/MinSizeRel 不注入）/ `ON`（全配置注入）/ `OFF`（不注入）。**仅库内部、不 PUBLIC 传播、不导出消费者**（与 `AURORA_ENABLE_SIMD` 一致，异于 `PROFILING`/`TRACING`）。调试 API 在头文件始终声明、`.cpp` 体按宏裁切，消费端调用始终可编译、关闭时返回 disabled。**可视化调试叠层（`DebugPaintFlags` / `paint_debug_overlays`）与控件拾取（`widget_picker`）即复用本宏，未新增任何编译开关** |
| `AURORA_ENABLE_SIMD`      | `ON`   | 光栅内核 SIMD 双实现（SSE2 基线 + AVX2 运行时分发）                                                                                                                                                                            | 注入 `AURORA_ENABLE_SIMD`（仅库内部、不 PUBLIC 传播）；`ON` 编译 SSE2/AVX2 快路径，`OFF` 仅标量；详见 §3.2                                                                                                                                                                                                                                                                                                                                                    |
| `AURORA_ENABLE_CCACHE`    | `ON`   | ccache 编译缓存（加速重复编译）                                                                                                                                                                                                | 设置 `CMAKE_C_COMPILER_LAUNCHER` 和 `CMAKE_CXX_COMPILER_LAUNCHER` 为 ccache；支持 winget 安装路径自动检测                                                                                                                                                                                                                                                                                                                                                     |
| `AURORA_ENABLE_LLD`       | `ON`   | 链接器选择（lld 加速静态链接）                                                                                                                                                                                                 | GNU/Clang 下 `find_program(ld.lld)` + `check_linker_flag` 探测通过则全局注入 `-fuse-ld=lld -B<lld 目录>`；非 PATH 安装的 LLVM 可用 `-DAURORA_LLD_DIR=<LLVM bin>` 提示位置；找不到或检查失败静默回退 GNU ld；**不注入 feature 宏**                                                                                                                                                                                                                             |

- `AURORA_ENABLE_COVERAGE` 与 `AURORA_ENABLE_ASAN` **互斥**：同时 `ON` 触发 `FATAL_ERROR`（都改写代码生成）。
- ⚠️ **Clang 下禁止注入 `--coverage`**（历史实现对所有编译器统一注入）：clang 的 gcov 兼容运行时 （`llvm_gcda_*`）在 Windows
  进程退出刷写 .gcda 时稳定崩溃（关闭窗口/测试退出即 0xC0000005）； 现已按 `CMAKE_CXX_COMPILER_ID` 自动分流，同一开关对两套工具链透明。
- 覆盖率摘要用法（GCC 与 Clang 工具链相同命令）：
  ```powershell
  cmake -S . -B build -DAURORA_ENABLE_COVERAGE=ON
  cmake --build build --target coverage -- -j $env:NUMBER_OF_PROCESSORS
  ```
    - GCC：ctest 后经 `tools/coverage_report.ps1` 聚合 gcov 行覆盖。
    - Clang：ctest 在 `LLVM_PROFILE_FILE=<build>/profraw/aurora-%p.profraw`（按 pid 分文件，并行不互覆） 环境下运行，再经
      `tools/coverage_report_llvm.ps1`（`llvm-profdata merge` + `llvm-cov report`， 多可执行文件 `-object` 聚合，过滤
      third_party/tests/tools/examples）输出终端摘要。
- 覆盖率需覆盖测试目标（大量 widget 是 header-only，仅在测试 TU 中编译，否则覆盖率严重偏低）。
- 插桩构建（-O0 全量插桩）退出前写 profile 较慢：关闭窗口后进程可能需数秒至十余秒才退出，属正常现象。

### 3.1 `AURORA_ENABLE_PROFILING`（渲染性能插桩，三态）

与 `COVERAGE`/`ASAN` 不同，本开关 **注入 feature 宏**（`AURORA_ENABLE_PROFILING`，PUBLIC 传播），
控制 `aurora::perf` 子系统（`Stopwatch` / `Profiler` / `ScopedTimer` / `RenderCounters`）的编译期存在性。

| 取值           | Debug / RelWithDebInfo | Release / MinSizeRel | 说明                                                                            |
|----------------|------------------------|----------------------|---------------------------------------------------------------------------------|
| `AUTO`（默认） | 注入                   | 不注入               | 经生成器表达式 `$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:…>` 按配置分流 |
| `ON`           | 注入                   | 注入                 | 全配置强制开启；同时进入 `AURORA_EXPORTED_DEFINES`（安装期导出给消费者）        |
| `OFF`          | 不注入                 | 不注入               | 全配置强制关闭                                                                  |

分级语义（宏关闭时全部退化为 **零开销** 空语句，不产生任何指令）：

| 层级       | 接口                                                            | 宏关闭时行为                                           |
|------------|-----------------------------------------------------------------|--------------------------------------------------------|
| 帧级       | `PerfLog` / `PerfOverlay` / `FrameStats`（ **不受本开关控制**） | 始终可用（帧级计时开销恒定，不进热点）                 |
| 作用域级   | `AURORA_PROFILE_SCOPE(name)` / `AURORA_PROFILE_FUNCTION()`      | 展开为 `((void)0)`，`ScopedTimer` 对象不构造           |
| 计数器级   | `AURORA_PROFILE_COUNT(field, n)` / `AURORA_PROFILE_SET(f, v)`   | 展开为 `((void)0)`，`RenderCounters::current()` 不调用 |
| 编译期查询 | `constexpr bool aurora::profiling_enabled()`                    | 返回 `false`（可用于 `if constexpr` 剪裁）             |

常用组合：

```powershell
# 日常开发（Debug 自动带插桩）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 性能压测：Release 优化级别 + 插桩（计数器/作用域计时可读）
cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=ON

# 纯净基线：Release 无插桩（时间类硬门槛以此配置为准）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=OFF

# 时间线落盘（Chrome about:tracing / Perfetto 可读）
cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_TRACING=ON
```

> **门槛配置约定**： **时间类**硬门槛（帧时间、P99、长任务）在
> `Release + PROFILING=OFF` 下测量，避免插桩本身污染读数； **计数类**硬门槛（`RenderCounters` 各字段、
> 脏区面积比、full-redraw 帧数）在 `Release + PROFILING=ON` 下测量——计数器在宏关闭时恒为 0，无法作为门槛。
> 两者互不冲突：计数是确定性的（与机器无关），可作为 CI 回归锚点；时间是环境相关的，只做趋势对比。
> 配套 `tools/bench_render.cpp`（原语/整帧光栅矩阵）与 `tools/bench_scroll.cpp`（业务树滚动采样，基于 `ScrollBenchHarness`）提供确定性测量，使用配方见 `GUIDELINE_ASYNC_SERIAL.md` §10d。

---

### 3.2 `AURORA_ENABLE_SIMD`（光栅内核 SIMD 双实现）

| 项         | 值                                                                                                                                                                         |
|------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 默认值     | `ON`                                                                                                                                                                       |
| 类别       | `AURORA_ENABLE_*` 插桩/分析/能力开关（见 §0.1）                                                                                                                            |
| feature 宏 | 注入 `AURORA_ENABLE_SIMD`，**仅库内部使用，不 PUBLIC 传播、不导出给消费者**（与 `AURORA_ENABLE_PROFILING`/`TRACING` 不同）                                                 |
| 编译期行为 | `ON`：编译 `painter_simd.inl` 中的 SSE2（x86-64 基线恒可用）+ AVX2（运行时 CPUID 分发）快路径；`OFF`：仅编译标量黄金路径（`gradient_*_scanline_scalar`），无 SIMD 代码生成 |
| 运行时分发 | `detect_simd_level()` 懒初始化 `g_simd_level`；SSE2 恒可用，AVX2 仅在 CPU 支持时启用，未支持则回退 SSE2 / 标量尾补                                                         |
| 确定性约束 | SIMD 路径必须与标量黄金路径**逐位一致**（`-ffp-contract=off` 禁 FMA + 同序浮点运算 + `cvtt` 整型截断）；CI 由 `test_simd_parity` 逐位比对，G-13 一票否决                   |

- 该开关为 **纯内部优化开关**：消费者代码与 ABI 均不感知 SIMD 是否存在，关闭后仅损失性能、不改变任何像素输出。
- 开启 SIMD 不引入新的公共 API；`gradient_linear_fill` / `gradient_radial_fill` / `gradient_*_scanline_*` 均位于 `aurora::detail`（头文件 `include/aurora/render/detail/painter_simd.h`），不计入 `aurora_api.json`。

### 3.3 `AURORA_ENABLE_CCACHE`（编译缓存加速）

| 项         | 值                                                                                     |
|------------|----------------------------------------------------------------------------------------|
| 默认值     | `ON`                                                                                   |
| 类别       | `AURORA_ENABLE_*` 插桩/分析/能力开关（见 §0.1）                                        |
| feature 宏 | 不注入 feature 宏，仅设置 `CMAKE_C_COMPILER_LAUNCHER` 和 `CMAKE_CXX_COMPILER_LAUNCHER` |
| 编译期行为 | `ON`：自动检测并配置 ccache 作为编译器启动器；`OFF`：不使用 ccache                     |
| 缓存策略   | 启用压缩（level 6）、硬链接、默认缓存大小 2GB                                          |

- **安装方式**：支持 PATH 中的 ccache，也支持 winget 安装路径自动检测（`%LOCALAPPDATA%/Microsoft/WinGet/Packages/Ccache.Ccache_*/ccache-*/ccache.exe`）
- **配置变量**：
    - `AURORA_CCACHE_DIR`：ccache 缓存目录（默认使用系统默认）
    - `AURORA_CCACHE_MAXSIZE`：ccache 最大缓存大小（默认 `2G`）
- **性能提升**：重复编译速度提升约 50%（实测从 0.38 秒降至 0.18 秒）
- **使用示例**：
  ```powershell
  # 默认启用 ccache
  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
  
  # 禁用 ccache
  cmake -S . -B build -G Ninja -DAURORA_ENABLE_CCACHE=OFF
  
  # 自定义缓存目录和大小
  cmake -S . -B build -G Ninja -DAURORA_CCACHE_DIR=D:/ccache -DAURORA_CCACHE_MAXSIZE=5G
  ```

### 3.4 `AURORA_ENABLE_LLD`（链接器选择加速）

| 项         | 值                                                                                                                                               |
|------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| 默认值     | `ON`                                                                                                                                             |
| 类别       | `AURORA_ENABLE_*` 插桩/分析/能力开关（见 §0.1）                                                                                                  |
| feature 宏 | 不注入 feature 宏，仅影响链接器选择（GNU/Clang 下全局注入 `-fuse-ld=lld -B<lld 目录>`）                                                          |
| 编译期行为 | `find_program(ld.lld)` + `check_linker_flag` 探测通过则全局切换至 lld 链接；找不到或检查失败静默回退 GNU ld（250+ 可执行目标的静态链接显著提速） |
| 自定义     | 非 PATH 安装的 LLVM 可用 `-DAURORA_LLD_DIR=<LLVM bin>` 提示链接器位置                                                                            |
| 关闭       | `-DAURORA_ENABLE_LLD=OFF`                                                                                                                        |

- 该开关 **不注入任何 feature 宏**，仅改变链接器；与 `AURORA_ENABLE_CCACHE`（编译启动器）同属构建加速类开关，按 `AURORA_ENABLE_*` 命名组归类于此（原置于 `BUILD_OPTIONS_BUILD.md`，因命名组一致性已并入本节）。
- **使用示例**：
  ```powershell
  # 默认启用 lld 链接（GNU/Clang 工具链）
  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++

  # 关闭 lld，回退 GNU ld
  cmake -S . -B build -G Ninja -DAURORA_ENABLE_LLD=OFF

  # 非 PATH 安装的 LLVM，显式指定 lld 位置
  cmake -S . -B build -G Ninja -DAURORA_LLD_DIR="D:/Development/Environment/LLVM/bin"
  ```


