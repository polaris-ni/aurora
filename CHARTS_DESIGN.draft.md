# 图表控件族 设计（草案 v2 · 已采纳归档）

> 状态：**已采纳**（切片 1–9 全部随 alpha.5 落地，代码在 `dev-1.0.0-alpha.5` 分支）｜ 日期：2026-09-16 ｜ 分支：dev-1.0.0-alpha.5
> 关联：ROADMAP.draft.md（alpha.5「图表控件族」条目已勾选）、specification/04-widget.md §3.8（控件清单已补全五图）、specification/03-layout-render.md §5/§8（Painter 原语与 DisplayList）
> 采纳去向：控件契约 → 04 §3.8；Painter 新原语 → 03 §5 + §8.6；跨框架映射 → CONCEPTS.md §3.1；使用配方 → GUIDELINE.md §33；本文件已归档（内容全部落地，保留作设计溯源）。
> 设计要点速记：每图一个 `LeafWidget` + 纯值 `XxxProps` + `defaults()`；数据进序列化面、回调旁挂不进面；轴域与命中同源（`LinearScale`/`BandScale`）；绘制不越 `bounds`；半透明系列降透明依赖真 SDF 折线（切片 1）；grow-in 动画经 `Animator::current()` 且无 Animator 时降级到终态（golden 稳定）。

## 0. v2 变更摘要（相对 v1 的查漏补缺）

**事实更正（v1 与代码不符处）**

| # | v1 说法 | 代码事实 | 影响 |
|---|---------|---------|------|
| F1 | 「AI 可经 `generate_ui` 直接生成含数据图表」 | `app/generate_ui.h` 是 8 条关键词硬编码映射（button/text/…），**不读 schema、不产出数据属性** | AI-first 闭环应改写为「schema + `from_json` + `validate_ui_tree`」，见 §3.12 |
| F2 | 「grow-in 经 `AnimationController`，reduce_motion 自动短路」 | 短路成立（`AnimationController::tick` 内），但**现无任何 widget 自持控制器**（仅 `NavigatorHost`/`TransitionLayer`，且由外部注入 `Animator&`）；`Animator` 无 `Scheduler::current()` 式全局访问器 | 需新增接入机制 + 析构注销，见 D11 |
| F3 | 「props_io 无数组编解码先例」 | 已有**字符串数组**先例（`text_decoration_to_json`）；缺的是 `vector<double>` 与**对象数组** | 风险降级，退路不变 |
| F4 | 「轴刻度标签直接复用 `i18n/format`」 | `render_to_png` 用 `constexpr BuildContext`（`env == nullptr`），`env_of<Locale>` 会断言失败 | 必须 `ctx.environment<Locale>()` + `Locale{}` 回退（同 `Text::resolved_text`） |
| F5 | 「每图 1 张 golden 基线」 | `render_to_png` 单帧且**无 Application ⇒ 无 Animator** | golden 必须落到 t=1，否则基线是空图，见 D11 |
| F6 | 「交互：hover 高亮 + 值框」 | `on_hover_change` 默认**不标脏**；`on_pointer_event` 基类 Move 分支只在按下时动作；`wants_click()` 默认 false | 图表必须三处全部覆写，见 §7.1 |

**结构性补缺（v1 完全未覆盖）**

| # | 缺口 | 落点 |
|---|------|------|
| G1 | 命名与 API 形态未对齐 `CODING_STANDARDS` §6.4 / §11.1（`XxxProps` 聚合 + `class Xxx : public LeafWidget, public XxxProps` + `defaults()` + 三形态等价） | D10 |
| G2 | 越界绘制：`paint_bounds_` 决定脏区，值框/标签画出 `bounds` 会留残影 | D12 |
| G3 | 半透明系列（图例联动 0.35）⇒ 折线不能「逐段 `draw_line` 复合」，必须真 SDF | D13 |
| G4 | 缺省色板完全绕过 `Theme`（`Theme` 有 `tokens` 命名令牌表可承接） | D14 |
| G5 | 数据健壮性：空数据 / 全 0 / NaN·inf / 负值 / 超量点 的行为未定义 | D15 |
| G6 | 无障碍语义（`accessibility_label/value`）缺失，只考虑了 reduce_motion | D16 |
| G7 | 未与既有 `Canvas`（自定义绘制逃生舱）划边界 | D17 |
| G8 | 轴留白 / 值框定宽 / 图例换行需要的文本度量 API 未提 | §3.1 |
| G9 | 接入清单缺 `itest_default_construct.cpp` 强制登记 | §9.6 |
| G10 | 控件级色板与网格/标签色的主题来源未定 | D14 / §6.6 |

---

## 1. 目标与非目标

**目标**

1. 五类图表控件：`BarChart` / `LineChart` / `PieChart` / `ScatterChart` / `Sparkline`，纯软件 `Painter` 矢量绘制，零三方依赖。
2. API 形态对齐 fl_chart：每图一个独立 widget，数据与视觉配置为**纯值属性包**（`XxxChartProps`），整包可 `to_json/from_json/diff/apply_patch`——AI 可经 **schema（`aurora_api.json` / `describe_static`）+ `from_json` + `validate_ui_tree`** 生成含真实数据的图表（F1）。
3. 最小受限矢量原语扩展：`stroke_polyline`（AA 圆角连接多段线）+ `fill_sector`（AA 扇形/环扇，内外半径）+ `stroke_arc`（弧线描边，`fill_sector` 环带糖），同步 `DisplayList`/`CmdKind` + `SoftwareRhi` + `GpuGlRhi` 三路。
4. 交互：hover 最近数据点高亮 + 自绘悬浮值框 + 十字准线（cartesian 图）+ `on_point_tapped`/`on_section_tapped` 回调 + 图例 hover 联动（系列高亮/其余降透明）。
5. 进入动画（grow-in）：数据首次绑定/变更时经 `AnimationController` 驱动 0→1 进度（柱生长/折线描线/扇形展开），`reduce_motion` 经主控制器 tick 自动短路；**无 Animator 时静态降级到 t=1**（D11）。
6. D3 式纯值轴模型：`LinearScale`/`BandScale`（domain→px + nice ticks + 反查 invert），轴渲染与命中测试同源。

**非目标**

- 面积填充（area fill）——需通用 AA 多边形填充，超出最小原语集，留后续 Path 阶梯。
- 缩放/平移、拖拽选区、双 Y 轴、对数/时间轴（Scale 仅 linear + band）。
- 动态数据流优化（实时 append 增量绘制）；完整 Path API / SVG path 导入。
- 图表主题化色板**接管**（不引入 chart 主题层；仅允许经 `Theme` 命名令牌覆盖内置色板，见 D14）。
- 轴/标签的 RTL 镜像（本波统一 LTR 布局；`Locale` 只影响数字格式化）。

## 2. 决策表

| # | 决策 | 结论 |
|---|------|------|
| D1 | API 形态 | **独立 widget + 纯值 Props**（fl_chart 路线）：每图一个 `LeafWidget` 子类并继承自身 `XxxChartProps`；交互回调旁挂、不进序列化面 |
| D2 | Painter 原语 | **最小受限原语集**：`stroke_polyline` + `fill_sector` + `stroke_arc`（糖），不做通用 Path |
| D3 | 交互范围 | **hover + 选中 + 十字准线 + 图例联动**；不做缩放/选区 |
| D4 | 动画 | **进本波**：grow-in 经 `AnimationController`，reduce_motion 自动短路；不做数据点级 morph 动画 |
| D5 | 数据进序列化面 | AI-first 卖点闭环；`Repeater` items 不序列化是构建回调不可序列化，图表数据是纯数值无此障碍 |
| D6 | 轴模型 | D3 概念的 C++ 纯值化；轴渲染/刻度生成/命中反查三处同源，不引入独立 Axis widget |
| D7 | 模块归属 | 公共类型 + Scale → `widget/chart_common.h`；五控件 → `widget/bar_chart.h` / `line_chart.h` / `pie_chart.h` / `scatter_chart.h` / `sparkline.h`（沿用扁平目录） |
| D8 | 缺省色板 | 系列未显式设色时按索引取**内置 8 色 Material 风格色板** |
| D9 | 数据更新方式 | `set_data()`（值语义、标脏重绘并触发 grow-in 重放）+ Props 直接赋值；`State<std::vector<T>>` 响应式接线随实现期验证（见 §12） |

**v2 新增**

| # | 决策 | 理由 |
|---|------|------|
| D10 | 命名与三形态 | 用 `BarChartProps` 等（非 `XxxChartData`），`class BarChart : public LeafWidget, public BarChartProps`，配 `static defaults()`；链式 setter / Props 聚合 / 成员赋值三形态等价（`CODING_STANDARDS.md` §6.4、§11.1 强制）。内嵌 `ScatterSeries`/`PieSection` 提升到 `chart_common.h`，避免 Props 内嵌类型导致的聚合初始化歧义 |
| D11 | 动画驱动接入 | 新增 `Animator::current()`（对齐既有 `Scheduler::current()`，由 `Application::run()` 起止设置）。图表在 `on_mount` 注册、`~Xxx()` 调 `animator.remove(ctrl_)`（裸指针悬垂，见 `Animator::drive` 告警）；**`current() == nullptr` 时进度恒为 1 并 `Diagnostics::warn` 一次**——这同时保证 `render_to_png` golden 稳定落在终态（F2/F5） |
| D12 | 绘制边界硬约束 | 所有绘制（含值框、十字准线、轴标题、图例、百分比标签）**严格夹在 `bounds` 内**：`paint_bounds_` 决定脏区，越界像素不会被擦除 → 残影。轴留白在图内部以 `EdgeInsets` 预留，值框按可用区翻转/夹取（G2） |
| D13 | polyline 必须真 SDF | 图例联动把非高亮系列降到 alpha 0.35；「逐段 `draw_line` + 顶点圆盘」复合实现在半透明下顶点处会二次合成 → 串珠状加深。软件路径一次性实现真距离场；复合降级**仅**用于 `GpuGlRhi` 首版（G3） |
| D14 | 色板与 Theme 的关系 | 内置 8 色为硬编码常量数组；额外支持 `Theme::tokens` 覆盖键 `chart.palette.0..7`（`token_or` 读取，缺失回退内置）。网格线/轴标签/值框背景取 `inherit_theme(ctx)` 的 `text`/`background` 派生 alpha，不新增 chart 主题层（G4/G10） |
| D15 | 数据健壮性 | 见 §6.7 降级表：空数据 / 全 0 / NaN·inf / 负值 / 超量点各有确定行为，异常输入走 `Diagnostics::degraded` 不崩溃、不抛异常（不可信 JSON 通道可达） |
| D16 | 无障碍语义 | 覆写 `accessibility_label()`（如 `"BarChart, 3 series, 12 categories"`）与 `accessibility_value()`（当前 hover/选中点的 `"series: value"`）；`reduce_motion` 沿用控制器短路，零额外代码（G6） |
| D17 | 与 `Canvas` 的边界 | `Canvas`（`widget/canvas.h`）保留为**任意自绘逃生舱**，其 `on_paint` 回调不可序列化；图表族是「数据可序列化 + 有轴/命中/动画语义」的**一等控件**，二者不重叠、不互相替代（G7） |

## 3. 现状事实（设计依据 · v2 已核对代码）

1. **Painter**（`render/painter.h`）：有 `draw_line`（SDF 线段、**固定圆帽**、受裁剪与 `global_alpha`）、`fill_rect`/`fill_rounded_rect`（AA）、`draw_rounded_border`、`draw_text`、渐变、`draw_shadow`、`blur_region`、裁剪栈（`push_clip`/`push_clip_rounded`）、`composite`。**扇形填充不存在**；折线逐段 `draw_line` 顶点无 join。`global_alpha` 必须被新原语乘入——系列降透明（0.35）依赖它。
   - **文本度量**（轴留白 / 值框定宽 / 图例换行必需）：`FontEngine::measure_width(text, font[, opts])`、`measure_height(font)`（`render/font_engine.h:121-143`）。
2. **DisplayList**（`render/display_list.h:25`）`CmdKind` 现 18 条全覆盖上屏原语；`DrawCmd` 仅 `f0/f1/f2` 三个浮点槽（:54），扇形需 4 个（外半径/内半径/起角/止角）→ 加 `f3`。`SoftwareRhi`（转发回 Painter）与 `GpuGlRhi`（alpha.4）为平级消费者。变长数据走数据池（现有 `str/color/float/font/image/matrix` 六池）。
3. **序列化链路**：`Widget::serialize_props/deserialize_props/type_name/describe` 虚接口（`widget.h:483-531`）；工厂 `WidgetRegistry::reg_default<T>` 集中注册（`serialization.cpp:167`）；`describe_static()` → `aurora_api.json`（`--target aurora_api_json` 刷新，`check_api_schema_sync` 守护）。`props_io.h` 现覆盖标量 + `EdgeInsets` + 枚举，**已有字符串数组先例**（`text_decoration_to_json`），**无 `vector<double>` / 对象数组先例**。
   - `validate_ui_tree` 按 `PropDescriptor::json_type` 做**顶层类型**校验（`type_matches`，`src/aurora/app/validate_ui.cpp:194`）：`json_type="array"` 能通过，**元素 schema 不校验** → 畸形 series 数组要靠 `deserialize_props` 自保（D15）。
4. **矢量数据响应式无先例**：`Repeater` 持 `shared_ptr<State<std::vector<T>>>`，仅 size 变化重建（`repeater.h:101-127`）；`Binding`/`Reactive` 现仅用于标量。
5. **动画**（v2 更正）：`AnimationController` 输出归一化进度，`reduce_motion` 在其 `tick` 内统一短路（落端点，`src/aurora/animation/animator.cpp:38`）——grow-in 复用即自动继承无障碍语义。但：
   - 全仓**无 widget 自持控制器**；`NavigatorHost`/`TransitionLayer` 由构造函数注入 `Animator&`。
   - `Animator::drive()` 存**裸指针**，宿主析构前必须 `remove(c)`，否则帧循环 UAF（`animator.h:91-106`）。
   - `Application::animator()` 是实例成员，**无全局访问器**；相对照 `Scheduler::current()` 存在（`app/scheduler.h:153`），`Timer` 在 `on_mount` 取用并对 nullptr 降级告警（`widget/timer.h:100-116`）——D11 沿用该范式。
6. **hover / 指针**（v2 更正）：
   - `Widget::on_hover_change(bool)` 默认**只置 `hover_` 不标脏**（`widget.h:316-319`）→ 图表必须覆写并追加 `mark_needs_paint()`。
   - `Widget::on_pointer_event(MouseEvent&)` 的 Move 分支**仅在 `pressed_ && has_gesture()` 时动作**（`widget.h:372-382`）→ 图表必须自行覆写处理悬停 Move。
   - 坐标用 `MouseEvent::local_position`（相对本控件，`event/event.h:72`），不要用 `position`（全局）。
   - `wants_click()` 默认 false → 需覆写为 true 才能消费 Press/Release 并阻止冒泡到父容器（`DataTable` 即此范式，`data_widgets.h:117`）。
   - `Modifier::tooltip` 是静态文本 + 延迟气泡，**不适合数据驱动值框**——值框自绘。
7. **i18n**：`i18n/format.h` 提供 `format_number(double, const Locale&, int fraction_digits = 2)`（分组 + 小数点，纯函数）。**取 Locale 必须用 `ctx.environment<Locale>()` + `Locale{}` 回退**（`Text::resolved_text`，`widget/text.h:172-175`）；`render_to_png` 传的是 `constexpr BuildContext`（`env == nullptr`），`env_of<Locale>` 会 `AURORA_CHECK` 失败（F4）。
8. **golden 设施**：`render_to_png`（`render/offscreen.h:33`，mount→layout→paint 单帧、无 Application）+ `compare_snapshots`（`snapshot_diff.h:38`，默认零容差）+ `AURORA_UPDATE_GOLDEN` 重生成；`tests/golden/` 现仅 `golden_basic_column.png` + `logical_snapshots.json`。
9. **主题**：`Theme`（`theming/theme.h:59`）有扁平字段（`background/primary/on_primary/text/font`）+ 命名令牌表 `tokens`（`set_token`/`token`/`token_or`）；`inherit_theme(ctx)` 未注入时回退 `Theme::light()`（`theme_scope.h:53`）→ 图表取色同样不得崩溃。
10. **接入全清单**（Dismissible 先例 + v2 补 G9）：控件头 + `serialization.cpp` 注册 + `aurora.h` 挂载 + `examples/demos/demo_*.cpp` + `tests/unit/utest_*.cpp` + **`tests/integration/itest_default_construct.cpp` 登记（§6.2 强制）** + `known_enums.h` + `itest_known_enums.cpp` + `aurora_api.json` + CONCEPTS/GUIDELINE/spec 回写。
11. **命名与形态强约束**（`CODING_STANDARDS.md` §6.4 / §11.1）：属性以 `XxxProps` 具名聚合暴露，`class Xxx : public XxxProps` 继承；须提供 `static defaults()`；三形态（链式 setter / Props 聚合 / 成员赋值）语义等价；所有 Props 字段须有合理默认值（§6.2）。`Button` 是标准范式（`widget/button.h:20-83`）。
12. **`generate_ui` 的真实能力**（F1）：`app/generate_ui.h:29-54` 为 8 条关键词→类型硬编码映射，只输出 `text` 一个属性，**不会产出 series 之类数据属性**。AI-first 闭环路径应为：`aurora_api.json` / `describe_static()`（schema，含 `json_type`、`enum_values`、`examples`）→ 外部 LLM 或手写 JSON → `validate_ui_tree()` 校验 → `from_json()` 重建 → `InspectorAPI::apply_patch` 增量改。
13. **既有 `Canvas`**（`widget/canvas.h`）：接受 `PaintFn` 回调自由绘制，回调不可序列化，尺寸缺省 100×100 → 定位为逃生舱（D17）。

## 4. Painter 原语扩展（切片 1）

```cpp
// painter.h 新增（与既有 draw_line / fill_rounded_rect 同级的 AA 原语）
/// 抗锯齿多段线：宽度 width，圆角连接（round join）+ 圆帽（round cap）。
/// 逐像素覆盖 = smoothstep 于「到折线的最小距离场」±0.5px（join/cap 由距离场 min 天然融合）。
/// 必须乘入 global_alpha_（半透明系列降透明不得在顶点二次合成，见 D13）。
auto stroke_polyline(const std::vector<Point> &pts, float width, Color c) -> void;

/// 抗锯齿扇形/环扇：圆心 center，内/外半径（inner=0 即实心扇形），角度 [a0, a1) 弧度制（y 轴向下），
/// 角差 ≥2π 视为整圆/整环。边缘（径向两侧 + 角向两侧）各 1px smoothstep。
auto fill_sector(Point center, float outer_r, float inner_r, float a0, float a1, Color c) -> void;

/// 弧线描边 = 环带 fill_sector 的语义糖（inner = radius - thickness/2, outer = radius + thickness/2）。
auto stroke_arc(Point center, float radius, float thickness, float a0, float a1, Color c) -> void;
```

**实现路线（D13 定稿）**

- 软件 `stroke_polyline`：**真距离场一次性实现**。逐像素对候选线段求 min 距离；按线段 x 区间分桶（scanline bucket）把每像素候选段降到常数级，千点无压力。不做「逐段 `draw_line` 复合」——半透明系列（图例联动 0.35）在顶点会二次合成出串珠。
- 软件 `fill_sector`：包围盒内逐像素极坐标判定（半径区间 + 角度区间），各边界 1px smoothstep；`inner_r <= 0` 走实心扇形分支。
- `stroke_arc`：纯转发 `fill_sector`。

**DisplayList 同步**：`CmdKind` 新增 `Polyline` / `Sector`（18 → 20）；`DrawCmd` 加 `float f3`（Sector: f0=outer_r, f1=inner_r, f2=a0, f3=a1, pt0=center）；新增 `point_pool_` + `add_points()`/`points_at()`（模式同 `color_pool_`），Polyline 经索引引用。`SoftwareRhi` 两行转发。

**GpuGlRhi**：`Polyline` 首版 = 分解为既有 `DrawLine` 段 + 顶点圆盘（`Sector` 全圆），复用已验证的线段 SDF 路径；`Sector` 首版 = 包围盒 quad + fragment SDF。与软件路径逐位一致**不作要求**（容差 golden 属后续 GPU 阶梯），以 GPU smoke + 结构断言为准。半透明串珠在 GPU 首版属已知降级，随容差 golden 阶梯收敛。

**单测**：原语级 golden（`tests/golden/painter_polyline.png` / `painter_sector.png`，含 join/cap/AA 边缘/inner=0/整圆/**半透明 alpha=0.35 无串珠**用例）+ `utest_painter_primitives` 几何断言（纯色区域采样 + 包围盒外无写入断言）。

## 5. 公共数据层（chart_common.h，切片 2）

```cpp
/// 数据点（Scatter 显式坐标用；Line/Bar/Sparkline 为等距，x = 索引）。
struct ChartPoint { double x = 0, y = 0; };

/// 系列（Line/Bar 共用）：name 进图例与值框；color 缺省按索引取内置色板（D8/D14）。
struct ChartSeries {
    std::string name;
    std::vector<double> values;
    std::optional<Color> color;
};

/// 散点系列（Scatter 专用，提升到本文件以免 Props 内嵌聚合类型）。
struct ScatterSeries {
    std::string name;
    std::vector<ChartPoint> points;
    std::optional<Color> color;
    float dot_radius = 4.0F;
};

/// 扇区（Pie 专用）。
struct PieSection { std::string name; double value; std::optional<Color> color; };

/// 轴（纯值，渲染/刻度/反查三用）。
struct ChartAxisSpec {
    bool visible = true;
    std::string label;                        // 轴标题（可选；非空时额外占留白）
    int tick_count = 5;                       // 期望刻度数（nice 化后可能 ±1）
    std::optional<double> min, max;           // 缺省 = 数据域 nice 化
    bool show_grid_lines = true;
    bool include_zero = true;                 // Bar 恒含 0 基线；Line/Scatter 可关
};

/// 图例。
enum class LegendPosition { Top, Bottom, Right };
struct ChartLegendSpec { bool visible = true; LegendPosition position = LegendPosition::Top; };

/// D3 式线性比例尺（纯值）：domain→px，含 nice 域与刻度生成、反查。
class LinearScale {
  public:
    static auto from_domain(double d0, double d1, int tick_count) -> LinearScale;
    [[nodiscard]] auto to_px(double v, float px0, float px1) const -> float;
    [[nodiscard]] auto invert(float px, float px0, float px1) const -> double;   // hover 命中反查
    [[nodiscard]] auto ticks() const -> std::vector<double>;
    [[nodiscard]] auto domain() const -> std::pair<double, double>;
};

/// D3 式带状比例尺（Bar 类目轴）：n 个类目等分带，取带中心。n == 0 时带宽 = 0（调用方须先判空）。
class BandScale {
  public:
    explicit BandScale(std::size_t n);
    [[nodiscard]] auto band_center_px(std::size_t i, float px0, float px1) const -> float;
    [[nodiscard]] auto band_width(float px0, float px1) const -> float;
    [[nodiscard]] auto index_at(float px, float px0, float px1) const -> std::size_t;  // 越界夹取
};

/// 内置色板（Material 风格 8 色，索引超界取模）。
[[nodiscard]] auto chart_palette(std::size_t index) -> Color;

/// 系列取色：显式 color > Theme 令牌 chart.palette.<i%8> > 内置色板（D14）。
[[nodiscard]] auto resolve_series_color(std::size_t index, const std::optional<Color> &explicit_color,
                                        const Theme &theme) -> Color;
```

**nice 刻度算法**：`step = nice_number(range / (tick_count - 1))`（nice_number ∈ {1,2,5}×10^k），domain 上下界向 step 对齐。`range == 0` 时退化为 `[0, 1]`（D15）。

**props_io 数组扩展**（本切片最大风险项，先行验证）：新增 `vector<double>` 与对象数组（`ChartSeries`/`ScatterSeries`/`PieSection`）编解码，`PropDescriptor.json_type` 取 `"array"`、`note` 标元素 schema；产物形如
`"series":[{"name":"...","values":[1,2,3],"color":[r,g,b,a]}, ...]`（`color` 未设时不输出，保留「按索引取色板」语义，同 `ProgressIndicator`）。同步 `itest_serialization` 真实键名用例 + `known_enums.h` 登记 `LegendPosition` + `itest_known_enums` 覆盖。

## 6. 五控件设计（切片 3–6）

### 6.1 通用形态（D10）

```cpp
struct BarChartProps {
    std::vector<ChartSeries> series;
    std::vector<std::string> categories;  // 缺省 "1","2",...
    bool stacked = false;                 // 见 §12.2
    float bar_width_ratio = 0.7F;         // 带内占比，夹取 (0,1]
    float bar_corner_radius = 2.0F;
    ChartAxisSpec axis_x, axis_y;         // x = Band（类目），y = Linear
    ChartLegendSpec legend;
    EdgeInsets padding{8, 8, 8, 8};       // 轴留白 + 值框避让区（D12）
};

class BarChart : public LeafWidget, public BarChartProps {
  public:
    BarChart() = default;
    explicit BarChart(BarChartProps props) : BarChartProps(std::move(props)) {}
    [[nodiscard]] static auto defaults() -> BarChartProps { return BarChartProps{}; }
    std::function<void(int series_idx, int point_idx)> on_point_tapped;  // 不进序列化面
    // ...
};
```

其余四图同构（`LineChartProps` / `PieChartProps` / `ScatterChartProps` / `SparklineProps`）。

### 6.2 各图属性要点

| 图 | 关键属性 | 绘制 |
|---|---------|------|
| BarChart | `series` / `categories` / `stacked` / `bar_width_ratio` / `bar_corner_radius` | `fill_rounded_rect`；y 域恒含 0 |
| LineChart | `series`（等距，x=索引）/ `show_dots` / `line_width` / 双 `Linear` 轴 | `stroke_polyline` + 圆点（`fill_rounded_rect` radius=尺寸/2） |
| Sparkline | `values` / `color` / `line_width` / `show_end_dot`；**无轴无网格无图例** | 同 Line，最薄 |
| PieChart | `sections` / `center_space_ratio`（>0 即 donut）/ `start_angle`（度，12 点起）/ `show_percentage_labels` | `fill_sector`（扇区间 2px 间隙用背景色描边）+ 可选 `stroke_arc` 外沿 |
| ScatterChart | `series`（`ScatterSeries`，显式 `ChartPoint`）/ 双 `Linear` 轴 / `dot_radius` | 圆点 |

轴/网格 = `draw_line` + `draw_text`；刻度标签经 `format_number(v, loc, digits)`（`digits` 由 nice step 推导：step ≥ 1 → 0 位，否则按 step 量级取位）。

### 6.3 布局（measure）

无固有自然尺寸：约束有界取 `c.constrain(c.max)`，无界（`Size::infinity()`）缺省 300×200。`padding` + 轴留白（由 `FontEngine::measure_width/measure_height` 实测最长刻度标签推导）在 `on_layout` 内一次性算完并缓存到成员，供绘制与命中同源使用。

### 6.4 命中测试

`LeafWidget` 默认命中整块 `bounds`，图表无需覆写 `on_hit_test`；命中索引由 `on_pointer_event` 在 `local_position` 上换算（§7.1）。

### 6.5 绘制分工与顺序

背景网格 → 数据（柱/线/点/扇）→ 轴与刻度 → 图例 → hover 叠层（十字准线 → 高亮环 → 值框）。hover 叠层最后画且**全部夹在 `bounds` 内**（D12）。

### 6.6 取色（D14）

网格线 = `theme.text` × alpha 0.12；轴标签/图例文本 = `theme.text`；值框背景 = `theme.background` × alpha 0.95 + 1px 描边；系列色走 `resolve_series_color`。`inherit_theme(ctx)` 未注入时回退 `Theme::light()`，永不崩溃。

### 6.7 数据健壮性降级表（D15）

| 输入 | 行为 |
|------|------|
| `series` 为空 / `values` 全空 | 只画轴与网格（或空态占位线），不绘制数据，`Diagnostics::degraded` 一次 |
| 所有值相等（range = 0） | domain 退化为 `[0, 1]`（或 `[v-1, v+1]`），不产生除零/NaN |
| 含 NaN / ±inf | 该点跳过（Line 断线、Bar 不画、Scatter 不画），`Diagnostics::degraded` 计数一次 |
| 负值 | 正常参与域计算；y 轴含 0 基线时零线位置由 domain 线性映射 |
| Bar `include_zero=true` 且全为负 | domain 上界强制 ≥ 0 |
| Pie `Σvalue <= 0` | 不绘制扇区，`Diagnostics::degraded` |
| 单数据点 | Band 带宽 = 绘图区宽（`n=1`）；Line 单点退化为一个圆点 |
| 点数 > `AURORA_CHART_MAX_POINTS`（默认 5000） | 截断到前 N 点 + `Diagnostics::degraded`（不引入抽样导致 golden 不稳定） |
| `bar_width_ratio <= 0 或 > 1` | 夹取到 `(0,1]`（`validate_prop` 的 `min_value/max_value` 同步登记） |

所有反序列化路径对畸形 JSON（元素非对象、`values` 非数组、`color` 长度不足）**逐项跳过**而非抛异常。

## 7. 交互设计（随切片 3–6 落基础，切片 7 统一增强）

### 7.1 事件接线（v2 新增，F6）

```cpp
// 每个图表控件均需：
[[nodiscard]] auto wants_click() const -> bool override { return true; }   // 消费 Press/Release，阻止冒泡
auto on_hover_change(bool entered) -> void override {
    hover_ = entered;
    if (!entered) { hover_index_ = std::nullopt; }
    mark_needs_paint();                                                    // 基类默认不标脏
}
auto on_pointer_event(MouseEvent &e) -> void override {
    // 基类 Move 分支只在按下时动作 → 必须自行处理悬停 Move
    if (e.action == MouseAction::Move) { update_hover(e.local_position); }
    if (e.action == MouseAction::Release && hover_index_) { /* 触发 on_point_tapped */ }
    e.is_handled = true;
}
```

- 坐标一律用 `e.local_position`；换算纯几何，可无头单测。
- 可选 `Modifier::cursor(CursorShape::PointingHand)` 提升可发现性（非必须）。

### 7.2 命中规则

- cartesian 图：`LinearScale::invert` / `BandScale::index_at` 反查最近索引；Scatter 按最近点欧氏距离（命中半径 = `dot_radius + 4dp`）；Pie 按指针角度/半径判扇区。
- 图例项区域命中优先于数据区（图例在图内自绘，无子 widget）。

### 7.3 视觉反馈

命中点放大高亮环 + **自绘悬浮值框**（`fill_rounded_rect` + `draw_text`，多系列纵向列出：系列色点 + 名称 + 值）；cartesian 图叠加**十字准线**（垂直参考线 `draw_line`，吸附最近数据 x）。图例 hover → 该系列高亮、其余系列 alpha 降至 0.35。

### 7.4 回调

`on_point_tapped(int series_idx, int point_idx)`（Bar/Line/Scatter）、`on_section_tapped(int section_idx)`（Pie）——`std::function` 旁挂，登记进 `describe_static().events`，不进序列化面。

### 7.5 越界约束（D12）

值框宽高由 `FontEngine::measure_width/measure_height` 实测；绘制前按可用区夹取，靠近右/下边界时翻转到反侧；**任何像素不得越出 `bounds`**。

## 8. 动画设计（切片 8）

每图内部持一个 `AnimationController`（时长缺省 450ms）：

- **驱动接入（D11）**：`on_mount` 取 `Animator::current()`；非空则 `drive(ctrl_)`，`~Xxx()` 中 `remove(ctrl_)`。
- 数据首次绑定或 `set_data()` 变更 → `forward()` 重放；进度 t 驱动：Bar = 柱高 0→1（ease-out）、Line = 描线进度（绘制到 t 对应折线前缀 + 端点插值）、Pie = 扇形角度按 t 展开、Sparkline = 同 Line。
- **降解（D11）**：`Animator::current() == nullptr`（`render_to_png`、未进 `run()` 循环）→ 进度恒 1，`Diagnostics::warn` 一次。golden 因此永远落在终态，零容差稳定。
- `reduce_motion` 经 `AnimationController::tick` 既有路径自动短路（直接 t=1），无障碍语义零额外代码。
- 动画期间 `can_cache_display_list()` 返回 `false`（同 `NavigatorHost` 策略），结束后恢复 `true`。
- 值框/十字准线为直接交互反馈，不参与动画。

## 9. 测试策略

1. **纯值单测**（`utest_chart_common`）：LinearScale nice 域/刻度/invert 往返、BandScale 带计算（含 n=0/1）、色板取模与 Theme 令牌覆盖、`resolve_series_color` 三级优先、Pie 归一化、§6.7 降级表逐条。
2. **每图单测**（`utest_bar_chart` 等 5 份）：构造不变量、`defaults()` 与 `describe_static()` 属性完备、序列化往返（`to_json/from_json/diff/apply_patch`，含 series 数组嵌套与畸形输入）、hover 命中几何（无头纯计算）、grow-in 进度函数值断言、`on_point_tapped` 触发（TestController `tap`）。
3. **像素 golden**：每图 1 张（`tests/golden/chart_*.png`）+ 原语级 2 张；零容差，锁软件 SSOT。**golden 路径无 Animator ⇒ 恒为终态**（§8）。
4. **AI fixture**：`tests/fixtures/ai_compat/` 新增，**沿用既有命名前缀**：`valid_chart_bar.json` / `valid_chart_line.json` / `interact_chart_point_tap.json` 等（现有目录为 `valid_*` / `error_*` / `interact_*` 三类，v1 的 `gen_chart_*` 与之不符）。
5. **守护联动**：`aurora_api_json` 刷新（`check_api_schema_sync`）、`check_code_doc_sync`（测试头「目标单元」路径）、全量 ctest 绿。
6. **默认构造冒烟**（v2 补 G9）：5 个控件全部登记进 `tests/integration/itest_default_construct.cpp`（`CODING_STANDARDS.md` §6.2 强制）。
7. **Animator 生命周期用例**：`utest_*.cpp` 内构造图表 → 注册 → 销毁 → 再 tick，断言无 UAF（ASan 下跑）。

## 10. 实现切片（顺序即依赖序）

| # | 切片 | 交付物 | 验收 |
|---|------|--------|------|
| 1 | Painter 原语 | `stroke_polyline`（真 SDF）/ `fill_sector` / `stroke_arc` 软件实现 + `CmdKind` 20 条 / `f3` / 点池 + `SoftwareRhi` 转发 + `GpuGlRhi` 实现 + 原语 golden 2 张 + `utest_painter_primitives` | 全量 ctest 绿、golden 零漂移 |
| 2 | 公共数据层 | `chart_common.h`（类型 + Scale + 色板 + `resolve_series_color`）+ props_io 数组编解码 + `known_enums` 登记 + `utest_chart_common` + `itest_serialization` 数组用例 | 序列化往返含嵌套数组绿；畸形输入不崩 |
| 3 | BarChart | 全链路首打通：控件头 + `serialization.cpp` 注册 + `aurora.h` 挂载 + demo + utest + golden + `itest_default_construct` 登记 + hover/值框基础 | 切片验收同 §9 |
| 4 | LineChart + Sparkline | 两控件全链路（Sparkline 无轴/图例，最薄） | 同上 |
| 5 | PieChart | 全链路 + 扇区命中 + 百分比标签 | 同上 |
| 6 | ScatterChart | 全链路 + 最近点命中 | 同上 |
| 7 | 交互统一增强 | 十字准线（cartesian 三图回填）+ 图例渲染与 hover 联动 + 值框越界夹取 + 交互 AI fixture | 同上 |
| 8 | grow-in 动画 | 新增 `Animator::current()` + 各图 `on_mount`/析构接线 + 数据变更重放 + reduce_motion 与「无 Animator 降级 t=1」utest + `can_cache_display_list` 联动 | 同上 |
| 9 | 文档回写 | spec 04 §3.8 图表控件清单 + spec 03 §5 Painter 原语 + CONCEPTS 控件映射（5 行）+ GUIDELINE 配方（图表 1–2 条）+ BUILD_OPTIONS（若有新开关）+ CHANGELOG alpha.5 条目 + ROADMAP 勾选 + 归档本文件 | `check_codespec_xref` / `check_code_doc_sync` 绿 |

每切片 = 编译 + 相关测试绿才进下一片；不与本阶梯多窗口遗留项（Wasm 事件路由）混做。

## 11. 风险清单

| 风险 | 缓解 |
|------|------|
| props 对象数组编解码无先例 | 切片 2 先行单独验证；退路 = series 序列化为子 JSON 树挂单一 string prop（AI 可读性等价，schema 校验内移） |
| `validate_ui_tree` 不校验数组元素 schema | `deserialize_props` 逐项防御性解析 + `Diagnostics::degraded`；元素 schema 校验留后续 JSON-Schema 阶梯 |
| `DrawCmd` 加 `f3`（+4B/命令）影响全命令族 | alpha 破坏性许可；录制/回放路径单一改动点，golden 守护 |
| polyline SDF 性能（逐像素 × 候选段） | 按线段 x 区间分桶；Sparkline/常观数据 ≤ 千点无压力；> 5000 点截断 + degraded |
| 半透明系列的 join 串珠 | 真 SDF 实现（D13）；golden 含 alpha=0.35 专项用例 |
| golden 零容差对 AA 边缘敏感 | 原语实现切片 1 一次定稿即锁基线；后续改动须显式重生成并评审 |
| GPU 与软件路径不逐位一致 | 既定策略（软件 golden 为 SSOT，GPU 容差框架属后续 GPU 阶梯）；本波 GPU 仅 smoke + 结构断言 |
| 图例/值框文本布局（多系列长名溢出） | 值框按最长系列名测量定宽 + 省略号；图例 Right 位超宽换行截断 |
| **图表自持 `AnimationController` 的 UAF**（v2 新增） | `on_mount` 注册 / 析构 `remove()`；切片 8 生命周期用例 + ASan |
| **值框越界残影**（v2 新增） | D12 硬约束 + `utest` 断言「绘制前后包围盒外像素不变」 |
| **`Animator::current()` 新增全局态**（v2 新增） | 对齐 `Scheduler::current()`（线程局部、`run()` 起止设置）；单测覆盖「无 App 时返回 nullptr」 |

## 12. 待实现期决策（不阻塞评审）

1. `GpuGlRhi` Sector 路线：包围盒 quad + fragment SDF（首选项）vs 三角扇几何 + 边缘 feather。
2. 堆叠柱状（`stacked`）：若分组几何实现后额外成本 > 1 天，降级为后续增量（属性先留位、`stacked=true` 时按分组渲染并 `Diagnostics::degraded`）。
3. `State<std::vector<ChartSeries>>` 响应式接线：若 `Reactive<T>`/`Binding<T>` 对聚合类型可直接实例化，切片 3 顺手接入并回填 D9；否则维持 `set_data()` + Props 赋值。
4. 图例命中区与值框的 z 序：直接叠加绘制（无子树抬升）是否够用；若遮挡数据则值框加边缘翻转避让（D12 已含夹取）。
5. `AURORA_CHART_MAX_POINTS` 是否暴露为 `BUILD_OPTIONS.md` 编译期开关（默认 5000，倾向保持编译期常量不新增开关）。
6. `Animator::current()` 是否同步给 `Scheduler` 之外的消费者复用（若复用面广，考虑统一为 `Application::current()` 门面）——倾向先只加 `Animator::current()`，最小改动。
