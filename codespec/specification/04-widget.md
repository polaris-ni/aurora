# 控件（widget）

> 覆盖 `include/aurora/widget/`（59 个头文件）、`include/aurora/ui/` 与根级 `todo.h`。
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
| 容器与导航壳 | `drawer.h`、`tab_bar.h`、`menu_bar.h`、`toolbar.h`、`title_bar.h`、`dialog.h`、`popup.h`、`toast.h`、`expansion_panel.h`、`stepper.h`、`bottom_nav_bar.h` |
| 绘制与占位 | `canvas.h`、`placeholder.h`、`skeleton.h`、`progress.h`、`image_widget.h`、`chip.h`、`form.h` |
| 数据展示与跨域控件 | `data_widgets.h`（`DataTable`/`TreeView`/`ListView`）；另散布于 `media/video_player.h`、`media/video_controls.h`、`navigation/hero.h`、`app/perf_overlay.h` |
| 工厂与配方 | `recipes.h`、根级 `include/aurora/ui/` |
| 调试 | `inspect.h`、`inspector_panel.h` |

---

## 2 控件基类契约

`Widget`（`widget/widget.h`）是所有控件的基类。继承层级 **≤ 2 层**：叶控件直接继承 `Widget`，多子容器继承 `Container`，单子容器继承 `SingleChild`。

### 2.1 类型标识与自描述

| 成员 | 签名 | 位置 |
|:---|:---|:---|
| `type_name()` | `[[nodiscard]] virtual auto type_name() const -> const char *` —— 纯虚 | `widget.h:397` |
| `describe()` | `[[nodiscard]] virtual auto describe() const -> WidgetDescriptor` | `widget.h:403` |
| `collect_signals(out)` | `virtual auto collect_signals(std::vector<SignalViewBase*>&) -> void` | `widget.h:146` |
| `child_nodes()` | `[[nodiscard]] virtual auto child_nodes() const -> const std::vector<Node>&` | `widget.h:450` |

各具体控件另提供**静态** `describe_static()`（例如 `au::Button::describe_static()`，见 `button.h`），便于无需实例即可查询元数据。`describe_static()` 不是 `Widget` 的虚成员。

**自描述结构**（`widget/descriptor.h:49`）：

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

**属性元数据**（`descriptor.h:17`）：

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

**批量发现**：`serialization::component_schema(name) -> Json`（`widget/serialization.h:87`，属 `aurora::serialization`）与 `list_all_schemas() -> std::vector<Json>`（`serialization.h:105`，属 `aurora` 命名空间）。`aurora::Inspector` 门面（`inspector/inspector_api.h:25`）另提供静态 `Inspector::component_schema(std::string_view)`（`inspector_api.h:84`）。

`descriptor_to_json(...)` 把描述符序列化为 JSON（两个重载），供 `component_schema` 与 `gen_api` 消费。

```cpp
auto info = au::Button::describe_static();
// info.name == "Button"
// info.properties[0].name == "label", .type == "LocalizedString", .required == true
// info.events == ["on_click"]
// info.children_policy == "none"
```

### 2.2 属性值验证

`validate_prop<T>(json, desc) -> Result<T>`（`descriptor.h:81`）按 `PropDescriptor` 的约束校验 JSON 值，已特化 `Color` / `float` / `int` / `bool` / `LocalizedString` / `Length` / `EdgeInsets`。

`validate_or_default<T>(json, desc, fallback)`（`descriptor.h:241`）是反序列化侧的降级助手：校验非法时经 `Diagnostics::degraded` 上报并回退默认值；严格模式下 `degraded` 升级为硬失败。

约束串解析（`parse_constraint_float` / `parse_constraint_int`）在遇到坏串时**降级为跳过该约束**，绝不终止进程。

### 2.3 生命周期与虚拟回调

| 回调 | 签名 | 位置 |
|:---|:---|:---|
| `on_layout(const Constraints&, const BuildContext&) -> Size` | 纯虚 | `widget.h:460` |
| `on_paint(Painter&, const Rect& bounds, const BuildContext&) -> void` | 纯虚 | `widget.h:463` |
| `on_hit_test(const Point& local, const Rect& bounds, const BuildContext&) -> Widget*` | 默认返回 `nullptr`（叶控件无子可下探）；后代命中由 `on_hit_test_chain` 递归提供，容器覆写 | `widget.h:466` |
| `on_mount(const BuildContext&) -> void` | 挂载后恰好一次 | `widget.h:482` |
| `tick(time_point) -> void` | NVI 入口 | `widget.h:350` |
| `tick_gestures(time_point) -> void` | 手势推进 | `widget.h:486` |

**可见性约定**：布局 / 绘制 / 命中测试类内部虚回调 `on_layout` / `on_paint` / `on_hit_test` / `on_mount` / `tick_gestures` 位于 `protected` 区（`widget.h:457` 起）；指针事件入口 `on_pointer_event` 的两个重载（`MouseEvent`，`widget.h:288`；`TouchEvent`，`widget.h:345`）与 `on_hover_change` / `wants_click` 位于 **public 区**且为虚函数——派发器与外部工具直接调用，子类按需要覆写；`tick` 走 NVI（模板方法），不由子类直接覆盖。

`on_paint` 收到的 `bounds` 是**全局坐标**（相对窗口客户区）；绘制原语必须基于 `bounds.origin` 计算。

### 2.4 脏标记与缓存

| 成员 | 说明 | 位置 |
|:---|:---|:---|
| `mark_needs_layout()` | 标记需要重排 | `widget.h:182` |
| `mark_needs_paint()` | 标记需要重绘 | `widget.h:223` |
| `can_cache_display_list()` | 虚，返回是否允许缓存显示列表，默认 `true` | `widget.h:197` |
| `request_frame(bool layout = false)` | 在不击穿祖先缓存的前提下请求重绘 / 重排 | `widget.h:630` |
| `width(Length)` / `height(Length)` | 虚，返回 `Widget&` 以支持链式 | `widget.h:155,161` |

> **自驱动动画**：在 `on_paint` 末尾调用 `mark_needs_paint()` 自调度下一帧的控件，必须覆写 `can_cache_display_list()` 返回 `false`，否则开启显示列表缓存后动画被冻结。

### 2.5 双模 API 与 Props 约定

控件统一采用**继承式双模 API**：`class Xxx : public XxxProps`，`XxxProps` 的字段即控件自身的公有字段，不再用私有 `m_*` 重复声明同一属性。

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
- `Widget::defaults()` 不是虚成员；各控件提供静态 `defaults()`（如 `au::Button::defaults() -> ButtonProps`，见 `button.h:61`），返回该控件的默认 `Props`。

`Node(W&&)` 是非 explicit 转换构造函数，值类型控件可隐式转为 `Node`。仅在两分支类型不同的 `?:` 三元、或需要连续两次用户转换的场景才显式包 `Node{...}`。

**二层属性划分**：固有属性（`XxxProps` 字段）描述控件身份并随控件序列化；`Modifier` 承载跨切面装饰。重叠能力以**固有属性优先**；绘制时 `Modifier` 在外、固有属性在内（详见 [`03-layout-render.md`](03-layout-render.md) §7.4）。

---

## 3 控件清单

全部控件位于 `au::` 扁平命名空间，命名遵循 snake_case 属性 + CamelCase 类型。

### 3.1 文本

| 控件 | 关键属性 |
|:---|:---|
| `Text` | `content`、`font_size`、`color`、`bold()`、`italic()`、`family()`。支持指针拖选、键盘扩选与复制 |
| `RichText` / `TextSpan` | 富文本片段组合。`RichText` 接收 `Reactive<std::vector<TextSpan>>` |
| `RichTextEdit` | 富文本编辑器（`widget/rich_text_edit.h`）；序列化键 `text`（纯文本内容），回调 `on_text_input` |
| `TextInput` | `value`（`Reactive<std::string>`）、`placeholder`、`text_color`、`placeholder_color`、`background`、`focused_background`、`border_color`、`focused_border_color`（缺省跟随主题 primary）、`border_width`、`selection_color`、`max_length`（码点限长）、`read_only`、`obscure_text`；回调 `on_changed`（每次编辑）/`on_submit`（Enter） |

`TextInput` 点击经 `FocusManager` 获焦；读当前文本用 `value()`（`text_input.h:509`），程序化改值用 `set_value()`；无 `.text()` 方法。

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

### 3.3 布局容器

| 控件 | 关键属性 |
|:---|:---|
| `Row` / `Column` | `children`、`gap`、`flex`（含 `main_axis` / `cross_axis` / `main_axis_size`）。`modifier` 属 `Widget` 基类，不在此列 |
| `Stack` | 层叠，`children` 叠加 |
| `Grid` | `columns`、`children` |
| `Grid` 虚拟化版 `GridView` | `count`、`columns`、`cell_extent`、`cache_extent`、`scroll_offset` |
| `Scroll` | 可滚动容器，`child` 单子节点，`step` 滚动步长 |
| `Spacer` / `Divider` | 弹性空间 / 分隔线 |
| `Splitter` | 可拖拽分隔 |
| `LayoutBuilder` | 按布局约束动态构建子树 |
| `Drawer` / `ExpansionPanel` / `Stepper` | 折叠面板 / 展开面板 / 步骤条 |
| `PageView` | 分页容器（`widget/drawer.h`）；`current`（当前页码）、`show_indicator`（圆点指示器）；回调 `on_page_change`；仅布局当前页 |

`GridView` / `LazyList` / `LazyRow` 是虚拟化容器，仅实例化可见窗口加 `cache_extent` 缓冲内的子项，复杂度 O(可见单元数)。三者的 `on_paint` 内均含 `push_clip(bounds)` / `pop_clip()` 配对，被圆角裁剪容器包裹时不越界。

`Scroll` 把内容录进**滑窗**离屏缓冲 `m_content`（尺寸 = 视口高 ×(1 + 2 × `overscan`)，`m_buffer_origin_y` 为缓冲锚点），滚动帧只做一次 blit。

### 3.4 列表与虚拟化

| 控件 | 关键属性 |
|:---|:---|
| `LazyList` | `count`、`item_extent`（固定行高，默认 48dp）、`scroll_offset`、`cache_extent`（可见区外预取缓冲）；辅助 API `set_scroll_offset` / `scroll_to_item` / `visible_range` / `live_item_count` / `set_cache_extent`；滚轮滚动经 `on_scroll` 覆写处理（`widget/lazy_list.h`） |
| `LazyRow` | 主轴为水平；`item_count`、`item_extent`（子项固定宽度，默认 96）、`cache_extent`、`padding`；`set_padding` 与 `set_on_item_click`（`on_item_click` 事件，参数为索引）属本控件（`widget/lazy_row.h`） |
| `Repeater` | `items`（信号驱动），按模板渲染每个元素 |
| `ListView` | `items`（行数据）、`multi_select`（多选模式）；回调 `on_select` / `on_remove`（`widget/data_widgets.h`） |
| `DataTable` | `columns`（列描述）、`row_count`（只读）、`selected_row`（-1 = 无）、`sort_column`（-1 = 无）；回调 `on_sort` / `on_select` |
| `TreeView` | 树形数据展示；`selected_row`（选中可见行，-1 = 无）；回调 `on_select` / `on_toggle` |

### 3.5 结构与生命周期

| 控件 | 关键属性 |
|:---|:---|
| `Show` | `when`（bool 信号），为真才渲染子节点 |
| `Lifecycle` | `on_mount`（挂载回调，可访问 `BuildContext`）、`on_unmount`（卸载 / 析构回调）。对齐 React `useEffect` 与 Flutter `initState` + `dispose`。`Node` 析构时清理，覆盖 `Repeater` 缩容与 `Navigator` pop |
| `Timer` | 组件级定时器 |
| `Provider` | 环境注入（详见 [`07-environment-modifier.md`](07-environment-modifier.md)） |
| `Hero` | 共享元素转场包装（`navigation/hero.h`）；`tag`（跨页配对键），单子节点 |
| `Canvas` | 自定义绘制回调，用于高频绘制场景 |

### 3.6 图像、绘制与占位

| 控件 | 说明 |
|:---|:---|
| `ImageView` | `bitmap`、`source`（源文件路径，用于序列化/占位，**不支持 URL 加载**）。解码经 `Image::load`（便捷静态 `from_file`，解码失败返回空图像占位盒）。**序列化类型名为 `Image`**。`width()` / `height()` 是 widget 级方法，不进 `Props` |
| `VideoPlayer` | 视频播放控件（`media/video_player.h`）；`fit`（`BoxFit` 枚举：Fill / Contain / Cover 等）、`show_controls`；回调 `on_tap` / `on_double_tap`；帧源经 `set_source` 注入 |
| `VideoControls` | 视频播放控件叠层（`media/video_controls.h`），配 `VideoPlayer` 使用，单子容器 |
| `Placeholder` | 通用降级占位盒（`widget/placeholder.h:24`），`.message` 说明文字 |
| `Skeleton` | 骨架屏加载占位（shimmer 动画） |
| `BottomNavBar` | `items`（每项含 icon 绘制器与 label）、`selected_index`、`on_select`；按项等分宽度布局 |
| `TitleBar` | 自绘标题栏 / CSD |
| `ToolBar` / `MenuBar` / `TabBar` | 工具栏 / 菜单条 / 标签页 |
| `StatusBar` | 底部状态栏（`widget/toolbar.h`）；`bar_height`（默认 24dp）、`gap`（区域间距），多子节点 |
| `Dialog` / `Popup` / `ToastHost` | 对话框 / 弹出层 / 轻提示宿主（`ToastHost::show(text, duration_ms)` 投放） |
| `ProgressDialog` | 模态进度对话框（`widget/drawer.h`）；`message`、`progress`（0..1，-1 = 不确定态）、`open`、`cancellable`；回调 `on_cancel` |
| `OverlayHost` | 浮层宿主（`widget/popup.h`）；`add_overlay(Node)` 追加浮层并返回序号，允许多子 |

### 3.7 平台与调试

| 控件 / 设施 | 说明 |
|:---|:---|
| `InspectorPanel` | 左右分栏 Widget 树浏览器 + 属性编辑器（详见 [`08-tooling.md`](08-tooling.md)） |
| `PerfOverlay` | 帧率 / 性能 HUD（`app/perf_overlay.h`）；`visible`、`show_counters`；每帧从 `FrameStats` 重读，故 `can_cache_display_list()` 为 `false` |
| `inspect.h` | 控件树检查函数集 |
| `recipes.h` | 高频组合配方 |

---

## 4 控件可定制性契约

全部交互控件（`Button` / `Checkbox` / `Switch` / `Slider` / `ProgressIndicator` / `RadioGroup` / `SpinBox` / `TextInput` / `Dropdown` / `TabBar` / `Chip` / `Badge` / `SegmentedControl`）统一遵循以下四条契约。新增控件必须同样遵守。

### 4.1 主题回退

强调色（`active_color` / `accent_color` / `color` / `focused_border_color` 等）一律为 `std::optional<Color>`。未显式设置时，绘制期经 `inherit_theme(ctx).primary` 解析（`ThemeScope` 换肤即生效），且**未设置不序列化**——保留「跟随主题」语义，`to_json` / `from_json` 往返不丢失意图。

### 4.2 状态反馈与禁用态

hover / 按下统一用 `Color::shaded(k)` 乘性调暗（hover ≈ ×0.90 / ×0.92，pressed ≈ ×0.78 / ×0.80），淡色底与选区用 `Color::with_alpha(a)`。所有交互控件提供 `set_enabled(bool)`：禁用态统一灰化绘制、**吞掉指针事件**（置 `e.handled = true` 不冒泡）且不改值。

### 4.3 继承友好：protected 绘制分阶段钩子

`on_paint` 分解为若干 **protected 虚函数**，状态色由 `resolve_*` 钩子解析；成员一律 `protected`（非 private），子类可单点覆盖某个绘制阶段而无需重写整个 `on_paint`。

| 控件 | 绘制钩子 |
|:---|:---|
| `Button` | `resolve_background` / `resolve_text_color` / `paint_background` / `paint_border` / `paint_label` |
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
| `ui::lazy_list(parent, count, builder, item_extent = 48.0f)` | `LazyList` |
| `ui::lazy_row(parent, count, builder, item_extent = 96.0f)` / `ui::lazy_row(parent, LazyRowProps)` | `LazyRow`（双重载） |
| `ui::bottom_nav_bar(parent, BottomNavBarProps)` | `BottomNavBar` |

---

## 6 需求规格

### 6.1 #7 扁平组合模型 + 共享所有权组件

**核心目标：** AI 易追踪逻辑。

```cpp
// ❌ 深层继承链（AI 难以追踪）：
// Widget → Container → InteractiveWidget → ButtonBase → ThemedButton → MaterialButton

// ✅ 组合模式（AI 容易理解）：
// au::Button = 内容(Text) + 背景(Modifier) + 点击行为(Modifier::clickable)
// 通过 Modifier 正交组合能力，而非继承
```

**关键约束：**

- 组件继承层级 ≤ 2 层（叶控件继承 `Widget`；多子容器继承 `Container`，单子容器继承 `SingleChild`）。
- 用组合替代继承；横切能力由 `Modifier` 正交组合表达（见 [`07-environment-modifier.md`](07-environment-modifier.md)）。
- 整棵 UI 树保存在一个 `Node` 中，`Node` 持有 `std::shared_ptr<Widget>`：拷贝即共享、移动即转移，整棵树可被复制 / 移动，析构由 `shared_ptr` 自动管理。
- **几何权威在 `Node`**：`Node` 持有 `Rect m_bounds`（原点 + 尺寸），是布局与命中测试的**唯一几何来源**。布局阶段由父节点经 `child.set_bounds(box)` 写入，`Window::present_root` 把窗口矩形写入根 `Node`。`Widget` **不持有任何几何缓存**。
- 深层嵌套容易导致 AI 迷失，应把深树拆成命名子函数。

**输入坐标本地化**：由 `EventDispatcher` 在命中链冒泡时完成。命中链 `hit_test_chain` 返回 `std::vector<HitNode>`（`origin` 即该控件相对根的全局 origin），派发器对每个控件写入 `MouseEvent::local_position = position - origin`，控件在 `on_pointer_event` 中直接消费本地坐标。

**`HitNode` 生命周期契约**：节点同时持有裸指针 `ptr` 与弱引用 `guard`（构造时探测该控件是否由 `shared_ptr` 持有；栈 / 成员控件的 `weak_from_this()` 为空弱引用，回退为裸指针）。取用方式有二：

- `get()`：仅返回存活指针，**不带生命周期保证**，只可用于「不解引用」的用途（如比较是否同一控件）；
- `lock(out_keepalive)`：返回存活指针**并把强引用写入出参**，把控件生命周期延长至调用方作用域结束。

凡要**解引用**命中链节点（调用 `on_pointer_event` / `focusable()` 等）都必须用 `lock()`：派发回调（用户 `on_click`）可能销毁控件自身所在子树（如点击按钮触发 `push_replacement` 重建页面），而回调返回后基类 `Widget::on_pointer_event` 仍要写 `m_pressed` 等成员，用裸指针即 use-after-free。

**验收标准：** 控件继承深度 ≤ 2；`Widget` 上不存在任何几何字段；命中链的解引用路径全部经 `lock()` 持有强引用。

### 6.2 #22 可逆性：UI → 代码的参考还原（控件侧）

**核心目标：** AI 可分析现有界面并重构。定位是「结构化往返」而非「完全可逆」。

- 序列化格式（JSON）是 **canonical form**，不含代码风格信息；风格由调用方通过 `CodeStyle` 选择。
- 部分控件的序列化 `type` 名与 C++ 类名不同（`Image` → `ImageView` / `ImageViewProps`）。

> 工具链侧入口（`to_code`、MCP 工具、CLI 子命令、Inspector 导出）见 [`08-tooling.md`](08-tooling.md)。
