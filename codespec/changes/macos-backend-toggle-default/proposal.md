# CHG-046 macOS 后端开关的默认值与不可构建组合的显式拒绝

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-046 |
| 提出日期 | 2026-10-28 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | **构建面（后端矩阵）修正**，零代码语义变更：`cmake/AuroraBackends.cmake`（`AURORA_BACKEND_MACOS` 的默认值按平台取，并对「Apple 上显式关掉」新增 configure 期拒绝）、顶层 `CMakeLists.txt`（该开关的 `option()` 声明与既有的 `enable_language(OBJCXX)` 前置同处）；`codespec/BUILD_OPTIONS.md` §1.1 / §3 开关表行、`codespec/ARCHITECTURE.md` §8.4；**不动任何源文件、不动公共 API、不动错误码** |

## 动机

1. **CI 的 macOS 作业在当前分支上是构建不过的，而原因是一枚默认值。** `.github/workflows/release.yml` 与 `.github/workflows/e2e-probe.yml` 的 macos-latest 作业都不传 `AURORA_BACKEND_MACOS`（后者传的是 `AURORA_BACKEND_GLFW=ON`），取的是 `cmake/AuroraBackends.cmake` 里 `option(AURORA_BACKEND_MACOS ... OFF)` 的默认值。OFF 在 Apple 上不是一台可构建的机器：`src/aurora/app/detail/macos_notification.cpp` 与 `macos_tray.cpp` 的门控只读**平台宏** `AURORA_PLATFORM_MACOS`（该宏由 `include/aurora/core/platform.h` 按 `__APPLE__` 定义，与开关无关），于是这两枚 ObjC++ TU 照样进编译，却按 C++ 模式解析 `#import <Foundation/Foundation.h>`——本机实测一串 `unknown type name 'NSString'`（AppleClang，十余条 error 起）。**这些腿是本仓 macOS 补齐批次陆续加进来的**，它们落地时没有同步开关的默认值，故这条矩阵洞是新出现的，不是历史遗留。
2. **对照组说明这不是 Apple 的特有怪癖，而是缺了一次同批调整。** `AURORA_BACKEND_WIN32` 的写法是「`if (WIN32)` 默认 `ON`，`else ()` 默认 `OFF`」——**在自己平台上默认开**；`AURORA_BACKEND_MACOS` 却是无条件的 `OFF`。同理 `AURORA_ENABLE_AUDIO_WASAPI` / `_ALSA` / `_WEBAUDIO` / `_COREAUDIO` 四枚音频后端开关一律「在自己的平台上默认 ON，异平台开启即 FATAL」。macOS 腿缺的正是这条同族口径。
3. **把话说清比留一个假开关更有价值。** 即便把 ObjC++ 语言标记与 `-framework` 链接从后端块里拆出来（本提案曾实测过这条路：语言拆出来以后，下一处红灯是 `src/aurora/app/recent_documents.cpp` 引用只在「平台 && 后端」下声明的 `detail::macos_recent_documents_ensure_registered()`，再下一处是 AppKit / Carbon 符号无框架可链），也只是把同一个洞摊成三处要改的门控口径——而这三处改动属于「让 AppKit 集成腿不再依赖 Surface 后端」的**语义决定**，须另立批次论证（见「不做清单」）。本批只把矩阵的默认值取对，并把不可构建的组合变成 configure 期的一句人话。

## 变更内容

1. **`AURORA_BACKEND_MACOS` 默认值按平台取**：Apple 默认 `ON`、其余平台 `OFF`，与 `AURORA_BACKEND_WIN32` 逐字同构。不改开关名、不改宏名、不改 `aurora_define_feature(... EXPORT)` 的传播面。**声明位置落在顶层 `CMakeLists.txt`**（原提案写「在 `cmake/AuroraBackends.cmake` 里改」，落码时按实测修正，见「实施记录与偏离」第 1 条）。
2. **Apple 上显式关掉走 configure 期拒绝**：新增 `if (APPLE AND NOT AURORA_BACKEND_MACOS)` → `aurora_error`，消息点名三处不可构建的因（ObjC++ TU 的门控只读平台宏 / 桥符号只在双门控下声明 / Cocoa·AppKit·Carbon 只随本开关链接）并给出可达的路（保持 ON，或改选 `AURORA_BACKEND_GLFW` / `AURORA_BACKEND_X11` / Headless——三者在 Apple 均可用）。判据是「错误信息在 configure 期、用人话写」而不是「编译器报出的 `NSString` 未声明」。
3. **ObjC++ TU 名单补记两枚**（同批实测发现的本仓缺陷，与上一条同一开关相关，故并入本批）：`cmake/AuroraBackends.cmake` 的 `foreach(_macos_objc_src ...)` 名单此前漏了 `src/aurora/window/detail/macos_menu_bar.cpp` 与 `src/aurora/app/detail/macos_clipboard.cpp`——两枚 ObjC++ TU 由源文件 glob 恒采集却未标 `LANGUAGE OBJCXX`，在 Apple 上以 C++ 编译即炸在 AppKit 头。名单与就地注释同步补齐（注释里逐枚点名各腿职责）。
4. **`enable_language(OBJCXX)` 的守卫不动**：保持 `if (APPLE AND AURORA_BACKEND_MACOS)`。第 2 条的拒绝成立后，Apple 上「开关 ON」是唯一可达形态，该守卫与「开关 ON」等价，无需改动——少改一处是一处。

## 验收判据

1. **Apple 上不带任何 `-D` 的裸 configure 可用**：`cmake -S . -B build` + `cmake --build build` 全绿，且 `CMakeCache.txt` 内 `AURORA_BACKEND_MACOS:BOOL=ON`。这条正是 CI 的 macos-latest 作业的现状，判据必须是「作业本身不再需要额外 flag」。**且必须走到 generate 之后**：只断言 `option()` 的默认值不够，本批实测暴露的正是「默认值取对了、语言规则却没启用」这一形态。
2. **显式关掉得到 configure 期的一条人话错误**：`cmake -S . -B build -DAURORA_BACKEND_MACOS=OFF` 在 configure 阶段终止、消息含 `not a buildable configuration` 与替代路径，**不得**走到编译期才报 `NSString` 未声明；退出码非零且不产生可用构建。
3. **非 Apple 平台零变化**：Linux / Windows 上裸 configure 的 `AURORA_BACKEND_MACOS` 仍为 `OFF`，`check_platform_macros` 与既有 CI 矩阵（含 `toggles` 作业）结论不变；`enable_language(OBJCXX)` 仍不在非 Apple 路径上被调用。
4. **本机双配置实测**：ON 配置（默认，本机 `build-macos`）跑全量 `ctest` 全绿；OFF 配置按第 2 条断言在 configure 期即拒。探针清单里 `aurora_verify_macos_*` 与 `aurora_verify_coreaudio_audio` 照常登记。
5. **门禁**：`check_codespec_xref` / `check_change_proposals` / `check_code_doc_sync`（开关默认值与拒绝条件同批回写文档）、`format-check`、`check_no_hardcoded_paths`（CMake 注释内只写仓库相对路径）。

## 回写落点

- `codespec/BUILD_OPTIONS.md`：§1.1 CMake 脚本布局（记「模块内 `option()` 与顶层等价」的唯一例外）、§2 模块职责表里 `cmake/AuroraBackends.cmake` 一行、§3 后端开关表的 `AURORA_BACKEND_MACOS` 行（默认值改为「Apple `ON`、其余 `OFF`」+ 不可构建组合的显式拒绝 + 声明位置与语言前置的时序理由）、§10 速查表同开关行。
- `codespec/ARCHITECTURE.md`：§8.4 后端家族表的 `MacOSSurface` 开关列，以及段末「编译 / 链接期代码剪裁」口径补一条 Apple 例外（AppKit Surface 与其平台腿同开关，剔除须另立批次）。
- 同批校正两处「默认值一律 OFF」的滞后表述：`codespec/CONCEPTS.md` 的 Surface 开关段、`codespec/specification/03-layout-render.md` 后端表 `MacOSSurface` 行。
- 变更记录（根 CHANGELOG）：`[Unreleased] / Fixed` 一条（两处后果、否决的替代解法、本机两向实测数字）。

## 落码次序

`cmake/AuroraBackends.cmake` + 顶层 `CMakeLists.txt`（默认值与语言前置同处声明、拒绝、ObjC++ TU 名单补记）→ 本机 ON 配置全量 `ctest` + OFF 配置的拒绝断言 → 文档三处回写 → 门禁复跑。

## 不做清单

1. **不把 macOS 应用域腿的门控从平台宏改成后端宏**：这三处改动（ObjC++ 语言标记、`-framework` 链接、桥符号声明条件）是「AppKit 集成腿是否独立于 Surface 后端」的语义决定，须另立批次论证其运行期降级形态；本批只让不可构建的组合在 configure 期说人话。
2. **不在 Apple 上支持「无 AppKit 后端」构建**：GLFW / X11 / Wayland 后端在 Apple 可用是 Surface 层的事实，但应用域腿仍按平台宏编译，故「Apple + 只开 GLFW + 关 macOS 后端」当前不成立（CI 的 e2e-probe macOS 作业正落在此形态，其通过依赖 macOS 后端仍默认 ON）。
3. **不改 `enable_language(OBJCXX)` 的守卫**：保持 `if (APPLE AND AURORA_BACKEND_MACOS)`；第 2 条的拒绝成立后，Apple 上「开关 ON」是唯一可达形态，该守卫与「开关 ON」等价，少改一处是一处。

## 实施记录与偏离（落码期回写）

1. **默认值声明位置与提案不同，且这是本批真正的修法**：原提案写「在 `cmake/AuroraBackends.cmake` 里把 `option()` 改成按平台取默认值」，落码后本机裸 configure 仍在 **generate** 阶段报 `The language OBJCXX was requested for compilation but was not enabled`。因由是时序：后端模块在 `add_library(aurora)` 之后才被 `include()`，而语言规则必须在目标定义之前就绪，于是顶层那句 `if (APPLE AND AURORA_BACKEND_MACOS)` 求值时开关尚无默认值——CI 的 macOS 作业显式传 `-D`，缓存里已有值，恰好掩盖了这条。修法是把 `option()` 声明上移到顶层、与语言前置同处，`cmake/AuroraBackends.cmake` 只保留平台守卫与接线，并在两处就地注释写明理由。`codespec/BUILD_OPTIONS.md` §1.1 那句「`include()` 不创建新作用域，各模块内 `option()` 与写在顶层完全等价」由此出现唯一例外，同批写明。
2. **判据 1 兑现**：本机不带任何 `-D` 的裸 configure（Unix Makefiles 生成器；这台虚拟机没有 ninja）退出码 `0`、`CMakeCache.txt` 内 `AURORA_BACKEND_MACOS:BOOL=ON`，全量构建退出码 `0`，`ctest` **449/449 全绿**。`-G Ninja` 与 Makefiles 在此判据上等价——报的错本就发生在 generate 阶段、与编译器无关，而 CI 用的正是 Ninja。
3. **判据 2 兑现**：`cmake -S . -B build -DAURORA_BACKEND_MACOS=OFF` 在 configure 期终止（退出码非零、不产出可用的构建目录），消息含 `not a buildable configuration` 并点名三处因由与三条替代路径。
4. **判据 3 以静态口径兑现，未做非 Apple 真构建**：这台 macOS 虚拟机没有 Linux / Windows 工具链，无法真跑异平台 configure。可证的是分支不可达：`option()` 的 `else ()` 分支只在非 Apple 求值，`enable_language(OBJCXX)` 与新增拒绝都在 `if (APPLE)` 前提内，异平台的默认值与语言面逐字不变；`check_platform_macros` 对 `AURORA_BACKEND_MACOS` 的门控核对同批复跑，异平台真构建由 CI 矩阵兜底，本批不留「已验证」的说法。
5. **CI 面的连带效果（不改 workflow 文件）**：`.github/workflows/release.yml` 与 `.github/workflows/e2e-probe.yml` 的 macOS 作业不传该开关，此前取默认 OFF 即落进「应用域腿按 C++ 编译」的坏形态，现在自动取 ON、无需加 flag；`.github/workflows/ci.yml` 两处显式 `-DAURORA_BACKEND_MACOS=ON`（macOS 构建作业与 lint-macos）与本批默认值重合，属冗余但无害，保留不动——删掉它们会把「显式申报覆盖面」的 CI 纪律一起删掉。
6. **判据 4 的探针登记面不变**：codespec/BUILD_OPTIONS.md §2.4 的 macOS 探针与 CoreAudio 探针条件仍写「`APPLE` 且开对应开关」，本批不动其语义（开关默认值翻 ON 后，Apple 上开 `AURORA_BUILD_VERIFY_TOOLS` 即自动定义这些目标，与旧写法一致）。
7. **判据 5 门禁兑现情况**：`check_change_proposals` / `check_codespec_xref` / `check_code_doc_sync` / `check_no_hardcoded_paths` 本机复跑全绿，全量 `ctest` 与 `format-check` 结果见本批最终通报；`clang-tidy` 在本机不可用（这台机器只有 Apple clang-format，CommandLineTools 不带 clang-tidy，CI 钉 apt.llvm.org 的 llvm-22 快照），故 `lint` 腿留待 CI，不以本机凑数。
