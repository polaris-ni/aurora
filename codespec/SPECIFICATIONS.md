# Aurora 规格总纲

> **项目名称**：Aurora —— AI-First C++ 跨平台 UI 库
> **设计内核**：声明式、响应式、概念可枚举
> **版本说明**：本文档为设计规格（文档自身无版本号）。库发布版本见 `CHANGELOG.json`（`currentVersion`）；文档与实现冲突时以**代码运行时**为准并回填本文档。
>
> 本文件是**总纲与索引**：定位、设计原则、范围、约束、24 条特性清单与文档导航。各主题的详细契约见 `specification/` 八份子系统文档与五份顶层文档。

---

## 1 项目定位

Aurora 是一个 C++20 跨平台 **AI-first** GUI 库：以「声明式、响应式、概念可枚举」为设计内核，使用纯软件栅格 `Painter` 渲染，**不依赖 GPU**；以**编译型静态库**形式交付（`include/` 放声明，`src/aurora/*.cpp` 放实现），消费者 `#include "aurora/aurora.h"` 并链接静态库。

- 命名空间：`namespace aurora;`，推荐别名 `namespace au = aurora;`
- 入口头：`include/aurora/aurora.h`；另有可选前向声明头 `include/aurora/aurora_fwd.h`（仅前向声明重量级门面类型，供只需指针 / 引用的编译单元降低瞬时包含成本）
- 渲染内核：软件 `Painter`（**无 `Renderer` 接口**）
- 线程模型：单线程 UI、同步事件、响应式细粒度信号

---

## 2 设计原则

- **声明式优先**：界面是「状态 → 视图」的纯函数；改状态而非改树。
- **概念最小正交**：控件 + 修饰 + 状态原语，互不重叠。
- **概念可枚举**：全部 UI 原语可枚举、可命名、可映射（见 [`CONCEPTS.md`](CONCEPTS.md) §1）。
- **Token 经济**：API 表面紧凑，AI 在有限上下文即可装载全部概念。
- **AI 友好错误**：错误信息携带「修复建议」与文档锚点（见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §1）。
- **降级而非中止**：非法输入产出 `Diagnostics` 并降级，而非崩溃（见 [`01-core.md`](specification/01-core.md) §8.3）。
- **跨平台零依赖**：核心渲染（软件栅格）不依赖 GPU；Win32/GDI 零三方依赖；D3D11 仅作可选 GPU 像素上屏（默认 OFF）。

---

## 3 为什么需要 AI First

AI（LLM）使用 UI 库的方式与人类有本质不同：

| 维度 | 人类开发者 | AI / LLM |
|:---|:---|:---|
| 学习方式 | 读文档、看教程、试错 | 基于训练语料中的**模式匹配** |
| 记忆方式 | 理解原理后灵活应用 | 依赖**高频模式**和**一致性** |
| 调试方式 | 设断点、看调用栈、凭经验 | 依赖**错误信息文本**和**代码上下文** |
| 生成方式 | 从需求出发设计 | 从**已有模式**组合拼装 |

Aurora 本质上是一个**把 UI 开发变成「结构化数据描述」问题**的库。AI 最擅长处理结构化的、模式一致的、可验证的任务——Aurora 的设计让 UI 开发恰好落入这个区间。

**AI 生成代码的两大失败来源**：① 内存与所有权错误（悬空指针、use-after-free、泄漏）；② 并发与线程安全错误（死锁、竞态、在错误线程更新 UI）。Aurora 的设计从根本上去除这两类错误的可能性（分别见 #18 与单线程 UI 不变量）。

---

## 4 范围、非目标与技术约束

### 4.1 范围

**包含**：声明式组件、响应式状态、布局引擎、软件渲染、平台 Surface（Headless / Win32 / Glfw / X11 / Wayland / Wasm / macOS）、序列化、导航、动画、异步、定时任务（Scheduler / Timer）、环境注入、偏好与存储。

**不包含**：3D、硬件加速渲染、原生控件嵌入、跨进程、复杂数据网格。

### 4.2 非目标

- 不做「又一套 CSS」：布局用代码表达，不引入样式表语言。
- 不做服务端渲染。
- 不做可视化拖拽编辑器（除非社区驱动）。
- 不提供 Playground / REPL；MCP / CLI 与 LSP 已提供（见 #17）。

### 4.3 技术约束（#24 Token 效率 + 编译速度约束）

- C++20 最小标准。
- 静态库交付（非 header-only）。
- 软件栅格渲染，核心不依赖 GPU（D3D11 仅用于可选 GPU 像素上屏，默认 OFF）。
- Windows 优先验证（Win32/GDI），跨平台后端可选。
- 模板深度 ≤ 3 层，避免模板元编程导致的编译爆炸与不可读错误。
- 头文件尽量只放声明、实现下沉 `src/aurora/*.cpp`，减少编译单元重编范围。
- 平台后端头一律 pimpl 隔离，消费者编译单元不被拉入重型平台头（见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §6.7）。

---

## 5 特性清单（#1–#24）

需求编号 `#N` 是稳定的需求标识。下表给出每条需求的**规格落点**（文档 + 章节）。

| # | 特性 | 核心目标 | 规格落点 |
|---:|:---|:---|:---|
| 1 | 声明式双模 API（链式 / 分步 / 配置块等价） | AI 易生成 | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §11.1 |
| 2 | 极致命名一致性 + 扁平命名空间 | AI 易补全 | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §11.2 |
| 3 | 正交可组合的最小核心 API | AI 少幻觉 | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §11.3 |
| 4 | 强类型 + 单位标注 + 编译期校验 | 编译即验证 | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §11.4 |
| 5 | 合理默认值（声明处可见） | AI 少写少错 | [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §11.5 |
| 6 | 单向数据流 + 细粒度信号状态模型 | AI 易理解状态 | [`02-state.md`](specification/02-state.md) §7.1 |
| 7 | 扁平组合模型 + 共享所有权组件 | AI 易追踪逻辑 | [`04-widget.md`](specification/04-widget.md) §6.1 |
| 8 | 显式优于隐式（含样式继承） | AI 无理解盲区 | [`05-event-navigation.md`](specification/05-event-navigation.md) §8.1 |
| 9 | 结构化错误信息（JSON 可解析） | AI 易调试 | [`08-tooling.md`](specification/08-tooling.md) §10.1 |
| 10 | 内置 UI Inspector（HTTP / MCP 接口） | AI 可观测运行时 | [`08-tooling.md`](specification/08-tooling.md) §10.2 |
| 11 | 确定性渲染 + 逻辑快照测试 | AI 可验证正确性 | [`03-layout-render.md`](specification/03-layout-render.md) §10.1 |
| 12 | 机器可读 API Schema | AI 工具链直接消费 | [`08-tooling.md`](specification/08-tooling.md) §10.3（工具链侧）、[`07-environment-modifier.md`](specification/07-environment-modifier.md) §8.1（控件自描述侧） |
| 13 | UI 树序列化 + 差分 Patch 协议 | AI 可增量修改 UI | [`08-tooling.md`](specification/08-tooling.md) §10.4 |
| 14 | 零 `#ifdef` 跨平台 + 插件式平台扩展 | AI 无需处理平台分支 | [`06-app-platform.md`](specification/06-app-platform.md) §12.1 |
| 15 | 跨平台一致行为 + 黄金文件验证 | AI 无需考虑平台差异 | [`06-app-platform.md`](specification/06-app-platform.md) §12.2 |
| 16 | 示例驱动文档（Recipe 形式） | AI 从示例高效学习 | [`08-tooling.md`](specification/08-tooling.md) §10.5 |
| 17 | LSP / MCP Server / CLI 工具链 | AI Agent 直接集成 | [`08-tooling.md`](specification/08-tooling.md) §10.6 |
| 18 | 安全的内存与所有权模型 | AI 生成无内存错误的代码 | [`01-core.md`](specification/01-core.md) §8.1 |
| 19 | 结构化异步与并发模型 | AI 轻松处理耗时操作 | [`01-core.md`](specification/01-core.md) §8.2、[`02-state.md`](specification/02-state.md) §7.2 |
| 20 | 布局系统的代数一致性 | AI 可推理尺寸和位置 | [`03-layout-render.md`](specification/03-layout-render.md) §10.2 |
| 21 | 错误恢复与降级渲染 | AI 生成的错误 UI 不会崩溃 | [`01-core.md`](specification/01-core.md) §8.3 |
| 22 | 可逆性：UI → 代码的参考还原 | AI 可分析现有界面并重构 | [`08-tooling.md`](specification/08-tooling.md) §10.7（工具链侧）、[`04-widget.md`](specification/04-widget.md) §6.2（控件侧） |
| 23 | 部分代码容错（半成品可编译可运行） | AI 可增量开发 | [`01-core.md`](specification/01-core.md) §8.4 |
| 24 | Token 效率 + 编译速度约束 | AI 迭代循环效率 | 本文 §4.3 |

---

## 6 特性间的张力与取舍

| 张力对 | 冲突点 | 解决方案 |
|:---|:---|:---|
| #5 默认值 vs #8 显式 | 默认值是「隐式」的 | **默认值在声明处可见**（LSP hover 显示），运行时可查询 `Xxx::defaults()`，但代码中可省略 |
| #1 链式 vs #3 最小 API | 链式需要每个方法返回 `this`，增加 API 面 | 链式方法 = 属性 setter 的语法糖，不增加新概念 |
| #6 细粒度信号 vs #7 共享所有权 | 信号变化需定点刷新，但组件树需可被复制 / 移动 | `Node` 持有 `shared_ptr<Widget>`（拷贝即共享），信号变化仅重绘依赖组件（见 #7 / #18） |
| #4 强类型 vs 编译速度 | 大量模板 / 概念检查拖慢编译 | 核心路径用简单类型，高级校验放在**独立验证工具**中（CLI），不阻塞编译 |
| #12 机器 Schema vs #2 命名一致 | Schema 需要额外维护 | Schema 从代码**自动生成**（`gen_api_tools`），保证与实现同步 |
| #6 信号刷新 vs 高频交互 | 每帧走全链路延迟不可接受 | 默认细粒度信号定点刷新；高频绘制用 `Canvas` opt-in |
| #18 单线程 UI vs #19 异步 | 异步结果需回 UI 线程 | `au::async` 经主线程投递器回到 UI 线程 |
| #2 命名一致 vs 历史 API | AI 可能混用新旧命名 | 文档统一 snake_case 属性 + CamelCase 类型，废弃名仅在兼容层标注 |

---

## 7 分层蓝图

```text
┌──────────────────────────────────────────────────────────────┐
│                  Aurora AI Tooling Layer                      │
│   aurora_mcp · aurora_lsp · aurora_cli · ai_compat_test      │
│   aurora_api.json · Recipe Search · Diff Patch · to_code()   │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Serialization Layer                   │
│   to_json / from_json · diff / apply_patch                   │
│   to_yaml · to_code · canonical form                         │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Component Layer                       │
│   控件清单（见 04-widget.md）· 共享所有权(shared_ptr<Node>)   │
│   稳定 ID(Node::id) · 双模 API · 降级渲染 · 部分代码容错      │
├──────────────────────────────────────────────────────────────┤
│   Modifier · 动画(Animator) · 环境(Provider) · 导航(Navigator)│
│   序列化(to_json/diff) · i18n(LocalizedString) · 焦点管理     │
├──────────────────────────────────────────────────────────────┤
│                  Aurora State + Event Layer                   │
│   信号 State/Reactive/Computed/Effect · 可选 Store<S>         │
│   细粒度定点刷新 · Canvas 高频 opt-in                         │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Async Layer                           │
│   ThreadPool(有界 worker) · async() · co_async()              │
│   CoroTask<T> 协程 · 自动 UI 线程回归 · 取消 / 超时           │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Layout Engine                         │
│   正交原语 + 组合配方 · 代数一致盒模型                        │
│   无边距合并 · 显式百分比参照 · 溢出策略 · 确定性求解         │
│   两遍布局（宽 → 高）· 动画不影响布局                         │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Platform Abstraction                  │
│   运行时能力查询 · 插件式扩展 · Surface 后端家族              │
│   Win / Mac / Linux(X11·Wayland) / Web(WASM) / Headless       │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Rendering Core                        │
│   自研光栅化 · 像素级一致 · 离屏渲染 · 无头模式               │
│   结构快照 · 盒模型快照 · 像素快照                            │
└──────────────────────────────────────────────────────────────┘
```

---

## 8 文档导航

### 8.1 子系统规格（`specification/`）

| 文档 | 覆盖 |
|:---|:---|
| [`01-core.md`](specification/01-core.md) | 基础层 `core/`：几何与尺寸意图、错误与结果、诊断与降级、日志、线程池、`au::TODO`；需求 #18 / #19 / #21 / #23 |
| [`02-state.md`](specification/02-state.md) | 响应式 `state/`：信号原语、订阅生命周期、`Store`、异步与协程、依赖图与撤销；需求 #6 / #19 |
| [`03-layout-render.md`](specification/03-layout-render.md) | `layout/` + `render/` + `image/` + `media/`：布局协议、Flex / Grid 算法、Painter、字体引擎、Surface 与后端；需求 #11 / #20 |
| [`04-widget.md`](specification/04-widget.md) | `widget/` + `ui/`：控件基类契约、自描述、控件清单、可定制性契约；需求 #7 / #22 |
| [`05-event-navigation.md`](specification/05-event-navigation.md) | `event/` + `animation/` + `navigation/`：事件模型、命中测试、焦点、手势、动画、页面栈；需求 #8 |
| [`06-app-platform.md`](specification/06-app-platform.md) | `app/` + `window/` + `platform/` + `preferences/` + `storage/` + `perf/` + `debug/`：应用驱动、帧循环、窗口生命周期、定时任务、平台 Shell、持久化、调试门面；需求 #14 / #15 |
| [`07-environment-modifier.md`](specification/07-environment-modifier.md) | `environment/` + `theming/` + `i18n/` + `modifier/`：环境注入、媒体查询、窗口装饰、主题、国际化、Modifier；需求 #12 |
| [`08-tooling.md`](specification/08-tooling.md) | 序列化 / 代码生成 / YAML、控件树检查、Inspector 面板与远程服务、自描述发现、MCP / CLI / LSP、测试原语、日志通道；需求 #9 / #10 / #12 / #13 / #16 / #17 / #22 |

### 8.2 顶层文档（`codespec/`）

| 文档 | 角色 |
|:---|:---|
| [`SPECIFICATIONS.md`](SPECIFICATIONS.md) | 本文件：总纲、设计原则、特性清单与文档导航 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 分层、运行时、模块映射、核心数据流、组件树、事件、渲染、性能、设计不变量、错误处理架构、AI-first 原则、测试与 CI |
| [`CONCEPTS.md`](CONCEPTS.md) | 核心概念审计（可枚举 UI 原语）、状态作用域决策树、React / Flutter / Qt 概念映射、迁移要点 |
| [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | 错误处理、命名、文档与示例、元数据与可观测、契约表达、AI 友好性、版本管理、函数签名、内部工具层、提交规范；需求 #1–#5 |
| [`GUIDELINE.md`](GUIDELINE.md) | 复制即用配方集 |
| [`BUILD_OPTIONS.md`](BUILD_OPTIONS.md) | 全部 CMake 开关、feature 宏、缓存变量、环境变量、安装与 find_package |

### 8.3 数据文件（`codespec/`，位置固定）

| 文件 | 说明 |
|:---|:---|
| [`errors.toml`](errors.toml) | 错误码单一声明源（slug / severity / category / 元数据 / message 模板） |
| [`debug_api.toml`](debug_api.toml) | `aurora::debug` 公共自由函数声明源 |
| [`ERROR_CATALOG.md`](ERROR_CATALOG.md) | 由 `tools/gen/gen_error_codes.cpp` 生成的错误码全量清单（**生成物，勿手改**） |

三个文件路径被 `tools/` 与 `src/aurora/core/diagnostics.cpp` 硬编码，**不可移动或改名**。

---

## 9 命名速查

| 场景 | 用法 |
|:---|:---|
| 完整命名空间 | `aurora::Button` |
| 推荐别名 | `namespace au = aurora;` → `au::Button` |
| 头文件 | `#include "aurora/aurora.h"` |
| CLI 工具 | `aurora components / describe / search / validate / snapshot / render / preview / to-code / to-yaml / schema` |
| MCP 服务 | `aurora_mcp` |
| LSP 服务 | `aurora_lsp` |
| API Schema 文件 | `aurora_api.json` |
| 错误码（枚举 / slug） | `LayoutNullChild` / `layout-null-child`（slug 冻结为对外契约；命名空间 `aurora::ErrorCode`） |

---

## 10 API 兼容策略

AI 的训练数据中可能包含同一库的多个版本。当 Aurora API 演进时：

```cpp
// ✅ 向后兼容的废弃策略
[[deprecated("Use .text() instead.")]]
Button& setCaption(std::string s) { return text(std::move(s)); }
```

**原则**：非主版本**只增不删**——API 一旦发布，签名在 MINOR / PATCH 内不改变。因为 AI 无法「忘记」旧 API。破坏性变更只能进 MAJOR，并须在 `CHANGELOG.json` 记录 `breakingChanges` 且提供 `migrations`。详见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §7。

---

## 11 终极检验标准

> **如果一个从未见过 Aurora 的 LLM，仅凭有限 token 的 system prompt（含 API schema 摘要 + 3 个示例），能否正确生成一个包含 5 个组件的表单 UI，且一次编译通过？**

如果不能，说明 API 设计还不够 AI-First。

这个「一次通过」标准应当作为 Aurora 的**持续集成测试**——每次 API 变更后，用多个 LLM 做生成测试，通过率低于阈值就回滚变更。离线近似由 `ai_compat_test` 承担：遍历 `tests/fixtures/ai_compat/` 下的 JSON fixture（`valid_*` 期望通过、`error_*` 期望报错），无 LLM 调用。

```bash
./build/ai_compat_test
# ✓ 9/10 generations compiled successfully
# ✓ 8/10 matched expected structure
# ✗ 1/10 used deprecated .setCaption() → FAIL (need better naming)
```

**这才是 AI-First 的真正含义：不是为 AI 加功能，而是让 AI 成为 API 设计的第一用户和持续测试者。**
