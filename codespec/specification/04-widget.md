# 控件（widget）

> 覆盖 `include/aurora/widget/`（75 个头文件）、`include/aurora/ui/` 与根级 `todo.h`。
> 本文件是控件清单、`Props` 约定、自描述契约与可定制性契约的**唯一权威**。
> 绘制与文本内核见 [`03-layout-render.md`](03-layout-render.md) §8；序列化契约见 [`08-tooling.md`](08-tooling.md)；响应式属性见 [`02-state.md`](02-state.md) §2.2。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 基类与容器 | `widget.h`、`containers.h`、`stack.h` |
| 自描述与属性 IO | `descriptor.h`、`props_io.h` |
| 序列化 / 代码生成 / YAML | `serialization.h`、`codegen.h`、`yaml.h` |
| 文本 | `text.h`、`rich_text.h`、`text_span.h`、`rich_text_edit.h`、`text_input.h` |
| 输入与选择 | `button.h`、`checkbox.h`、`switch.h`、`slider.h`、`dropdown.h`、`radio_spin.h`、`segmented_control.h`、`pickers.h` |
| 布局与滚动 | `grid.h`、`grid_view.h`、`lazy_list.h`、`lazy_row.h`、`scroll.h`、`spacer.h`、`divider.h`、`splitter.h`、`layout_builder.h`、`layout_query.h` |
| 结构控制 | `show.h`、`repeater.h`、`lifecycle.h`、`timer.h`、`provider.h` |
| 无障碍语义树 | `a11y_tree.h`（几何盒 / Name 回退链 / 递归节点与整树构建：`build_accessibility_tree`）、`a11y_diff.h`（`TreeSnapshot` / `NodeSnapshot` / `TreeDiff` / `build_tree_snapshot`）。二者需 `Widget` 完整定义，故归本层；纯类型与桥抽象留在 [`01-core.md`](01-core.md) §7.2 |
| 容器与导航壳 | `drawer.h`、`tab_bar.h`、`menu_bar.h`、`toolbar.h`、`title_bar.h`、`dialog.h`、`popup.h`、`toast.h`、`expansion_panel.h`、`stepper.h`、`bottom_nav_bar.h` |
| 绘制与占位 | `canvas.h`、`placeholder.h`、`skeleton.h`、`progress.h`、`image_widget.h`、`chip.h`、`form.h` |
| 数据展示与跨域控件 | `data_widgets.h`（`DataTable`/`TreeView`/`ListView`）；另散布于 `media/video_player.h`、`media/video_controls.h`、`navigation/hero.h`、`app/perf_overlay.h` |
| 工厂与配方 | `recipes.h`、根级 `include/aurora/ui/` |
| 调试 | `inspect.h`、`inspector_panel.h` |

---

## 2 控件基类契约

`Widget`（`widget/widget.h`）是所有控件的基类。继承层级 **≤ 3 层**：叶控件统一继承中间基类 `LeafWidget`（`Widget → LeafWidget → Xxx`，`LeafWidget` 定义于 `widget.h`），多子容器继承 `Container`，单子容器继承 `SingleChild`，两者均最终继承 `Widget`。

### 2.1 类型标识与自描述

| 成员 | 签名 | 位置 |
|:---|:---|:---|
| `type_name()` | `[[nodiscard]] virtual auto type_name() const -> const char *` —— 纯虚 | `widget.h` |
| `describe()` | `[[nodiscard]] virtual auto describe() const -> WidgetDescriptor` | `widget.h` |
| `collect_signals(out)` | `virtual auto collect_signals(std::vector<SignalViewBase*>&) -> void` | `widget.h` |
| `child_nodes()` | `[[nodiscard]] virtual auto child_nodes() const -> const std::vector<Node>&` | `widget.h` |

各具体控件另提供**静态** `describe_static()`（例如 `au::Button::describe_static()`，见 `button.h`），便于无需实例即可查询元数据。`describe_static()` 不是 `Widget` 的虚成员。

**自描述结构**（`widget/descriptor.h`）：

```cpp
struct WidgetDescriptor {
    std::string name;                        // 控件类型名，如 "Button"
    std::string ns = "aurora";               // 命名空间
    std::vector<PropDescriptor> properties;  // 属性列表
    std::vector<std::string> events;         // 事件 / 回调名，如 ["on_click"]
    std::string children_policy;             // "none" | "single" | "multiple"
    std::vector<std::string> allowed_child_types;  // 合法子类型（空 = 任意）
    std::vector<std::string> invariants;           // 控件级不变量描述
    std::vector<std::string> examples;             // 构造示例代码
};
```

**属性元数据**（`descriptor.h`）：

```cpp
struct PropDescriptor {
    std::string name;                    // 序列化键名，如 "label"
    std::string type;                    // C++ 类型名，如 "LocalizedString"
    std::string default_value;           // 字符串化默认值，如 "\"\""
    bool required = false;
    std::string note;
    // JSON Schema 约束字段（编译期零开销，仅 Schema 生成时读取）
    std::string json_type;
    std::vector<std::string> enum_values;
    std::string min_value, max_value, pattern, constraint;
    std::vector<std::string> requires_props;   // 属性依赖
    std::vector<std::string> conflicts_with;   // 属性互斥
};
```

**批量发现**：`serialization::component_schema(name) -> Json`（`widget/serialization.h`，属 `aurora::serialization`）与 `list_all_schemas() -> std::vector<Json>`（`serialization.h`，属 `aurora` 命名空间）。`aurora::Inspector` 门面（`inspector/inspector_api.h`）另提供静态 `Inspector::component_schema(std::string_view)`（`inspector_api.h`）。

`descriptor_to_json(...)` 把描述符序列化为 JSON（两个重载），供 `component_schema` 与 `gen_api` 消费。

```cpp
auto info = au::Button::describe_static();
// info.name == "Button"
// info.properties[0].name == "label", .type == "LocalizedString", .required == true
// info.events == ["on_click"]
// info.children_policy == "none"
```

### 2.2 属性值验证

`validate_prop<T>(json, desc) -> Result<T>`（`descriptor.h`）按 `PropDescriptor` 的约束校验 JSON 值，已特化 `Color` / `float` / `int` / `bool` / `LocalizedString` / `Length` / `EdgeInsets`。

`validate_or_default<T>(json, desc, fallback)`（`descriptor.h`）是反序列化侧的降级助手：校验非法时经 `Diagnostics::degraded` 上报并回退默认值；严格模式下 `degraded` 升级为硬失败。

约束串解析（`parse_constraint_float` / `parse_constraint_int`）在遇到坏串时**降级为跳过该约束**，绝不终止进程。

### 2.3 生命周期与虚拟回调

| 回调 | 签名 | 位置 |
|:---|:---|:---|
| `on_layout(const Constraints&, const BuildContext&) -> Size` | 纯虚 | `widget.h` |
| `on_paint(Painter&, const Rect& bounds, const BuildContext&) -> void` | 纯虚 | `widget.h` |
| `on_hit_test(const Point& local, const Rect& bounds, const BuildContext&) -> Widget*` | 默认返回 `nullptr`（叶控件无子可下探）；后代命中由 `on_hit_test_chain` 递归提供，容器覆写 | `widget.h` |
| `on_mount(const BuildContext&) -> void` | 挂载后恰好一次 | `widget.h` |
| `tick(time_point) -> void` | 框架容器基类可覆写的公开入口（见下） | `widget.h` |
| `tick_gestures(time_point) -> void` | 手势推进 | `widget.h` |
| `on_scroll(ScrollEvent&) -> void` | 滚轮入口：默认按 `overflow_` 的内建滑窗夹取滚动并写 `remaining_y` 余量；真实滚动控件覆写（路由见 `05-event-navigation.md` §3.3） | `widget.h` |
| `wants_scroll() -> bool` | 是否参与滚轮命中链路由。默认 = 声明了 `OverflowStrategy::Scroll`；`Scroll` / `LazyList` / `LazyRow` / `GridView` / `PullToRefresh` 覆写为 `true`，保证嵌套时**最深滚动者优先** | `widget.h` |
| `is_sticky_header() -> bool` | 是否为吸顶头部（`StickyHeader` 覆写为 `true`），供滚动宿主在 blit 后以覆盖层按 pin 位重绘 | `widget.h` |
| `extra_hit_box(ctx, ancestor_offset = {0,0}) -> std::optional<Rect>` | 追加命中盒：**覆盖绘制**（面板画在自身布局盒之外）的控件在此声明那段区域（本地坐标），祖先下降闸据此并入判定；缺省 `std::nullopt` = 不追加（可命中区 == 自身布局盒）。位于 **public 区**：祖先要跨对象查询子控件的声明。`ancestor_offset` 是本控件原点在视口坐标系中的 y（祖先下降时累加、带默认值），**只在需要按「离视口多远」决定几何时**用（如面板翻转），纯本地几何的覆写方忽略即可。详见 `05-event-navigation.md` §3.2.1 / §3.2.2 | `widget.h` |
| `covers_extra_hit_box(local, ctx, ancestor_offset = {0,0}) -> bool` | 追加盒**聚合**判定入口（非虚）：祖先的下降闸调它，问的是「以本控件为根的**子树**是否覆盖此局部点」（自身申报 ∪ 逐子节点折算递归），故孙辈申报能穿过不申报的中间层上达；无任何覆写时恒 `false`，与改动前逐位等价。详见 `05-event-navigation.md` §3.2.2 | `widget.h` |
| `covers_own_extra_hit_box(local, ctx, ancestor_offset = {0,0}) -> bool` | 只问**本控件自身**申报的追加盒（非虚，恒 O(1)）：`hit_test_chain` 判定自身是否入链用此入口，避免把中间容器一并拽进命中链 | `widget.h` |
| `covers_descendant_extra_hit_box(local, ctx, ancestor_offset) -> bool` | 聚合的递归下降段（**protected virtual**，不含自身）：缺省遍历 `child_nodes()` 折算下探。仅两类控件覆写——子节点来源/坐标系与布局盒不一致者（`LazyList` / `GridView` 走 `live_`、`Popup` 按 `anchor_` 返回 `false`）、带视口裁剪者（`Scroll` / `LazyList` / `GridView` 必须加视口钳位，否则不可见区域变得可点） | `widget.h` |

**可见性约定**：布局 / 绘制 / 命中测试类内部虚回调 `on_layout` / `on_paint` / `on_hit_test` / `on_mount` / `tick_gestures` 位于 `protected` 区（`widget.h` 起）；指针事件入口 `on_pointer_event` 的两个重载（`MouseEvent`，`widget.h`；`TouchEvent`，`widget.h`）与 `on_hover_change` / `wants_click` 位于 **public 区**且为虚函数——派发器与外部工具直接调用，子类按需要覆写；业务控件覆写 `tick_gestures` 以推进手势，框架容器基类（`Container` / `SingleChild`）可覆写公开 `tick` 以递归子树（见 `widget.h` / `widget.h`），而非经 NVI 模板方法。

`on_paint` 收到的 `bounds` 是**全局坐标**（相对窗口客户区）；绘制原语必须基于 `bounds.origin` 计算。

**布局父链的生命周期契约（`Node` 所有权）**：布局父链（`layout_parent_`）是脏标记上溯与缓存向上失效的共同前提，其完整性由两条**父侧**不变量保证，`Node::~Node` **不参与**：

1. **句柄析构不得清活控件的父链**——`Node` 是可共享句柄（`shared_ptr` 语义），拷贝 / 临时（如 `SingleChild` 的视图缓存、`set_children` 的初始化列表）/ `std::move` 后的旧对象析构时，控件仍在世、仍挂在那一只父下面。`~Node` 无从区分「临时句柄没了」与「控件真脱离」（`use_count()` 对这两种情形取值相同），故**一律不碰** `layout_parent_`。
2. **真摘除与父先亡都由父侧清零**——「该控件确实脱离那一只父」走 `Widget::detach_child_layout_parent`（经 `Container::remove_child` / `set_children` / `adopt_children` / `SingleChild::set_child`）；「那一只父先亡、子控件仍被外部 `shared_ptr` 持有」走 `Widget::detach_all_children_layout_parent`（`~Container` / `~SingleChild` 首行）。两者是**同一份真值源**——父容器明确知道「这些子节点要脱离我了」，而子控件析构时猜不出来。

**断链必须可见**：`detach_child_layout_parent` 清的是**仍存活**控件的父指针（真正销毁控件的路径不经此函数），此时发一条 `AURORA_LOG_WARN`（category `widget`），供消费方把「控件已脱离树却仍被标脏」与「忘记标脏」区分开。`StrictMode` 另有影子断言（`request_frame` 内），但它在本仓多数构建档关闭，**不能**当作唯一防线。

**新增容器的义务**：持有子节点的类须在析构体首行调 `detach_all_children_layout_parent()`。该函数虚分派 `child_nodes_mut()`，故**不能**放在 `~Widget` 基类里——那时派生部分已析构，会分派到基类空实现、静默漏清。另须注意 `SingleChild::child_nodes()` 返回的是**缓存副本**（`child_view_`），对它取可写引用会把清父链的副作用落在副本上，故 `SingleChild` 单独覆写 `child_nodes_mut()` 返回与 `child_` 同步的可写表。

**验收**：`utest_layout_parent_chain` 三条——① 拷贝 / 进 `vector` 后丢弃 / `std::move` 后旧对象析构后，父链仍在且 `mark_needs_layout()` 仍抵达根侧汇聚回调；② `remove_child` 后 `layout_parent()` 为空、此后标脏不崩不悬垂；③ 父容器析构后仍存活的子控件 `layout_parent()` 为空。判据用「`on_dirty` 命中而根侧 `on_subtree_dirty` 不命中」区分「控件知道自己脏了」与「脏抵达了根」——只钉前者等于没钉病灶。变异自证：只在应用侧「顺手多标一层祖先脏」而不清断链 ⇒ ①②转红（补标是掩盖，不是修复）。

### 2.4 脏标记与缓存

| 成员 | 说明 | 位置 |
|:---|:---|:---|
| `mark_needs_layout()` | 标记需要重排 | `widget.h` |
| `mark_needs_paint()` | 标记需要重绘 | `widget.h` |
| `can_cache_display_list()` | 虚，返回是否允许缓存显示列表，默认 `true`（`overflow_ == OverflowStrategy::Scroll` 时为 `false`） | `widget.h` |
| `request_frame(bool layout = false)` | 在不击穿祖先缓存的前提下请求重绘 / 重排 | `widget.h` |
| `width(Length)` / `height(Length)` | 虚，返回 `Widget&` 以支持链式 | `widget.h` |

> **自驱动动画**：在 `on_paint` 末尾调用 `mark_needs_paint()` 自调度下一帧的控件，必须覆写 `can_cache_display_list()` 返回 `false`，否则开启显示列表缓存后动画被冻结。

### 2.5 双模 API 与 Props 约定

**部分控件**（布局 / 文本 / 图片 / 滚动 / 导航 / 图表 / 下拉刷新类）采用**继承式双模 API**：`class Xxx : public XxxProps`，`XxxProps` 的字段即控件自身的公有字段，不再用私有 `m_*` 重复声明同一属性。采用此模式的控件（共 16 个）：`BarChart` / `BottomNavBar` / `Button` / `Column` / `Divider` / `Grid` / `ImageView` / `LazyRow` / `LineChart` / `PieChart` / `PullToRefresh` / `Row` / `ScatterChart` / `Scroll` / `Sparkline` / `Text`（见各自头声明）；例外——`Checkbox` / `Switch` / `Slider` / `ProgressIndicator` / `Dropdown` / `RadioGroup` / `SpinBox` / `SegmentedControl` / `Chip` / `TextInput` 等仍使用私有 `*_` 成员 + setter，未采用双模。

```cpp
// 形态一：*Props 具名聚合（推荐，可分块生成）
auto btn = au::Button(au::ButtonProps{ .label = "OK" });

// 形态二：对已构造对象逐行赋值
auto btn2 = au::Button();
btn2.label = au::LocalizedString{ "OK" };
btn2.on_click = fn;

// 形态三：链式 setter（setter 返回引用）
au::Text("Welcome").font_size(24).bold();
```

**关键约束：**

- **控件类不是聚合类型**，不能用 `au::Button{ .label = ... }` 这类指定初始化器构造控件（编译失败）。指定初始化器**仅适用于 `*Props` 聚合结构**（如 `au::ButtonProps{ .label = ... }`）与 `Theme` 等纯数据聚合，其中字段顺序无关，遗漏字段回退默认值。
- 初始化列表形式 `au::Column{ au::Text("A"), au::Text("B") }` 可用——`Column` / `Row` 接受 `std::initializer_list<Node>`。
- 链式 setter 返回引用；作为子节点放入 `children` 时必须用 `std::move` 包裹（`Widget` 拷贝构造被删除，`Node` 仅移动派生对象）。
- `Widget::defaults()` 不是虚成员；仅 `Button` 提供静态 `defaults()`（如 `au::Button::defaults() -> ButtonProps`，见 `button.h`），返回该控件的默认 `Props`；其余控件的默认属性来源待补。

`Node(W&&)` 是非 explicit 转换构造函数，值类型控件可隐式转为 `Node`。仅在两分支类型不同的 `?:` 三元、或需要连续两次用户转换的场景才显式包 `Node{...}`。

**二层属性划分**：固有属性（`XxxProps` 字段）描述控件身份并随控件序列化；`Modifier` 承载跨切面装饰。重叠能力以**固有属性优先**；绘制时 `Modifier` 在外、固有属性在内（详见 [`03-layout-render.md`](03-layout-render.md) §7.4）。

### 2.6 无障碍虚钩子

控件通过一组**虚钩子**自述无障碍语义；语义树构建（`a11y_tree.h`，见 §1）与平台桥只读这些钩子，不探控件内部。全部为虚函数且**基类默认值即合法**——自定义控件零改动仍可被推断（`infer_accessibility_role(type_name())` 兜底）。

**身份与角色**

| 钩子 | 默认 | 说明 |
|:---|:---|:---|
| `runtime_id() const -> std::uint64_t` | 基类构造时分配的进程级原子自增值（从 1 起） | 节点稳定身份；**非虚**，生命周期内恒定，直接进入 `AccessibilityNode::id` |
| `stable_key()` / `set_stable_key(std::string)` | 空串（未设） | **非虚**，宿主命名的跨重建稳定键（对标 HTML `id`），入 `AccessibilityNode::stable_key`；`set_labelled_by` 只认它，不认 `runtime_id`。不动名字故不上报事件 |
| `labelled_by_key()` / `set_labelled_by(std::string)` | 空串（未声明） | **非虚**，引用式标签关联（对标 `aria-labelledby`）：名字取同树内该键控件之名，Name 回退链最高优先级；变化上报 `NameChanged`。解析与降级判据见 [`01-core.md`](01-core.md) §7.2 |
| `accessibility_role() const -> AccessibilityRole` | 走 `infer_accessibility_role(type_name())` 表 | 控件可覆写（如图表族五控件统一返回 `Image`） |
| `accessibility_label() const -> std::string` | 空串 | 可访问名首选来源 |
| `accessibility_value() const -> std::string` | 空串 | 平台 Value 属性来源 |
| `accessibility_hint() const -> std::string` | 空串 | HelpText 来源 |

**状态 / 取值域 / 层级**（结构化，供平台映射为位属性）

| 钩子 | 默认 | 典型覆写 |
|:---|:---|:---|
| `accessibility_state() const -> AccessibilityState` | 只填 `focused` | `Checkbox` / `Switch` 补 `checkable` + `checked`；`Slider` / `Progress` 补 `read_only`；`TextInput` 补 `read_only` / `password` / `multiline` |
| `accessibility_range() const -> std::optional<AccessibilityRange>` | `nullopt` | `Slider`（min/max/step/value）、`Progress`（0–1） |
| `accessibility_level() const -> std::optional<int>` | `nullopt` | 标题层级（`Header` 角色用于文档结构导航） |
| `accessibility_scroll() const -> std::optional<AccessibilityScrollRange>` | `nullopt` | `Scroll`（可滚动范围的当前位置与边界） |
| `accessibility_is_semantic() const -> bool` | `true` | 声明「不参与语义树」的纯装饰控件返回 `false`（该控件及其标记含义被平台完全忽略） |

**可编辑文本（TextPattern 支撑）**

| 钩子 | 默认 | 典型覆写 |
|:---|:---|:---|
| `accessibility_text() const -> std::string_view` | 空 | `TextInput`（原文）/ `RichTextEdit` |
| `accessibility_selection() const -> std::optional<AccessibilityTextSelection>` | `nullopt` | 同上；UTF-8 **字节**半开区间 |
| `accessibility_set_selection(std::size_t start, std::size_t end) -> void` | no-op | 同上 |
| `accessibility_char_bounds(std::size_t utf8_index) const -> std::optional<Rect>` | `nullopt` | 同上；须经 `FontEngine` 度量，**无头环境不加载字体**故只能返回 `nullopt` |
| `accessibility_replace_text(std::size_t start, std::size_t end, std::string_view) -> void` | no-op | 同上；以 UTF-8 偏移表达 |

**动作通道**

| 钩子 | 说明 |
|:---|:---|
| `perform_accessibility_action(const AccessibilityActionRequest&) -> bool` | 读屏反向操作控件的**唯一**入口。基类默认实现把动作**路由到真实事件路径**（聚焦 / 点击 / 调用 / 滚动经 `EventDispatcher` 与 `resolve_focus_manager`），不另造旁路；控件可覆写定制语义（如 `Toggle` → `set_value`、`Value` → `set_value`）；无法处理返回 `false` |
| `accessibility_scroll_to(double offset) -> void` | 默认 no-op；`Scroll` 覆写为「语义滚动到指定偏移」（平台 `ScrollIntoView` 的落点） |
| `announce(const std::string&) const -> void` | 动态播报（Live Region）：上抛 `AccessibilityEventKind::Announcement`，不经语义树 diff。无控件归属的播报走自由函数 `notify_accessibility_announcement(text, nullptr)` |

> **只读钩子必须无副作用**：`accessibility_*` 查询会被桥在**任意平台查询时刻**调用，链路是「平台回调 → 快照重投影 → 构建节点 → 读钩子」，其间不允许改控件状态、发布局请求或触发事件。唯一的例外是 `announce()`——它是动作而非查询，且经独立事件通道。

---

## 3 控件清单

全部控件位于 `au::` 扁平命名空间，命名遵循 snake_case 属性 + CamelCase 类型。

### 3.1 文本

| 控件 | 关键属性 |
|:---|:---|
| `Text` | `content`、`font_size`、`color`、`bold()`、`family()`（注：斜体未实现——仅经 `font_style` 设置且当前降级为 `Normal`，见 `text.h`）。支持指针拖选、键盘扩选与复制 |
| `RichText` / `TextSpan` | 富文本片段组合。`RichText` 接收 `Reactive<std::vector<TextSpan>>` |
| `RichTextEdit` | 富文本编辑器（`widget/rich_text_edit.h`）；序列化键 `text`（纯文本内容），回调 `on_text_input` |
| `TextInput` | 仅 `value`（**`std::string`**，非 `Reactive`）、`placeholder`、`font_size` 进 `TextInputProps`（`text_input.h`）；`text_color` / `placeholder_color` / `background` / `focused_background` / `border_color` / `focused_border_color` / `border_width` / `selection_color` 等颜色类为**私有字段 + `set_*` 链式**（非 Props，见 `text_input.h` 附近）。回调 `on_changed`（每次编辑）/`on_submit`（Enter） |

`TextInput` 点击经 `FocusManager` 获焦；读当前文本用 `value()`（`text_input.h`），程序化改值用 `set_value()`；无 `.text()` 方法。

### 3.2 按钮与选择

| 控件 | 关键属性 |
|:---|:---|
| `Button` | `label`、`on_click`、`color`（背景色字段，链式 setter 为 `background()`）/ `on_color`（文字色字段，链式 setter 为 `text_color()`）、`corner_radius`（默认 6dp）、`padding`、`enabled`；状态样式 `hover_color` / `pressed_color`（缺省由背景色自动调暗 ×0.92 / ×0.80）、`border_color` + `border_width`（Outlined 风格）、`disabled_color` / `disabled_text_color`、`min_width` / `min_height` |
| `Checkbox` | `checked`、`on_changed`、`active_color`（缺省跟随主题 primary）、`border_color`、`check_color`、`size`、`corner_radius`（<0 自动 = 边长 ×0.2）、`border_width`、`enabled` |
| `Switch` | `checked`、`on_changed`、`active_color`（缺省 = primary）、`inactive_color`、`thumb_color`、`track_width`、`track_height`、`thumb_inset`、`enabled` |
| `Slider` | `value`、`min`、`max`、`on_changed`、`active_color`（缺省 = primary）、`inactive_color`、`thumb_color`、`track_height`、`thumb_size`（<0 自动）、`step`（>0 步进吸附）、`enabled` |
| `ProgressIndicator` | 0..1 进度；`color`（主题回退）、`track_color`、`thickness`（决定自然高度）、`corner_radius`（<0 自动 = 厚度一半胶囊） |
| `RadioGroup` | `active_color`（主题回退）、`border_color`、`text_color`、`dot_size`、`row_height`、`font_size`、`enabled` |
| `SpinBox` | `background`、`border_color`、`text_color`、`arrow_color`、`corner_radius`、`font_size`、`enabled`；获焦后 ArrowUp / ArrowDown 调节 |
| `Dropdown` | `accent_color`（主题回退）、`box_color`、`border_color`、`text_color`、`arrow_color`、`box_height`、`item_height`、`font_size`、`corner_radius`、`placeholder`、`enabled` |
| `SegmentedControl` | `active_color`（主题回退）、`text_color`、`selected_text_color`、`border_color`、`font_size`、`corner_radius`、`enabled`、`segments`（已序列化，支持完整重建） |
| `Chip` | 胶囊圆角（`corner_radius` <0 自动）、`text_color`、`delete_color`、`font_size` |
| `Badge` | 胶囊徽章；`badge_color`、`text_color` |
| `DatePicker` | `year`、`month`（1..12）、`day`（1..31）；回调 `on_change`（`widget/pickers.h`） |
| `TimePicker` | `hour`（0..23）、`minute`（0..59）；回调 `on_change` |
| `ColorPicker` | `color`（选中颜色）；回调 `on_change` |
| `Form` | `gap`（字段垂直间距）；回调 `on_submit`；仅允许 `FormField` 子节点；`validate_all()` 递归验证（`widget/form.h`） |
| `FormField` | 单子字段包装；`error_text`（空 = 通过）；回调 `on_validate`；`validate()` / `clear_error()` |

**`Button` 标签的 i18n 契约（绘制 / 无障碍 / 自然宽同源）**：`ButtonProps::label` 是 `LocalizedString`，经
`tr(key)` 构造时 `text` 恒空、只带 `key`。故三处消费标签的路径一律经 `Button::resolved_label(ctx)` 查
`StringTable`（`Environment` 注入了 `Locale` 就按它解析，否则回落 `Locale{}` 走 `default_string_table()`
的缺省档）——**不得直读 `label.get().text`**，否则 `tr()` 出的键会被画成空白：

- `on_layout`：以解析后的显示串量宽，自然宽 = 量宽 + 左右内边距（`min_width` 不足时）；
- `on_paint`：本帧只解析一次，`paint_label` 收到的是该串的**同一份副本**（按 const 引用传入），量宽与
  `draw_text` 必然同值，不会因同帧内 locale 变化而分叉；子类覆写 `paint_label` 应直接用该串，不要再自行解析 `label`；
- `accessibility_label()`：与绘制同源——优先复用布局 / 绘制期落在 `cached_display_text_` 的解析结果，尚未布局
  或绘制时按默认 locale 现场解析（口径同 `Text::resolved_text`）。

字面档（`set_label("Save")`）不受影响：非本地化的 `LocalizedString` 经 `resolve()` 原样返回 `text`，与查表路径
输出等价。宽度缓存按「上次测量所用的显示串」比对失效：显示串变化即重测，不复用另一 locale 下的旧宽。

### 3.3 布局容器

| 控件 | 关键属性 |
|:---|:---|
| `Row` / `Column` | `children`、`gap`、`flex`（含 `main_axis` / `cross_axis` / `main_axis_size`）。`cross_axis` 的 `Baseline` 取值仅对水平主轴（`Row`）有语义（`03-layout-render.md` §3.8）。`modifier` 属 `Widget` 基类，不在此列 |
| `Stack` | 层叠，`children` 叠加 |
| `Grid` | `columns`、`children` |
| `Grid` 虚拟化版 `GridView` | `count`、`columns`、`cell_extent`、`cache_extent`、`scroll_offset`、`restore_key`、`snap_extent` / `snap_paging` / `snap_alignment`（吸附三属性，见下「滚动增强契约」）；辅助 API `set_scroll_offset` / `scroll_to` / `set_snap` / `offset_signal` / `is_gliding` / `visible_row_range` / `live_item_count` / `set_item_builder`（JSON 重建后挂单元格） |
| `Scroll` | 可滚动容器，`child` 单子节点，`step` 滚动步长、`restore_key`、`snap_extent` / `snap_paging` / `snap_alignment`；运行时偏移经序列化键 `offset` 可读回；程序化跳转 `set_offset(offset) -> bool`（纯夹取）、`scroll_to(offset, animate = true) -> bool`；滚动驱动动画原语 `offset_signal()`；观测点 `is_gliding()`（`widget/scroll.h`） |
| `PullToRefresh` | 下拉刷新容器（`widget/pull_to_refresh.h`）：`threshold`（触发距离 64dp）、`max_pull`（橡皮筋上限 128dp）；回调 `on_refresh`；API `finish_refresh()` / `state()` / `pull_distance()` / `progress()` |
| `StickyHeader` | 吸顶头部包装（`widget/sticky_header.h`），单子节点、无自有属性；正常参与布局，滚动经过视口顶部时由宿主以覆盖层钉驻 |
| `Spacer` / `Divider` | 弹性空间 / 分隔线 |
| `Splitter` | 可拖拽分隔 |
| `LayoutBuilder` | 按布局约束动态构建子树 |
| `Drawer` / `ExpansionPanel` / `Stepper` | 折叠面板 / 展开面板 / 步骤条 |
| `PageView` | 分页容器（`widget/drawer.h`）；`current`（当前页码）、`show_indicator`（圆点指示器）；回调 `on_page_change`；仅布局当前页 |
| `BreakpointBuilder` | 响应式布局构建器（`widget/breakpoint_builder.h`）：按宽度断点（`medium_max_width` / `expanded_min_width`）切换 Compact / Medium / Expanded，经 `builder` 闭包重建子树；仅断点档位变化或闭包替换时重建 |

`GridView` / `LazyList` / `LazyRow` 是虚拟化容器，仅实例化可见窗口加 `cache_extent` 缓冲内的子项，复杂度 O(可见单元数)。三者的 `on_paint` 内均含 `push_clip(bounds)` / `pop_clip()` 配对，被圆角裁剪容器包裹时不越界。

**三者的 JSON 重建契约**：五个占位/容器控件（`LazyList` / `LazyRow` / `GridView` / `Skeleton` / `BottomNavBar`）都已接入 `register_core_widgets()`，`from_json` 可还原。差别在**条目**：`ItemBuilder` 是运行时回调、不入 JSON，故三个虚拟化容器重建后「标量属性齐备而暂无条目」，宿主须随后调 `set_item_builder(...)` 挂上构建器（赋值即标布局脏，下一帧按当前窗口建条目）；`Skeleton` 与 `BottomNavBar` 无回调成员，属性可完整往返。构造期默认工厂以占位入参建实例再回填属性，`ReorderableList<T>` 因条目由 `State<std::vector<T>>` 驱动而**不**注册（`Rebuildable: no`）。注册表与逐控件的可重建性标注见 `CONCEPTS.md` §1.1。

**对齐原语**：容器与 `Stack` 的子项落点由 `widget/alignment.h` 统一提供——`enum class Alignment`（`TopLeft` / `TopCenter` / … / `BottomRight` 九宫格取值）配自由函数 `align_origin(Alignment, child_size, container_size) -> Point`（返回子项左上角相对容器的对齐落点）。该枚举同时被 `StackProps::align`、`Modifier::align(Alignment)`（`AlignNode`）与 flex 布局消费，是「子项在容器内如何对齐」的**单一类型来源**。

`Scroll` 把内容录进**滑窗**离屏缓冲 `content_`（尺寸 = 视口高 ×(1 + 2 × `overscan`)，`buffer_origin_y_` 为缓冲锚点）：短内容（`max_origin == 0`）滚动帧仅一次 blit 平移合成；长内容（`max_origin > 0`）在滚动帧按增量条带重录（`scrolling_` 触发重锚，`shift_pixels` memmove + 重绘新暴露带，见 `scroll.h`），非纯 blit。

**滚动的两套坐标系（不可混用）**：几何权威在 `Node::bounds_`，而各滚动宿主写子节点 bounds 时取的坐标系不同，命中链的换算方式随之不同——这是两类滚动容器**不可互相参照实现**的根因。

- **`Scroll`：内容坐标**。`children_[0].bounds()` 的原点恒为 (0,0)、不含滚动偏移，且该原点必须在每次 `on_layout` 落定（否则内容子树停在默认零盒，既不落笔也不可命中）。三条路径共用这一基准：绘制按 `bounds.origin.y + buffer_origin_y_ - offset_y_` 平移合成；命中把局部命中点 **加上** `offset_y_` 换算回内容坐标再下降；吸顶由 `collect_stickies` 沿内容子树累加 `bounds().origin.y` 得 `natural_y`、以 `natural_y >= offset_y_` 判钉驻——若 bounds 烘焙了偏移，该判据即失配。命中另受**可视区裁剪**：先按视口盒显式钳位（点在视口外一律不命中，含被 `offset_y_` 推出的那段），再逐层由内容子树各自的 `bounds.contains` 收紧。容器自身仍在命中链上（滚轮/拖拽需要它）且排在内容之后，故内容先收事件、不被容器吞掉。
- **`LazyList` / `LazyRow` / `GridView`：视口坐标**。偏移参与子布局，条目 bounds 直接写 `y = index * extent - offset`（LazyList 固定行高）/ 等价折算（LazyRow 横向、GridView 二维），故命中链可原样复用 `Container::on_hit_test_chain`，无需自行换算。

**运行期改条目数（`LazyList::set_count`）**：`LazyList` 的虚拟化窗口此前只能在构造或反序列化时定条目数，运行期变化只能重建整个列表或重挂 `ItemBuilder`。`set_count(int)` 补上这条入口，口径三条：负值按 0 处理（与带参构造、`deserialize_props` 同一口径，不分叉）；下标仍在新范围内的**存活条目不重建**（其展开态、输入内容、子控件身份原样保留），只有滚出窗口或越界的条目被回收、下次进入窗口时由 `ItemBuilder` 重建；条目数减少后当前偏移按新的 `max_scroll_offset()` 夹取（否则列表停在内容末尾之外的空白处）。焦点落在被回收条目上时随之失效（`FocusManager` 持弱引用）——宿主若需保留焦点应先把焦点移到仍在范围内的条目上。

**滚动位置保存/恢复**：四个滚动控件（`Scroll` / `LazyList` / `LazyRow` / `GridView`）都有 `restore_key`（空 = 不参与）。控件在**首次可滚动布局**时按 `app::ScrollStorage` 恢复偏移（由 `deserialize_props` 显式给入的偏移优先），此后位置变化（滚轮 / 拖拽 / `set_scroll_offset`）即写回（仅内存，落盘由 App 决定）；恢复只生效一次，用户主动滚动不会再被回拉。契约与多窗口作用域隔离见 `06-app-platform.md` §9.3。

**滚动增强契约**（吸附 / 下拉刷新 / 吸顶 / 余量上冒 / 偏移信号）：

- **吸附的单一实现**：`snap_extent`（吸附周期 dp，`<= 0` = 关闭）、`snap_paging`（以视口高为周期，此时 `snap_extent` 被忽略）、`snap_alignment`（`Start` / `Center` / `End`，条目对齐点相对视口顶部的取法）三属性由 `ScrollSnap` 承载，对齐点计算集中于 `ScrollViewport::snap_target`（`widget/scroll_viewport.h`），`Scroll` / `LazyList` / `GridView` 三个宿主共用（`LazyRow` 暂无吸附）；三键 `snap_extent` / `snap_paging` / `snap_alignment` 在三个宿主下一致往返，运行时偏移键仍各自沿用既有的 `offset`（`Scroll` / `LazyRow`）与 `scroll_offset`（`LazyList` / `GridView`）。收位周期内不做整除假设：末段不足一格时结果夹到 `max_scroll_offset`，故分页末尾停在最后一个**整页**对齐点。
- **收位是短滑动，不是跳变**：滚轮 / 拖拽落点后由 `begin_snap_glide()` 启动 `ScrollGlide`（150ms easeOutCubic）逐帧推进，自驱动 `tick_gestures`（不占 `Animator`，同 `ReorderableList`）。`set_offset` / `set_scroll_offset` 这类**外部程序化跳转作废进行中的滑动**；滑动帧自身走私有 `apply_offset(offset, cancel_glide)` 且传 `cancel_glide = false`——否则收位只推进一帧即冻结在中途。`scroll_to(offset, animate)` 是公开的程序化滚动入口：`animate = false` 即时落位，`true` 时经同一条滑动时序（`reduce_motion` 下仍直落）。观测点 `is_gliding()`。
- **`reduce_motion` 短路**：`current_accessibility_settings().reduce_motion` 为真时，snap 收位与 `scroll_to(…, true)` 一律直落端点、不产生中间帧（状态与「走完」一致），对齐 `AnimationController::tick` / `Dismissible` / `ReorderableList` 的既有短路语义。
- **`PullToRefresh` 的双输入通道**：仅当滚动子树**已在顶部**才积累下拉距离——顶部判定取最近可滚动后代的 `accessibility_scroll()` 区间（`position <= min + 0.5`；无可滚动后代视为在顶部，静态内容可整体下拉）。① `DragRecognizer` 判定主轴为垂直（`|dy| ≥ |dx|`）且正在向下拖时劫持手势，位移经**双曲橡皮筋**映射 `raw·max_pull/(raw+max_pull)`：起段近 1:1 跟手、渐近 `max_pull` 且恒不越界（拖满 `max_pull` 恰达半上限，与缺省 `threshold = max_pull/2` 对齐保证可触发）；② 嵌套滚动协调下，子级到顶后未消费的滚轮余量经 `ScrollEvent::remaining_y` 上冒给本容器（本容器在命中链上位于子滚动控件的更浅层，路由见 `05-event-navigation.md` §3.3），余量按 16dp / 增量单位折算后同样走橡皮筋（与 `ScrollProps::step` 缺省同值）；门控不通过（刷新中 / 子树不在顶部 / 增量方向相反）时**原样回传 `remaining_y`** 交还更浅层，而非静默吞掉。三态 `Idle` / `Pulling` / `Refreshing`：松手时 `pull >= threshold` 且已注册 `on_refresh` 才触发一次回调并回弹到驻留高度（约 `threshold/2`，spinner 持续旋转），否则直接回弹归零；数据加载完成后由宿主调 `finish_refresh()` 收拢——不回调解散回调则 spinner 常驻。指示器是**覆盖层**（顶部半透明带 + 弧线 spinner）——不动布局盒、不推挤内容（`05-event-navigation.md` §6.5「动画不改布局」），回弹走同一 `ScrollGlide`，`progress()`（`pull/threshold`，可 >1）供宿主联动。
- **`StickyHeader` 是纯绘制层语义**：控件正常参与布局（占据自身高度、随内容滚动被录入内容缓冲），宿主在内容 blit 合成之后把「已滚过头顶」的头部按 pin 位重绘于视口顶部——内容缓冲不因此逐帧重录（否则滑窗模型破产）。宿主识别钩子是 `Widget::is_sticky_header()`（默认 `false`）。**顶出规则**同 CSS `position: sticky` / `RecyclerView`：下一条头部的视口顶部逼近时把本条向上顶出，最后一条钉到底。`Scroll` 经 `child_nodes()` 递归累加 y 收集多个头部（多段堆叠），`LazyList` 的 `child_nodes()` 为空、粘性项作为普通行实例化，故各条目自管其 sticky。
- **偏移信号驱动滚动动画**：`offset_signal()` 懒创建 `State<float>` 并返回 `SignalView<float>` 引用，任何滚动通道（滚轮、拖拽、收位滑动帧、程序化跳转）落位即发布；镜像值比较而非 `State::get()` 回读（后者在 `Effect` 作用域会自订阅本控件而闭环）。宿主由此把偏移映射为视差、透明度、进度条——无需轮询，也无需子类化。

### 3.4 列表与虚拟化

| 控件 | 关键属性 |
|:---|:---|
| `LazyList` | `count`、`item_extent`（固定行高，默认 48dp）、`scroll_offset`、`cache_extent`（可见区外预取缓冲）、`restore_key`、`snap_extent` / `snap_paging` / `snap_alignment`（`0` 表示无吸附）；辅助 API `set_scroll_offset` / `scroll_to_item` / `scroll_to` / `set_snap` / `offset_signal` / `is_gliding` / `visible_range` / `live_item_count` / `set_cache_extent` / `set_restore_key` / `set_item_builder`（JSON 重建后挂条目）；滚轮滚动经 `on_scroll` 覆写处理（`widget/lazy_list.h`） |
| `LazyRow` | 主轴为水平；`item_count`、`item_extent`（子项固定宽度，默认 96）、`cache_extent`、`padding`、`restore_key`；辅助 API `scroll_offset` / `max_scroll_offset` / `set_scroll_offset`（仅标绘制脏——可见窗口在 `on_paint` 现算，与 `LazyList` 需标布局脏不同）/ `set_item_builder`（JSON 重建后挂条目）；`set_padding` 与 `set_on_item_click`（`on_item_click` 事件，参数为索引）属本控件（`widget/lazy_row.h`） |
| `Repeater` | `items`（信号驱动），按模板渲染每个元素 |
| `ListView` | `items`（行数据）、`multi_select`（多选模式）；回调 `on_select` / `on_remove`（`widget/data_widgets.h`） |
| `DataTable` | `columns`（列描述）、`row_count`（只读）、`selected_row`（-1 = 无）、`sort_column`（-1 = 无）；回调 `on_sort` / `on_select` |
| `TreeView` | 树形数据展示；`selected_row`（选中可见行，-1 = 无）；回调 `on_select` / `on_toggle` |
| `ReorderableList<T>` | 可拖拽重排列表（全量实例化、可变行高、内建垂直滚动）：`gap`、`scroll_offset`、`restore_key`、`drag_handle`（是否限定右侧手柄带起拖）、`auto_scroll_threshold`、`keyboard_reorder`（键盘重排路径开关，默认 `true`）；回调 `on_reorder(from, new_index)`；辅助 API `reorder` / `slot_for_center` / `item_top` / `drag_index` / `drop_slot` / `is_dragging` / `is_settling`，键盘通道 `set_keyboard_reorder` / `set_keyboard_index` / `keyboard_index` / `is_keyboard_grabbed` / `keyboard_grab_index` / `grab_keyboard_item` / `drop_keyboard_item` / `cancel_keyboard_grab`（`widget/reorderable_list.h`） |

**拖拽重排的数据契约与交互边界**：

- **控件直接改写数据**：构造注入 `State<std::vector<T>>` + `ItemBuilder`；松手落位后**控件自己**改写该 vector（`std::rotate` 语义）并重建子项，`on_reorder(from, new_index)` 在数据已改写之后触发（供宿主持久化）——避免「UI 动了数据没动」。`reorder(from, to)` 可程序化重排，`to` 为**落位后的最终下标**（与 `drop_slot()` 同语义）。
- **手柄带边界**：条目若自带点击（`Clickable` / `Button`），其 Press 被子项消费（冒泡 stop-on-handled），列表收不到按下事件 ⇒ 必须 `set_drag_handle(true)`：右侧 48dp 手柄带内命中链**不下降给子项**，由列表自己起拖；纯展示型条目（无点击）则整项可拖。
- **让位是绘制期偏移**：跟手 1:1；其余条目按「移除被拖项后的目标序」在 `on_paint` 位移、`on_hit_test_chain` 同步补偿 —— `Node::bounds` 保持不动（几何权威在 Node，逐帧改会击穿子控件 Display List 缓存）。换位判定取相邻项中点并带 **±2dp 滞回**（防边界抖动）。
- **落位动画**：松手后 spring 收敛到目标槽位（初速度按帧间差分估计），静止才提交数据；`reduce_motion` 下直接落位（同 `Dismissible` / `AnimationController` 的短路语义）。自驱动 `tick_gestures`，不使用 `Animator`。
- **近边缘自动滚动**：被拖项进入视口上下 48dp（`auto_scroll_threshold`）带内时按侵入深度比例滚动；滚动量吃进跟手位移（被拖项**屏幕位置守恒**）。拖拽期间滚轮被吞（同轴冲突）。虚拟化列表的重排**不做**（`LazyList` 保持只读滚动）。
- **键盘替代路径**（`keyboard_reorder`，默认开）：指针拖拽不是唯一取径，键盘操作者可经「移动光标 → 抓取 → 落位」完成同一次重排。获焦落在**首个可见项**（不是第 0 项，也不触发滚动——恢复滚动位置的控件获焦后位置不变），`↑` / `↓` 移光标、`Home` / `End` 跳首尾（越界自动 `scroll_to_item` 拉回可视），`Space` / `Enter` 抓取与落位（落位即走与拖拽同一条 `reorder(from, to)` 提交通道），`Esc` 取消。抓取项以「抬起态」重绘，光标环仅在该列表**持有焦点**时绘制（`inherit_theme(ctx).primary`，抓取时加粗）。鼠标拖拽进行中键盘路径整体让位（`drag_state_ != Idle`），两条通道不会互相踩。
- **键盘路径的可访问性与可定制性**：光标移动 / 抓取 / 落位 / 取消四类节点各播报一次（`Widget::announce` → `AccessibilityEventKind::Announcement`，文案键 `aurora.reorder.position` / `.grabbed` / `.dropped` / `.dropped_in_place` / `.cancelled`，经 `default_string_table()` 解析、缺失时回落内置英文串，故宿主可整表替换）；落位项与原位相同时播报「未移动」而非静默。方向键之所以能到达列表，依赖 `Widget::wants_navigation_keys()`（默认 `false`）这一控件级 opt-in：见 `05-event-navigation.md` §4.2。`set_keyboard_reorder(false)` 关闭该路径（进行中的抓取同时清除），光标状态可经 `keyboard_index()` / `is_keyboard_grabbed()` / `keyboard_grab_index()` 观测，也可用 `set_keyboard_index(i)` 程序化设定。

### 3.5 结构与生命周期

| 控件 | 关键属性 |
|:---|:---|
| `Show` | `visible`（bool 信号，序列化键亦为 `visible`；构造入参名 `condition`），为真才渲染子节点 |
| `Lifecycle` | `on_mount`（挂载回调，可访问 `BuildContext`）、`on_unmount`（卸载 / 析构回调）。对齐 React `useEffect` 与 Flutter `initState` + `dispose`。`Node` 析构时清理，覆盖 `Repeater` 缩容与 `Navigator` pop |
| `Timer` | 组件级定时器 |
| `Provider` | 环境注入（详见 [`07-environment-modifier.md`](07-environment-modifier.md)） |
| `Hero` | 共享元素转场包装（`navigation/hero.h`）；`tag`（跨页配对键），单子节点 |
| `Canvas` | 自定义绘制回调，用于高频绘制场景 |
| `Dismissible` | 滑动消除包装（`widget/dismissible.h`）：单子节点沿 `axis`（默认 Horizontal）拖拽至阈值后消除，对标 Flutter `Dismissible`；手势由每帧 `tick` 驱动 |
| `CommandPalette` | 模态命令面板（`widget/command_palette.h`）：居中浮层，按关键字模糊检索并执行命令，依赖 `CommandRegistry`（未绑定时为空列表）；`open` / `close` 切换 |

### 3.6 图像、绘制与占位

| 控件 | 说明 |
|:---|:---|
| `ImageView` | `bitmap`、`source`（源文件路径，用于序列化/占位）。URL 源经注入的 `ImageFetcher`（`ImageFetcher` / `from_url` / `begin_load` / `ImageLoadState::Loading/Loaded/Failed` / `ImageCache`，见 `image_widget.h`）异步加载；未注入 fetcher 时降级为占位。**序列化类型名为 `Image`**。`width()` / `height()` 是 widget 级方法，不进 `Props` |
| `VideoPlayer` | 视频播放控件（`media/video_player.h`）；`fit`（`BoxFit` 枚举：Fill / Contain / Cover 等）、`show_controls`；回调 `on_tap` / `on_double_tap`；帧源经 `set_source` 注入 |
| `VideoControls` | 视频播放控件叠层（`media/video_controls.h`），配 `VideoPlayer` 使用，单子容器 |
| `Placeholder` | 通用降级占位盒（`widget/placeholder.h`），序列化键 `message` 说明文字（`au::Placeholder("…")` 或 `.set_message("…")` 构造，非聚合类型、无 initializer_list 构造） |
| `Skeleton` | 骨架屏加载占位（shimmer 动画，`width` / `height` / `color` / `highlight` / `duration` 五属性全量序列化往返） |
| `BottomNavBar` | `items`（每项含 icon 绘制器与 label）、`selected_index`、`bar_height`；回调 `on_select`；按项等分宽度布局（`selected_index` / `bar_height` 序列化往返，`items` 含绘制器故不入 JSON） |
| `TitleBar` | 自绘标题栏 / CSD |
| `ToolBar` / `MenuBar` / `TabBar` | 工具栏 / 菜单条 / 标签页 |
| `TabBody` | 标签内容体（`widget/recipes.h`，`detail` 命名空间）：按 `selected` 索引显示对应页，随状态刷新；与 `TabBar` 配套使用 |
| `StatusBar` | 底部状态栏（`widget/toolbar.h`）；`bar_height`（默认 24dp）、`gap`（区域间距），多子节点 |
| `Dialog` / `Popup` / `ToastHost` | 对话框 / 弹出层 / 轻提示宿主（`ToastHost::show(text, duration_ms)` 投放）。**Dialog 的几何与命中契约**：打开态 `on_layout` 把居中后的内容盒写入 `children_[0]`（几何权威唯一在 `Node::bounds_`），`on_paint` 直接读该盒落笔、遮罩按自身 bounds 铺满——两处各算一遍居中必然分叉。命中分两区：内容盒内下降到内容子树（按钮等可交互控件正常命中）；内容盒外（遮罩区）**本控件自身入链吸收该次点击、不下落穿透**到对话框下方的视口，且**不触发** `on_close_`（遮罩吸收 ≠ 取消）。关闭态命中链恒为空，即便上帧 `bounds` 仍有效。`show()` / `close()` / `set_content()` 均须标布局脏（`on_layout` 以 `open_` 为分支且布局缓存只看约束相等，不标脏则开/关切换与换内容都会被缓存跳过）。焦点作用域（`show`/`close` 经 `current_focus_manager()` push/pop）语义不受此影响 |
| `ProgressDialog` | 模态进度对话框（`widget/drawer.h`）；`message`、`progress`（0..1，-1 = 不确定态）、`open`、`cancellable`；回调 `on_cancel` |
| `OverlayHost` | 浮层宿主（`widget/popup.h`）；`add_overlay(Node)` 追加浮层并返回**可移除序号**（`std::optional<std::size_t>`，宿主尚无基础内容时为 `std::nullopt`——此时新节点落在序号 0，而 0 恒被解释为**基础内容**、`remove_overlay` 拒收，返回它等于给调用方一个永远删不掉的下标），`remove_overlay(index)` 拒收 0 与越界序号；允许多子 |

### 3.7 平台与调试

| 控件 / 设施 | 说明 |
|:---|:---|
| `InspectorPanel` | 左右分栏 Widget 树浏览器 + 属性编辑器（详见 [`08-tooling.md`](08-tooling.md)） |
| `PerfOverlay` | 帧率 / 性能 HUD（`app/perf_overlay.h`）；`visible`、`show_counters`；每帧从 `FrameStats` 重读，故 `can_cache_display_list()` 为 `false`；读数陈旧（`FrameStats::is_stale()`）时第一行灰化并追加 `stale <空闲秒数>` |
| `inspect.h` | 控件树检查函数集 |
| `recipes.h` | 高频组合配方 |

### 3.8 图表控件

图表控件族按「每图一个叶控件 + 纯值 Props」组织，现已全部落地。五图共享公共数据层 `widget/chart_common.h`、事件三件套（`wants_click` / `on_hover_change` 标脏 / `on_pointer_event` 自处理 Move·Release）、`geom_` 缓存（绘制与命中同源）、十字准线 + 图例 hover 联动、grow-in 进入动画（`Animator::current()`，无 Animator 时降级到终态）。

| 控件 | 说明 |
|:---|:---|
| `BarChart` | 柱状图（`widget/bar_chart.h`）。`series`（`ChartSeries{name, values, color?}` 数组，多系列分组并排）、`categories`（类目标签，缺省序号）、`stacked`、`bar_width_ratio`、`bar_corner_radius`、`show_crosshair`、`axis_x`（类目轴）/ `axis_y`（数值轴）、`legend`、`padding`；回调 `on_point_tapped(series_idx, point_idx)` |
| `LineChart` | 折线图（`widget/line_chart.h`）。`series`（等距 x = 索引）、`show_dots`、`line_width`、`axis_x` / `axis_y`（双 `LinearScale`）、`legend`、`padding`；`show_crosshair` 控制十字准线；回调 `on_point_tapped(series_idx, point_idx)` |
| `Sparkline` | 迷你走势图（`widget/sparkline.h`），最薄：**无轴 / 无网格 / 无图例 / 无交互**。`values`（单系列）、`color`（可选，缺省按色板取色）、`line_width`、`show_end_dot`、`dot_radius`、`padding` |
| `PieChart` | 饼图 / 环形图（`widget/pie_chart.h`）。`sections`（`PieSection{name, value, color?}` 数组）、`center_space_ratio`（> 0 即环形）、`start_angle`（度，12 点起）、`show_percentage_labels`、`section_gap`、`legend`、`padding`；命中按极坐标（半径 + 角度）判扇区；回调 `on_section_tapped(section_idx)` |
| `ScatterChart` | 散点图（`widget/scatter_chart.h`）。`series`（`ScatterSeries{name, points: [{x,y}], dot_radius, color?}`，显式 `ChartPoint`）、`axis_x` / `axis_y`（双 `LinearScale`）、`legend`、`padding`；命中按最近点欧氏距离（半径 = `dot_radius + 4dp`）；回调 `on_point_tapped(series_idx, point_idx)` |

**公共数据层**（`widget/chart_common.h`，纯值、可无头单测）：`ChartPoint` / `ChartSeries` / `ScatterSeries` / `PieSection` / `ChartAxisSpec` / `LegendPosition` / `ChartLegendSpec` / `LinearScale` / `BandScale` / `chart_palette` / `resolve_series_color`。

四条族级契约：

1. **数据与视觉配置都是纯值属性**，整包进序列化面（`to_json` / `from_json` / `diff` / `apply_patch`）——AI 可经 schema + `from_json` 生成带真实数据的图表；交互回调（如 `on_point_tapped`）旁挂、**不进序列化面**。
2. **轴域与命中反查同源**：渲染、刻度生成、hover 命中都消费同一份 `LinearScale` / `BandScale`，不得各算一遍。
3. **绘制不得越出控件 `bounds`**：控件自身的绘制越界不会被擦除（残影），脏区按 `Widget::dirty_bounds()`（= `paint_bounds_` ∪ 盒外装饰）标记。轴留白与图例带在控件内部以 `padding` 预留，悬浮值框按可用区夹取 / 翻转。唯一被认可的盒外绘制是基类统一焦点环，其外扩已由 `dirty_bounds()` 承担（见 [`05-event-navigation.md`](05-event-navigation.md) §4.4）。
4. **取色与取 Locale 一律带回退**：系列色 = 显式 `color` > `Theme` 命名令牌 `chart.palette.<i%8>` > 内置 8 色板；网格 / 标签色取自 `inherit_theme(ctx)`；刻度文本经 `format_number(v, locale, digits)`，Locale 用 `ctx.environment<Locale>()` 取值、**未注入回退 `Locale{}`**（`render_to_png` 传 `constexpr BuildContext`，`env_of<Locale>` 会断言失败）。

健壮性降级：空数据只画轴；`range == 0` 时域退化为 `[v, v+1]`（全 0 即 `[0,1]`）；NaN / ±inf 数据点跳过；点数超限时截断。反序列化对畸形数组元素逐项跳过并 `Diagnostics::degraded`，绝不抛异常。

---

## 4 控件可定制性契约

全部交互控件（`Button` / `Checkbox` / `Switch` / `Slider` / `ProgressIndicator` / `RadioGroup` / `SpinBox` / `TextInput` / `Dropdown` / `TabBar` / `Chip` / `Badge` / `SegmentedControl`）统一遵循以下四条契约。新增控件必须同样遵守。

### 4.1 主题回退

强调色（`active_color` / `accent_color` / `focused_border_color` 等）一律为 `std::optional<Color>`。未显式设置时，绘制期经 `inherit_theme(ctx).primary` 解析（`ThemeScope` 换肤即生效），且**未设置不序列化**——保留「跟随主题」语义，`to_json` / `from_json` 往返不丢失意图。例外：`Button` 的 `color`（`ButtonProps::color`，`button.h`）是 **`Reactive<Color> = Color::blue()`**，**非** optional，默认即蓝底、不跟随主题，`on_paint` 直接 `resolve_background()`（见 `widgets_paint.cpp`）。

### 4.2 状态反馈与禁用态

hover / 按下统一用 `Color::shaded(k)` 乘性调暗（hover ≈ ×0.90 / ×0.92，pressed ≈ ×0.78 / ×0.80），淡色底与选区用 `Color::with_alpha(a)`。按钮 / 输入 / 选择类控件（`Button` / `Checkbox` / `Switch` / `Slider` / `TextInput` / `Dropdown` / `RadioSpin` / `SegmentedControl`）提供 `set_enabled(bool)`：禁用态统一灰化绘制、**吞掉指针事件**（置 `e.handled = true` 不冒泡）且不改值。例外：`Chip` / `TabBar` / `ProgressIndicator` 无 `set_enabled` 方法，也无禁用态分支。

### 4.3 继承友好：protected 绘制分阶段钩子

`on_paint` 分解为若干 **protected 虚函数**，状态色由 `resolve_*` 钩子解析；成员一律 `protected`（非 private），子类可单点覆盖某个绘制阶段而无需重写整个 `on_paint`。

**显示串单一来源约束**：凡绘制钩子需要可本地化文本，一律由 `on_paint` 解析一次后按 const 引用传给钩子
（如 `Button::paint_label` 的 `display` 形参），钩子内部不得再自行解析；`accessibility_label()` 必须与绘制
所用的显示串**逐字相等**（复用同一份解析结果，不得各算一遍）。

| 控件 | 绘制钩子 |
|:---|:---|
| `Button` | `resolve_background` / `resolve_text_color` / `paint_background` / `paint_border` / `paint_label`（末者多收一个 `const std::string &display` 形参 = 本帧显示串） |
| `Switch` | `paint_track` / `paint_thumb` |
| `Slider` | `paint_track` / `paint_active_track` / `paint_thumb`；几何 `track_rect` / `value_fraction` |
| `ProgressIndicator` | `paint_track` / `paint_fill` |
| `RadioGroup` | `paint_option` |
| `SpinBox` | `paint_box` / `paint_value` / `paint_arrows` |
| `Dropdown` | `paint_box` / `paint_item` |
| `TabBar` | `paint_tab` |
| `Chip` | `paint_background` / `paint_foreground`（勿覆写 `paint_content`：它是 `Widget` 绘制管线核心） |
| `SegmentedControl` | `paint_segment` |
| `Checkbox` | `resolve_palette`（状态色，含禁用灰化）/ `corner_radius` / `paint_checked_box` / `paint_check_mark` / `paint_idle_box`；`on_paint` 仅做编排，一般无需覆写 |

> 子类新增与基类虚函数**同名**的方法会静默 override 基类虚函数。新增方法前须确认基类中不存在同名虚函数。

### 4.4 尺寸、字号、圆角可配

原硬编码常量（行高、盒高、字号、圆角）升级为可序列化属性（如 `row_height` / `dot_size` / `box_height` / `item_height` / `tab_height` / `font_size` / `corner_radius` / `track_*` / `thickness`）。`corner_radius < 0` 统一表示「自动」（胶囊或比例圆角）。

影响几何的 setter 调 `mark_needs_layout()`，仅影响外观的调 `mark_needs_paint()`。

---

## 5 `au::ui` 工厂语法糖

Aurora 的「真值来源」仍是声明式 `Node` 树加 `XxxProps` 聚合属性。`aurora::ui`（别名 `au::ui`）是叠其上的**语法糖**：一组工厂函数把「构造 + 加父 + 返回强类型指针」三步合一，压缩 AI 生成代码的 token 与出错面。

它**不引入新控件类型**，底层仍是 `Text` / `Button` / `Column` 等，与控件清单完全一致。

已提供的便利构造器（对应控件已注册工厂，声明于 `ui/factories.h`；均接收父容器引用、就地追加并返回**强类型裸指针**，指针生命周期由父树持有）：

| 工厂 | 对应控件 |
|:---|:---|
| `detail::make_add<T>(parent, args...)` | 任意控件的通用模板底座：构造 `T` + 包 `Node` + 追加到父容器 |
| `ui::label(parent, text, TextProps = {})` | `Text`（`text` 覆盖 `props.content`） |
| `ui::button(parent, text, ButtonProps = {}, on_click = {})` | `Button`（`text` 覆盖 `props.label`，`on_click` 可选） |
| `ui::input(parent, value = "", TextInputProps = {})` | `TextInput`（`value` 为初始文本） |
| `ui::checkbox(parent, checked, on_changed = {})` | `Checkbox`（双模：`Reactive<bool>` 或 `bool` 初值） |
| `ui::slider(parent, value, on_changed = {})` | `Slider`（双模：`Reactive<double>` 或 `double` 初值） |
| `ui::vbox(parent, ColumnProps = {})` | `Column` |
| `ui::hbox(parent, RowProps = {})` | `Row` |
| `ui::stack(parent, align = Alignment::TopLeft)` | `Stack` |
| `ui::grid(parent, GridProps = {})` | `Grid` |
| `ui::scroll(parent, ScrollProps = {})` | `Scroll` |
| `ui::lazy_list(parent, count, builder, item_extent = 48.0F)` | `LazyList` |
| `ui::lazy_row(parent, count, builder, item_extent = 96.0F)` / `ui::lazy_row(parent, LazyRowProps)` | `LazyRow`（双重载） |
| `ui::bottom_nav_bar(parent, BottomNavBarProps)` | `BottomNavBar` |

---

## 6 需求规格

### 6.1 SPEC.FEAT.WIDGET.FLAT-COMPONENTS.001 扁平组合模型 + 共享所有权组件

**核心目标：** AI 易追踪逻辑。

```cpp
// ❌ 深层继承链（AI 难以追踪）：
// Widget → Container → InteractiveWidget → ButtonBase → ThemedButton → MaterialButton

// ✅ 组合模式（AI 容易理解）：
// au::Button = 内容(Text) + 背景(Modifier) + 点击行为(Modifier::clickable)
// 通过 Modifier 正交组合能力，而非继承
```

**关键约束：**

- 组件继承层级 ≤ 3 层（叶控件继承 `LeafWidget`，再由 `LeafWidget` 继承 `Widget`；多子容器继承 `Container`，单子容器继承 `SingleChild`）。
- 用组合替代继承；横切能力由 `Modifier` 正交组合表达（见 [`07-environment-modifier.md`](07-environment-modifier.md)）。
- 整棵 UI 树保存在一个 `Node` 中，`Node` 持有 `std::shared_ptr<Widget>`：拷贝即共享、移动即转移，整棵树可被复制 / 移动，析构由 `shared_ptr` 自动管理。
- **几何权威在 `Node`**：`Node` 持有 `Rect bounds_`（原点 + 尺寸），是布局与命中测试的**唯一几何来源**。布局阶段由父节点经 `child.set_bounds(box)` 写入，`Window::present_root` 把窗口矩形写入根 `Node`。`Widget` **不持有任何几何缓存**。
- 深层嵌套容易导致 AI 迷失，应把深树拆成命名子函数。

**输入坐标本地化**：由 `EventDispatcher` 在命中链冒泡时完成。命中链 `hit_test_chain` 返回 `std::vector<HitNode>`（`origin` 即该控件相对根的全局 origin），派发器对每个控件写入 `MouseEvent::local_position = position - origin`，控件在 `on_pointer_event` 中直接消费本地坐标。

**`HitNode` 生命周期契约**：节点同时持有裸指针 `ptr` 与弱引用 `guard`（构造时探测该控件是否由 `shared_ptr` 持有；栈 / 成员控件的 `weak_from_this()` 为空弱引用，回退为裸指针）。取用方式有二：

- `get()`：仅返回存活指针，**不带生命周期保证**，只可用于「不解引用」的用途（如比较是否同一控件）；
- `lock(out_keepalive)`：返回存活指针**并把强引用写入出参**，把控件生命周期延长至调用方作用域结束。

凡要**解引用**命中链节点（调用 `on_pointer_event` / `focusable()` 等）都必须用 `lock()`：派发回调（用户 `on_click`）可能销毁控件自身所在子树（如点击按钮触发 `push_replacement` 重建页面），而回调返回后基类 `Widget::on_pointer_event` 仍要写 `pressed_` 等成员，用裸指针即 use-after-free。

**验收标准：** 控件继承深度 ≤ 2；`Widget` 上不存在任何几何字段；命中链的解引用路径全部经 `lock()` 持有强引用。

### 6.2 SPEC.FEAT.TOOLING.UI-TO-CODE.001 可逆性：UI → 代码的参考还原（控件侧）

**核心目标：** AI 可分析现有界面并重构。定位是「结构化往返」而非「完全可逆」。

- 序列化格式（JSON）是 **canonical form**，不含代码风格信息；风格由调用方通过 `CodeStyle` 选择。
- 部分控件的序列化 `type` 名与 C++ 类名不同（`Image` → `ImageView` / `ImageViewProps`）。

> 工具链侧入口（`to_code`、MCP 工具、CLI 子命令、Inspector 导出）见 [`08-tooling.md`](08-tooling.md)。
