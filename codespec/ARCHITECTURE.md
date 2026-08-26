# Aurora 架构（ARCHITECTURE.md）

> 权威文档：架构（architecture）、运行时（runtime）、分层（layers）、模块映射（module map）、设计不变量（invariants）。
> 配套：`SPECIFICATIONS.md`（功能规格与 API 契约）、`CODING_STANDARDS.md`（编码规范）。
> 概念语义见 `CONCEPTS.md`；使用配方见 `GUIDELINE.md`。

---


本文档已划分为以下子文档（位于 `./<主题>/` 下）：

- [ARCHITECTURE_RUNTIME.md](./architecture/ARCHITECTURE_RUNTIME.md) — 运行时 / 模块映射 / 核心数据流
- [ARCHITECTURE_WIDGET.md](./architecture/ARCHITECTURE_WIDGET.md) — 组件树与组合模型 / 事件与命中测试 / 渲染与布局 / 序列化与元信息
- [ARCHITECTURE_PERF.md](./architecture/ARCHITECTURE_PERF.md) — 性能检测体系 / 设计不变量（实施记录见 `architecture/ARCHITECTURE_PERF_LOG.md`）
- [ARCHITECTURE_AI.md](./architecture/ARCHITECTURE_AI.md) — AI-first 设计原则
- [ARCHITECTURE_ERRORS.md](./architecture/ARCHITECTURE_ERRORS.md) — 错误处理架构（设计模型 / 分类 / 生成管线 / 传播策略）
- [ARCHITECTURE_TESTING.md](./architecture/ARCHITECTURE_TESTING.md) — 测试与 CI 架构（分层 / 组织约定 / 覆盖率 / CI 矩阵）


## 1. 概览（Overview）

Aurora 是一个现代 C++20 跨平台 GUI 库，以「声明式（declarative）+ 响应式（reactive）」为核心范式。
其 API 经过专门设计以对 **AI 编码助手友好**（详见第 12 章「AI-first 设计原则」）：概念可枚举、
声明式优于命令式、数据单向流动、错误信息携带「修复建议」。

区别于传统 GUI 框架，Aurora 不引入 GPU 依赖，采用纯软件栅格渲染（`Painter`），
并以内核/适配分离的分层架构实现跨平台（Win32/GDI、D3D11 GPU 上屏、GLFW/OpenGL、X11/Xlib、Wayland、Wasm/Canvas、macOS 骨架、Headless 内存）。

---

## 2. 分层架构（Layered Architecture）

```
┌───────────────────────────────────────────────┐
│  App / Examples (examples/)                      │  应用层：组装组件树、注册自定义控件
├───────────────────────────────────────────────┤
│  Widgets / Controls (include/aurora/widget/*)    │  组件层：声明式 UI 原语（Text/Button/Column…）
├───────────────────────────────────────────────┤
│  Reactive Core (include/aurora/state/*)          │  响应式核心：State/Signal/Store/Effect
├───────────────────────────────────────────────┤
│  Layout Engine (include/aurora/layout/*)         │  布局引擎：Constraints/flex/grid 求解
├───────────────────────────────────────────────┤
│  Render Core (include/aurora/render/*)           │  渲染核心：Painter（软件栅格）
├───────────────────────────────────────────────┤
│  Platform Abstraction (include/aurora/window/*)  │  平台抽象：Surface（Headless/Win32/Glfw）
├───────────────────────────────────────────────┤
│  Foundation (include/aurora/core/*)              │  基础层：types/Result/Error/Event/Log
└───────────────────────────────────────────────┘
```

- **依赖方向单向向下**：上层依赖下层，下层不感知上层（例如渲染核心不知道具体 widget）。
- **内核/适配分离**：`Painter` 只操作抽象帧缓冲；平台差异收敛到 `Surface` 实现。
- **响应式与渲染解耦**：状态变更经 `State/Signal` 精确投递到受影响的 widget 子树，
  不经过「整树 diff + 重建」；渲染核心只负责把 widget 树绘制到 `Surface`。

---

> 参考 [3. 运行时（Runtime）](./architecture/ARCHITECTURE_RUNTIME.md#3-运行时runtime)。

> 参考 [4. 模块映射（Module Map）](./architecture/ARCHITECTURE_RUNTIME.md#4-模块映射module-map)。

> 参考 [5. 核心数据流（Core Data Flow）](./architecture/ARCHITECTURE_RUNTIME.md#5-核心数据流core-data-flow)。

> 参考 [6. 组件树与组合模型（Widget Tree & Composition）](./architecture/ARCHITECTURE_WIDGET.md#6-组件树与组合模型widget-tree--composition)。

> 参考 [7. 事件与命中测试（Event & Hit-testing）](./architecture/ARCHITECTURE_WIDGET.md#7-事件与命中测试event--hit-testing)。

> 参考 [8. 渲染与布局（Render & Layout）](./architecture/ARCHITECTURE_WIDGET.md#8-渲染与布局render--layout)。

> 参考 [9. 序列化与元信息（Serialization & Meta）](./architecture/ARCHITECTURE_WIDGET.md#9-序列化与元信息serialization--meta)。

> 参考 [10. 性能检测体系（Performance Profiling）](./architecture/ARCHITECTURE_PERF.md#10-性能检测体系performance-profiling)。

> 参考 [11. 设计不变量（Invariants）](./architecture/ARCHITECTURE_PERF.md#11-设计不变量invariants)。

> 参考 [12. AI-first 设计原则](./architecture/ARCHITECTURE_AI.md#12-ai-first-设计原则)。

> 参考 [13. 错误处理架构（Error Handling Architecture）](./architecture/ARCHITECTURE_ERRORS.md#131-设计目标)。

> 参考 [14. 测试与 CI 架构（Testing & CI Architecture）](./architecture/ARCHITECTURE_TESTING.md#141-测试体系目标)。
