# AGENTS.md — Aurora 项目结构与文档导航

> 本文件是 **AI 编码助手（及人类协作者）访问 Aurora 项目的单一入口**。
> 改代码 / 写文档 / 理解设计前，先读本文件，再按下方导航表定位到 `codespec/` 下的具体文档。

---

## 1. 项目定位（一句话）

**Aurora** 是一个 C++20 跨平台 **AI-first** GUI 库：以「声明式、响应式、概念可枚举」为设计内核， 使用纯软件栅格 `Painter` 渲染， **不依赖 GPU**；以 **编译型静态库** 形式交付 （`include/` 放声明，`src/aurora/*.cpp` 放实现），消费者`#include "aurora/aurora.h"` 并链接静态库。

- 命名空间：`namespace aurora;` 推荐别名 `namespace au = aurora;`，入口头 `include/aurora/aurora.h`。 另有可选前向声明头
  `include/aurora/aurora_fwd.h`（仅前向声明重量级门面类型， 供只需指针/引用的 TU 降低瞬时包含成本；需完整 API 时仍用
  `aurora.h`）。
- 渲染内核：软件 `Painter`（ **无 `Renderer` 接口**）。
- 后端：`Surface` 抽象 + `HeadlessSurface`（内存/离线 PNG）、`GlfwSurface`（OpenGL 3.x 兼容 profile，默认请求 3.3，CMake 开关）、
  `Win32Surface`（Win32/GDI，仅 `_WIN32`，零三方依赖）、`D3D11Surface`（Windows GPU 增量上屏偏置，CMake 开关）、
  `X11Surface`（X11/Xlib，Linux 桌面，CMake 开关）、
  `WaylandSurface`（原生 Wayland：wl_shm+xdg-shell+xkbcommon，Linux 桌面，CMake 开关）、
  `WasmSurface`（Emscripten/Canvas 2D，浏览器 rAF 驱动）、`MacOSSurface`（AppKit/CoreGraphics，骨架）。
- 线程模型：单线程 UI、同步事件、响应式细粒度信号。
- **版本状态**：当前版本 **1.0.0-alpha.1**（早期预览开发版）。alpha 阶段 API 形态已完整但 **尚不构成稳定性承诺**，
  仍可能破坏性变更；任何变更须遵循 semver 在 `CHANGELOG.json` 记录并提供迁移路径。

---

## 2. 目录布局

| 路径                | 作用                                                                                                                                                                            |
|---------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `include/`          | 公共 API 头（`include/aurora/aurora.h` 为唯一入口），声明与少量 header-only 控件                                                                                                |
| `src/`              | 实现（`src/aurora/*.cpp`），非模板纯逻辑类实现放此，头只留声明                                                                                                                  |
| `examples/`         | 示例：每个组件一个 `demo_<组件>.cpp` 位于 `examples/demos/`（1:1，CMake 自动 GLOB）；`examples/demos/demo_common.h` 含 `Card`/`BrandBadge`/`GradientTitle` 等demo使用的全局控件 |
| `tests/`            | 独立可执行测试 + CTest（`tests/*.cpp`）                                                                                                                                         |
| `third_party/`      | 三方库文件                                                                                                                                                                      |
| `tools/`            | 工具链，按职责分子目录：`gen/`（三生成器 `gen_api`/`gen_error_codes`/`gen_debug_api`）、`servers/`（mcp / lsp / cli / aurora_lint）、`bench/`（4 基准 + `bench_common.h`）、`check/`（校验与门禁脚本 + `perf_gates.json`）、`coverage/`（GCC/Clang/LLVM 覆盖率聚合）、`include/`（共享头，含枚举 SSOT `known_enums.h` 与 LSP 三层 `lsp_*.h`）。API 生成落盘 `aurora_api.json`，CMake 聚合目标 `aurora_api_json`；详见 `cmake/AuroraTools.cmake` 与 `cmake/AuroraInstrumentation.cmake` |
| `cmake/`            | CMake 模块（顶层 `CMakeLists.txt` 只做编排）：`AuroraThirdParty`（三方构建）/`AuroraImageCodecs`（图片编解码）/`AuroraCcache`（编译缓存）/`AuroraSimd`（SIMD）/`AuroraBackends`（后端开关）/`AuroraTools`（工具）/`AuroraDemos`（示例）/`AuroraTests`（测试）/`AuroraInstrumentation`（插桩）/`AuroraInstall`（安装）/`AuroraLint`（Clang-Tidy 门禁：`lint` / `lint-fix` 聚合目标），共 11 个；布局与职责详见 `codespec/BUILD_OPTIONS.md` §1.1 |
| `codespec/`         | **全部项目文档**（需求/架构/规范/指南/概念），见下方导航表                                                                                                                      |
| `build/`            | 构建产物，CMake 生成，不纳入版本管理                                                                                                                                            |
| `aurora_api.json`   | 由 `gen_api_tools` 生成的 API 描述数据（schema/类型/属性键），**非文档、不移动**                                                                                                |
| `CHANGELOG.json`    | 变更记录数据文件，**非文档、不移动**                                                                                                                                            |
| `compile_flags.txt` | 编译标志（供 clangd 识别 `-std=c++20` 与 include 路径）                                                                                                                         |

---

## 3. 构建与测试要点

- 工具链：需 **CMake（≥ 3.20）** 与 **C++20 编译器**（如 gcc/clang/MSVC 任一）在 `PATH` 中；推荐 **Ninja** 作为生成器（空转/增量调度远快于
  Make）。各工具的具体安装目录请按本机环境配置， **勿将绝对路径写死进仓库文档**。
- 构建（Ninja 默认用满全部核心，无需 `-j`；如回退 Make 须加 `-- -j $env:NUMBER_OF_PROCESSORS`）：
  - 推荐用仓库内置 `CMakePresets.json` 的 `ninja` preset（已预置生成器与 gcc/g++）：
    ```powershell
    cmake --preset ninja
    cmake --build build --target <tgt>
    ```
  - 或显式指定生成器：
    ```powershell
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
    cmake --build build --target <tgt>
    ```
  （若 PATH 含 LLVM/clang bin，configure 会自动探测 `ld.lld`——`AURORA_ENABLE_LLD=ON` 时链接用 lld 提速，缺失则自动回退 GNU
  ld；lld 仅为可选加速，不影响构建正确性。）
- ⚠️ demo 不进默认构建（`EXCLUDE_FROM_ALL`）：`cmake --build build` 只建库/工具/测试； 单个 demo 按名构建（
  `--target demo_lazy_list`），全部 demo 用聚合目标（`--target demos`）。
- CMake 选项 / 编译宏 / 运行时环境变量： **全部统一列于 `codespec/BUILD_OPTIONS.md`（唯一权威来源）**。该文按三层命名分类法组织——
  `AURORA_BUILD_*`（构建产物开关）/ `AURORA_BACKEND_*`（后端开关=feature 宏）/ `AURORA_ENABLE_*`（插桩/分析），并含
  `NOMINMAX` 等全局编译定义、golden 测试的 4 个运行时环境变量与快速速查表。此处不再重复罗列，以免漂移。
- 测试：`cmake/AuroraTests.cmake` 把 `tests/*.cpp` 全部链入**单一可执行** `aurora_test_runner`（注册式 runner，见下条），
  在 `build/` 下运行 `ctest` 即逐条执行（每条 = `aurora_test_runner --run=<stem>`，进程隔离）。`cmake/AuroraTests.cmake`
  已为依赖相对路径的测试（含 `tests/golden`）显式设置 `WORKING_DIRECTORY` 为仓库根， 故 `ctest` 下直接可跑；仅手工直跑时须从仓库根执行
  （`./build/aurora_test_runner --run=test_offscreen`，或设 `AURORA_GOLDEN_DIR` 覆盖），并可用 `--list`/`--filter=`/`--verbose` 辅助。
- **测试/示例组织约定**（详见 `CODING_STANDARDS.md` §3 与 §6.2 默认参数章节）：每个公共源文件对应一个 `demo_*.cpp`（`examples/demos/`
  ）与一个 `test_*.cpp`（`tests/`），二者用文件夹区分；测试文件以 `test` 为前缀（非 `_test` 后缀）。测试用例经 `AURORA_TEST()` 宏静态注册
  （用例名 = 文件名 stem，由 CMake 按源文件注入 `AURORA_TEST_NAME`），断言走 `AURORA_TEST_CHECK*`/`AURORA_TEST_REQUIRE*` 家族；
  **测试文件禁止自定义 `main()`**（`main` 由 `tests/au_test_main.cpp` 唯一提供）。后端/平台专属用例在 feature 宏未开启的 `#else` 分支用
  `AURORA_TEST_SKIP(宏名)` 注册空通过桩。新增漏注册由 `registry_integrity` 守护（比对 `--list` 与 GLOB 清单）在 ctest 阶段兜底。
- ⚠️ 新增 `.cpp` 后需让 CMake 刷新 GLOB（`CONFIGURE_DEPENDS` 多数情况自动；否则碰一下 `CMakeLists.txt` 或删 `build/` 重建）。

---

## 4. 文档导航表（codespec/）

`codespec/` 共 **14 份手写文档 + 1 份生成物**（`ERROR_CATALOG.md`），外加 2 份生成源数据（`errors.toml` / `debug_api.toml`）。
各文档的章节号统一为纯数字点分层级（`1` / `1.1` / `1.1.1`）；需求编号 `#1–#24` 是独立的需求标识体系，与章节号并存。

**顶层文档（6 份，均为自包含正文，非外链索引）**

| 你想了解                                              | 读这个文件                     | 权威性说明                                                                                                 |
|-------------------------------------------------------|--------------------------------|-------------------------------------------------------------------------------------------------------------|
| **项目定位 / 设计原则 / 需求清单 / 文档导航**         | `codespec/SPECIFICATIONS.md`   | 总纲与索引：24 条特性清单（`#1–#24`）逐条指向其规格落点；分层蓝图、命名速查、API 兼容策略                  |
| **架构 / 运行时 / 分层 / 模块映射 / 设计不变量**      | `codespec/ARCHITECTURE.md`     | 🥇 架构与设计以它为准：分层、运行时、模块映射、核心数据流、组件树、事件、渲染、性能、11 条设计不变量、错误处理架构、AI-first 原则、测试与 CI |
| **核心概念 / 跨框架映射 / 概念可枚举性**              | `codespec/CONCEPTS.md`         | 可枚举 UI 原语审计、状态作用域决策树、React / Flutter / Qt 概念映射、迁移要点                              |
| **编码规范 / 命名 / 错误 / AI 友好性 / 版本管理**     | `codespec/CODING_STANDARDS.md` | 🥇 编码规则以它为准：错误处理、命名、文档与示例、日志纪律、契约标注、AI 友好性、SemVer、函数签名、内部工具层、提交信息规范 |
| **使用指南 / 复制即用配方**                           | `codespec/GUIDELINE.md`        | 28 组最小可编译片段：界面 / 布局 / 状态 / 异步 / 持久化 / 媒体 / 字体 / Inspector / 工厂 / 测试 / 样式 / 坑 / 调试 |
| **编译选项 / 宏 / 环境变量（统一参考）**              | `codespec/BUILD_OPTIONS.md`    | 🥇 所有 CMake 开关、缓存变量、feature 宏、运行时环境变量与 find_package 集成以它为准                       |

**子系统规格（8 份，按 `include/aurora/` 模块域切分）**

| 文件 | 覆盖 | 需求 |
|:---|:---|:---|
| `specification/01-core.md` | `core/`：几何与尺寸意图、错误与结果、诊断与降级、日志、线程池 | #18 #19 #21 #23 |
| `specification/02-state.md` | `state/`：信号原语、订阅生命周期、`Store`、异步与协程 | #6 #19 |
| `specification/03-layout-render.md` | `layout/` `render/` `image/` `media/`：布局协议、Flex/Grid 算法、Painter、字体引擎、Surface 与后端 | #11 #20 |
| `specification/04-widget.md` | `widget/` `ui/`：控件基类契约、自描述、控件清单、可定制性契约 | #7 #22 |
| `specification/05-event-navigation.md` | `event/` `animation/` `navigation/`：事件模型、命中测试、焦点、手势、动画、页面栈 | #8 |
| `specification/06-app-platform.md` | `app/` `window/` `preferences/` `storage/` `perf/` `debug/`：应用驱动、帧循环、窗口生命周期、定时任务、平台 Shell、持久化、调试门面 | #14 #15 |
| `specification/07-environment-modifier.md` | `environment/` `theming/` `i18n/` `modifier/`：环境注入、媒体查询、窗口装饰、主题、国际化、Modifier | #12 |
| `specification/08-tooling.md` | 序列化 / 代码生成 / YAML、控件树检查、Inspector、自描述发现、MCP / CLI / LSP、测试原语、日志通道 | #9 #10 #12 #13 #16 #17 #22 |

**数据文件（`codespec/`，位置固定不可移动）**

| 文件 | 说明 |
|:---|:---|
| `errors.toml` | 错误码单一声明源（slug / severity / category / message 模板） |
| `debug_api.toml` | `aurora::debug` 公共自由函数声明源 |
| `ERROR_CATALOG.md` | `tools/gen/gen_error_codes.cpp` 生成的错误码全量清单（**生成物，勿手改**） |

三个文件路径被 `tools/` 与 `src/aurora/core/diagnostics.cpp` 硬编码。

### 模块划分

- **需求规格书** → `SPECIFICATIONS.md`
- **架构 & 设计** → `ARCHITECTURE.md` + `CONCEPTS.md`
- **编码规范 & 提交规范** → `CODING_STANDARDS.md`
- **使用指南** → `GUIDELINE.md`
- **编译选项/宏/环境变量** → `BUILD_OPTIONS.md`
- **各子系统 API 契约** → `specification/01`–`08`
- **数据存储抽象层** → `specification/06-app-platform.md` §9.2 + `ARCHITECTURE.md` §4.8
- **总入口** → 根 `AGENTS.md`

---

## 5. 给 AI 协作者的硬规则

1. **改任何公共 API 须「改前读、改后回写」**：动手改之前，先读 `SPECIFICATIONS.md` 与 `ARCHITECTURE.md`
   ，保持契约与不变量一致；改完之后，必须按本节的「代码与文档同步」规则回写对应文档，避免文档漂移。
2. **代码与文档必须同步**（变更类型 → `codespec/` 文档映射）：凡对公共
   API（函数签名、类/类型、信号、属性键、命名空间）或核心设计的增删改，落完代码后须同步更新对应文档；映射如下：
    - **API 契约 / 功能规格 / 需求**（函数签名、类、信号、属性键、公共行为）→ `specification/NN-*.md` 中该符号所属模块域的那一份（见 §4 子系统规格表）；跨模块的定位与需求清单 → `SPECIFICATIONS.md`
    - **架构 / 运行时 / 分层 / 模块边界 / 设计原则** → `ARCHITECTURE.md`
    - **核心概念 / 跨框架映射 / 控件语义**（如新增或删除 widget 的对照）→ `CONCEPTS.md`
    - **编码规范 / 命名 / 错误 / AI 友好性**（强类型、命名序、默认参数等）→ `CODING_STANDARDS.md`
    - **编译期开关 / feature 宏 / 环境变量** → `BUILD_OPTIONS.md`
    - **新增可复现用法 / 最小可编译配方** → `GUIDELINE.md`
    - **提交信息写法** → `CODING_STANDARDS.md` §10
    - **文档章节号与需求编号写法**：章节号一律纯数字点分层级（`1` / `1.1` / `1.1.1`），禁止中英文序号；需求编号用 `#N`，与章节号并存
    - 新增 / 删除 widget 或类型时，除 `aurora_api.json`（运行 `gen_api_tools`）外，还应在 `CONCEPTS.md`的控件映射中体现（如适用）。
    - **冲突回写原则**：当文档与代码运行时行为冲突时，以 **代码运行时**为准，并 **回填文档**
      消除冲突，杜绝「文档有、代码无」或「文档缺、代码有」的漂移；不得为迁就旧文档而保留错误实现。
3. **新增 widget / 类型**，同步更新 `aurora_api.json`（运行 `gen_api_tools`）并在 `CODING_STANDARDS.md`的「AI
   友好性」章节约束下设计（强类型、命名序、默认参数等）。
4. **不要**凭训练记忆假设存在某个API接口。
5. 文档一律放 `codespec/`，根目录只保留 `AGENTS.md`、数据文件（`aurora_api.json`/`CHANGELOG.json`）与代码。
6. 概念语义若有疑问，查 `CONCEPTS.md` 的跨框架映射，避免误用 React/Flutter 式命令式写法。
7. **新增代码必须配套单元测试**：任何新增的公共 API、widget、类型或核心逻辑（位于 `include/` 或 `src/`）， 都应在 `tests/`
   下补充对应的可执行测试并接入 CTest（参见第 3 节构建与测试要点）。
    - 优先覆盖：构造函数与不变量、属性/信号的读写与订阅、序列化往返（`toJson`/`fromJson`/`diff`/`apply_patch`）、 跨平台纯逻辑（如
      `Painter` 几何、布局测量、`Result`/`Error`）。
    - 依赖真实后端（`Win32Surface`/`GlfwSurface`）或需要交互/渲染像素的，可用 `HeadlessSurface`（内存 PNG）做断言， 避免引入GUI
      交互测试。
    - 新增 `tests/*.cpp` 后 CMake 用 GLOB 自动收集（`CONFIGURE_DEPENDS`），必要时碰一下 `CMakeLists.txt` 或重建 `build/`。
    - 无法稳定测试的临时代码（如一次性示例、`examples/` 演示），须在提交说明中标注「无单测」，不计入此规则。
8. **禁止直接使用标准输出**：
    - 所有日志/打印/诊断一律走封装好的 `Logger` 接口（`AURORA_LOG_*` 诊断通道写 stderr；
    - CLI 的JSON/usage、benchmark 表格、LSP/MCP 线协议帧等「程序产品」功能输出走 `AURORA_LOG_RAW` 写 stdout，无前缀、 始终输出）；
    - `tools/`、`examples/`、`tests/` 中遗留的 `printf` 风格诊断用 `AURORA_TEST_PRINTF`/`AURORA_TEST_PRINTF_ERR` 桥接宏， 勿新增
      `std::cout`/`std::cerr`/`printf`/`fprintf`/`puts`。唯一允许直接触达标准输出的是 `src/aurora/core/log.cpp` 内的 sink
      实现。详见 `CODING_STANDARDS.md` §4.1 与 `specification/08-tooling.md` §9（日志通道）。
9. **不得无故改变既有函数/成员的可见性**：AI 在生成或修改代码时，常会顺手调整 `public`/`protected`/`private`
   可见性（如把 `protected` 虚回调改成 `public`、把 `private` 字段挪到 `public` 区等）。这类改动往往与本次任务无关，
   却会破坏封装不变量或改变派生类契约。 **规则**：保持被改文件/类改动前的可见性划分不变；仅当本次改动本身在语义上
   确实要求调整可见性（如新增的公开 API、需要被子类覆盖的钩子）时才改，且须显式说明原因。改动前先确认目标符号
   原本所处访问区，改动后不要把它移到别的访问区。
