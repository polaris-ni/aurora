# ARCHITECTURE_AI

> 本文件由 [`ARCHITECTURE.md`](../ARCHITECTURE.md) 划分而出（AI-first 设计原则），章节采用全局连续编号 §12.x（与 §3–§14 各架构文档统一）。
> 返回主线见 [`ARCHITECTURE.md`](../ARCHITECTURE.md)。

**本文包含章节：**

- [12. AI-first 设计原则](#12-ai-first-设计原则)

## 12. AI-first 设计原则

> 本库从立项起就面向「AI 编码助手友好」设计，以下为架构与设计理念层的原则。
> **编码规则层** 的条目（命名/错误/工具链等）见 `CODING_STANDARDS.md`「AI 友好性」章节；
> 跨框架概念映射与核心概念审计见 `CONCEPTS.md`。

### 12.1 Token 经济性 API 表面保持紧凑（约 40 个 widget + 修饰 + 状态原语），核心概念可枚举、可命名、可映射
（见 `CONCEPTS.md`），使 AI 在有限上下文窗口内即可装载全部概念。

### 12.2 概念可枚举性 全部 UI 原语可枚举、可命名、可映射。详见 `CONCEPTS.md` 的「核心概念审计」。

### 12.3 概念映射透明性 Aurora 概念与 React / Flutter / Qt 一一对应，见 `CONCEPTS.md`「跨框架概念映射」。

### 12.4 声明式优于命令式 组件以不可变声明式树表达，状态变更驱动局部刷新；
禁止命令式事后改树（如 React 时代 `ref.current.appendChild`）。

### 12.5 最小正交 API API 正交：布局用 `Column/Row/Stack/Grid/Scroll`，装饰用 `Modifier`，状态用 `State/Signal/Store`，
互不重叠；避免「多种方式做同一件事」带来的选择困惑。

### 12.6 线程模型 单线程 UI + 跨线程安全写入（`State::set`）。在文档与示例明确标注，避免 AI 误用多线程改树。

### 12.7 显式数据流 状态经 `State/Signal/Store` 显式流动，依赖图可静态推导，便于 AI 推断「改 X 会影响哪些 widget」。

### 12.8 单向 / 纯函数 状态变更单向；`Computed`（代码中无独立 `Memo` 类型，`Memo` 为历史/别名说法）为纯函数派生值，无副作用，可安全重算。

### 12.9 扁平组合 以 `Modifier` 包裹 + 容器组合替代深层继承嵌套；库提供 `Spacer`、布局对齐等简化扁平表达。

### 12.10 布局代数 布局以可组合的 `Constraints` + 对齐参数表达（flex 权重、对齐枚举），避免魔法数字。

### 12.11 事件模型 统一事件类型（`MouseEvent`/`KeyEvent`/`ScrollEvent`/`TextInputEvent`）+ 冒泡协议，
事件坐标在 `e.position`（非 `e.x/e.y`），见 `CODING_STANDARDS.md` 1.5。

### 12.12 状态作用域 状态作用域显式：局部 `State` 由组件持有，全局 `Store` 由应用持有并通过 `Environment` 注入；
`Binding<T>` 为非拥有引用，上游生命周期须更长。
