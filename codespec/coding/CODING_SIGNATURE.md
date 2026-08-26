# CODING_SIGNATURE

> 本文件由 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) 划分而出（函数签名 / 内部工具层约定）。章节编号保持原样。
> 返回主线见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)。

**本文包含章节：**

- [9. 函数签名（参数与返回类型）](#9-函数签名参数与返回类型)
- [10. 内部工具层约定（Internal Utilities）](#10-内部工具层约定internal-utilities)

## 9. 函数签名（参数与返回类型）

> 函数参数的传递方式与返回类型写法，统一如下，面向可读性与 AI 生成友好性。

- **优先使用引用（9.1）**：当参数不需要转移所有权、且为必传（不可空）时，优先使用左值引用（`const T&` / `T&`
  ）而非裸指针；仅当需表达「可选 / 可空 / 可重绑定」或 C 互操作边界时才使用指针。引用消除空指针歧义、约束调用方传入有效对象，提升静态分析与
  AI 可读性。
- **未使用形参保留名称注释（9.2）**：在虚函数重写 / 接口实现等「签名必须保留但实现内不使用」的场景，不得省略参数名称（不写成
  `void f(int)`），而应以注释保留其语义名称，例如 `void on_event(Event /* ctx */)`。此写法既保留 API
  自描述性，又避免未使用参数告警。属向前遵循的约定，存量代码逐步落实。
- **尾置返回类型（9.3）**：函数（成员函数、自由函数）统一使用尾置返回类型 `auto f(...) -> Ret`，与
  `include/aurora/widget/widget.h` 等现有头文件风格一致（lambda
  与显然的短返回可省略）。尾置写法使参数列表首屏完整可见、复杂 / 模板返回类型更易对齐，利于 AI 生成与 diff 比对。
- **控制语句大括号不可省略（9.4）**：`if` / `for` / `while` / `do-while` 等控制语句，**即使受控体仅一行也必须使用 `{}`
  包裹**，禁止「尾随单语句省略大括号」的写法（如 `if (x) do_y();`）。该约束由 `.clang-format` 机械强制——已设置
  `AllowShortIfStatementsOnASingleLine: false` 与 `AllowShortLoopsOnASingleLine: false`，运行 `clang-format`
  会自动补齐缺失大括号；任何新增 / 修改的代码在格式化后将自动合规。此规则对 `switch` 的 `case` 标签（无受控体括号语义）不适用。

---

## 10. 内部工具层约定（Internal Utilities）

> 重构抽取的内部通用函数统一收口于 `include/aurora/core/`，避免新增命名空间与额外公共导出面。

- **header-only 编码/数学助手**：`core/utf8.h`（`utf8_encode` / `utf8_cp_len` / `utf8_cp_count` / `utf8_cp_slice`）为 header-only、零依赖、可被常量上下文使用，供 widget/window 统一调用以消除重复编码实现。
- **非导出内部辅助**：`core/string_util.h` + `core/string_util.cpp` 提供 `aurora::internal::string_format`（printf 风格、vsnprintf 自动扩容），**仅内部使用、不进 `aurora.h` 公共导出**，用于收敛各模块 `std::snprintf` 进栈缓冲的重复样板。
- **饱和（saturate）助手**：`core/math.h`（header-only）提供 `aurora::saturate(float)->[0,1]` 与 `aurora::saturate_u8(float)->uint8[0,255]`，收口渲染热路径（如 `render/painter.cpp` 像素/覆盖度钳制）中散落的 `std::clamp(x,0,255)` / `std::clamp(x,0.0f,1.0f)` 样板；二者为 `std::clamp` + 端点/cast 的语义收口，**不抽取与 `std` 重复的通用 `clamp`/`lerp`**。`saturate_u8` 保留 `static_cast<uint8_t>` 的截断（向零）语义。
- **归属原则**：跨多模块高频且语义中立的纯函数收口 `core/`；单领域复用就近归入所属模块；平台特定逻辑保留在对应后端（如 `window/swizzle.h`），不污染 `core/`。



