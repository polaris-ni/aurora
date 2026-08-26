# 错误处理架构（Error Handling Architecture）

> 本文档描述 Aurora **错误处理的设计模型与架构分层**，属于架构层（architecture）。
> - 错误**规则与写法**（命名、何时返回 `Result`、断言边界等）见 [`CODING_ERRORS_NAMING.md` §1](../coding/CODING_ERRORS_NAMING.md#1-错误处理error-handling对应规格-9)（对应规格 §9）。
> - 错误**码目录（数据）**由 `tools/gen_error_codes.cpp` 自动生成并维护于 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)，**请勿手改**。
> - 本文档不重复上述两份内容，只说明「错误在系统里如何被建模、分类、生成与传播」。

---

## 13.1 设计目标

错误处理遵循规格 §9 P0 约束：**机器可解析的错误**（machine-parseable errors）。

- 每个错误同时服务于两类受众：
  - **进程外**（JSON / 日志 / IDE 工具 / AI 编码助手）：只认冻结 `slug`（如 `"nav-depth-exceeded"`，改名标识符也不变）与 `code_enum`（C++ 标识符，调试用）。
  - **进程内**：用 `code_enum` 做类型安全分支（`err.code_enum == ErrorCode::X`），用 `severity` / `category` / `auto_fixable` / `retryable` 等元数据做策略判断。
- 失败路径统一：可恢复失败经 `Result<T>` 沿调用链返回，不靠异常横跨业务边界。

---

## 13.2 错误类型模型

核心类型定义在 [`include/aurora/core/result.h`](../../include/aurora/core/result.h)，错误码枚举在 [`include/aurora/core/error_codes.gen.h`](../../include/aurora/core/error_codes.gen.h)（自动生成）。

### 13.2.1 `Error` — 结构化错误

```cpp
struct Error {
    std::string code;        // 冻结对外 slug，只增不删
    std::string message;     // 人类可读描述（模板 + params 渲染，或调用方覆盖）
    std::string suggestion;  // 可选：修复建议
    std::string docs;        // 可选：文档链接/章节
    std::string where;       // 可选：发生位置（file:line）
    std::string hint;        // 修复提示（来自表，可被 make_error 覆盖）
    ErrorCode     code_enum;        // 编译期枚举码
    ErrorSeverity severity;          // 来自 errors.toml
    ErrorCategory category;          // 来自 errors.toml
    bool          auto_fixable;      // 来自 errors.toml
    bool          retryable;         // 来自 errors.toml
    std::string   fix_category;      // 修复策略分类
    std::string   fix_params;        // 修复参数（JSON 字符串）
};
```

字段分两类受众（见 §13.1）：`code` / `code_enum` 供机器解析，`severity` / `category` / `auto_fixable` / `retryable` / `fix_category` 供进程内策略判断。所有表驱动元数据由 `errors.toml` 经生成器产出，经 `make_error` 自动填充，无需手填。

### 13.2.2 `Result<T>` — 统一失败路径

```cpp
template<typename T> class Result {
    std::variant<T, Error> m_data;
    bool ok() const;
    explicit operator bool() const;   // if (result) 语境
    const T& value() const;           // 成功值
    const Error& error() const;       // 失败错误
    T unwrap() const;                 // 失败抛 std::runtime_error（仅不可恢复场景）
};
```

成功持 `T`，失败持结构化 `Error`。`ok()` / `operator bool()` / `value()` / `error()` 提供解包；`unwrap()` 仅在不可恢复场景下把错误转为异常。

### 13.2.3 `Result<void>` 特化

用于只关心「是否出错」的接口（如 `flush` / `reload`），统一失败路径。因 `std::variant<void, ...>` 非法，特化以 `bool m_ok` 标记成功态。

### 13.2.4 构造入口 `make_error` + `ErrorParams`

- `make_error(ErrorCode code, const ErrorParams& params, std::string hint = {})`：用 `errors.toml` 的 `message` 模板渲染 `message`（占位符 `{key}` 由 `ErrorParams` 渲染）。
- 另提供「调用方自定义 message」与「向后兼容（message + suggestion/docs/where）」重载；slug / severity / category / fix 元数据始终来自表，来源唯一。

---

## 13.3 错误分类与元数据

枚举定义在 `error_codes.gen.h`：

| 维度 | 取值 | 用途 |
|------|------|------|
| `ErrorCategory` | `General` / `Layout` / `Widget` / `Render` / `Io` / `Validation` / `Navigation` / `Platform` / `Runtime` / `Generation` / `Diagnostic`（共 11 域） | 错误域归类，过滤与聚合 |
| `ErrorSeverity` | `Info` / `Warning` / `Error` / `Fatal` | 严重度分级，决定上报与中断策略 |
| `auto_fixable` | `bool` | 是否可被工具/IDE 自动修复 |
| `retryable` | `bool` | 是否可重试（如异步超时 `RuntimeAsyncTimeout`） |
| `fix_category` / `fix_params` | 字符串 | 修复策略分类与参数（如 `type_error` / `missing_prop`） |

> 所有元数据由 `codespec/errors.toml` 声明，生成器填充，不在代码中手填。完整取值见 [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)。

---

## 13.4 错误码生成管线

错误码**不是手写的常量**，而是一条代码生成链：

```
codespec/errors.toml          (源：slug / severity / category / 元数据 / message 模板)
        │  tools/gen_error_codes.cpp
        ▼
┌───────────────────────────────────────────────────────────────┐
│  include/aurora/core/error_codes.gen.h   (ErrorCode 枚举 + 表)  │
│  codespec/ERROR_CATALOG.md               (人类可读目录)          │
│  aurora_api.json  (error_codes 段，供 AI/工具消费)               │
└───────────────────────────────────────────────────────────────┘
```

- **`slug` 冻结对外契约**：跨语言 / JSON / 日志只认 `slug`；`enum` 为 C++ 标识符可自由改名。
- **只增不删**：新增错误码追加，不重用/删除已发布 `slug`（避免破坏对外契约）。
- 三处生成物读现有文件、只写各自段，可任意顺序运行（详见 `aurora_api.json` 维护约定）。

---

## 13.5 错误传播策略

- **可恢复失败**：沿调用链返回 `Result<T>`，调用方用 `if (result)` / `result.ok()` 检查；所有返回 `Result` 的接口标注 `[[nodiscard]]`，避免吞错。
- **不可恢复错误**（断言边界，对应规格 §9 不可恢复决策树）：使用 `assert` / 前置条件检查，见 [`include/aurora/core/assert.h`](../../include/aurora/core/assert.h) 与 [`CODING_ERRORS_NAMING.md`](../coding/CODING_ERRORS_NAMING.md#1-错误处理error-handling对应规格-9)。
- `unwrap()` 仅用于不可恢复场景（把错误转为 `std::runtime_error`），业务边界不应依赖它做流程控制。

---

## 13.6 与其他文档的关系

| 主题 | 权威文档 | 本文档的角色 |
|------|----------|--------------|
| 错误写法 / 命名 / 断言边界 | [`CODING_ERRORS_NAMING.md` §1](../coding/CODING_ERRORS_NAMING.md#1-错误处理error-handling对应规格-9)（规格 §9） | 设计模型概述 + 交叉引用 |
| 错误码目录（数据） | [`ERROR_CATALOG.md`](../ERROR_CATALOG.md)（自动生成） | 不复制；只说明生成管线与契约 |
| 错误码源定义 | `codespec/errors.toml` + `tools/gen_error_codes.cpp` | 说明「码从哪来」 |
| 类型定义 | `include/aurora/core/result.h` / `error_codes.gen.h` | 引用，不重述成员 |
