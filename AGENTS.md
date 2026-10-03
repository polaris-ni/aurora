# AGENTS.md — Aurora 协作单一入口（路由 + 硬约束）

> **本文件只做两件事**：① 用一张导航表把你（人或 AI）导到 `codespec/` 里唯一的权威文档；② 列出不可违反的硬约束。
> **细则一律不在这里**——构建选项、测试框架用法、编码规则、CI 矩阵全部下沉到 §4 标注 🥇 的文件，避免双份真相漂移。
> 动手前看 §5 硬规则，动手后按 §4「变更 → 回写落点 + 必跑门禁」闭环。

---

## 1 项目定位

**Aurora** 是一个 C++20 跨平台 **AI-first** GUI 库：声明式 + 响应式 + 概念可枚举，纯软件栅格 `Painter` 渲染，**不依赖 GPU**，以**编译型静态库**交付（`include/` 声明、`src/aurora/*.cpp` 实现）。

| 维度 | 事实 |
|:---|:---|
| 命名空间 / 入口 | `namespace aurora;`（推荐别名 `namespace au = aurora;`）；`#include "aurora/aurora.h"`（伞头）；`aurora_fwd.h` 只前向声明重量级门面类型，供只需指针/引用的 TU 降包含成本 |
| 渲染内核 | 软件 `Painter`——**没有 `Renderer` 接口**，帧级产出 DisplayList |
| 线程模型 | 单线程 UI、同步事件、细粒度响应式信号 |
| 版本状态 | alpha 预览版；版本号以 `CHANGELOG.md` 的 `currentVersion` 为准（`check_version_consistency` 守门），**不构成稳定性承诺**，破坏性变更须按 semver 记入 `CHANGELOG.md` 并给迁移路径（`SPECIFICATIONS.md` §12、`CODING_STANDARDS.md` §7） |

**后端矩阵**（`Surface` 抽象；全部开关语义、依赖与互斥见 `BUILD_OPTIONS.md` §3）：

| 后端 | 宿主 | 开关 | 备注 |
|:---|:---|:---|:---|
| `HeadlessSurface` | 任意 | `AURORA_BACKEND_HEADLESS`（默认 ON） | 内存缓冲 / 离线 PNG，golden 测试基座 |
| `Win32Surface` | Windows | `AURORA_BACKEND_WIN32`（Windows 默认 ON） | GDI，零三方依赖 |
| `D3D11Surface` | Windows | `AURORA_BACKEND_D3D11` | GPU 增量上屏偏置 |
| `GlfwSurface` | 跨平台 | `AURORA_BACKEND_GLFW` | OpenGL 3.3 兼容 profile；`AURORA_ENABLE_GLFW_GPU_GL` 切 GPU 栅格 |
| `WgpuWin32Surface` | Win32/X11/Wayland | `AURORA_BACKEND_GPU_WGPU` | 经 `WgpuRhi` 在 Vulkan/D3D12/Metal/GLES 光栅化，需 Rust 工具链 |
| `X11Surface` / `WaylandSurface` | Linux | `AURORA_BACKEND_X11` / `AURORA_BACKEND_WAYLAND` | Wayland 为原生 wl_shm + xdg-shell + xkbcommon |
| `WasmSurface` | 浏览器 | `AURORA_BACKEND_WASM` | Emscripten / Canvas 2D，rAF 驱动 |
| `MacOSSurface` | macOS | `AURORA_BACKEND_MACOS` | AppKit / CoreGraphics，骨架 |

音频设备后端（`AURORA_ENABLE_AUDIO` / `_WASAPI` / `_ALSA`）、无障碍平台桥（Win32 UIA / Linux AT-SPI2，**无独立开关**）等同属该口径，见 `BUILD_OPTIONS.md` §3.7 / §4 与 `ARCHITECTURE.md` §8.5。

---

## 2 目录布局

| 路径 | 作用 | 细则去处 |
|:---|:---|:---|
| `include/` | 公共 API 头，`aurora.h` 为唯一入口；少量 header-only 控件 | `ARCHITECTURE.md` §4 模块映射 |
| `src/aurora/` | 实现；非模板纯逻辑一律离头 | 同上 |
| `examples/demos/` | `demo_<组件>.cpp` 与公共源文件 1:1（CMake 自动 GLOB）；`demo_common.h` 放共享控件，`scenes/` 放被 E2E 引用的 header-only 场景头（与 demo 同源） | `specification/08-tooling.md` §8.2 |
| `tests/` | `unit/utest_*`、`integration/itest_*`、`e2e/etest_*`（受 `AURORA_BUILD_E2E` 门控）；`framework/` 私有测试框架、`support/` fixture、`fixtures/` 语料、`golden/` 基准图 | `CODING_STANDARDS.md` §3、`ARCHITECTURE.md` §14 |
| `tools/` | `gen/` 三生成器（api / error_codes / debug_api）、`servers/`（`aurora_mcp` / `aurora_lsp` / `aurora_cli`）、`bench/`（5 个基准）、`check/`（门禁脚本 + `perf_gates.json` + 非门禁观测 `build_baseline.py`）、`verify/`（真机验收探针，不进 CTest）、`coverage/`、`e2e/`（`aurora_e2e_client`）、`include/`（共享头，含枚举 SSOT `known_enums.h`） | `cmake/AuroraTools.cmake`、`BUILD_OPTIONS.md` §2.4 |
| `cmake/` | 16 个模块，顶层 `CMakeLists.txt` 只做编排 | `BUILD_OPTIONS.md` §1.1 |
| `codespec/` | **全部项目文档**（见 §4） | — |
| `third_party/` | 三方库源码 | `BUILD_OPTIONS.md` §5 |
| 根：`aurora_api.json` | 生成的 API 描述数据（**非文档，勿移动勿手改**） | `BUILD_OPTIONS.md` §3.6 |
| 根：`CHANGELOG.md` / `CMakePresets.json` / `compile_flags.txt` | 版本与变更记录 / 构建预设 / clangd 编译标志 | — |

`build/`（含 `build-wasm/` 等）为产物目录，不入版控、不得被任何引用指向。

---

## 3 构建 · 测试 · 门禁（复制即用）

```powershell
cmake --preset ninja                    # configure（Ninja + gcc/g++，等价 -G Ninja）
cmake --build build --target <tgt>      # 建库/工具/测试；全量 demo 用 --target demos，单个用 --target demo_lazy_list
ctest --preset ninja-test               # 全量测试 + 全部静态门禁（并行、进程隔离）
cmake --build build --target lint       # Clang-Tidy 门禁（-fix 版为 lint-fix）
cmake --build build --target format-check   # clang-format 门禁（改写用 format）
cmake --build build --target aurora_api_json  # 新增/改动 widget、类型、属性键后刷新
cmake --build build --target docs       # Doxygen，WARN_AS_ERROR=YES，查「注释写了但读不出」
```

- **预设**（`CMakePresets.json`）：`ninja`（默认）、`mingw`、`wasm`（Emscripten）、test 预设 `ninja-test`。
- **工具链门槛**：CMake ≥ 3.20 + 任一 C++20 编译器在 `PATH`；推荐 Ninja。**仓库文档内不得写本机绝对路径**（`check_no_hardcoded_paths`）。
- ⚠️ **demo 不进默认构建**（`EXCLUDE_FROM_ALL`）：`cmake --build build` 只建库/工具/测试。
- ⚠️ **新增 `.cpp` 后须让 CMake 刷新 GLOB**：`CONFIGURE_DEPENDS` 多数情况自动，否则碰一下 `CMakeLists.txt` 或重建 `build/`。
- 全部 CMake 选项 / 编译宏 / 运行时环境变量**只列于** `BUILD_OPTIONS.md`（三层命名：`AURORA_BUILD_*` / `AURORA_BACKEND_*` / `AURORA_ENABLE_*`），此处不重复。

**测试写法（要点，全文见 `CODING_STANDARDS.md` §3.1–§3.2）**：一公共源文件 ↔ 一 `utest_*.cpp` ↔ 一 `demo_*.cpp`；用例经 `AURORA_TEST_CASE(<Case>)` 静态注册，**套件名恒等于文件 stem**（这样 `--run=<stem>` 一定筛得中）；断言用 `AURORA_TEST_CHECK*` / `AURORA_TEST_REQUIRE*`（匹配器 `*_THAT`），生命周期用 `AURORA_TEST_F`，参数化用 `AURORA_TEST_P` / `AURORA_TYPED_TEST_SUITE`；平台专属用例在 feature 宏未开的 `#else` 分支写 `AURORA_TEST_SKIP`。依赖相对路径的测试（含 golden）须**从仓库根**直跑，`ctest` 已设 `WORKING_DIRECTORY`。

### 3.1 变更 → 必跑门禁 + 回写落点

`ctest --preset ninja-test` 已含下列全部 `check_*`（`check_gen_api_merge` / `check_api_schema_sync` 须先构建生成器）。按改动类型优先跑对应项：

| 改了什么 | 必跑门禁（CTest 名 / 目标） | 回写文档 |
|:---|:---|:---|
| 公共 API：函数签名、类/类型、信号、属性键、枚举 | `check_api_schema_sync`（先 `--target aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`lint`、`format-check` | `specification/NN-*.md`（按模块域，见 §4）+ `CODING_STANDARDS.md` §6 + 必要时 `CONCEPTS.md` |
| `include/aurora/aurora.h` 伞头 | `check_umbrella_header` | 同步 `tools/check/umbrella_manifest.txt` 基线（直连集合不得缩减） |
| `core/` 层依赖、模块边界、目录 | `check_core_layer_boundary`、`check_arch_module_map` | `ARCHITECTURE.md` §2 / §4 |
| 平台/后端分支、feature 宏 | `check_platform_macros`、`toggles` 对应矩阵（CI） | `BUILD_OPTIONS.md` §3 / §4 |
| Win32 宿主的 DPI / dp↔物理换算 | `check_dpi_single_source`（换算只许在 `to_physical` / `to_logical`，DPI 只许在 `refresh_scale()` 读）；建窗尺寸那一腿另需真机探针 `--target aurora_verify_win32_dpi`（不进 CTest，100% DPI 环境下判据恒真并记 SKIP） | `specification/08-tooling.md` §8.2 |
| 字符串字面量 | `check_no_cjk_literals` | — |
| NOLINT 豁免排版 | `check_nolint_layout` | `CODING_STANDARDS.md` §5.2 |
| 测试文件 | `registry_integrity`、`check_test_temp_hygiene`、`framework_selftest` | `CODING_STANDARDS.md` §3 |
| `codespec/` 文档与交叉引用 | `check_codespec_xref`、`check_code_doc_sync` | — |
| `codespec/manual-test/*.md` | `check_manual_test_format` | — |
| 版本号 / `CHANGELOG.md` | `check_version_consistency` | `CODING_STANDARDS.md` §7 |
| 任何代码行为 | 相关 `ctest -R <stem>`、`--target docs` | 见 §4 权威表 |

CI 全量矩阵（core / backends / toggles / asan / coverage / wasm / install-consumer / lint / lint-wasm / fmt 十组作业）见 `ARCHITECTURE.md` §14.4 与 `.github/workflows/`。

### 3.2 AI 常犯错清单（反例 → 正确做法）

| 反例 | 正确做法 |
|:---|:---|
| 凭训练记忆调用「应该有」的 API | 先 `grep` / 查 `aurora_api.json` / 查 `GUIDELINE.md` 配方，找不到就当不存在 |
| 顺手把 `protected` 虚回调改 `public`、把私有字段挪区 | 保持改动前的访问区，除非本次语义确实要求（并在说明里写理由） |
| 用 `#ifdef` 包住整个虚函数/成员**定义** | 定义无条件写、分支放进函数体——否则 Release vtable 悬空引用 |
| `#if` 包住 `AURORA_TEST_CASE` 声明 | 条件编译只能在**用例体内**，声明须无条件可见，否则 `registry_integrity` 单边失踪 |
| 测试文件里自己写 `main()` | `main` 由 `tests/framework/test_main.cpp` 唯一提供 |
| 新增 `std::cout` / `printf` / `puts` | 走 `AURORA_LOG_*`（stderr）/ `AURORA_LOG_RAW`（stdout 产品输出）/ 测试与工具里的 `AURORA_TEST_PRINTF*` |
| 字符串字面量里写中文 | 字面量写英文 + ASCII 标点；确属功能必需中文时就地 `CJK-LITERAL: <类别> - <原因>` |
| 只写 `@brief` 就交差 | 按 `CODING_STANDARDS.md` §13.5.2「命令必选矩阵」补齐 `@param[in/out]`、`@return`、`@tparam`、枚举项/常量说明 |
| 在代码/文档留 `P0` / `Phase 1` / 旧编号 | 一律改成语义化表述（唯一稳定标识是需求 ID 与文档章节号） |
| 引用本机路径、临时草稿、构建目录 | 只引用随仓库分发的相对路径 + 真实存在的章节锚点 |
| 改完代码不回写文档 | 按 §3.1 表最后一列补齐；文档与运行时冲突时以代码为准并回填文档 |
| 把 demo 当成默认构建目标 | `--target demos` 或按名单独构建 |

---

## 4 文档导航（codespec/）

**顶层 6 份自包含正文** + **子系统规格 9 份** + **生成物** `ERROR_CATALOG.md` + **数据源** `errors.toml` / `debug_api.toml`（后三者路径被 `tools/` 与 `src/aurora/core/diagnostics.cpp` 硬编码，不可移动）+ **待评审手工测试记录** `manual-test/`（22 份，按约定**不进本导航、不被其他文档引用**，格式由 `check_manual_test_format` 守护）。

| 你要解决的问题 | 读这个（🥇 = 该领域唯一权威，冲突时以它为准） |
|:---|:---|
| 项目定位、设计原则、30 条需求清单、文档地图、版本门禁 | `codespec/SPECIFICATIONS.md`（§5 特性清单 / §8 文档导航 / §12 版本与稳定性门禁） |
| 架构、分层、运行时、模块映射、数据流、事件、渲染、11 条设计不变量、性能、测试与 CI | 🥇 `codespec/ARCHITECTURE.md`（§14 测试与 CI 架构） |
| 编码规则：错误处理、命名、日志纪律、契约标注、注释形态、字面量语言、AI 友好性、SemVer、**提交信息** | 🥇 `codespec/CODING_STANDARDS.md`（§10 提交信息 / §13 注释 / §14 字面量） |
| CMake 开关、缓存变量、feature 宏、运行时环境变量、find_package 集成、CMake 模块布局 | 🥇 `codespec/BUILD_OPTIONS.md` |
| 概念语义、跨框架对照（React / Flutter / Qt）、状态作用域决策树、避免误用命令式写法 | `codespec/CONCEPTS.md` |
| 复制即用的最小可编译配方（42 组） | `codespec/GUIDELINE.md` |

**子系统规格（9 份，按 `include/aurora/` 模块域切分；需求 ID 全表见 `SPECIFICATIONS.md` §5）**：

| 文件 | 覆盖 |
|:---|:---|
| `specification/01-core.md` | `core/`：几何与尺寸意图、错误与结果、诊断与降级、日志、线程池 |
| `specification/02-state.md` | `state/`：信号原语、订阅生命周期、`Store`、异步与协程 |
| `specification/03-layout-render.md` | `layout/` `render/` `image/` `media/`：布局协议、Flex/Grid、Painter、字体、Surface 与后端 |
| `specification/04-widget.md` | `widget/` `ui/`：控件基类契约、自描述、控件清单、可定制性契约 |
| `specification/05-event-navigation.md` | `event/` `animation/` `navigation/`：事件、命中测试、焦点、手势、动画、页面栈 |
| `specification/06-app-platform.md` | `app/` `window/` `preferences/` `storage/` `perf/` `debug/`：驱动、帧循环、窗口、定时任务、平台 Shell、持久化（§9.2）、调试门面 |
| `specification/07-environment-modifier.md` | `environment/` `theming/` `i18n/` `modifier/`：环境注入、媒体查询、装饰、主题、国际化、Modifier |
| `specification/08-tooling.md` | 序列化 / 代码生成 / YAML、控件树检查、Inspector、自描述发现、MCP / CLI / LSP、测试原语、日志通道（§9） |
| `specification/09-cli.md` | `cli/`：argv 语法、声明表与静态校验、字面量强类型、`cli-*` 错误码、usage / help / schema |

> **模块存在性提醒**：`a11y` 横跨 `core/`（类型 / 事件 / 桥抽象）与 `widget/`（语义树与快照，需 `Widget` 完整定义，见 `ARCHITECTURE.md` §8.5）；`audio` 归属 `media/`（音频图 API 恒编译，设备后端经 `AURORA_ENABLE_AUDIO` 编入）。二者未单列 spec 文件，但**不是不存在的模块**。

**编号写法**：文档章节号一律纯数字点分层级（`1` / `1.1` / `1.1.1`），禁中英文序号混排；需求 ID 形如 `SPEC.<类目>.<域>.[<子域>…]` + 语义短名 + 三位数字尾，前缀相同才递增，是与章节号并存的稳定标识体系（细则见 `CODING_STANDARDS.md` §11）。

---

## 5 硬规则（编号对外有引用，勿改序）

1. **改公共 API 前先读、改完后回写**：动手前读 `SPECIFICATIONS.md` 与 `ARCHITECTURE.md` 保持契约与不变量一致；落码后按 §3.1 的映射回写文档。
2. **代码与文档必须同步**：公共 API（签名、类/类型、信号、属性键、命名空间）或核心设计一旦增删改，同一批次提交内更新对应 `codespec/` 文档；映射见 §3.1。
3. **新增 widget / 类型必须刷新 schema**：`cmake --build <build> --target aurora_api_json`，并受 `CODING_STANDARDS.md` §6「AI 友好性」（强类型、命名序、默认参数）约束；漂移即 `check_api_schema_sync` 红灯。
4. **不得凭训练记忆假设某个 API 存在**——以头文件与 `aurora_api.json` 为唯一事实。
5. **文档一律放 `codespec/`**：根目录只留 `AGENTS.md` 与数据文件（`aurora_api.json`、`CHANGELOG.md` 等），变更记录写 `CHANGELOG.md`。
6. **概念语义有疑问查 `CONCEPTS.md`** 的跨框架映射，别按 React/Flutter 的命令式直觉写代码。
7. **新增公共 API / widget / 核心逻辑必须配套单测**（`tests/unit/`，GLOB 自动接入 CTest）：优先覆盖构造与不变量、属性与信号读写订阅、序列化往返（`toJson`/`fromJson`/`diff`/`apply_patch`）、跨平台纯逻辑；要像素就用 `HeadlessSurface`，**不引入 GUI 交互测试**；一次性示例/演示须在说明里标注「无单测」。
8. **禁止直接触达标准输出**：日志走 `AURORA_LOG_*`（stderr）；程序产品的输出走 `AURORA_LOG_RAW`（stdout，无前缀）；`tools/` `examples/` `tests/` 遗留 printf 风格用 `AURORA_TEST_PRINTF*` 桥接；唯一允许裸写 sink 的是 `src/aurora/core/log.cpp`。详见 `CODING_STANDARDS.md` §4.1 与 `specification/08-tooling.md` §9。
9. **不得无故改变既有可见性**（`public`/`protected`/`private`）：保持被改符号改动前所处访问区；仅当本次改动语义确实要求时调整，且显式说明理由。
10. **禁止任务/优先级/阶段编号**（`P0`、`D1`、`Phase 1`、`T1`、`Step 3` 之类）出现在代码、提交信息、`codespec/` 文档与本文件——编号重排即成反向错误；一律改语义化表述。唯一例外是 §4 的需求 ID 体系。本地 `*.draft.md` 可自用编号，但提升进仓库前必须改写。
11. **引用必须可达且随仓库分发**：文档引用写仓库相对路径 + 真实章节锚点（如 `ARCHITECTURE.md` §8.5），代码引用写路径不写行号；**禁引** `*.draft.md`、`.workbuddy/`、`.codebuddy/`、`build*/`、本机绝对路径与个人目录；未落地的规划须标注「计划 / 待建」。
12. **公共 API 注释按 `CODING_STANDARDS.md` §13 写全**：`///`（成员尾注 `///<`）、命令一律 `@` 前缀、`@brief` 居块首，并按 §13.5.2 矩阵补齐 `@param`/`@return`/`@tparam`/枚举项/常量说明；实现叙述与 TODO 用 `//` 且不紧贴可文档化声明；描述以代码实际行为为准，禁零信息套话；豁免写 `DOC-EXEMPT: <规则> <原因>`。
13. **字符串字面量不得含中文**（`CODING_STANDARDS.md` §14）：注释可中文，字面量会抵达不受本库控制的终端代码页（GBK 下必乱码）；例外仅限功能必需中文并就地标 `CJK-LITERAL: <类别> - <原因>`，**诊断文案不属例外**。

> 上述规则的**增量**由 §3.1 表中的 CTest 门禁把关（`check_doc_comments`、`check_no_cjk_literals`、`check_codespec_xref`、`check_code_doc_sync` 等），存量豁免以白名单内置于脚本并注明原因；门禁自身输出必须全 ASCII，否则日志读不清。
