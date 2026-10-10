# CHG-005 平台 lint 子作业存量清零（Windows / macOS）+ macOS PCH 基建 + 落地回归修复

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-005 |
| 提出日期 | 2026-10-11 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.QUALITY.CORE.MEMORY-SAFETY.001 |
| 影响面 | CI 的 `lint-macos`（作业 8mac）configure 口径新增 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`；按处置阶梯修改的平台门控源与公共头：`tools/verify/{win32_os_hotkey,win32_insert_key,win32_dpi,win32_syskey,win32_cursor}_live_probe.cpp`、`tools/verify/macos_cursor_live_probe.mm`、`include/aurora/widget/scroll.h`、`include/aurora/window/macos_surface.h`；落地回归修复：`src/aurora/render/rhi/software_rhi.cpp`（哨兵常量 `constexpr` → `const`）与 13 个 clang-format 漂移文件（`src/aurora/window/{wayland_surface,x11_surface,window_factory}.cpp`、`src/aurora/render/gpu/wgpu_rhi.cpp`、`src/aurora/window/detail/{atspi_bridge.cpp,csd_geometry.h}`、`include/aurora/perf/{profiler,counters}.h`、`tests/unit/{utest_dropdown,utest_csd_shadow_compose,utest_csd_geometry}.cpp`、`tools/verify/{x11_ime,alsa_audio}_live_probe.cpp`）。`MacOSSurface` 新增 4 个 deleted 特殊成员属公共头 API 表面变化（该类型自此不可拷贝 / 移动）；不改分层边界与后端支持矩阵 |

## 动机

CHG-002 交付了 `lint-windows` / `lint-macos` 两道平台子作业（该提案条目 7 与条目 11），但**其存量告警从未被清点**：CHG-002 的存量清零口径（验收判据 5）只覆盖「宏全开 native 遍」，且其记述明确把平台子作业的 TU 排除在分母之外。于是两道子作业自诞生起即为红——`unique_findings` 非 0，与 `codespec/BUILD_OPTIONS.md` §4.5「一个永远红的必过位不等于门禁，只是把红灯常态化」的纪律直接冲突。

本批以 CI 现场产物 `tidy-findings-windows.json` / `tidy-findings-macos.json` 为准（**数值取自这两份清单，非文档转述**）：`lint-windows` 76 TU、`unique_findings = 29`、`broken_tus` 为空；`lint-macos` 16 TU、`unique_findings = 6`、**`broken_tus` 15 个**。

macOS 的 15 个 broken TU 是**基建缺陷而非代码缺陷**，且它使那 6 条告警失去意义：该作业**只 configure 不 build**，而 `AURORA_PCH_ENABLED` 在 clang 侧默认 ON（GCC 侧一律 OFF，见 `cmake/AuroraUtils.cmake`），clang-tidy 消费的 `compile_commands.json` 每条命令都带 `-include .../cmake_pch.hxx`，而 PCH 产物从未生成 ⇒ 每个 TU 起步即 `PCH file '...' not found: module file not found [clang-diagnostic-error]` 被判 broken。只修那 6 条告警而不清 broken，等于在一张只覆盖 1/16 TU 的面上宣布「干净」——正是 CHG-002 §动机 反复点名的那种「不是干净，而是不存在」的塌陷。

另有两类只有动手才会兑现的问题，同在「先清后切」的尾巴上，须同批收口：

1. **CHG-002 存量处置引入的落地回归**：`src/aurora/render/rhi/software_rhi.cpp` 的 5 个哨兵常量（`AURORA_EMPTY_STR` / `AURORA_EMPTY_COLORS` / `AURORA_EMPTY_FLOATS` / `AURORA_IDENTITY_MATRIX` / `AURORA_EMPTY_POINTS`）被误从 `const` 改为 `constexpr`。MSVC STL 的 `constexpr std::string` / `std::vector` 仅 `_ITERATOR_DEBUG_LEVEL == 0` 可用（Debug IDL=2 下构造 / 析构非法，C2131）⇒ `core (windows-msvc-debug)` 与 `core (windows-llvm)` 编译失败。该文件顶部注释本就写明了「不用 constexpr」的原因，改动与注释自相矛盾。
2. **全仓 clang-format 22 的排版漂移**：13 个文件与 `.clang-format`（`ColumnLimit: 120`）不一致，`clang-format` 门禁红。其中 `wayland_surface.cpp` 的一处 `NOLINTNEXTLINE` 因排版折行与目标代码不再相邻，豁免静默失效——`check_nolint_layout` 抓不到「相邻但被折行挤开」这一形态，须靠 `clang-format` 复排恢复邻接。

## 变更内容

1. **Windows 平台存量清零（29 条，按代价从小到大处置）**：

   - `cppcoreguidelines-avoid-non-const-global-variables`（8 条）：探针的失败计数器与观察面集合由命名空间级可变全局改为「函数局部静态 + 访问器」（`failures()` / `records()` / `scale_is_unity()` / `original_wndproc()` / `syscommands()`），沿用本仓真机探针既有收敛写法，不落可变命名空间作用域状态。
   - `readability-identifier-naming`（5 条）：`kInteractiveTimeoutMs` / `kPumpSliceMs` / `kInteractiveComboCount` / `kScClose` / `kMaskCommand` 按 `GlobalConstantCase=UPPER_CASE` + `GlobalConstantPrefix='AURORA_'` 归位为 `AURORA_*` 全大写（与 `AURORA_PI` / `AURORA_CLICK_BUDGET_MS` 同形）。
   - `readability-container-size-empty`（4 条）：`win32_insert_key` / `win32_syskey` 的 `size() >= 1U` → `!empty()`；`include/aurora/widget/scroll.h` 的 `kids.size() > 0` 改迭代器判空——**`std::initializer_list` 无 `empty()`**（标准只给 `size` / `begin` / `end`），clang-tidy 的 fix-it 提示的 `!kids.empty()` 在 libstdc++ / libc++ 下均不可编译，故以 `kids.begin() != kids.end()` 落码并就地写明理由。
   - `readability-math-missing-parentheses`（3 条，`src/aurora/window/swizzle.h`）：按检查自带 fix-it 给 `*` 子表达式加括号（行为等价、仅消歧义）。
   - `cppcoreguidelines-pro-type-reinterpret-cast`（3 条）+ `performance-no-int-to-ptr`（1 条）：Win32 消息 API 的 `lParam` 承载结构体指针、窗口过程换装（`SetWindowLongPtrA` 形参为 `LONG_PTR`）均无类型安全替代，逐点 `NOLINTNEXTLINE(check, ...)` + 紧邻注释写明理由；**注意 `NOLINTNEXTLINE` 只在下一物理行生效**，理由写上一行、指令独占短行，避免超 120 列被 clang-format 折行挤开。
   - `cppcoreguidelines-pro-type-static-cast-downcast`（1 条，`win32_cursor_live_probe.cpp`）：`Backend::D3D11` 打开后的表面动态类型即 `D3D11Surface`，静态下行转换安全，提取命名局部变量 + 定点 `NOLINTNEXTLINE` + 理由（不用 `dynamic_cast`，避免在探针里引入 RTTI 依赖与失败分支语义）。
   - `modernize-use-designated-initializers`（2 条）+ `modernize-use-auto`（1 条）：`tagPOINT` 改指定初始化器；`static_cast<float>(...)` 初值改用 `auto` 承接。
   - `readability-implicit-bool-conversion`（1 条）：`while (pump_one_message() != 0)` → `while (pump_one_message())`（该 API 返回 `bool`）。

2. **macOS 平台存量清零（6 条）**：

   - `cppcoreguidelines-special-member-functions`（1 条，`include/aurora/window/macos_surface.h`）：`MacOSSurface` 定义了析构却未定义拷贝 / 移动，按本仓所有 `Surface` 子类一致形态补 4 个 deleted 特殊成员（`x11_surface.h` / `wayland_surface.h` 等同类）。该类型持有唯一窗口壳与整帧 `Painter`，拷贝 / 移动语义本就不成立，deleted 是语义的显式化而非行为变更。
   - `readability-use-concise-preprocessor-directives`（3 条）：`#if !defined(X)` → `#ifndef X`、`#if defined(__has_feature)` → `#ifdef __has_feature`（`macos_cursor_live_probe.mm`）。
   - `bugprone-exception-escape`（1 条）：探针 `main()` 入口按本仓「入口不吞异常」口径就地 `NOLINTNEXTLINE` + 理由（未捕获异常以非零退出码 / `terminate` 呈现失败，捕获后返回 0 反把真机现场压成成功）。
   - `clang-diagnostic-error`（1 条，`macos_cursor_live_probe.mm`）：`busyButClickableCursor` 是 AppKit 自 macOS 10.14 起提供的 `NSCursor` class method，部分 SDK 的 `NSCursor` 头未暴露该声明，直接发消息被 clang 判「未知类方法」。探针内补一条同名 `category` 声明（`@interface NSCursor (AuroraVerifyBusyCursor)`），运行期仍由 AppKit 实现响应。**此处属本机不可验证项**（Linux 主机无 macOS SDK），以 CI 的 `lint-macos` 首跑为证。

3. **macOS PCH 基建修复**：`lint-macos` 作业的 configure 步骤补 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`（CMake ≥ 3.16 支持），使 `compile_commands.json` 不再带 `-include-pch`，从根上消除 15 个 broken TU。GCC 侧（native / wasm / Windows 各遍）本就 PCH-off，不受影响；本 flag 只对该「仅 lint 的 clang configure」生效。**本机不可验证**（同 2），以 CI 首跑 broken_tus 归零为证。

4. **落地回归修复**：`software_rhi.cpp` 的 5 个哨兵常量回退为 `const`（恢复与文件顶部注释「不用 constexpr」一致；namespace-scope `const` 内部链接语义等价、静态初始化一次性构造、热路径零差异）。13 个 clang-format 漂移文件用 clang-format 22.1.8 复排至全库一致，并复验 `wayland_surface.cpp` 的 `NOLINTNEXTLINE` 恢复与目标行相邻。

5. **覆盖面自述**：修复后 `lint-macos` 的 `broken_tus` 应为空、`tu_count` 应覆盖全量 macOS 编译库（而非修复前的 16），两道平台作业的 `unique_findings` 均为 0——「跑到了且干净」与「宏没开 / PCH 缺位」自此在产物里可区分。

## 验收判据

1. **Windows 口径清零**：`lint-windows` 的 `tidy-findings-windows.json` `unique_findings = 0`、`broken_tus = []`、`tu_count` 不下降（本仓选取集不变）。基线为 CI 现场的 76 TU / 29 条。
2. **macOS 口径清零且 broken 归零**：`lint-macos` 的 `tidy-findings-macos.json` `unique_findings = 0`、`broken_tus = []`。基线为 CI 现场的 16 TU / 6 条 / 15 broken——判据要求 **broken 与告警同时归零**，只降其一不算达成（broken 未清时覆盖面不足 1/16，0 告警无意义）。
3. **回归修复的跨工具链验证**：`core (windows-msvc-debug)` 与 `core (windows-llvm)` 编译通过（`software_rhi.cpp` 的哨兵常量不再是 `constexpr`）；`clang-format` 门禁 0 diff。以 CI 首跑为证。
4. **报告型门禁不回归**：完整 `ctest --preset ninja-test` 全绿，含 `check_nolint_layout`（本批新增的 `NOLINTNEXTLINE` 排版须过）、`check_change_proposals`（本提案）、`check_codespec_xref`、`check_code_doc_sync`、`check_no_hardcoded_paths`、`check_doc_comments`、`check_agents_size`、`check_version_consistency`。
5. **诚实边界**：条目 2 的 `category` 声明与条目 3 的 PCH flag 均**本机不可验证**（Linux 主机无 macOS SDK），以 CI 的 `lint-macos` 首跑结果为唯一证据；若首跑仍报 `busyButClickableCursor` 未知类方法，退化为「探针内用 `sel_registerName` + `objc_msgSend` 显式取选择器」并同步在本提案记录实际形态。

## 回写落点

- `codespec/BUILD_OPTIONS.md` §4.5（平台子作业一节：补 macOS 作业的 `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` 成因——「只 configure 不 build + clang 侧 PCH 默认 ON ⇒ 全 TU broken」，并记两道平台子作业存量清零点位）
- `codespec/ARCHITECTURE.md` §14.4（CI 执行层的 lint 作业 configure 口径：`lint-macos` 的 PCH 关闭）与 §14.6（按改动类型的门禁矩阵：`tools/verify/` 下平台真机探针的必跑口径含 `lint-windows` / `lint-macos`）
- `codespec/changes/windows-macos-lint-clearance/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）
- 仓库根 CHANGELOG.md（交付纪律：平台 lint 子作业存量清零、macOS PCH configure 口径；根变更记录不作为本表的 codespec 目标路径，仅列出落点）