# CHG-031 文件与目录选择器在非 Windows 平台的真实实现

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-031 |
| 提出日期 | 2026-10-22 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `include/aurora/app/file_dialog.h`（`#else` 分支内 Linux / macOS 两支由 `inline` 定义改为声明，WASM / Android / BSD 支**保留** `inline` 定义体；公共签名一字不改）、`include/aurora/app/application.h`（新增一枚薄公共逃生舱 `run_modal(...)`）、新增 src/aurora/app/detail/ 下内部头与 src/aurora/app/file_dialog_selfdrawn.cpp（平台中性实现，Linux 与 macOS 桌面平台编译）、`src/aurora/app/file_dialog_win32.cpp`（Win32 腿不动）、`tests/unit/utest_file_dialog.cpp`（并入新增用例，不新建文件）、`codespec/specification/06-app-platform.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/19-app.md`；无新增公共 widget / 枚举 / 错误码，`aurora_api.json` 预期零差异（理由见变更内容第 8 条） |

## 动机

1. **三个入口的公共签名齐备，真实实现只有一条腿。** `open_file` / `save_file` / `open_folder` 与三个 headless 钩子、`interactive` 开关在公共头上一应俱全，但真实实现只有 Windows 的 `IFileDialog` 系（`src/aurora/app/file_dialog_win32.cpp`）。非 Windows 平台落在 `include/aurora/app/file_dialog.h` 的 `#else` 分支：三段 `inline` 回退各自 `(void)opts` 丢弃全部 `Options`、不弹任何 UI、也不看 `interactive`，只回钩子或空串。
2. **这是静默降级，不是「不支持」。** 调用方在非 Windows 平台上拿到的 `Ok(空串)` 与「用户主动取消」逐位不可分辨，而公共注释却承诺了 `title` / `initial_dir` / `filters` 三项语义——文档承诺面与行为面整体脱节。「自选目录」这类交互因此直接等价取消，且不留下任何可归因的痕迹。
3. **依赖纪律排除了两条显而易见的捷径。** 引入 GTK / Qt 会把新的链接期桌面依赖加到每一个使用本库的应用上；引入 DBus 客户端则要为一条对话框通路拉进事件循环、异步请求与信号收尾。故必须在「不新增第三方依赖」的前提下补齐这条腿。
4. **Win32 腿自身也有两处待对齐的缝**（本提案只记录、不在本批修）：`save_file` 与 `open_folder` 未接 `initial_dir`（`src/aurora/app/file_dialog_win32.cpp` 仅 `open_file` 路径有接线注释）；`Show` 的非 COM 类失败目前是 WARN + 空而非 `Error`，与头注释「仅 COM 初始化或对话框创建失败返回 Error」有缝。
5. **补齐后消费侧零改动**：三个入口的签名与钩子语义两腿共用同一份，接线判据可无头证，既有调用点无需迁移。

## 变更内容

1. **选路裁决：框架自绘文件 / 目录选择器**（用既有 widget、命中盒与浮层宿主画，零第三方依赖）。三条理由：**零新增依赖与运行期前提**（不引入链接期桌面库，也不把「装了某个外部二进制」变成运行期前提）；**与本库既有形态同源**（X11 / Wayland 下无 xdg-decoration 时本库已在自绘标题栏装饰，`include/aurora/widget/drawer.h` 的 `ProgressDialog` 亦为库内自绘模态先例）；**可无头断言**（状态机层完全脱离 UI 无头证，不引入 GUI 交互测试）。
2. **否决的另两路（写入提案以免后续重复讨论）**：
   - **外部子进程**（zenity / kdialog 一类）：一条 `--file-selection --directory` 即可同步拿结果、工程量最小，但把「装了这个二进制」变成运行期前提，且 GNOME / KDE / 无桌面环境三态要分别兜底，无法无头断言。
   - **xdg-desktop-portal**：`FileChooser` 方法形态为异步（回 handle、结果走 `Request::Response` 信号），需要 DBus 客户端与超时上界；本库无 DBus 依赖，且「库不带 DBus 客户端、异步信号收尾与超时治理成本」是独立且更重的否决理由。
3. **两层架构（把成本关进一个提案内）**：

   | 层 | 内容 | 落点 | 无头性 |
   |:---|:---|:---|:---|
   | L0 数据源 | `DirectorySource` 概念：`list_dir(path)` / `is_dir`；默认 `std::filesystem` 实现 | 内部头，落 src/aurora/app/detail/ | mock 注入即测 |
   | L1 状态机 | 当前目录、条目、过滤匹配、选择、确认 / 取消；纯逻辑 | 内部 TU | 完全无头 |
   | L2 UI 装配 | `include/aurora/widget/dialog.h` 的 `Dialog`（居中内容 + 遮罩 + 焦点作用域 push/pop）+ `include/aurora/widget/data_widgets.h` 的 `ListView` / `TreeView` + `include/aurora/widget/text_input.h` 的 `TextInput`（保存文件名）+ Button；命中与焦点复用既有 | 内部 TU | 离屏逻辑快照 |
   | L3 平台入口 | 三函数体：钩子优先 → `interactive == false` 回空 → 有 Application 上下文走 run_modal、无上下文 WARN 回空 | 新 TU + `src/aurora/app/file_dialog_win32.cpp`（不动） | 脚本化点击收尾 |

4. **形态重定（第二轮复审实测裁决，取代初版「自由函数内自建帧循环」）**：原形态不可实现——`Application::step_frame()` 是 private（application.h:598），三个入口是 namespace 级自由函数、手上没有 Application 上下文；另建 Application 会撞进程级单例（Scheduler::set_current / Animator::set_current / Task::set_main_poster / main_poster / current_focus_manager / FrameStats::instance），且内层退出会把它们置空导致外层半身不遂。改为三件：
   - **（a）选择器做成纯 widget**：内部复合体 `FilePickerView`，只承载状态机与装配，**自带零帧循环**；
   - **（b）新增一枚薄逃生舱 `Application::run_modal(...)`**：挂在 Application 上以拿到上下文，入口保存上述进程级单例、出口还原，然后进循环；
   - **（c）三个同步入口的路由**：有当前 Application 上下文 → 走 run_modal；无上下文（无头 / 纯库调用 / 尚未 run）→ 按既有口径 WARN 回空（与「不静默说谎」一致）。
5. **已裁决的形态对比表**：

   | 维度 | 原形态（自由函数内嵌帧循环） | 新形态（纯 widget + run_modal） |
   |:---|:---|:---|
   | 公共面增量 | 无，但不可实现（private 帧循环 + 进程级单例冲突） | 一枚 `Application::run_modal(...)`，薄且不扩张语义 |
   | 与 CHG-032 记账的相互影响 | 无处挂抑制标志，模态期点击会翻转 `active_window` | 可在 run_modal 入口置抑制、出口还原（第 6 条） |
   | 无头可测性 | 无 Application 上下文即完全不可测 | 无上下文分支可无头断言；有上下文走有限帧驱动 |

6. **与 CHG-032 的互引裁决（落码次序：031 在 032 之前）**：模态期间派发不记账——嵌套循环里的点击经 `WindowHost::dispatch` 会翻转 `active_window`，故 run_modal **入口置抑制标志、出口还原**。031 先落码，032 的记账点全集一次成型，否则 032 落码后要为模态抑制再改一遍。
7. **执行模型（定死）**：三个入口同步返回 `Result<T>`，真交互路径在调用线程内阻塞到用户作答（Qt `QFileDialog::exec()` 同口径）；与系统拖放「不引入嵌套模态循环」的裁决不矛盾（那条裁决的出发点是会话发起方要立刻返回）。硬约束据此改为：**允许恰好一层嵌套**（选择器 A 的确认回调内再开 B，结果走实例局部变量、不经全局钩子），**更深即拒绝并 WARN 回空**——与判据 5 同口径，不再写「禁止重入 run()」。选择器运行期间宿主自己的关闭请求照常进入 CHG-034 闸门。
8. **不做清单（独立）**：① 选择器复合体**不进 WidgetRegistry、不新增公共 widget**（这是「`aurora_api.json` 零差异」判据成立的前提：该文件无 classes / methods / functions 段，唯一会动的段就是 widgets）；② 不做自由函数内嵌帧循环；③ 不做 `include/aurora/app/recent_documents.h` 的隐式写入（写入归调用方）；④ 不做异步 / 回调式入口；⑤ 不扩展 `Options`、不新增 feature 宏（平台寄生于平台 TU，照 CHG-027 横切纪律）。
9. **平台族三分支**：本提案中「非 Windows」仅指桌面平台 Linux 与 macOS。`#else` 改为显式三分支：`#if defined(AURORA_PLATFORM_WINDOWS)`（Win32 COM，不动）/ `#elif defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)`（桌面自绘）/ `#else`（WASM / Android / BSD 等）**保留 `inline` 定义体**——前两支改为声明后，若第三支也改成声明则三支函数体无人定义。若要在第三支改为 WARN 回空，须接受公共头新增 `include/aurora/core/log.h` 依赖，并把 check_core_layer_boundary 与 check_arch_module_map 的层间影响写清；本提案选「保留定义体」。WASM 主线程不可阻塞、文件系统是 MEMFS 而非用户真实磁盘，**不做真实选择器**，仅在注释与规格中如实申报。
10. **新增 TU 的编译期自防护**：照 `src/aurora/app/file_dialog_win32.cpp` 首部的空壳宏模式做内部守卫，非目标平台编译为空 TU。**CMake 零改动**——`CMakeLists.txt` 对 src 用 GLOB_RECURSE(CONFIGURE_DEPENDS) 自动收编；唯一要留意的是 unity 合并的排除清单不把新 TU 卷进去。
11. **必须保留的既有语义（两腿共用，改动即回归）**：三个 headless 钩子**优先**（置非空即直接返回且不构造选择器）；`interactive == false` 不弹 UI 直接回空；**取消不是失败**（用户取消、目录不可选、无可用通道一律 `Ok(空)`，仅系统级异常回 `Error`，见判据 10）。
12. **必须真消费 `Options`**：`title` 作浮层标题、`initial_dir` 作初始位置；`open_folder` 忽略 `filters`（目录无类型语义），**三腿统一在头注释写明「忽略」**并可留一条 DEBUG 日志。若某平台态确实拿不出选择器，须 `AURORA_LOG_WARN`（tag `file_dialog`，文案英文）留痕后回空，不弹半残浮层、不回假路径。
13. **错误码复用、不新增**：桌面腿系统级失败钉死复用既有 `platform-dialog-create-failed`（`codespec/errors.toml`、`codespec/ERROR_CATALOG.md` 在册），Win32 腿沿用 `PlatformComInitFailed`，四份合计 errors.toml 零新增。
14. **性能口径**：目录枚举是 UI 线程上的同步 I/O，须按需创建、关闭即释（不常驻），并在规格中留记「超大目录的枚举帧卡顿风险」，第一版不设上限、不后台化。

## 验收判据

1. **L1 状态机无头全覆盖**：mock 数据源下断 `initial_dir` 定位、进入子目录 / 回父目录、`filters` 匹配、确认返回**绝对路径**、取消返回空。
2. **`initial_dir` 不存在 / 不可读** → 降级到数据源根 + 一条可归因 `AURORA_LOG_WARN`，不挂起、不抛。
3. **`Options` 真生效（无头部分与 UI 部分拆开）**：无头只断言 `initial_dir` 传到了内部状态机的**当前目录字段**（直接读该字段，不为它新增公共 getter）；**UI 观测点**（浮层标题字段、首节点文本）走离屏逻辑快照与 manual-test 真机走查，**不计入 ctest**。
4. **嵌套帧循环可退（改挂 run_modal）**：有 Application 上下文 + 有限帧驱动，脚本化触发确认（Button 回调直调）→ run_modal 退出且返回值 = 所选，且**退出后进程级单例已还原**（与入口快照逐项相等）；取消同理退出且返回空。
5. **可重入（与变更内容第 7 条同口径）**：**允许恰好一层**嵌套——选择器 A 的确认回调内再开 B，两者结果互不串；**第二层嵌套请求即拒绝并 WARN 回空**（原「禁止在 `open_*` 重入路径再次进入 run()」作废）。
6. **钩子三态（按腿拆分）**：
   - **桌面自绘腿**：钩子置值 → 优先返回且**未构造浮层**（用计数器证未进入 UI 路径）；钩子空 + `interactive=false` → 回空；钩子空 + `interactive=true` → 进入 UI 路径（由脚本化点击收尾）。
   - **Win32 腿**：只证「钩子优先」与「`interactive=false` 回空」两分支（既有 `tests/unit/utest_file_dialog.cpp` 已覆盖）；**不断「钩子空 → 回空串」**——Win32 钩子空且 interactive 时会弹真框等用户，那条断言在 Win32 上不成立。
7. **无 Application 上下文即等价非交互**：无头 / 尚未 run / 纯库调用时 `interactive=true` + 钩子空 ⇒ WARN 后回空、不得挂起。**直接对三个自由函数断言**，不构造窗口、不依赖指针能力观测对象（原形态下的观测对象在自由函数形态里根本不存在）。
8. **WASM inert**：`AURORA_PLATFORM_WASM` 分支编译期 inert + WARN 回空（代码审查 + wasm 构建 CI 证，native 单测不可模拟宏）。
9. **取消无副作用**：取消后全局钩子 / `interactive` / 数据源零改动。
10. **取消 → `Ok(空)`、不报 Error**（新增，承接原第 11 条的变异指向）：取消、目录不可选、无可用通道三条路径均返回 `Ok(空串)` 而非 `Error`；仅系统级异常回 `Error`。
11. **Win32 腿不回归**：既有 `tests/unit/utest_file_dialog.cpp` 全绿且断言零改写。
12. **门禁**：`check_api_schema_sync`（先 `--target aurora_api_json`；预期零差异——无新增公共 widget，理由见变更内容第 8 条）、`check_naming_conventions`、`check_doc_comments`（新 TU 的函数级 `@note Thread: main-thread only` 须补全）、`check_api_budget`、`check_platform_macros`、`check_no_cjk_literals`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`registry_integrity`、`check_version_consistency`、`check_test_registry`、`check_no_hardcoded_paths`、`check_gen_api_merge`、`check_arch_module_map`、`check_core_layer_boundary`（新 TU 落 src/aurora/app/ 并依赖 widget/ 与 std::filesystem，是一条新的层间边）全绿；`lint`（`--print-tus` 确认新 TU 入集）与 `format-check` 全绿；按 ARCHITECTURE.md §14.5 补跑 wasm 与 Linux X11/Wayland 两配置；全量 `ctest` 绿。
13. **变异自证**：把 `initial_dir` 改成硬编码路径 → 判据 3 转红；把钩子优先级移到浮层之后 → 判据 6 转红；把取消改回 `Error` → 判据 10 转红；去掉降级 WARN → 判据 2 转红；把「无上下文即回空」改成硬建 Application → 判据 7 转红；把 run_modal 出口的单例还原删掉 → 判据 4 转红。

## 回写落点

- `codespec/specification/06-app-platform.md`（文件 / 目录选择器节：两腿实现现状与三分支平台口径、`Options` 三键在各腿的消费口径、取消与降级的错误语义、钩子与 `interactive` 的优先级次序、**run_modal 执行模型与「无上下文即等价非交互」路由**、恰好一层嵌套的裁决、与 CHG-032 的模态期派发抑制、WASM inert 申报、性能口径）
- `codespec/SPECIFICATIONS.md`（§5 特性表：跨平台一致行为口径下「选择器」一行的现状复核；必要时附各平台实现形态说明）
- `codespec/manual-test/19-app.md`（取下一个可用序号 `TC-APP-014`；既有 file_dialog 覆盖行由「headless 钩子与取消路径由单测覆盖」改写为「状态机 / 离屏快照入 utest、真机弹框走查保留」；体例对标 `codespec/manual-test/23-net.md`）
- `codespec/ERROR_CATALOG.md` 与 `codespec/errors.toml`（核对复用 `platform-dialog-create-failed`，零新增 slug）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 版本与发布记录：条目类型 Added，落仓库根变更记录的 [Unreleased] 节；**本批不单独升 alpha 号**，四份收口时一次性三处同步（该记录顶部 currentVersion、CMakeLists.txt 的 AURORA_VERSION_SUFFIX、核心版本头兜底串）并跑 check_version_consistency。上述文件均不属 codespec 树，按 AGENTS.md 硬规则 5 只在此说明、不列为回写落点。

## 消费侧用法（最小示例）

```cpp
// 阻塞直到用户作答；取消或平台起不来同为空串（调用方不必分支）
const auto r = au::file_dialog::open_folder({.title = "选择密钥目录", .initial_dir = current_dir});
if (r && !r->empty()) { apply(*r); }   // 空串 = 取消或平台无选择器
// 自动化环境：测试装配层预设钩子，或把 interactive 置 false，两种都不弹 UI
au::file_dialog::headless_folder_result = "/tmp/fixture";
```
