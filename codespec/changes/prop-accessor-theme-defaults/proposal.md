# CHG-056 属性访问器写拦截与主题默认值：单一属性表驱动的 setter、脏标记与形状/间距令牌

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-056 |
| 提出日期 | 2026-10-31 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.API.SENSIBLE-DEFAULTS.001 |
| 影响面 | 16 个继承式双模控件的公共头（`BarChart` / `BottomNavBar` / `Button` / `Column` / `Divider` / `Grid` / `ImageView` / `LazyRow` / `LineChart` / `PieChart` / `PullToRefresh` / `Row` / `ScatterChart` / `Scroll` / `Sparkline` / `Text`）由裸公有字段改为「同名访问器 + 表驱动 setter」；新增内部属性表头 `include/aurora/widget/prop_accessor.h`；`include/aurora/theming/theme.h` 新增形状与间距令牌，并新增 `shape_scale.h` / `spacing_scale.h`；主题敏感固有属性 optional 化并在布局/绘制期解析；`describe_static()` / `serialize_props()` / `deserialize_props()` 改为表转发；状态采集协议扩展（`collect_signals` / `StateSink` 登记项随源携带脏类别，权限模型与去重语义不变）；`aurora_api.json` 重建（`default_expr` 出现主题令牌名，optional 字段不出 `default`）；`tests/unit/` 相应用例与负向编译判据；`XxxProps` 增加纯数据成员函数（归一化、主题解析、`defaults`、单键解析、`validate`，不含对控件的反向引用）；`codespec/` 六篇文档与两篇手工用例；`CHANGELOG.md` 记破坏性变更与迁移路径 |

## 动机

1. **同一属性在四处重复登记**：`*Props` 字段声明、`describe_static()` 的属性项、`serialize_props()` / `deserialize_props()` 的键分发、以及各处 setter 的标脏调用。新增一个属性要在三到四个位置同步，漏一处就产生不一致（描述符里有、序列化里没有，或反之）。
2. **标脏靠人手，已经真实漏标**：`Button::text_color(Color)` 只做 `on_color = c; return *this;`，挂载后改文字色不重绘；`set_corner_radius` / `set_padding` / `set_hover_color` / `set_pressed_color` / `set_border` / `set_disabled_colors` / `set_min_size` 同样只写字段不标脏。这些是可用性缺陷，不是风格问题。
3. **裸公有字段可被直接写且没有任何拦截**：`*Props` 是控件公有基类，`btn.label = ...` 编译通过并绕过标脏；更严重的是句柄属性的私有镜像，`btn.color = X` 编译通过但绘制读 `color_.get()`，形成**静默无效写**（编译、运行都不报错，画面不响应）。调用方无法从类型或编译器得到任何提示。
4. **默认值写死，主题只能换色**：`Button` 的圆角 6、内边距 12/6、图表留白 8 等全部硬编码在控件头文件；`Theme` 现有 `ColorRoles` / `TypeScale` / `CsdRoles` / `ColorScheme` / `ThemeExtensions`，没有形状与间距令牌。结果是换主题只能换配色与字阶，无法换密度与圆角风格（圆角按钮 / 直角按钮、紧凑 / 宽松布局），且每个控件的「合理默认值」散落在各自头文件里，无法集中治理。
5. **三套写入模型并存**：基类公共属性是「私有字段 + 链式 setter」，句柄属性是「公有字段 + 私有镜像 + 访问器」，标量属性是「裸公有字段」。同一语义三种写法，人类与 AI 都会选错；业界（Qt `Q_PROPERTY` 的 `MEMBER` 不能带 `NOTIFY`、WPF / Avalonia 依赖属性、Slint 生成 `get_`/`set_`、Vue Proxy、Svelte 编译改写、Flutter 不可变重建）的共同结论是：**C++ 没有 property 设施，可拦截的写法只能是私有字段加访问器，或编译器 / 宏改写语句**。本库已对句柄属性采用访问器（`ReadOnlyState` / `State` 本身也是可拦截包装），标量属性缺失同一层。

三个问题共享一个根因：**没有一份属性表**。属性表一旦成为单一事实来源，样板函数、脏标记与默认值三件事都能由它派生。

## 变更内容

### 属性表 `PropSpec`（新增内部头 `include/aurora/widget/prop_accessor.h`）

1. 每个采用继承式双模的控件在 `.cpp` 的匿名命名空间内定义一张 `PropSpec` 表，一行一个固有属性，表项字段为：属性名、存储类别（**值字段** = 标量 / optional / `EdgeInsets` 等纯值；**来源句柄** = `ReadOnlyState<T>` / `State<T>`）、C++ 类型描述（供描述符 `type` / `enum` / `minimum` / `maximum` 使用）、`DirtyKind`（`None` / `Paint` / `Layout`，`Layout` 蕴含绘制）、是否主题绑定、`default_expr`（主题令牌表达式或字面量的显示串），以及三个由宏生成的静态函数指针：反序列化写入（`apply(Widget&, const Json&)`）、序列化读取（`capture(const Widget&)`）、信号采集（`collect(Widget&, StateSink&)`，句柄行把来源连同本行 `DirtyKind` 一并登记，值字段行为空）。**表本身不向公共头暴露**（调用方无需感知），`describe_static()` / `serialize_props()` / `deserialize_props()` / `collect_signals()` 各改为对表的一行转发。
2. 新增两个宏，均只在本库内部头使用：`AURORA_PROP_DECL(Widget, name, Type)` 在控件公共头展开为 `[[nodiscard]] auto name() const -> Type;` 与 `auto set_##name(Type value) -> Widget&;` 两条声明；`AURORA_PROP_IMPL(Widget, name, Type, Dirty, Flags, DefaultExpr)` 在 `.cpp` 展开为 setter 定义（写 `WidgetProps::name`、按 `DirtyKind` 标脏、信号项 `resubscribe_signals()`）与表行（含三个函数指针）。存储类别为**来源句柄**的行，DECL 额外展开 `set_##name##_source(Handle)` 声明，IMPL 展开值 setter 与换源 setter（退订旧源、订阅新源）两个定义——值 setter 语义按句柄权限分派：`ReadOnlyState` 行换入自有常量来源，`State<T>` 行写回当前来源（`Props::name.set(v)`）。`04-widget.md` §2.5.1 既有的「值 / 来源两种 setter」约定由此从手写纪律变成生成保证。宏名与展开形态均为本库内部约定，不构成对外稳定 API。
3. 既有非属性成员保持原样：回调成员（如 `Button::on_click`）不参与序列化，不进表，也不加访问器；`Modifier` 承载的跨切面能力不进固有属性表（见 `04-widget.md` §2.5 的二层属性划分）。

### 写拦截：同名访问器遮蔽（编译期、零额外存储）

1. 对 16 个继承式控件的每个标量 / optional 公有字段，在派生类中声明**同名访问器**（上面宏的第一条声明）。由于同名访问器遮蔽基类数据成员，`btn.corner_radius = 12.0F` 编译失败（g++ 报 `invalid use of member function ... did you forget the '()' ?`），错误发生在编译期、不增加任何存储，也不需要私有镜像。
2. 合法写入路径收敛为三条：`btn.set_corner_radius(12.0F)`（标脏由表驱动）、`btn.corner_radius(12.0F)` 之类取值 / 读取、以及**显式逃生舱** `btn.ButtonProps::corner_radius = 12.0F`（内部实现与确需绕过标脏的极少数场景，语义上等价于「我知道我在直接改字段」）。
3. 构造路径不受影响：`Button(ButtonProps{ .corner_radius = 10 })` 与内部 `Props::field` 读写仍合法，批量构造仍走 `*Props` 聚合（`04-widget.md` §2.5 形态一）。
4. 句柄属性不是表的例外：`ReadOnlyState<T>` / `State<T>` 行同样进表、同样由宏生成访问器（见下节）；`btn.color = X` 这类绕过句柄存储的静默无效写在同名访问器落地后**编译失败**（`color` 的访问器遮蔽 `ButtonProps::color` 字段）。
5. 批量解码走聚合脏：`deserialize_props()` 逐键写字段后在末尾按表中出现过的 `DirtyKind` 各标脏一次（`Layout` 一次、`Paint` 一次），句柄行重订阅一次，不按键重复标脏。

### 与 State / ReadOnlyState 的有机结合

编译期与运行时两层拦截各管一半、共用同一张表：访问器拦「经 API 的写」（保证标脏 / 重订阅），`State` 订阅拦「来自外部的变」（宿主写回、换源、`Provider` 换主题）。两层查同一表行，脏语义在两条路径上天然一致。

1. **一行覆盖两种存储**：句柄属性与值字段同表同行，行首「存储类别」决定宏的展开形态与序列化口径；两种存储不互相包装——标量不强行包成 `State`（避免无谓的分配与订阅开销），句柄不退化成裸字段（丢失变更传播）。
2. **句柄行三件套**：宏对句柄行生成 `name()`（读当前句柄）、`set_name(v)`（值 setter：换入自有常量来源并写值）、`set_name_source(src)`（换源 setter：退订旧源、订阅新源），三者均按表行标脏并重订阅；现有 `Button::set_label` / `set_background_source` / `background(Color)` 即此形态的手写版，迁移后删除。
3. **消灭私有镜像**：句柄存储只保留 `Props` 基类字段一份（访问器遮蔽后外部直写编译失败），删除 `label_` / `color_` 这类私有镜像——镜像与基类字段可能分叉正是 `btn.color = X` 静默无效写的根源；单存储后该缺陷类别整体消失，`collect_signals` 也直接登记基类字段，不再镜像同步。
4. **订阅回调按表定点标脏**：`collect_signals` 登记来源时随行携带该行的 `DirtyKind`（`StateSink` 登记项扩展，不改变 CHG-047 确立的 `State` / `ReadOnlyState` 权限模型与按来源身份去重的语义）；订阅回调据此**只标该行需要的脏**（`Layout` 行含绘制，`Paint` 行不动布局），替代「任何信号变更一律 layout + paint」的粗粒度行为——宿主写一个只影响绘制的来源（如按钮背景色）不再触发整树重排。
5. **序列化与主题解析口径编码进表**：句柄行序列化读当前值（`get()`），反序列化经值 setter 写回——只读行换入常量来源、可编辑行写回当前来源（未绑定时落自有状态）——§2.5.1「序列化只还原值、宿主重建后重新接线」由表统一保证，不再逐控件手写；`ThemeBound` 句柄行（如 `ReadOnlyState<std::optional<Color>>`）的解析链为「来源值 > 主题令牌」，`nullopt` 跟随主题的 CHG-011 两态语义不变，换主题经既有订阅通路定点重绘。
6. **可编辑行（`State<T>`）的写回接入**：值 setter 即写回原语——`set_name(v)` 调 `State<T>::set`，绑定宿主来源时宿主同步可见（可编辑模型的本义），未绑定时落在行内自有的默认 `State`（`State<T>{}` 自持来源，控件自治可用）；控件内交互处理器（勾选、拖动、输入）一律改调该原语，不再手写 `value_.set(v)`——表行成为写回的唯一入口，今后加钩子（自定义 `Eq`、`AlwaysPublish`）只改表行一处。
7. **写回的脏传播单一通路**：挂载后写回 → `State` 源变更通知 → 订阅回调按表行 `DirtyKind` 定点标脏，值 setter 自身**不重复标脏**（同一次变更只走一条脏路径）；`State` 的相等去重使无效写回天然短路，无需 setter 比旧值；挂载前写回无需标脏（mount 全量首排）。
8. **权限与落点**：`set_name_source` 形参类型恒为 `State<T>`，以 `ReadOnlyState` 接可编辑行**编译失败**（`State` → `ReadOnlyState` 单向降级在类型上不可逆）；现有可编辑模型控件（`Checkbox` / `Switch` / `Slider` / `TextInput`）不走 Props 聚合、状态为私有成员（如 `Checkbox::value_`），**不进同名访问器遮蔽范围**（不做清单不变），其既有交互写回已满足第 6、7 条语义，`describe_static()` / `serialize_props()` / `deserialize_props()` 的表行迁移随迁随接；继承式 16 控件中若出现 `State<T>` 行（如 `PullToRefresh` 公有化刷新态）按同一行机制落地。可编辑行在描述符 / `aurora_api.json` 中只出现值类型 `T`，存储类别不进 AI 面。

### 数据域下沉：Props 承担纯数据操作，Widget 只做副作用

`*Props` 是纯数据聚合（无构造、无虚函数、可拷贝、保持指定初始化器可用；成员函数不破坏聚合性质）。按「是否依赖控件生命周期」把操作分到两域：

1. **下沉到 Props**（只依赖自身字段与显式入参）：
   - 字段存储本体（唯一一份，无镜像）；
   - 纯数据变换：归一化 / 夹取（`corner_radius` 非负、`padding` 非负）、派生值（hover / pressed 由背景调暗）；
   - 主题解析的纯值层：`resolved(const Theme&)` 或逐字段 `resolve_*(const Theme&)`，入参只有 `Theme` 值，不读环境；
   - `defaults()` / `defaults(const Theme&)`；
   - 单键解析原语 `parse_<field>(const Json&) -> std::optional<T>` 与 `to_json()`（只产出显式值）；
   - `validate()`：返回「字段名 + 问题」列表，不写日志、不标脏。
2. **必须留在 Widget**（依赖控件生命周期或环境）：标脏（`mark_needs_layout` / `mark_needs_paint` 依赖布局父链与挂载态）、信号登记与重订阅（`collect_signals` / `resubscribe_signals` 依赖 `StateSink` 与控件生命周期）、链式返回 `Widget&`、事件派发 / 命中 / 焦点、`inherit_theme(ctx)` 的环境读取。
3. **写入入口的固定形状**：`Widget::set_x(v)` = 调 Props 纯变换 → 写 `Props::x` → 按表行标脏 → 信号行重订阅。依赖方向单向（Widget 依赖 Props，Props 不依赖 Widget）。
4. **反序列化拆两段**：Props 层 `parse_<field>` 纯解析（缺键即不产生值，保持「只应用出现的键」语义），Widget 层逐键应用后聚合标脏；不做「`from_json` 返回完整 Props 在控件上整体覆盖」——那会把未出现的键重置为默认值。
5. **不引入 Props → Widget 的反向引用**：不在 Props 里存 `Widget*`、不用回调把标脏送回去。那会破坏 Props 的值语义（`defaults()` 快照、`ButtonProps` 聚合构造与拷贝都会携带可能悬垂的指针）、让序列化 / 代码生成拿到的不再是纯数据、单测无法脱离控件构造，并与 `*Props` 作为公有基类子对象的拷贝语义冲突。

### 主题默认值：形状 / 间距令牌 + optional 两态

1. `Theme` 新增两个纯值字段（与 CHG-011 的 `roles` / `type_scale` 同构，值语义、随主题值拷贝）：
   - `ShapeScale shapes`：`none = 0` / `extra_small = 4` / `small = 6` / `medium = 8` / `large = 12` / `extra_large = 16`（单位 dp，对标圆角 token）；
   - `SpacingScale spacing`：`none = 0` / `extra_small = 4` / `small = 6` / `medium = 8` / `large = 12` / `extra_large = 16` / `huge = 24`（单位 dp，对标间距 token）。
   - **初值必须逐位复现现行硬编码**：`Button` 圆角 6 → `shapes.small`，内边距水平 12 / 垂直 6 → `spacing.large` / `spacing.small`，图表留白 8 → `spacing.medium`；`Theme::light()` / `dark()` 的这两组令牌共用同一份基准值（不分明暗）。
2. 主题敏感的固有属性由「字面量默认值」改为 `std::optional<T>`：`nullopt` 表示**跟随主题**，等价于 CHG-011 确立的「显式属性 > 主题角色」两态语义向形状 / 间距 / 字号推广。落地范围是现有硬编码默认值的字段：`Button` 的 `corner_radius` / `padding` / `font`，以及各继承式控件中同类默认值字段（图表族的内边距与字号、容器族的圆角与间距等），逐字段在控件头注释中写明对应的主题令牌。
3. 解析时机固定在布局 / 绘制期，经既有 `inherit_theme(ctx)` 取值：`corner_radius.value_or(theme.shapes.small)`、`padding.value_or(EdgeInsets{ spacing.large, spacing.small, ... })`、`font.value_or(theme.type_scale.body_medium)`。控件内部缓存解析结果供命中测试复用（与现有 `geom_` / `cached_display_text_` 同源策略），不在 setter 里读主题。
4. 换主题即生效：`Provider<Theme>` 的 `on_signal_dirty()` 已标脏布局 + 绘制并 `invalidate_subtree_caches()`，未显式设值的控件在新主题下自动取新令牌，无需重建控件；已显式设值的控件保持显式值（两态语义的负向判据）。
5. `defaults()` 拆两级（对应需求 SPEC.API.SENSIBLE-DEFAULTS.001「合理默认值」）：
   - `static auto defaults() -> XxxProps`：**未解析快照**，主题敏感字段为空（表示跟随主题），可选颜色字段为空；
   - `static auto defaults(const Theme &theme) -> XxxProps`：**解析快照**，把 `theme` 的形状 / 间距 / 字阶令牌烧进对应字段，供运行时查询、AI 代码生成与文档生成使用。
6. 序列化口径：`nullopt` 字段不进 JSON（未显式设置即未设置，避免把某一主题的瞬时默认值写进文档）；反序列化读到的值一律为显式值。描述符侧沿用 CHG-016 的 `default` / `default_expr` 口径：主题绑定条目只有显示串（`"theme.shapes.small"`、`"theme.spacing.large"`、`"theme.type_scale.body_medium"`），**没有**原生 `default`（因为 `serialize_props` 对它输出空），与 CHG-016「75 条无原生默认值的属性只以 `default_expr` 存在」同一条规则；字面量默认值属性仍同时给 `default` 原生值。

### 迁移与版本

1. 破坏性变更按 `SPECIFICATIONS.md` §12 与 `CHANGELOG.md` 的 semver 政策记入，迁移路径为一行机械替换：`x.f = v` → `x.set_f(v)`；句柄属性换源 `x.f = src` → `x.set_f_source(src)`、写值 `x.set_f(v)`；确需绕过标脏处改用 `x.XxxProps::f = v`。
2. 仓内调用点（`tests/` 与 `examples/demos/` 中的裸字段赋值）同批迁移；`04-widget.md` §2.5 形态二的示例同步改写为 setter。
3. 为避免新属性再次绕过拦截，新增轻量静态门禁 `tools/check/check_props_accessor.py`：对 16 个继承式控件的 `*Props` 公有字段做名字集合比对，断言派生类存在同名访问器与 `set_<name>` 声明；不做 C++ 解析，只做词法级声明扫描，白名单为空。

## 验收判据

1. **默认主题逐位不变**：`HeadlessSurface` 下默认构造的 `Button` 在 `Theme::light()` 与 `Theme::dark()` 中的背景 / 文字色 / 圆角（6）/ 内边距（12·6）/ 字号（14pt Regular）与改动前 golden 逐位相等；全量 golden 用例零 diff（含图表族与容器族的默认间距）。
2. **编译期拦截自证（负向编译判据）**：`Button b; b.corner_radius = 12.0F;` 与 `b.color = Color::white();` 必须编译失败（前者 `invalid use of member function`，后者同因访问器遮蔽）；`b.set_corner_radius(12.0F)`、`b.ButtonProps::corner_radius = 12.0F` 必须编译通过。16 个继承式控件各取至少一个标量字段与一个句柄字段施加同一判据。
3. **setter 标脏与表一致**：对表中每个 `DirtyKind = Layout` 的 setter，调用后 `needs_layout()` 与 `needs_paint()` 均为真；`DirtyKind = Paint` 的只置绘制脏；信号项 setter 触发一次重订阅（以测试用 `StateSink` / 计数 sink 断言）。变异自证：把 `Button::set_corner_radius` 的标脏去掉，对应断言必须转红——该变异即复现当前缺陷。
4. **聚合脏**：一次 `deserialize_props` 同时解码 6 个字段（覆盖 `Layout` / `Paint` / 信号三类），断言布局标脏 = 1、绘制标脏 = 1、重订阅 = 1；类型不符的键只拒收该键，其余照常应用且不重复标脏。
5. **序列化往返**：显式设置的字段往返逐位相等；未设置（`nullopt`）字段不出现在 JSON；`defaults()` 的主题敏感字段为空，`defaults(Theme::light())` 的圆角 = 6、内边距 = 12/6、字体 = `body_medium`；`describe_static()` 中 `corner_radius` / `padding` 条目无 `default`、`default_expr` 分别为 `"theme.shapes.small"` / `"theme.spacing.large"`。
6. **换主题默认值生效**：同一控件实例挂在 `Provider<Theme>` 下，从 `Theme::light()` 切到一份 `shapes.small = 18` / `spacing.large = 20` 的主题，断言未显式设值的实例圆角与内边距按新令牌变化，且布局与绘制各被标脏一次（复用既有 `on_signal_dirty` 通路，不重建实例）；显式设过 `corner_radius` 的实例切换后保持显式值。
7. **schema 与门禁**：`--target aurora_api_json` 重建后 `check_api_schema_sync` 绿；`props` 数组形状不变（每 widget 一个 `props` 数组、`default` 原生值 / `default_expr` 显示串口径不变），差异只允许「新增 `default_expr`」与「optional 字段不再出 `default`」两类；新增 `check_props_accessor` 绿，且把任一新增属性的访问器声明删掉时该门禁转红；`check_naming_conventions` / `check_api_budget` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` / `check_umbrella_header` / `check_change_proposals` 全绿；`format-check` 与改动相关 TU 的 `lint` 绿。
8. **全量测试**：`ctest --preset ninja-test` 全绿（含 wasm 腿——令牌为纯值类型、解析路径与平台无关）；`manual-test/11-widget.md` 与 `13-theming.md` 中受影响的交互步骤按新 API 改写后逐条实测通过。
9. **句柄行三件套与定点标脏**：挂载后调 `set_label("X")` 断言换入常量来源、重订阅一次、按本行 `DirtyKind` 标脏（`Layout` 行 layout 与 paint 各一次）；宿主经既有 `State` 来源改值触发订阅回调时，`Paint` 行（如 `color`）断言 `needs_layout()` 为假、仅重绘——变异自证：把该行 `DirtyKind` 改成 `Layout` 则该断言转红；句柄行序列化往返只还原值（`from_json` 后经常量来源显示反序列化值，不依赖宿主重新接线）；删除 `label_` / `color_` 私有镜像后 `btn.ButtonProps::label = src` 逃生舱写不触发重订阅（需显式换源才生效），该行为有单测钉住。
10. **可编辑行（`State<T>`）写回**：测试用最小控件持一个 `State<int>` 表行——绑定宿主 `State<int>` 后调 `set_count(3)`，断言宿主观察到 3、控件经订阅恰好标脏一次（无双重标脏）；宿主直接 `set(5)` 时控件按本行 `DirtyKind` 定点标脏；未绑定实例 `set_count(2)` 落自有状态且 `count()` 可读；`set_count_source(ReadOnlyState<int>{0})` 编译失败（负向判据）；JSON 往返还原值；`Checkbox` / `Slider` 既有交互写回路径回归全绿（本批不迁移、行为不变）。
11. **数据域边界**：`ButtonProps` 可脱离控件与 `BuildContext` 独立完成 `defaults()`、`defaults(Theme)`、归一化、单键解析、`validate()` 与 `to_json()`（单测直接构造 `ButtonProps` 调用即可，不需要挂载）；`ButtonProps` 不含 `Widget*` / 回调指针 / 环境读取（以类型检查与单测构造钉住）；`Widget::set_corner_radius(-4)` 经 Props 归一化后为 0 且按行标脏；缺键 JSON 反序列化只改出现的键，未出现的键保持现值（变异自证：改成整体覆盖后用「只含一个键的 JSON」断言未出现键被重置，该断言必须转红）。

## 回写落点

- `codespec/specification/04-widget.md`（§2.5 双模 API 与 Props 约定：形态二由裸字段赋值改为 setter、同名访问器遮蔽与 `XxxProps::` 显式逃生舱、`defaults()` 两级签名；§2.5.1 响应式属性与句柄权限：表驱动的标脏、换源与聚合解码口径）
- `codespec/specification/02-state.md`（采集协议扩展：`collect_signals` 登记项随源携带 `DirtyKind`、订阅回调定点标脏、「值 / 来源两种 setter」的表驱动口径与私有镜像废除）
- `codespec/specification/07-environment-modifier.md`（§5.1 主题值对象与角色集：新增 `ShapeScale` / `SpacingScale` 字段与初值表；§5.4 作用域、查询与废除面：主题敏感属性的 optional 两态与解析时机）
- `codespec/specification/03-layout-render.md`（§7.4 二层属性划分：固有属性主题默认值在布局 / 绘制期解析，不下沉到 setter）
- `codespec/CODING_STANDARDS.md`（§11.5 合理默认值的表述由字面量改为主题令牌表达式与 `defaults(const Theme&)`；属性访问器命名与 `AURORA_PROP_*` 宏的内部约定）
- `codespec/SPECIFICATIONS.md`（§12 版本门禁：本次破坏性变更的迁移路径与 CHANGELOG.md 登记口径）
- `codespec/manual-test/11-widget.md`（受影响的属性设置交互步骤按 setter 改写）
- `codespec/manual-test/13-theming.md`（换主题后形状 / 间距默认值生效的观测步骤）

## 属性设计原则与指导

以下规则是本次变更落地后的**长期口径**，新增 / 修改任何控件属性都按此执行；冲突时以 `04-widget.md` §2.5 与本节为准。

### 属性归属：先决定放哪里

- **固有属性**（描述控件身份、随控件序列化）→ `XxxProps` 字段进属性表；**跨切面装饰**（内外边距、背景、变换、裁剪、事件）→ `Modifier`。二层划分不变，重叠时固有属性优先，绘制上 `Modifier` 在外、固有属性在内（`03-layout-render.md` §7.4）。
- **回调成员不进表**（如 `on_click`）：不序列化、不加访问器、不参与脏标记。
- **派生值不入属性**：能由其他属性或主题算出的值（如按钮的 hover / pressed 派生色）只做缓存，不新增字段；缓存必须与源同源失效，不引入第二真值。

### 存储类别：由「谁写、谁读」决定

| 场景 | 存储 | 写路径 | 脏传播 |
|:---|:---|:---|:---|
| 纯值（标量 / optional / `EdgeInsets` / `Font`） | 值字段 | `set_x(v)` 写字段 | setter 按 `DirtyKind` 标脏 |
| 展示来源（控件只读、不代写上游） | `ReadOnlyState<T>` | `set_x(v)` 换自有常量源；`set_x_source(src)` 换源 | setter 标脏 + 重订阅；外部变更由订阅回调定点标脏 |
| 可编辑模型（控件按交互写回、宿主共享） | `State<T>` | `set_x(v)` 写回 `State::set`；`set_x_source(src)` 只收 `State<T>` | 单一通路：源变更 → 订阅回调定点标脏，setter 不重复标脏 |

- **不要为形式统一把标量包成 `State`**：纯值没有外部写入方，包一层只增加分配与订阅开销；**也不要把句柄退化成裸字段**：会丢失变更传播，并把「控件只读」降级为无约束写入。
- **权限即类型**：`State<T>` 单向隐式降级为 `ReadOnlyState<T>`，反向不存在；可编辑行的 `set_x_source` 形参恒为 `State<T>`，用只读句柄接会在编译期失败。
- **单一存储**：句柄属性只保留 `Props` 字段一份，禁止私有镜像（`label_` / `color_` 与基类字段分叉正是静默无效写的根源）。

### 数据与行为的分界：纯值下沉 Props，副作用留在 Widget

| 操作 | 归属 | 判据 |
|:---|:---|:---|
| 字段存储、归一化 / 夹取、派生值、`defaults()` / `defaults(Theme)`、单键解析、`to_json()`、`validate()` | `XxxProps` | 只依赖自身字段与显式入参（含显式传入的 `Theme` 值） |
| setter 侧效（标脏、重订阅、链式返 `Widget&`）、`collect_signals`、事件 / 命中 / 焦点、`inherit_theme(ctx)` | 控件类 | 依赖挂载态、布局父链、`StateSink` 或 `BuildContext` |

- **依赖方向单向**：Widget 依赖 Props，Props 不依赖 Widget；Props 里不出现 `Widget*`、回调指针或任何环境读取。
- **Props 保持纯数据**：不加构造函数（保住指定初始化器）、不加虚函数；成员函数只做值变换与查询，可脱离控件构造与测试。
- **缺键不重置**：反序列化由「Props 纯解析 + Widget 应用」两段完成；禁止用完整 `Props` 整体覆盖控件现状。

### 默认值：主题可决定的必须 optional

- **主题可决定的默认值 → `std::optional<T>`，`nullopt` 表示跟随主题**，头注释写明对应令牌（如 `///< 空 = theme.shapes.small`）。禁止把令牌数值抄成字面量（`float corner_radius = 6.0F` 会让换主题失效）。
- **主题不可决定的默认值 → 直接字面量**，并在头注释与描述符中可见（`SPEC.API.SENSIBLE-DEFAULTS.001` 的「声明处可见」）。
- **解析时机固定在布局 / 绘制期**，统一经 `inherit_theme(ctx)` 取令牌，禁止在 setter、构造期或静态初始化里读主题；换主题时由 `Provider<Theme>` 的既有标脏通路使未显式设值的控件自动生效。
- **两态语义不可倒置**：显式值恒优先于主题令牌；换主题只影响 `nullopt` 的行。

### 脏类别：必须显式声明，不许隐式约定

- `Layout`：几何、尺寸或内容变化（蕴含绘制）；`Paint`：仅外观；`None`：对当前绘制无影响（如计数类登记）。
- 每个属性**必须在表行写出 `DirtyKind`**；判不准时取 `Layout`（保守），不允许省略。
- **setter 内禁止手写 `mark_needs_layout()` / `mark_needs_paint()`**：标脏由表行驱动，手写会与表双份或漏标；唯一的表外标脏点是控件交互处理器对值字段的直接写（同样按行查表）。
- 批量解码（`deserialize_props`）只按出现的类别聚合标脏一次，不按键重复标脏。

### 主题绑定标记：只标真事

- `ThemeBound` 当且仅当该行默认值取自主题令牌；标了就对描述符生效——`aurora_api.json` 中只出 `default_expr`（`"theme.shapes.small"` 一类），不出原生 `default`。
- 令牌只能来自 `Theme` 的强类型字段（`roles` / `type_scale` / `shapes` / `spacing`），禁止字符串令牌表、禁止控件自定义私有令牌。
- 新增主题令牌须同步 `Theme` 值对象与描述符显示串，两者同批修改。

### 序列化：只还原显式值

- 显式值才写出；`nullopt` 不写、不补默认；句柄行写当前值（`get()`）。
- 反序列化走值 setter：只读行换入常量来源，可编辑行写回当前来源（未绑定则落自有状态）；类型不符只拒该键，未知键忽略。
- **回调、来源身份、条目工厂不进序列化**；`from_json` 后如需外部接线，由宿主显式重连（§2.5.1 的既有约定）。

### 命名与 AI 友好

- 一行一属性：字段名 = 访问器名 = setter 后缀，`set_<name>` / `set_<name>_source`，不造别名。
- 公共头只有访问器声明与 `Props` 字段；**属性表、`DirtyKind`、存储类别一律不出现在公共头**。
- 描述符 `type` 取真实 C++ 类型（`std::vector<std::string>` 不写 `"array"`），`note` / `enum` / `minimum` / `maximum` 与字段语义一致。

### 新增属性检查清单

1. `XxxProps` 加字段（写清存储类别与默认值形态）；
2. 头文件加 `AURORA_PROP_DECL`（含注释 `@brief` / `@param` / `@return`）；
3. `.cpp` 表加一行 `AURORA_PROP_IMPL`（`DirtyKind`、`Signal` / `Writable`、`ThemeBound`、`default_expr`）；
4. 按需补 `collect_signals` 的登记与交互写回（可编辑行只调 `set_x`）；
5. 单测覆盖：读写、标脏类别、序列化往返；主题绑定属性加「换主题生效 + 显式值不被覆盖」；句柄行加重订阅计数；可编辑行加「宿主可见 + 不双重标脏」；
6. `--target aurora_api_json` 重建并跑 `check_api_schema_sync` / `check_naming_conventions` / `check_doc_comments` / `check_props_accessor`。

### 反例速查

| 写法 | 后果 |
|:---|:---|
| `setter` 里再手写 `mark_needs_layout()` | 与表行双份，或表改而 setter 不改导致漂移 |
| `float corner_radius = 6.0F`（本应 optional） | 换主题不生效，且序列化把默认值写进文档 |
| 给句柄属性再加私有镜像 | 镜像与基类字段分叉，产生静默无效写 |
| `btn.label = src`（裸写公有字段） | 编译失败（访问器遮蔽）；须 `set_label_source(src)` |
| 用 `ReadOnlyState` 接可编辑行的换源 | 编译失败；可编辑行只收 `State<T>` |
| 把 `on_click` 放进属性表 | 回调进序列化，往返语义错误 |

## 不做清单

- 不引入通用 property 系统或反射式属性框架：表只在 `.cpp`，公共头不含任何属性元数据。
- 不给回调成员（`on_click` 等）加访问器，也不把它们纳入序列化。
- 不改 `Modifier` 体系与二层属性划分，不改 `State` / `ReadOnlyState` 的权限模型与按来源身份去重的采集语义（CHG-047 的结论保持；`StateSink` 仅扩展登记项携带脏类别）。
- 不把标量属性包成 `State`，也不把句柄属性退化成裸字段：存储类别由表行逐条选择，两种存储不互相包装。
- 不为 `TextInput` 等使用「私有 `*_` 成员 + setter」的控件补访问器遮蔽：它们本就没有裸公有字段可绕过。
- 不新增明暗两套形状 / 间距令牌，也不引入按控件定制的令牌表；令牌只表达全局形状与间距轴。
- 不合并或改写 CHG-011 的色角色、CHG-016 的 schema 形状；本提案只在既有口径上扩展。
