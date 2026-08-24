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
- 后端：`Surface` 抽象 + `HeadlessSurface`（内存/离线 PNG）、`GlfwSurface`（OpenGL1.1，CMake 开关）、
  `Win32Surface`（Win32/GDI，仅 `_WIN32`，零三方依赖）、`X11Surface`（X11/Xlib，Linux 桌面，CMake 开关）、
  `WaylandSurface`（原生 Wayland：wl_shm+xdg-shell+xkbcommon，Linux 桌面，CMake 开关）、
  `WasmSurface`（Emscripten/Canvas 2D，浏览器 rAF 驱动）、`MacOSSurface`（AppKit/CoreGraphics，骨架）。
- 线程模型：单线程 UI、同步事件、响应式细粒度信号。
- **版本状态**：当前版本 **1.0.0-alpha.1**（早期预览开发版）。alpha 阶段 API 形态已完整但 **尚不构成稳定性承诺**，
  仍可能破坏性变更；任何变更须遵循 semver 在 `CHANGELOG.json` 记录并提供迁移路径（详见 `codespec/API_STABILITY.md`）。

---

## 2. 目录布局

| 路径                | 作用                                                                                                                                                                            |
|---------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `include/`          | 公共 API 头（`include/aurora/aurora.h` 为唯一入口），声明与少量 header-only 控件                                                                                                |
| `src/`              | 实现（`src/aurora/*.cpp`），非模板纯逻辑类实现放此，头只留声明                                                                                                                  |
| `examples/`         | 示例：每个组件一个 `demo_<组件>.cpp` 位于 `examples/demos/`（1:1，CMake 自动 GLOB）；`examples/demos/demo_common.h` 含 `Card`/`BrandBadge`/`GradientTitle` 等demo使用的全局控件 |
| `tests/`            | 独立可执行测试 + CTest（`tests/*.cpp`）                                                                                                                                         |
| `third_party/`      | 三方库文件                                                                                                                                                                      |
| `tools/`            | API 生成与辅助工具（`gen_api_tools.cpp` 生成 `aurora_api.json`）                                                                                                                |
| `cmake/`            | CMake 模块（顶层 `CMakeLists.txt` 只做编排）：三方构建/后端开关/工具/测试/插桩/安装各一模块，布局详见 `codespec/BUILD_OPTIONS.md` §0.1                                          |
| `codespec/`         | **全部项目文档**（需求/架构/规范/指南/概念），见下方导航表                                                                                                                      |
| `build/`            | 构建产物，CMake 生成，不纳入版本管理                                                                                                                                            |
| `aurora_api.json`   | 由 `gen_api_tools` 生成的 API 描述数据（schema/类型/属性键），**非文档、不移动**                                                                                                |
| `CHANGELOG.json`    | 变更记录数据文件，**非文档、不移动**                                                                                                                                            |
| `compile_flags.txt` | 编译标志（供 clangd 识别 `-std=c++20` 与 include 路径）                                                                                                                         |

---

## 3. 构建与测试要点

- 工具链：需 **CMake（≥ 3.16）** 与 **C++20 编译器**（如 gcc/clang/MSVC 任一）在 `PATH` 中；推荐 **Ninja** 作为生成器（空转/增量调度远快于
  Make）。各工具的具体安装目录请按本机环境配置， **勿将绝对路径写死进仓库文档**。
- 构建（Ninja 默认用满全部核心，无需 `-j`；如回退 Make 须加 `-- -j $env:NUMBER_OF_PROCESSORS`）：
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
- 测试：在 `build/` 下运行 `ctest`。从仓库根运行测试以保证`tests/golden` 等相对路径解析（或设 `AURORA_GOLDEN_DIR`）； **
  `ctest` 会把测试 CWD 设为 `build/`，故 golden等依赖相对路径的测试须从仓库根直接运行可执行文件**（如
  `./build/test_offscreen`，原 test_golden 已并入）才能正确解析路径。
- **测试/示例组织约定**（详见 `CODING_STANDARDS.md` §3 4.8–4.10）：每个公共源文件对应一个 `demo_*.cpp`（`examples/demos/`
  ）与一个 `test_*.cpp`（`tests/`），二者用文件夹区分；测试文件以 `test` 为前缀（非 `_test` 后缀）。
- ⚠️ 新增 `.cpp` 后需让 CMake 刷新 GLOB（`CONFIGURE_DEPENDS` 多数情况自动；否则碰一下 `CMakeLists.txt` 或删 `build/` 重建）。

---

## 4. 文档导航表（codespec/）

| 你想了解                                              | 读这个文件                      | 权威性说明                                                                                                                                                                                                                                                                         |
|-------------------------------------------------------|---------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **需求 / 功能规格 / API 契约**                        | `codespec/SPECIFICATIONS.md`    | 🥇 API 与需求以它为准；「三、特性详细规范」分为 21 份子文档：`codespec/specification/features/`(A–G，8 份) / `codespec/specification/subsystems/`(H.1–H.10c，7 份) / `codespec/specification/subsystems_api/`(H.11–H.17+Log+AI-First，6 份)；逐文件清单见 `SPECIFICATIONS.md` 目录 |
| **架构 / 运行时 / 分层 / 模块映射 / 设计原则**        | `codespec/ARCHITECTURE.md`      | 🥇 架构与设计以它为准；分为 `codespec/architecture/ARCHITECTURE_RUNTIME.md`(运行时/模块映射/数据流)/`ARCHITECTURE_WIDGET.md`(组件树/事件/渲染/序列化)/`ARCHITECTURE_PERF.md`(性能/不变量)/`ARCHITECTURE_AI.md`(AI-first)，同目录                                                   |
| **核心概念 / 跨框架映射 / 概念可枚举性**              | `codespec/CONCEPTS.md`          | 概念认知与「React/Flutter/Qt 翻译表」；「核心概念审计」为 `codespec/concepts/CONCEPTS_CORE.md`                                                                                                                                                                                     |
| **编码规范 / 命名 / 错误 / AI 友好性规则 / 版本管理** | `codespec/CODING_STANDARDS.md`  | 🥇 编码规则以它为准；分为 `codespec/coding/CODING_ERRORS_NAMING.md`(错误/命名/文档/元数据/契约)/`CODING_AI.md`(AI 友好性)/`CODING_VERSIONING.md`(版本管理)/`CODING_SIGNATURE.md`(函数签名/内部工具)，同目录                                                                        |
| **使用指南 / 复制即用配方**                           | `codespec/GUIDELINE.md`         | 最小可编译片段集合（原 RECIPES）；分为 `codespec/guideline/GUIDELINE_BASICS.md`(基础)/`GUIDELINE_ASYNC_SERIAL.md`(异步/序列化)/`GUIDELINE_INTEGRATION.md`(集成)/`GUIDELINE_PITFALLS.md`(坑/调试)，同目录                                                                           |
| **编译选项 / 宏 / 环境变量（统一参考）**              | `codespec/BUILD_OPTIONS.md`     | 🥇 所有 CMake 开关、缓存变量、feature 宏与运行期环境变量以它为准；分为 `codespec/build_options/BUILD_OPTIONS_BUILD.md`(BUILD_*)/`BUILD_OPTIONS_BACKEND.md`(BACKEND_*)/`BUILD_OPTIONS_ENABLE.md`(ENABLE_*)/`BUILD_OPTIONS_INTERNAL.md`(缓存/定义/变量)，同目录                      |
| **提交信息规范（Commit Message）**                    | `codespec/COMMIT_CONVENTION.md` | 🥇 提交写法、type/scope 表、与 SemVer / `CHANGELOG.json` 对齐以它为准                                                                                                                                                                                                              |
| **数据存储抽象层（Storage 门面 + 后端抽象）**         | `codespec/ARCHITECTURE.md` §4.1 | 存储子系统设计：后端抽象、信封/类型化、异步卸载、变更通知、跨记录事务；当前为设计稿，API 契约最终以 `include/aurora/storage/*.h` 落地为准                                                                                                                                          |
| **项目整体结构 / 该读哪个文档**                       | 本文件 `AGENTS.md`              | 入口                                                                                                                                                                                                                                                                               |

### 模块划分

- **需求规格书** → `SPECIFICATIONS.md`
- **架构 & 设计** → `ARCHITECTURE.md` + `CONCEPTS.md`
- **编码规范** → `CODING_STANDARDS.md`
- **提交信息规范** → `COMMIT_CONVENTION.md`
- **使用指南** → `GUIDELINE.md`
- **编译选项/宏/环境变量** → `BUILD_OPTIONS.md`
- **数据存储抽象层** → `ARCHITECTURE.md` §4.1
- **总入口** → 根 `AGENTS.md`

---

## 5. 给 AI 协作者的硬规则

1. **改任何公共 API 须「改前读、改后回写」**：动手改之前，先读 `SPECIFICATIONS.md` 与 `ARCHITECTURE.md`
   ，保持契约与不变量一致；改完之后，必须按本节的「代码与文档同步」规则回写对应文档，避免文档漂移。
2. **代码与文档必须同步**（变更类型 → `codespec/` 文档映射）：凡对公共
   API（函数签名、类/类型、信号、属性键、命名空间）或核心设计的增删改，落完代码后须同步更新对应文档；映射如下：
    - **API 契约 / 功能规格 / 需求**（函数签名、类、信号、属性键、公共行为）→ `SPECIFICATIONS.md`
    - **架构 / 运行时 / 分层 / 模块边界 / 设计原则** → `ARCHITECTURE.md`
    - **核心概念 / 跨框架映射 / 控件语义**（如新增或删除 widget 的对照）→ `CONCEPTS.md`
    - **编码规范 / 命名 / 错误 / AI 友好性**（强类型、命名序、默认参数等）→ `CODING_STANDARDS.md`
    - **新增可复现用法 / 最小可编译配方** → `GUIDELINE.md`
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
    - `tools/`、`examples/`、`tests/` 中遗留的 `printf` 风格诊断用 `test_printf`/`test_printf_err` 桥接宏， 勿新增
      `std::cout`/`std::cerr`/`printf`/`fprintf`/`puts`。唯一允许直接触达标准输出的是 `src/aurora/core/log.cpp` 内的 sink
      实现。详见 `CODING_STANDARDS.md` §4 三.6 与 `SPECIFICATIONS.md` 日志（Log）子系统小节。
9. **不得无故改变既有函数/成员的可见性**：AI 在生成或修改代码时，常会顺手调整 `public`/`protected`/`private`
   可见性（如把 `protected` 虚回调改成 `public`、把 `private` 字段挪到 `public` 区等）。这类改动往往与本次任务无关，
   却会破坏封装不变量或改变派生类契约。 **规则**：保持被改文件/类改动前的可见性划分不变；仅当本次改动本身在语义上
   确实要求调整可见性（如新增的公开 API、需要被子类覆盖的钩子）时才改，且须显式说明原因。改动前先确认目标符号
   原本所处访问区，改动后不要把它移到别的访问区。
