# A. API 设计层（#1–#5）

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 相关核心子系统实现（H 系列）见 [`../subsystems/`](../subsystems)（H.1–H.10c 信号/动画/环境/事件/渲染/窗口/平台）与 [`../subsystems_api/`](../subsystems_api)（H.11–H.17 + Log + AI-First 序列化/布局/控件/Inspector/工具/日志）。

#### #1 声明式双模 API（链式 / 分步 / 配置块等价）

**核心目标：** AI 易生成

**问题背景：** Fluent API 往往产生长链式调用，AI 容易在链中间遗漏括号或参数顺序错误。

**规范：**

提供三种语义完全等价的构建模式，AI 可根据复杂度选择：

```cpp
// 模式 A：链式 setter（适合简短组件）—— setter 返回 *this
auto btn = au::Button(au::ButtonProps{ .label = "OK" });
btn.set_on_click(fn);

// 模式 B：*Props 聚合（C++20 聚合初始化，适合嵌套结构，AI 可分块生成）
auto btn2 = au::Button(au::ButtonProps{ .label = "OK" });
btn2.on_click = fn;                  // on_click 是 Button 成员（非 ButtonProps 字段）
btn2.background(au::colors::Blue);  // 背景色即"变体"区分手段

// 模式 C：分步（适合复杂组件，AI 可逐行生成）
auto btn3 = au::Button();
btn3.label = au::LocalizedString{ "OK" };
btn3.on_click = fn;
```

**关键约束：**

- **控件类不是聚合类型**，不能用 `au::Button{ .label = ... }` 这类「指定初始化器」构造控件（编译失败）。等价且合法的三种写法是：①
  位置式 / `*Props` 聚合：`au::Button(au::ButtonProps{ .label = "..." })`；② 初始化列表：
  `au::Column{ au::Text("A"), au::Text("B") }`（`Column`/`Row` 接受 `std::initializer_list<Node>`）；③ 对 **已构造对象**
  直接成员赋值：`auto b = au::Button(); b.label = au::LocalizedString{ "..." };`。链式 setter（如 `.font_size(24).bold()`
  ）返回引用，作为子节点放入 `children` 时必须用 `std::move` 包裹（`Widget` 拷贝构造被删除，`Node` 仅移动派生对象）。
- 指定初始化器 **仅适用于 `*Props` 聚合结构**（如 `au::ButtonProps{ .label = ... }`）与 `Theme` 等纯数据聚合；其中字段顺序无关，遗漏字段回退默认值。
- 嵌套声明推荐：`au::Column(au::ColumnProps{ .children = { ... } })` 的树形结构（控件组合见 §H.13）
- 控件可用 `au::` 或 `aurora::` 前缀；推荐 `namespace au = aurora;`
- 所有控件统一采用 **继承式双模 API**：`class Xxx : public XxxProps`，`XxxProps` 的字段即控件自身公有字段（如
  `Text.content`、`TextInput.value`、`Scroll.step`、`Grid.columns/gap`、`ImageView.bitmap/source`），不再用私有 `m_*`
  重复声明同一属性（见 §1 / 编码规范 §2 (1.5) / §7 (一.5)）

```cpp
auto page = au::Column(au::ColumnProps{
    .children = {
        std::move(au::Text("Welcome").font_size(24).bold()),
        au::Row(au::RowProps{
            .children = {
                au::TextInput(au::TextInputProps{ .placeholder = "Username" }),
                std::move(au::Button(au::ButtonProps{ .label = "Login" }).set_on_click(handle_login)),
            },
            .gap = 8,
        }),
        au::Spacer{},
        std::move(au::Text("v1.0").font_size(12).color(au::colors::Gray)),
    },
    .gap = 12,
});
```

---

#### #2 极致命名一致性 + 扁平命名空间

**核心目标：** AI 易补全

**命名规则：**

```text
属性用名词：      .content, .label, .color, .font_size
事件用 on_ 前缀： .on_click, .on_change, .on_focus_change
布尔用 is_/has_： .is_visible, .is_enabled
动作用动词：      .show, .hide, .request_focus, .reset
杜绝缩写：        background 而非 bg（与 aurora::Theme 成员一致）
```

**一致性要求：**

```cpp
// ✅ 一致：所有组件的相同语义属性使用完全相同的名称
Text.content   /  Label.content   /  TextInput.value
Button.label   /  (若有) Image.label
Button.on_click / Image.on_click / Row.on_click

// ❌ 不一致（AI 会混淆）：
Button.setCaption() / Label.setText() / TextInput.setValue()
Button.setWidth() / Label.w() / Image.size().x
```

**词序规则：** 统一为 `verb_noun` 或 `noun` 形式，属性设置器统一为属性直接赋值风格，避免 `setTitle`、`withTitle`、`title` 混用。

**命名空间组织：**

```cpp
namespace aurora {
    // 所有组件、类型、函数直接在此层（扁平）
    class Button;
    class Column;
    class Text;
    using Color = ...;
    using Dimension = ...;

    // 公共子命名空间（节选；另有 aurora::render / aurora::detail / aurora::ui /
    // aurora::preferences 等内部/辅助子命名空间，不属对外承诺稳定的公共 API 表面）
    namespace colors { /* Red, Blue, Gray... */ }
    namespace platform { /* 仅平台查询 API */ }
}

// 推荐别名：简短且不冲突
namespace au = aurora;

// 一个 using 即可使用全部核心 API
using namespace aurora;  // 官方推荐，无冲突风险

// ❌ 禁止深层嵌套
// aurora::widgets::buttons::MaterialButton
// ✅ 变体通过属性区分
// au::Button(au::ButtonProps{ .label = "Material" }).background(au::colors::Blue);  // 变体通过属性（背景色等）区分
```

---

#### #3 正交可组合的最小核心 API

**核心目标：** AI 少幻觉

**问题背景：** "最小"可能导致 API 功能缺失，AI 为补全功能会自行虚构 API（幻觉）。应改为"正交且可组合的最小核心 API"。

**规范：**

```text
核心组件 ≤ 30 个
每个组件核心方法 ≤ 15 个
高级功能通过组合而非继承实现
```

**布局原语（≤ 7 个）：**

```cpp
au::Row{}      // 水平排列
au::Column{}   // 垂直排列
au::Stack()    // 层叠
au::Grid()     // 网格
au::Scroll()   // 可滚动容器
au::Spacer()   // 弹性空间
au::Divider()  // 分隔线
```

**常用组合配方（≤ 15 个，可从原语推导）：**

```cpp
au::FormLayout()   // = Column + 固定 label 宽度 + 自动对齐
au::Toolbar()      // = Row + 固定高度 + 溢出折叠
au::Sidebar()      // = Column + 固定宽度 + 可折叠
au::TabView()      // = Stack + 顶部 Tab 切换
au::MenuBar()      // = Row + 下拉菜单
// ...
```

**原则：** AI 可以直接用组合配方（高频模式匹配），也可以从原语构建（低频自定义）。

---

#### #4 强类型 + 单位标注 + 编译期校验

**核心目标：** AI 生成的代码编译即验证

**规范：**

```cpp
// 强类型枚举 —— AI 不会传错值
button.alignment(au::Align::Center);       // 编译通过
button.alignment(au::Align::Cenetr);       // 编译错误！拼写错误立刻发现

// 类型化尺寸 —— 防止 px/dp/percent 混用
button.width(au::px(120));
button.width(au::percent(50));
button.width(au::dp(48));
// button.width(120);  // ❌ 编译错误，必须指定单位

// 编译错误信息需直接指出单位错误，而不是模版爆栈
// [Aurora::TypeError] Button.width():
//   - Expected: au::Dimension (px / dp / percent / auto)
//   - Received: int (120)
//   - Fix: Use au::px(120) or au::dp(120)

// 编译期布局校验（参数顺序：Child, Parent）
static_assert(au::is_valid_child_of<au::Text, au::Column>);   // Text 可作为 Column 的子元素 ✓
// static_assert(au::is_valid_child_of<au::Window, au::Button>); // Window 不可作为 Button 的子元素 ✗
```

**约束：** 模板深度 ≤ 3 层，避免模板元编程导致的编译爆炸和不可读错误。

---

#### #5 合理默认值（声明处可见）

**核心目标：** AI 少写少错

**规范：**

```cpp
// 只需写"与默认值不同"的部分
au::Button(au::ButtonProps{ .label = "OK" });
// 默认：enabled=true, visible=true,
//       alignment=Center, font_size=14,
//       padding={8,16}, corner_radius=4
```

**关键约束：**

- 默认值文档化在声明处，AI 工具可通过 LSP 直接显示默认参数，无需查阅外部文档
- 默认值应适用于 80% 场景，避免 AI 每次都需要显式指定
- 运行时可查询默认值：`btn.defaults()`
- 代码中可省略（保持简洁），但 LSP hover 时可见（保持透明）

---
