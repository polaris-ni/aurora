# Aurora：AI-First C++ 跨平台 UI 库设计规格书

> **项目名称：** Aurora
> **设计原则：** 以 AI 易于理解、易于使用、易于生成代码、易于调试为核心目标，构建一个 AI 友好型 C++ 跨平台 UI 库。
> **版本说明：** 本文档为设计规格（文档自身无版本号），库发布版本见 `CHANGELOG.json`（`currentVersion`）；文档与实现冲突时以代码运行时为准并回填本文档。
> **配套文档：** 架构与运行时不变量见 `ARCHITECTURE.md`；编码规范与 AI 友好性规则见 `CODING_STANDARDS.md`；核心概念与跨框架映射见
> `CONCEPTS.md`；复制即用配方见 `GUIDELINE.md`；项目结构与文档导航见根 `AGENTS.md`。

---

## 目录

- [〇、背景与需求](#〇背景与需求)
- [一、设计哲学](#一设计哲学)
- [二、完整特性清单（24 条）](#二完整特性清单24-条)
- [三、特性详细规范（划分为下列子文档）](#三特性详细规范)
    - 功能域（A–G）→ `specification/features/`（8 份）
        - [A. API 设计层 → `FEATURE_API_DESIGN.md`](./specification/features/FEATURE_API_DESIGN.md)
        - [B. 架构与状态层 → `FEATURE_ARCH_STATE.md`](./specification/features/FEATURE_ARCH_STATE.md)
        - [C. 运行时安全层 → `FEATURE_RUNTIME_SAFETY.md`](./specification/features/FEATURE_RUNTIME_SAFETY.md)
        - [D. 布局与渲染层 → `FEATURE_LAYOUT_RENDER.md`](./specification/features/FEATURE_LAYOUT_RENDER.md)
        - [E. 跨平台层 → `FEATURE_CROSS_PLATFORM.md`](./specification/features/FEATURE_CROSS_PLATFORM.md)
        - [F1. AI 工具链层：Inspector/序列化 → `FEATURE_AI_INSPECTION.md`](./specification/features/FEATURE_AI_INSPECTION.md)
        - [F2. AI 工具链层：Recipe/LSP → `FEATURE_AI_TOOLING.md`](./specification/features/FEATURE_AI_TOOLING.md)
        - [G. 工程约束 → `FEATURE_ENGINEERING.md`](./specification/features/FEATURE_ENGINEERING.md)
    - 核心子系统 API（H.1–H.10c）→ `specification/subsystems/`（7 份）
        - [H.1–H.2 信号 + Modifier → `SUBSYSTEM_SIGNAL_MODIFIER.md`](./specification/subsystems/SUBSYSTEM_SIGNAL_MODIFIER.md)
        - [H.3 动画 → `SUBSYSTEM_ANIMATION.md`](./specification/subsystems/SUBSYSTEM_ANIMATION.md)
        - [H.4/H.4.1 环境/DI + H.5 主题/i18n → `SUBSYSTEM_ENV_THEME.md`](./specification/subsystems/SUBSYSTEM_ENV_THEME.md)
        - [H.6 事件 + H.7 焦点 + H.8 导航 → `SUBSYSTEM_EVENT_FOCUS_NAV.md`](./specification/subsystems/SUBSYSTEM_EVENT_FOCUS_NAV.md)
        - [H.9 渲染与无头 → `SUBSYSTEM_RENDER_HEADLESS.md`](./specification/subsystems/SUBSYSTEM_RENDER_HEADLESS.md)
        - [H.10 App/Window + H.10b 调度 + H.10c DEBUG + H.10d 生命周期 → `SUBSYSTEM_APP_WINDOW.md`](./specification/subsystems/SUBSYSTEM_APP_WINDOW.md)
        - [H.11 平台 Shell + H.12 文字选中 → `SUBSYSTEM_PLATFORM_SHELL.md`](./specification/subsystems/SUBSYSTEM_PLATFORM_SHELL.md)
    - 核心子系统 API（H.11–H.17 + Log + AI-First）→ `specification/subsystems_api/`（6 份）
        - [H.11 序列化 → `SUBSYSTEM_API_SERIALIZE.md`](./specification/subsystems_api/SUBSYSTEM_API_SERIALIZE.md)
        - [H.12 布局引擎 → `SUBSYSTEM_API_LAYOUT_ENGINE.md`](./specification/subsystems_api/SUBSYSTEM_API_LAYOUT_ENGINE.md)
        - [H.13 控件清单 + H.13.1 可定制性 + VideoPlayer → `SUBSYSTEM_API_WIDGETS.md`](./specification/subsystems_api/SUBSYSTEM_API_WIDGETS.md)
        - [H.13b Inspector 面板 + H.13c 远程 → `SUBSYSTEM_API_INSPECTOR.md`](./specification/subsystems_api/SUBSYSTEM_API_INSPECTOR.md)
        - [H.14 自描述 + H.15 MCP/CLI + H.16 偏好 + H.17 性能 → `SUBSYSTEM_API_TOOLING.md`](./specification/subsystems_api/SUBSYSTEM_API_TOOLING.md)
        - [H.xx 日志 + H.yy AI-First → `SUBSYSTEM_API_LOG_AI.md`](./specification/subsystems_api/SUBSYSTEM_API_LOG_AI.md)
- [四、特性间的张力与解决](#四特性间的张力与解决)
- [五、不可分割组与开发路线图](#五不可分割组与开发路线图)
- [六、综合架构蓝图](#六综合架构蓝图)
- [七、终极检验标准](#七终极检验标准)
- [附录](#附录)

---

## 〇、背景与需求

> ⚠️ **冲突说明**：以真实实现为准

### 〇.1 愿景（Vision）

Aurora 要让「 **写 UI 像写声明式数据**」一样自然：开发者（与人类）用最小心智负担描述「界面应该是什么样」，而非「怎么一步步画出来」。它特别关注 **AI 编码助手能一次性生成可编译、可运行的界面代码**——这是区别于传统 GUI 库的根本目标。

### 〇.2 设计原则（Design Principles）

- **声明式优先**：界面是「状态 → 视图」的纯函数；改状态而非改树。
- **概念最小正交**：约 40 个 widget + 修饰 + 状态原语，无重叠。
- **概念可枚举**：全部 UI 原语可枚举、可命名、可映射（≤15 类，见 `CONCEPTS.md`）。
- **Token 经济**：API 表面紧凑，AI 在有限上下文即可装载全部概念。
- **AI 友好错误**：错误信息携带「修复建议」与文档锚点（见 §9 / `CODING_STANDARDS.md`）。
- **降级而非中止**：非法输入产出 `Diagnostics` 并降级，而非崩溃（见 §21）。
- **跨平台零依赖**：核心渲染不依赖 GPU；Win32/GDI 零三方依赖（见 §H.9 / `ARCHITECTURE.md`）。

### 〇.3 范围（Scope）

**包含**：声明式组件、响应式状态、布局引擎、软件渲染、平台
Surface（Headless/Win32/Glfw）、序列化、导航、动画、异步、定时任务（Scheduler / Timer）、环境注入。 **不包含（本版）**
：3D、硬件加速、原生控件嵌入、跨进程、复杂数据网格。

### 〇.4 非目标（Non-goals）

- 不做「又一套 CSS」：布局用代码表达，不引入样式表语言。
- 不做服务端渲染。
- 不做可视化拖拽编辑器（除非社区驱动）。
- 不做 Playground（见 `CODING_STANDARDS.md` 四.8）。MCP/CLI 与 LSP 已提供（见 §H.15）。

### 〇.5 技术约束（Constraints）

- C++20 最小标准。
- 静态库交付（非 header-only）。
- 软件栅格渲染，无 GPU 依赖。
- Windows 优先验证（Win32/GDI），跨平台后端可选（见 §H.9 / `ARCHITECTURE.md`）。

---

## 一、设计哲学

### 1.1 为什么需要 "AI First"？

AI（LLM）使用 UI 库的方式与人类有本质不同：

| 维度     | 人类开发者               | AI / LLM                             |
|:---------|:-------------------------|:-------------------------------------|
| 学习方式 | 读文档、看教程、试错     | 基于训练语料中的**模式匹配**         |
| 记忆方式 | 理解原理后灵活应用       | 依赖**高频模式**和**一致性**         |
| 调试方式 | 设断点、看调用栈、凭经验 | 依赖**错误信息文本**和**代码上下文** |
| 生成方式 | 从需求出发设计           | 从**已有模式**组合拼装               |

### 1.2 核心设计原则

> **可预测、一致、自描述、少歧义、易验证。**

Aurora 本质上是一个 **把 UI 开发变成"结构化数据描述"问题**的库。AI 最擅长处理结构化的、模式一致的、可验证的任务——Aurora
的设计让 UI 开发恰好落入这个甜蜜区。

### 1.3 AI 生成代码的两大失败来源

1. **内存与所有权错误**：悬空指针、use-after-free、double delete、内存泄漏
2. **并发与线程安全错误**：死锁、竞态条件、在错误线程更新 UI

Aurora 的设计必须从根本上消除这两类错误的可能性。

---

## 二、完整特性清单（24 条）

| #  | 特性                                       | 核心目标                  |
|:---|:-------------------------------------------|:--------------------------|
| 1  | 声明式双模 API（链式 / 分步 / 配置块等价） | AI 易生成                 |
| 2  | 极致命名一致性 + 扁平命名空间              | AI 易补全                 |
| 3  | 正交可组合的最小核心 API                   | AI 少幻觉                 |
| 4  | 强类型 + 单位标注 + 编译期校验             | 编译即验证                |
| 5  | 合理默认值（声明处可见）                   | AI 少写少错               |
| 6  | 单向数据流 + 细粒度信号状态模型            | AI 易理解状态             |
| 7  | 扁平组合模型 + 共享所有权组件              | AI 易追踪逻辑             |
| 8  | 显式优于隐式（含样式继承）                 | AI 无理解盲区             |
| 9  | 结构化错误信息（JSON 可解析）              | AI 易调试                 |
| 10 | 内置 UI Inspector（WebSocket/MCP 接口）    | AI 可观测运行时           |
| 11 | 确定性渲染 + 逻辑快照测试                  | AI 可验证正确性           |
| 12 | 机器可读 API Schema（JSON Schema）         | AI 工具链直接消费         |
| 13 | UI 树序列化 + 差分 Patch 协议              | AI 可增量修改 UI          |
| 14 | 零 #ifdef 跨平台 + 插件式平台扩展          | AI 无需处理平台分支       |
| 15 | 跨平台一致行为 + 黄金文件验证              | AI 无需考虑平台差异       |
| 16 | 示例驱动文档（Recipe 形式）                | AI 从示例高效学习         |
| 17 | LSP / MCP Server / CLI 工具链              | AI Agent 直接集成         |
| 18 | 安全的内存与所有权模型                     | AI 生成无内存错误的代码   |
| 19 | 结构化异步与并发模型                       | AI 轻松处理耗时操作       |
| 20 | 布局系统的代数一致性                       | AI 可推理尺寸和位置       |
| 21 | 错误恢复与降级渲染                         | AI 生成的错误 UI 不会崩溃 |
| 22 | 可逆性：UI → 代码的参考还原                | AI 可分析现有界面并重构   |
| 23 | 部分代码容错（半成品可编译可运行）         | AI 可增量开发             |
| 24 | Token 效率 + 编译速度约束                  | AI 迭代循环效率           |

---

## 三、特性详细规范

> 本章体量较大，已划分为 21 份子文档，分布在 `codespec/specification/` 下的三个子目录：
>
> - **`features/`**（功能域 A–G，8 份）：`FEATURE_API_DESIGN` / `FEATURE_ARCH_STATE` / `FEATURE_RUNTIME_SAFETY` / `FEATURE_LAYOUT_RENDER` / `FEATURE_CROSS_PLATFORM` / `FEATURE_AI_INSPECTION` / `FEATURE_AI_TOOLING` / `FEATURE_ENGINEERING`
> - **`subsystems/`**（核心子系统 H.1–H.10c，7 份）：`SUBSYSTEM_SIGNAL_MODIFIER` / `SUBSYSTEM_ANIMATION` / `SUBSYSTEM_ENV_THEME` / `SUBSYSTEM_EVENT_FOCUS_NAV` / `SUBSYSTEM_RENDER_HEADLESS` / `SUBSYSTEM_APP_WINDOW` / `SUBSYSTEM_PLATFORM_SHELL`
> - **`subsystems_api/`**（核心子系统 H.11–H.17 + Log + AI-First，6 份）：`SUBSYSTEM_API_SERIALIZE` / `SUBSYSTEM_API_LAYOUT_ENGINE` / `SUBSYSTEM_API_WIDGETS` / `SUBSYSTEM_API_INSPECTOR` / `SUBSYSTEM_API_TOOLING` / `SUBSYSTEM_API_LOG_AI`
>
> 完整的逐文件清单与链接见上方 [目录](#三特性详细规范)。各子文档内章节编号（A/B/C…、H.1/H.2…）保持原样；交叉引用（如 `§H.9`）仍按原名指代对应章节。

## 四、特性间的张力与解决

| 张力对                         | 冲突点                                      | 解决方案                                                                                |
|:-------------------------------|:--------------------------------------------|:----------------------------------------------------------------------------------------|
| #5 默认值 vs #8 显式           | 默认值是"隐式"的                            | **默认值在声明处可见**（LSP hover 显示），运行时可查询 `btn.defaults()`，但代码中可省略 |
| #1 链式 vs #3 最小API          | 链式需要每个方法返回 `this`，增加 API 面    | 链式方法 = 属性 setter 的语法糖，不增加新概念                                           |
| #6 细粒度信号 vs #7 共享所有权 | 信号变化需定点刷新，但组件树需可被复制/移动 | `Node` 持有 `shared_ptr<Widget>`（拷贝即共享），信号变化仅重绘依赖组件（#18/#H.1）      |
| #4 强类型 vs 编译速度          | 大量模板/概念检查拖慢编译                   | 核心路径用简单类型，高级校验放在**独立验证工具**中（CLI），不阻塞编译                   |
| #12 机器Schema vs #2 命名一致  | Schema 需要额外维护                         | Schema 从代码**自动生成**（通过宏/注解/编译插件），保证与实现同步                       |
| #6 信号刷新 vs 高频交互        | 每帧走全链路延迟不可接受                    | 默认细粒度信号定点刷新；高频绘制用 `Canvas`/`DirectHandler` opt-in（#H.1/#H.6）         |
| #18 单线程 UI vs #19 异步      | 异步结果需回 UI 线程                        | `au::async` 经主线程投递器回到 UI 线程（#18/#19）                                       |
| #2 命名一致 vs 历史 API        | AI 可能混用新旧命名                         | 文档统一 snake_case 属性 + CamelCase 类型，废弃名仅在兼容层标注（#2/附录 C）            |

---

## 五、不可分割组与开发路线图

### 5.1 不可分割组

某些特性 **必须同时实现**才有意义，单独实现任何一个都会产生"半成品陷阱"：

| 不可分割组      | 包含特性               | 理由                                        |
|:----------------|:-----------------------|:--------------------------------------------|
| **AI 生成闭环** | #1 + #2 + #3 + #4 + #5 | 缺任何一个，AI 生成的代码都无法一次编译通过 |
| **AI 调试闭环** | #9 + #21 + #10 + #11   | 缺任何一个，AI 无法从错误中恢复并迭代       |
| **AI 状态推理** | #6 + #18 + #19         | 缺任何一个，AI 无法正确处理交互和异步       |
| **AI 布局推理** | #20 + #4 + #11         | 缺任何一个，AI 无法预测布局结果             |
| **AI 工具集成** | #12 + #13 + #17 + #22  | 缺任何一个，AI Agent 无法与运行时交互       |

### 5.2 优先级排序

按"对 AI 友好度的边际贡献"排序：

| 优先级 | 特性                           | 理由                    |
|:-------|:-------------------------------|:------------------------|
| **P0** | #1 #2 #3 #4 #5（API 设计层）   | AI 生成正确代码的基础   |
| **P0** | #9 #21 #23（错误与容错）       | AI 调试闭环的基础       |
| **P0** | #11 #20（确定性渲染与布局）    | AI 验证正确性的基础     |
| **P0** | #6 #18 #19（状态、内存、异步） | AI 处理运行时逻辑的基础 |
| **P1** | #10 #12 #13 #17（工具链）      | AI Agent 集成的基础     |
| **P1** | #14 #15 #16（跨平台与文档）    | 减少 AI 需要处理的变量  |
| **P2** | #22 #24（可逆性与效率）        | 锦上添花，非核心依赖    |

### 5.3 能力闭环总览

Aurora 的特性按「AI 兼容性闭环组」组织，各组在当前版本均已交付：

```text
生成闭环：AI 可生成编译通过的静态 Aurora UI（API 设计层 + 内存安全）
布局与渲染推理：AI 可预测布局结果并验证（确定性渲染 + 布局推理）
状态与交互：AI 可生成交互式应用并迭代调试（信号状态 + 异步 + 错误恢复）
工具集成：AI Agent 可与 Aurora 运行时交互（MCP / LSP / CLI / Inspector）
跨平台生产化：多平台 Surface、示例文档、效率优化、可逆性（Undo/Diff）
```

各能力的规格详见 §H 与附录。

---

## 六、综合架构蓝图

```text
┌──────────────────────────────────────────────────────────────┐
│                  Aurora AI Tooling Layer                      │
│   aurora-mcp · aurora-lsp · aurora CLI · AI Compat Test     │
│   aurora_api.json · Recipe Search · Diff Patch · to_code()   │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Serialization Layer                   │
│   to_json / from_json · Diff/Patch · Undo · Canonical Form   │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Component Layer                       │
│   30 核心组件 · 共享所有权(shared_ptr<Node>) · WidgetId 引用 · 自描述 │
│   双模 API(指定初始化器/链式setter) · 降级渲染 · 部分代码容错      │
├──────────────────────────────────────────────────────────────┤
│   Modifier · 动画(AnimationController) · 环境(Provider) · 导航(Navigator) │
│   序列化(to_json/diff) · i18n(LocalizedString) · 焦点(FocusManager)    │
├──────────────────────────────────────────────────────────────┤
│                  Aurora State + Event Layer                   │
│   信号 State/Reactive/Computed/Effect · 可选 Store<State>      │
│   细粒度定点刷新 · Action 派发(可选) + DirectHandler(opt-in)    │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Async Layer                           │
│   aurora::ThreadPool(有界 worker) · au::async() · au::co_async() │
│   CoroTask<T> 协程 · 自动 UI 线程回归 · 取消/超时 · 错误处理    │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Layout Engine                         │
│   7 正交原语 + 15 组合配方 · 代数一致盒模型                  │
│   无边距合并 · 显式百分比参照 · 溢出策略 · 确定性求解        │
│   两遍布局（宽→高）· 动态内容支持 · 动画不影响布局          │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Platform Abstraction                  │
│   零 #ifdef · 运行时能力查询 · 插件式扩展                    │
│   Win / Mac / Linux / iOS / Android / Web(WASM)              │
│   WASM: rAF 事件循环 · Web Worker 异步 · Canvas 渲染        │
├──────────────────────────────────────────────────────────────┤
│                  Aurora Rendering Core                        │
│   自研光栅化 · 像素级一致 · 离屏渲染 · 无头模式             │
│   逻辑快照(L1) + 盒模型快照(L2) + 像素快照(L3)              │
└──────────────────────────────────────────────────────────────┘
```

---

## 七、终极检验标准

> **如果一个从未见过 Aurora 的 LLM，仅凭 500 token 的 system prompt（含 API schema 摘要 + 3 个示例），能否正确生成一个包含 5
个组件的表单 UI，且一次编译通过？**

如果不能，说明 API 设计还不够 AI-First。

这个"500 token 一次通过"应该作为 Aurora 的 **持续集成测试**——每次 API 变更后，用多个 LLM 做生成测试，通过率低于 90% 就回滚变更：

```bash
# CI 中的 AI 兼容性测试
$ aurora ai-compat-test --model gpt-4o --prompt "Create a login form" --pass-rate 0.9
✓ 9/10 generations compiled successfully
✓ 8/10 matched expected structure
✗ 1/10 used deprecated .setCaption() → FAIL (need better naming)
```

**这才是 AI-First 的真正含义：不是为 AI 加功能，而是让 AI 成为 API 设计的第一用户和持续测试者。**

---

## 附录

### 附录 A：API 版本兼容策略

AI 的训练数据中可能包含同一库的多个版本。当 Aurora API 演进时：

```cpp
// ❌ 破坏性重命名 —— AI 会混用新旧 API
// v1: button.setCaption("OK")
// v2: button.text("OK")

// ✅ 向后兼容的废弃策略
[[deprecated("Use .text() instead. Removed in Aurora v3.0")]]
Button& setCaption(std::string s) { return text(std::move(s)); }

// ✅ 提供版本命名空间（极端情况）
namespace aurora::v2 { /* ... */ }
```

**原则：API 一旦发布，签名永不改变，只增不删（直到大版本）。因为 AI 无法"忘记"旧 API。（详见 `CODING_STANDARDS.md` 版本管理）**

---

### 附录 B：组件运行时自描述能力

> 每个控件实现 `static describe_static()` + `virtual describe()`，返回完整 `WidgetDescriptor`；
> `component_schema()` / `list_all_schemas()` 消费 describe () 输出；`aurora_api.json` 自动包含增强字段。
> 详见 §H.14。

```cpp
// 任何 Aurora 组件都能在运行时描述自己的完整 API
auto info = au::Button::describe_static();
// WidgetDescriptor{
//   .name = "Button",
//   .ns = "aurora",
//   .properties = {
//     {"label","LocalizedString","\"\"",true,"按钮文字"},
//     {"color","Color","Color::blue()",false,"背景色"},
//     ...
//   },
//   .events = {"on_click"},
//   .children_policy = "none",
//   .examples = {"au::Button(au::ButtonProps{ .label = \"OK\" })"}
// }

// 运行时多态调用
au::Button btn;
au::Widget &w = btn;
auto desc = w.describe();  // 同上

// 批量发现
auto all = au::serialization::list_all_schemas();
```

这使得 AI Agent 可以在 **运行时**动态发现 Aurora API，而不完全依赖训练数据。

---

### 附录 C：Aurora 项目命名规范速查

| 场景               | 用法                                    |
|:-------------------|:----------------------------------------|
| 完整命名空间       | `aurora::Button`                        |
| 推荐别名           | `namespace au = aurora;` → `au::Button` |
| 头文件（兼容模式） | `#include <aurora/aurora.h>`            |
| CLI 工具           | `aurora validate / preview / snapshot`  |
| MCP 服务           | `aurora-mcp`                            |
| LSP 服务           | `aurora-lsp`                            |
| API Schema 文件    | `aurora_api.json`                       |
| 错误码前缀         | `Aurora::LayoutError::NullChild`        |

---

*Aurora Design Specification — 配套文档导航见根 AGENTS.md。*
