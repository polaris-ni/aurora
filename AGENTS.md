# AGENTS.md — Aurora 协作单一入口（路由 + 硬约束）

> 本文件只做两件事：① 把人 / AI 导到 `codespec/` 里的唯一权威文档；② 列出不可违反的硬约束。**细则一律不在这里**（体量受门禁 `check_agents_size` 守护，超 8 KiB 红灯）；完整导航见 §4。

---

## 1 项目定位

C++20 跨平台 **AI-first** GUI 库：声明式 + 响应式 + 概念可枚举，纯软件栅格 `Painter` 渲染（**无 `Renderer` 接口、帧级 DisplayList、不依赖 GPU**），单线程 UI + 同步事件 + 细粒度响应式信号，编译型静态库交付。

- 命名空间 `aurora`；唯一伞头 `aurora/aurora.h`。
- alpha 预览版，**不构成稳定性承诺**；版本号以 `CHANGELOG.md` 顶部 `currentVersion` 为准（`check_version_consistency` 守门），破坏性变更按 semver 记入并给迁移路径（`SPECIFICATIONS.md` §12）。

## 2 目录布局

代码在 `include/`（公共头）+ `src/aurora/`（实现，非模板纯逻辑离头）；`tools/`（生成器/服务器/bench/门禁/真机探针）、`cmake/`（16 模块，顶层编排）、`third_party/`；`tests/`（`utest_`/`itest_`/`etest_`）、`examples/demos/`（与公共源 1:1，CMake GLOB）；**文档全在 `codespec/`**。根留 `AGENTS.md`+数据 `aurora_api.json`（勿手改）、`CHANGELOG.md`、`CMakePresets.json`、`compile_flags.txt`；`build*/` 产物不入版控、不可引用。

## 3 构建 · 测试 · 门禁（复制即用）

```powershell
cmake --preset ninja                    # configure（Ninja + gcc/g++）
cmake --build build --target <tgt>      # 建库/工具/测试；全量 demo 用 --target demos
ctest --preset ninja-test               # 全量测试 + 全部静态门禁（并行、进程隔离）
cmake --build build --target aurora_api_json  # 改动 widget / 类型 / 属性键后刷新
```

- 预设 `mingw`/`wasm`；变体 `ninja-shards`（分片=4）、`measure`（ccache 关，供耗时对照）；门禁 `lint`（Clang-Tidy）、`format-check`（clang-format，改写 `format`）、`docs`（Doxygen）。门槛 CMake ≥ 3.20 + C++20 编译器。
- demo 不进默认构建（`EXCLUDE_FROM_ALL`）；新增 `.cpp` 须刷 GLOB（`CONFIGURE_DEPENDS` 多自动，否则碰 `CMakeLists.txt`）；禁本机绝对路径（`check_no_hardcoded_paths`）。开关/宏/变量只列 `BUILD_OPTIONS.md`。
- **测试写法**见 `CODING_STANDARDS.md` §3.1–§3.2：一源 ↔ 一 `utest_*.cpp` ↔ 一 `demo_*.cpp`；`AURORA_TEST_CASE` 静态注册、**套件名=文件 stem**；feature 宏未开 `#else` 写 `AURORA_TEST_SKIP`；含 golden 从仓库根直跑。

### 3.1 变更 → 必跑门禁

`ctest --preset ninja-test` 含全部 `check_*`（生成器类先建）。**完整矩阵见 `ARCHITECTURE.md` §14.6**，高频几类，回写落点见 §4：

| 改了什么 | 必跑门禁 |
|:---|:---|
| 公共 API（签名/类/信号/属性键/枚举） | `check_api_schema_sync`（先 `aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`lint`、`format-check` |
| `core/` 依赖/边界/目录/伞头 | `check_core_layer_boundary`、`check_arch_module_map`、`check_umbrella_header` |
| 平台/后端分支、feature 宏 | `check_platform_macros` + CI `toggles` 矩阵 |
| `codespec/` 文档与交叉引用 | `check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_change_proposals` |
| `AGENTS.md` 本身 | `check_agents_size`（> 8 KiB 红灯，超限下沉勿放宽） |
| 版本号/`CHANGELOG.md` | `check_version_consistency` |

---

## 4 文档导航（codespec/）

顶层 6 份正文（标「权威」= 该领域唯一权威，冲突时以它为准）+ 子系统规格 9 份 + 生成物 `ERROR_CATALOG.md`（数据驱动、路径硬编码不可移动）+ `manual-test/`（22 份手工测试，不进本导航）+ `changes/`（变更提案，契约见 `changes/README.md`）。

| 你要解决的问题 | 读这个 |
|:---|:---|
| 定位、设计原则、需求清单（30 条）、文档地图、版本门禁 | `SPECIFICATIONS.md`（§5 特性 / §8 地图 / §12 版本门禁） |
| 架构、分层、运行时、模块映射、数据流、事件、渲染、设计不变量（11 条）、性能、测试与 CI、端到端流程、门禁矩阵 | 权威 `ARCHITECTURE.md`（§14 测试与 CI / §15 变更落地流程） |
| 错误处理、命名、日志纪律、注释形态、字面量语言、AI 友好性、SemVer、提交信息、反例速查 | 权威 `CODING_STANDARDS.md`（§10 提交 / §13 注释 / §14 字面量 / §15 反例） |
| CMake 开关、缓存变量、feature 宏、环境变量、find_package 集成 | 权威 `BUILD_OPTIONS.md` |
| 概念语义、跨框架对照、状态作用域决策树 | `CONCEPTS.md` |
| 最小可编译配方（42 组） | `GUIDELINE.md` |

子系统规格 9 份（`codespec/specification/`，模块域→文件映射见 `SPECIFICATIONS.md` §5）：`01-core.md`、`02-state.md`、`03-layout-render.md`、`04-widget.md`、`05-event-navigation.md`、`06-app-platform.md`、`07-environment-modifier.md`、`08-tooling.md`、`09-cli.md`。

> 模块存在性：`a11y` 跨 `core/`+`widget/`，`audio` 属 `media/`（经 `AURORA_ENABLE_AUDIO`）；均未单列 spec 但非不存在。编号写法见 `CODING_STANDARDS.md` §11。

---

## 5 硬规则（编号对外有引用，勿改序）

1. **改公共 API 前先读、改完回写**：先读 `SPECIFICATIONS.md` 与 `ARCHITECTURE.md` 对齐契约/不变量，落码按 §3.1 回写。
2. **代码与文档同批次同步**：公共 API（签名/类/类型/信号/属性键/命名空间）或核心设计增删改，同批提交更新对应 `codespec/`。
3. **新增 widget/类型须刷 schema**：`--target aurora_api_json`，受 `CODING_STANDARDS.md` §6「AI 友好性」约束；漂移即 `check_api_schema_sync` 红灯。
4. **不得凭训练记忆假设某 API 存在**——头文件与 `aurora_api.json` 是唯一事实。
5. **文档一律放 `codespec/`**：根目录只留 `AGENTS.md` 与数据文件；变更记录写 `CHANGELOG.md`。
6. **概念语义有疑问查 `CONCEPTS.md`** 跨框架映射，勿按 React/Flutter 命令式直觉写码。
7. **新增公共 API/widget/核心逻辑须配套单测**（`tests/unit/`）：覆盖不变量、属性与信号读写订阅、序列化往返、跨平台纯逻辑；像素用 `HeadlessSurface` 且**不引入 GUI 交互测试**；一次性示例标「无单测」。
8. **禁止直触标准输出**：日志走 `AURORA_LOG_*`（stderr）、产品输出走 `AURORA_LOG_RAW`（stdout 无前缀）、测试与工具用 `AURORA_TEST_PRINTF*`；唯一例外 `src/aurora/core/log.cpp`（细则 `CODING_STANDARDS.md` §4.1）。
9. **不得无故改变既有可见性**：保持被改符号原访问区；仅当本次语义要求时调整并说明理由。
10. **禁止任务 / 优先级 / 阶段编号**（`P0`、`D1`、`Phase 1`、`Step 3`）出现在代码、提交信息、`codespec/` 与本文件；一律改语义化表述，唯一例外是需求 ID 体系。
11. **引用须可达且随仓库分发**：文档引用写仓库相对路径+真实章节锚点，代码引用不写行号；**禁引** `*.draft.md`/`.workbuddy/`、`build*/`本机绝对路径；未落地规划标「计划/待建」。
12. **公共 API 注释按 `CODING_STANDARDS.md` §13 写全**：`///`（尾注 `///<`）、命令 `@` 前缀、`@brief` 居首，按 §13.5.2 补 `@param`/`@return`/`@tparam`/枚举项；豁免写 `DOC-EXEMPT: <规则> <原因>`。
13. **字符串字面量不得含中文**（`CODING_STANDARDS.md` §14）：注释可中文，字面量抵不受本库控的终端代码页；例外仅限功能必需中文并就地标 `CJK-LITERAL: <类别> - <原因>`。
14. **改动先落变更提案再落码**：触及公共 API/分层边界/后端矩阵或性能门槛，先在 `codespec/changes/<语义短名>/proposal.md` 写清动机、变更内容、验收判据与回写落点，状态按 `已提议 → 实施中 → 已归档` 流转；日常小修无需提案。

> 规则增量由 §3.1 门禁把关；存量豁免白名单内置脚本并注原因；门禁输出必须全 ASCII。