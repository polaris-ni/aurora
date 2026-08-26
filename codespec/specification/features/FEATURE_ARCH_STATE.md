# B. 架构与状态层（#6–#9）

> 本文件是「三、特性详细规范」子文档，覆盖 **§B.**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #6 单向数据流 + 细粒度信号状态模型

**核心目标：** AI 易理解状态

Aurora 采用 **细粒度信号（fine-grained signals）** 模型：UI 是一棵静态构建的组件树，状态保存在 `State<T>`
中；组件在渲染期"读取"信号时自动登记依赖；信号变化只重跑依赖它的局部计算，不重建整棵树、不做 key/diff。

```cpp
// 1) 信号是状态的最小单元（读时登记依赖，写时定点刷新）
au::State<int> count{0};

// 2) 把信号直接作为属性传给组件（属性类型为 Reactive<T>，可持有常量或信号）
au::Column(au::ColumnProps{
    .children = {
        std::move(au::Text(au::Computed<au::LocalizedString>{ [&] {
            return au::LocalizedString{ "Count: " + std::to_string(count.get()) }; } })),
        std::move(au::Button(au::ButtonProps{ .label = "+1" })
                      .set_on_click([&]{ count.set(count.get() + 1); })),  // 写信号 → 仅刷新依赖
    },
});
```

**关键原则：**

- **State → View 是纯函数映射**：组件的 `paint` 是信号当前值的纯函数，AI 最容易生成
- **事件 → State 通过普通赋值**：`count.set(...)`，无需 Action / Reducer 样板
- **细粒度、定点刷新**：只有读取过该信号的组件/计算会被重跑（见 §H.1）
- **属性即信号**：`Reactive<T>` 属性既可接受常量（如 `.content = "Hi"`）也可接受信号——从 `State<T>` 构造为显式 `Reactive(std::shared_ptr<State<T>>)`，须经 `state()`/`shared()` 包装后传入，二者对组件透明
- **状态可快照**：`StateGraph::to_json()` 可导出依赖图结构（`state_graph.h`），便于 AI 做快照测试；`State<T>` 值本身不直接序列化

**可选：类 Redux 单向数据流（`Store`）**

若偏好单一可信源（single source of truth），Aurora 另提供 `au::Store<S>`（详见 `specification/subsystems/SUBSYSTEM_SIGNAL_MODIFIER.md` §H.1 与 `include/aurora/state/store.h`）：`Store` 内部持有 `State<S>`，用
`dispatch(Action)` + 纯函数 `Reducer` 产生新状态，并暴露 `as_signal()` 供组件像订阅普通信号一样订阅。两者可并存：高频局部状态用
`State<T>`，全局应用状态用 `Store<S>`。

```cpp
struct AppState { int counter = 0; };
using Action = au::Action;                         // 字符串标签 + 可选 payload
auto store = au::make_store(AppState{},
    [](const AppState& s, const Action& a) -> AppState {
        if (a.type == "increment") return { s.counter + 1 };
        return s;
    });
au::Button(au::ButtonProps{ .label = "+1" }).set_on_click([&]{ store->dispatch(au::Action{"increment"}); });
au::Text(std::to_string(store->as_signal().get().counter));
```

**约束：**

- 默认状态方式是细粒度 `State<T>`，AI 只需学这一条即可覆盖 90% 场景
- `State<T>` 变化固定在 UI 线程派发，无需锁（见 #18 单线程 UI）
- 禁止在渲染（构建）期产生副作用（写信号）——与常规声明式 UI 一致

---

#### #7 扁平组合模型 + 共享所有权组件

**核心目标：** AI 易追踪逻辑

**规范：**

```cpp
// ❌ 深层继承链（AI 难以追踪）：
// Widget → Container → InteractiveWidget → ButtonBase → ThemedButton → MaterialButton

// ✅ 组合模式（AI 容易理解）：
// au::Button = 内容(Text) + 背景(Modifier) + 点击行为(Modifier::clickable)
// 通过 Modifier 正交组合能力，而非继承
```

**关键约束：**

- 组件继承层级 **≤ 2 层**（叶控件直接继承 `Widget`；多子容器继承 `Container`，单子容器继承 `SingleChild`）
- 用 **组合（Composition）** 替代继承；横切能力（内边距、背景、可点击、尺寸、边框…）由 `Modifier` 正交组合表达（见 §H.2）
- 整棵 UI 树保存在一个 `Node` 中，`Node` 持有 `std::shared_ptr<Widget>`：
    - **拷贝即共享**：`auto b2 = b1;` 两个 `Node` 指向同一份 widget 实现，复制成本低
    - **移动即转移**：`parent.children.push_back(std::move(child))` 转移节点所有权
    - **整棵树可被复制/移动**，析构由 `shared_ptr` 自动管理，AI 无需手动 `delete`
- **几何权威在 `Node`**：`Node` 持有 `Rect m_bounds`（原点 + 尺寸），是布局与命中测试的唯一几何来源。 布局阶段由父节点经
  `child.set_bounds(box)` 写入，`Window::present_root` 把窗口矩形写入根 `Node` 的 `m_bounds`。
  `Widget` **不再持有任何几何缓存**（`m_bounds`/`bounds()`/`set_bounds()` 已彻底移除）。输入坐标本地化由
  `EventDispatcher` 在命中链冒泡时完成：命中链 `hit_test_chain` 返回 `std::vector<HitNode>`
  （`origin` 即该控件相对根的全局 origin），派发器对每个控件写入 `MouseEvent::local_position = position - origin`， 控件在
  `on_pointer_event` 中直接消费本地坐标（如 `Slider`/`Text`/`TextInput`），无需查询自身绝对位置。 **`HitNode`
  的生命周期契约**：节点同时持有裸指针 `ptr` 与弱引用 `guard`（构造时探测该控件是否由
  `shared_ptr` 持有，栈/成员控件的 `weak_from_this()` 为空弱引用，回退为裸指针）。取用方式有二：
    - `get()`：仅返回存活指针， **不带生命周期保证**，只可用于「不解引用」的用途（如比较是否同一控件）；
    - `lock(out_keepalive)`：返回存活指针 **并把强引用写入出参**，把控件生命周期延长至调用方作用域结束。

  凡要 **解引用**命中链节点（调用 `on_pointer_event`/`focusable()` 等）都必须用 `lock()`：派发回调 （用户 `on_click`
  ）可能销毁控件自身所在子树（如点击按钮触发 `push_replacement` 重建页面）， 而回调返回后基类 `Widget::on_pointer_event` 仍要写
  `m_pressed` 等成员，用裸指针即 use-after-free。
- **多数情况无需显式写 `Node{...}`**：`Node(W&&)` 是非 explicit 转换构造函数，值类型控件 （`au::Text("...")` /
  `au::Column{...}` / `std::move(w)` 等）可隐式转为 `Node`。仅在两分支类型不同的 `?:`
  三元、或 `std::make_shared<Widget>(...)` 这类需连续两次用户转换的场景才显式包 `Node{...}`。
- 属性（如 `Text.content`）为 `Reactive<T>`：既可持有常量也可持有信号（从 `std::shared_ptr<State<T>>` 显式构造），对组件透明
- 深层嵌套容易导致 AI 迷失，需提供 Builder 模式将深树拆成命名子函数

---

#### #8 显式优于隐式（含样式继承）

**核心目标：** AI 无理解盲区

**规范：**

```cpp
// ❌ 隐式 —— AI 不知道这里发生了什么
window.show();  // 内部自动创建渲染上下文、事件循环、平台窗口...

// ✅ 显式 —— AI 能完整理解控制流
auto app = au::Application{ build_ui() };   // 显式构造应用并传入根 UI
app.dispatch_click(x, y);                    // 显式注入输入
app.tick();                                  // 显式推进一帧（内部驱动布局/绘制/上屏）
```

**关键约束：**

- 没有"魔法"全局状态
- 没有隐藏的初始化顺序依赖
- 生命周期显式管理（RAII + 明确的 `create/destroy`）
- **主题、字体、颜色的继承链必须是显式参数或显式 scope 传递**，不能依赖上下文隐式获取

```cpp
// ❌ 隐式样式继承
// 父组件设了 font_size=20，子组件"自动继承"

// ✅ 显式样式传递
au::ThemeProvider{ my_theme, au::Column(au::ColumnProps{
    .children = {
        // 此子树内所有组件可经 ctx.environment<Theme>() 读取 my_theme
        // AI 可以明确看到样式来源
    },
}) };

// 或显式参数（不经隐式继承）
au::Text("Hello").font_size(20);   // 显式指定
```

---

#### #9 结构化错误信息（JSON 可解析）

**核心目标：** AI 易调试

**规范：**

```cpp
// ❌ 传统 C++ 错误（AI 无法有效解析）
// error: no matching function for call to 'children'

// ✅ Aurora 友好的错误信息
// [layout-null-child] Column.children():
//   - Received: 3 children (Text, Button, nullptr)
//   - Problem: 3rd child is nullptr. Did you forget to create the widget?
//   - Suggestion: Use Show for conditional children.（条件构造，见 widget/show.h）
//   - Location: main.cpp:42
```

**实现方式：**

- 自定义 `static_assert` 消息（C++20/23）
- 运行时错误返回 `au::Result<T>`（持 `Error`）而非抛异常
- 错误对象包含： **what / why / where / suggestion / docs_link**
- 提供 `au::validate(const Node& root, int max_depth = 64) -> Result<bool>` 在渲染前检查整棵 UI 树 （空子节点 /
  深度超限 / 未知控件类型；返回 `Result<bool>`，首个问题转为结构化 `Error`。见 `app/validate.h`）
- 错误格式化为 **机器可解析 JSON 格式**，AI 调试器 / 工具可直接消费，用于辅助定位并生成修复 **建议**（本库不自动修改用户代码）

> **设计决策：** 不使用 SARIF 格式。SARIF（Static Analysis Result Interchange Format）专为静态分析工具设计，而 Aurora
> 的错误涵盖运行时场景（如空子元素、非法属性值），不属于静态分析范畴。Aurora 采用自定义 JSON 错误格式，同时覆盖编译期和运行时错误。

```json
{
  "code": "layout-null-child",
  "message": "3rd child is nullptr",
  "location": {
    "file": "main.cpp",
    "line": 42,
    "column": 8
  },
  "suggestion": "Use Show for conditional children",
  "severity": "warning",
  "auto_fix": {
    "action": "wrap_with_show",
    "target": "child[2]"
  }
}
```

> **错误码全量清单（权威）**：所有 `au::Error`/`au::Result<T>` 错误码的真实产生点与语义见 [`ERROR_CATALOG.md`](../../ERROR_CATALOG.md)，由代码 `make_error(...)` 调用逐项核对，新增码须同步回写。

---
