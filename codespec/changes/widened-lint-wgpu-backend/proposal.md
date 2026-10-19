# CHG-003 wgpu GPU 后端纳入 lint 矩阵 + 其门控存量告警清零

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-003 |
| 提出日期 | 2026-10-10 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.QUALITY.CORE.MEMORY-SAFETY.001 |
| 影响面 | CI 的五道 clang-tidy 作业里 native `lint`（作业 8）与 `lint-incremental`（作业 8i）的 configure 口径与依赖步骤：两处 configure 补 `-DAURORA_ENABLE_GLFW_GPU_GL=ON -DAURORA_BACKEND_GPU_WGPU=ON -DAURORA_ENABLE_PROFILING=ON -DAURORA_ENABLE_TRACING=ON`（作业 8i 的 DEBUG=OFF / DEBUG=ON 两套编译库同补），两处安装步骤补 `libclang1-22`（wgpu-native 的 bindgen 在 configure 期探测 C 接口 `libclang.so`，`clang-tidy-22` 只拉 C++ 接口的 `libclang-cpp22`），并各新增一步 `Ensure Rust toolchain`（`AURORA_BACKEND_GPU_WGPU=ON` 的 configure 期硬探测 `cargo` / `rustc`）；按处置阶梯修改的源码：`tools/verify/{x11,wayland}_wgpu_live_probe.cpp`、`tools/verify/glfw_gpu_features_live_probe.cpp`、`tools/verify/atspi_live_probe.cpp`、`tools/bench/bench_gpu.cpp`、`tests/unit/utest_wgpu_rhi.cpp`、`tests/unit/utest_window_swizzle.cpp`、`tests/integration/itest_wgpu_golden.cpp`、`src/aurora/window/swizzle.h`、`src/aurora/window/detail/csd_shadow_compose.h`。公共 API、分层边界与后端支持矩阵本身不变（仅把已存在的后端宏纳入静态分析面，不新增/删除任何后端） |

## 动机

CHG-002 把主 Linux 的 lint 口径扩到「除 Windows / macOS 特有宏之外的全部后端与能力开关」，但**刻意把 `AURORA_BACKEND_GPU_WGPU` 留在门外**——原因是该后端的 configure 期硬探测 `cargo` / `rustc` / `libclang`（见 `cmake/AuroraBackends.cmake`），打开它会给 CI 的 lint 作业追加工具链前提。CHG-002 的验收判据 5 就此留了一条明账尾巴：wgpu 门控口径的存量「作为独立台账，在把 `AURORA_BACKEND_GPU_WGPU` 加进 lint 矩阵之前清完」。本批即该收尾——**先清存量、再把宏纳入矩阵**，与 CHG-002 的「先清后切」同一纪律。

不清就切的后果与 CHG-002 §动机 描述的塌陷同构：`#ifdef AURORA_BACKEND_GPU_WGPU` 收口的 RHI 层、GPU 表面与 wgpu 真机探针在门禁眼里不是「干净」而是「不存在」，`unique_findings = 0` 会被读成「查过了」。

另一条只有动手才会暴露的前提：**CI 钉的 clang-tidy 是 22，本机长期是 21.1.8**，两者检查集不同。本批实测确认了一例——`readability-math-missing-parentheses` 在 clang-tidy 22 下会报、在 21.1.8 下对同一 TU 开启该检查报 0 条。若只在 21 口径上把存量清到 0，CI 首跑仍会以 22 独有的检查项转红。故本批把本机 clang-tidy / clang-format 对齐到 **22.1.8**，门禁数字自此与 CI 同面。

## 变更内容

1. **主 Linux 遍 configure 补齐 4 个宏**：作业 8 与作业 8i 的 configure 各补 `-DAURORA_ENABLE_GLFW_GPU_GL=ON -DAURORA_BACKEND_GPU_WGPU=ON -DAURORA_ENABLE_PROFILING=ON -DAURORA_ENABLE_TRACING=ON`。前两者把 GPU GL 栅格与 wgpu RHI 后端纳入分析面，后两者补齐性能观测宏门控的分支。`AURORA_BACKEND_GPU_WGPU` 在 lint 口径下**只做 configure 期工具链探测**，`lint` 目标不 `DEPENDS` `wgpu_native_build`，故不触发真正的 cargo 编译。

2. **依赖步骤补 `libclang1-22`**：`clang-tidy-22.deb` 的 `Depends` 只含 `libclang-cpp22`（C++ 接口 `libclang-cpp.so`）与 `libclang-common-22-dev`，**不含 C 接口的 `libclang-22.so.1`**，而 wgpu-native 的 bindgen 探测的是 C 接口库；不显式补装即 configure 期 `aurora_error`。作业 8 / 8i 的 apt 列表同补。

3. **新增 `Ensure Rust toolchain` 步骤**：`AURORA_BACKEND_GPU_WGPU=ON` 的 configure 在 `find_program(cargo)` 失败时 `aurora_error`；运行器镜像通常已预装 Rust，步骤先 `command -v cargo/rustc` 探测，存在即跳过，缺失才经 rustup 装最小 stable 并追加 `$GITHUB_PATH`。作业 8 / 8i 各一步。

4. **wgpu 门控存量清零（处置阶梯：改名 / 就地 `NOLINT` + 理由 / 机械 fix-it）**：
   - 命名类：探针与 bench 的命名空间级常量按 `readability-identifier-naming` 归位（`GlobalConstantCase` 需 `AURORA_` 前缀 + 全大写，局部常量 `lower_case`），命名空间级可变全局计数改为函数局部静态 + 访问器收敛，避免可变的命名空间作用域状态。
   - 指针 / 越界类：`utest_wgpu_rhi.cpp` 的假 dmabuf fd（数值装入 `void *handle`，测试不解引用）走就地 `NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)` + 理由；常量数组下标由循环边界天然界定者逐点就地豁免。**注意 `NOLINT` 只在其所在物理行生效**，跨行写法会静默失效（本批曾因把尾注写在下一行而让 2 条告警漏过一轮复扫）。
   - 宏类：`itest_wgpu_golden.cpp` 的 `AURORA_ITEST_WGPU_GPU_OR_SKIP` 依赖 `AURORA_TEST_SKIP` 的就地早退控制流，`constexpr` 模板函数复刻不了，就地 `NOLINTNEXTLINE(cppcoreguidelines-macro-usage)` + 理由。
   - `bugprone-command-processor`：`atspi_live_probe.cpp` 以 `python3 -c` 探测 gi/Atspi 可用性，命令为就地字面量，就地豁免。
   - 数学括号：`readability-math-missing-parentheses`（clang-tidy 22 独有）18 处，按检查自带的 fix-it 把 `*` 子表达式显式加括号（`(a * b) + c`），行为等价、无歧义化。

5. **本机工具链对齐 CI**：安装 clang-tidy 22.1.8 与 clang-format 22.1.8（apt.llvm.org 的 `clang-tidy-22` / `clang-format-22` + `libclang-cpp22` / `libclang-common-22-dev` / `libllvm22` / `libclang1-22`），经本地前缀 + wrapper 暴露为 `PATH` 上的 `clang-tidy` / `clang-format`，使 `tools/check/run_clang_tidy.py` 与 `format-check` 量的是与 CI 同版本的检查集。本机 sudo 不可用，故用 `dpkg-deb -x` 解到本地前缀 + `LD_LIBRARY_PATH`，不改系统。

## 验收判据

1. **宏全开口径存量清零**：`build-lint-max`（`AURORA_BACKEND_GPU_WGPU` / `AURORA_ENABLE_GLFW_GPU_GL` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` 等 21 个能力宏全开）用 clang-tidy 22.1.8 全量复扫，`unique_findings = 0`、`broken_tus = []`。基线：tidy-21 口径 51 条（集中 5 个 wgpu 门控文件）修完后，tidy-22 口径复扫报 20 条（18 条 `readability-math-missing-parentheses` + 2 条 `utest_wgpu_rhi.cpp` 的错位 `NOLINT`），逐条修毕后终扫 0。
2. **覆盖面自述**：终扫 JSON 的 `tu_count` = 571、`generated_skipped` = 3（Wayland 生成物）、`broken_tus` 为空——即 wgpu 门控 TU 确实进了分析面且编译通过，而非「宏没开」的塌陷。
3. **格式一致**：改动文件在 clang-format 22.1.8 下 `--dry-run -Werror` 通过。
4. **CI 依赖闭合**：作业 8 / 8i 的 configure 打开 `AURORA_BACKEND_GPU_WGPU=ON` 后，apt 列表含 `libclang1-22` 且 configure 前有 `Ensure Rust toolchain` 步骤，二者缺一即 configure 期 FATAL——以 CI 首跑的实际 configure 通过为证。
5. **门禁不回归**：完整 `ctest --preset ninja-test` 全绿，含 `check_nolint_layout`（本次新增的 `NOLINT` 排版须通过）、`check_change_proposals`（本提案）、`check_codespec_xref`、`check_agents_size`、`check_version_consistency`。

## 回写落点

- `codespec/BUILD_OPTIONS.md` §4.5（lint 口径的 macro 全集与作业形状：补 GPU / PROFILING / TRACING 四个宏、`libclang1-22` 与 Rust 步骤的成因）
- `codespec/BUILD_OPTIONS.md` §3.8（wgpu GPU 栅格后端：补一句「CI 的 lint 口径已把该后端纳入静态分析面，configure 期工具链探测在 lint 作业里由 apt + rustup 步骤满足」）
- `codespec/ARCHITECTURE.md` §14.4（CI 执行层的 lint 作业 configure 口径）与 §14.6（按改动类型的门禁矩阵：`window/` 下 wgpu 表面与 RHI 层的必跑口径）
- `codespec/changes/widened-lint-coverage/proposal.md` 中验收判据 5 的 wgpu 尾巴已由本批收口（该提案维持「已归档」，本条为新增变更、不改其正文）
- 仓库根 CHANGELOG.md（CI 交付纪律：lint 矩阵新增 GPU 后端口径；根变更记录不作为本表的 codespec 目标路径，仅列出落点）