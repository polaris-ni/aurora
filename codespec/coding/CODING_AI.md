# CODING_AI

> 本文件由 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md) 划分而出（AI 友好性（编码类条目））。章节编号保持原样。
> 返回主线见 [`CODING_STANDARDS.md`](../CODING_STANDARDS.md)。

**本文包含章节：**

- [7. AI 友好性（编码类条目）](#7-ai-友好性编码类条目)

## 7. AI 友好性（编码类条目）

> 本库面向 AI 编码助手友好。以下为 **编码规则层** 的全部评估条目。
> 架构/设计理念层条目见 `architecture/ARCHITECTURE_AI.md` §12。

### 〇.3 部分生成容错 
当 AI 生成的代码片段（单文件/单函数/局部片段）单独编译/运行时，应「部分生成也能容错」： 缺失的前置声明、类型别名、include
不应导致不可理解的错误。 库须保证「复制任一 widget 的构造片段即可独立编译」（`Error.suggestion` 直接给出缺失件）。

### 一.3 命名词序 
属性/参数名序与主流框架（React/Flutter）对齐，降低迁移成本：回调用 `on_xxx`
（`on_click` 而非 `click_handler`），子节点用 `children`，尺寸用 `width/height`。避免自创词序。

### 一.4 默认参数 
高频构造提供默认参数/便捷工厂（`au::colors::Blue`、`dp(16)`），降低记忆负担。 已审计全部 ~55 个控件类型，为 7 个缺失默认构造器的控件补充了
`= default` 或自定义默认构造。 所有 `XxxProps` 结构体字段均已有合理默认值。编译期验证（`static_assert`）集中在
`test_default_construct.h`，运行时验证在 `test_default_construct.cpp`（`#include "test_default_construct.h"`）。

**同步流程（强制执行）**：

- 新增控件类时， **必须**提供默认构造器（`Xxx() = default` 或自定义默认构造）
- 新增 `XxxProps` 聚合类型时， **必须**确保所有字段都有合理默认值
- 新增公共构造函数时， **必须**评估是否可为高频参数提供默认值
- `tests/test_default_construct.h` 中的 `static_assert` 列表 **必须**随新增控件同步更新
- 新增控件时， **必须**在 `tests/test_default_construct.cpp` 中 `#include "test_default_construct.h"` 以确保编译期校验生效

### 一.5 顺序容错 
多参数构造优先用具名聚合类型（`ColumnProps{.children=...}`），避免位置顺序错误；
`initializer_list<Node>` 用于多子容器，`(T, W&&)` 用于单子容器，降低传参歧义。

### 一.6 用户字面量（`au::literals`）

提供用户字面量（`100_dp`、`16_ms`、`0xRRGGBB_rgb`）与强类型工厂，使尺寸/时间/颜色有 编译期单位校验；字面量与 `px(100)`/
`dp(16)` 工厂互补而非重复。 **禁止头文件全局 `using`，仅 TU 内 `using namespace au::literals`**。

### 一.7 所有权清晰 
资源所有权用 `unique_ptr`/`shared_ptr` 明确；跨边界传递用 `std::move`； 引用型（如 `Binding<T>`）为非拥有，文档标注「上游生命周期须更长」。

### 一.9 元编程边界（审计确认合规）

模板/宏等元编程仅用于「无法用普通函数表达」之处（如 `State/Signal/Modifier`
的声明式组合）；禁止为「炫技」引入深层模板，避免 AI 难以展开实例化错误。

控件样板减负同样遵循此原则：基类以 **非模板虚函数默认实现**消除重复——
`Widget::describe()` 默认返回 `{ .name = type_name() }`、`Container::collect_signals()`
默认遍历 `m_children` 收集子节点信号，新增控件只需实现 `type_name()`(+`describe_static()`)。 **禁止为减负引入 CRTP /
模板基类**：那会把实现搬进头文件，破坏声明 / 实现分离， 并增大 AI 理解单函数所需加载的上下文（与「头文件只放声明、实现下沉
`.cpp`」直接冲突）。

**审计结论**：

- 6 处 concept/requires 约束（全部为简单 `derived_from`/`convertible_to`/`is_arithmetic_v`）
- 4 处 `if constexpr`（全部为简单 void/非void 分支）
- 0 处 SFINAE/enable_if、0 处 CRTP/模板基类
- 结论：所有模板使用均属于「无法用普通函数表达」的场景，完全合规，无需代码改动

**同步流程（强制执行）**：

- 新增模板/概念/metaprogramming 代码时， **必须**确认属于「无法用普通函数表达」的场景
- 禁止引入 CRTP、模板基类、深层模板实例化（>2 层嵌套）， **禁止在头文件中引入 `std::mutex`/`std::shared_mutex`/
  `std::lock_guard`**
- 如确需引入， **必须**在代码注释中说明为何无法用普通函数替代

### 一.10 二层属性划分（固有属性 + 正交 Modifier）
控件可配置性由「固有属性（`XxxProps` 字段）」与「正交修饰（`Modifier` 链）」两层构成，概念边界清晰：

- **固有属性层**：描述控件 **自身**视觉/行为身份（如 `Text::text_align`、`Button::corner_radius`、
  `Slider::active_color`），以 `XxxProps` 的 snake_case 字段暴露，随控件序列化 （`serializeProps`/`deserializeProps`），是
  Inspector / `aurora_api.json` 可枚举的一等属性。
- **正交修饰层**：跨切面、可叠加、可 `Reactive` 变化的通用装饰，挂在每个 `Widget::modifier`
  （`Padding` `Background` `Border` `Clip` `Opacity` `Align` `Offset` `SizeModifier` `FlexWeight`
  `Clickable` `draggable` `long_press` `rotate` `scale` `transform`），作用于 **任意**控件。
- **重叠规则**：当某能力在「控件固有属性」与「`Modifier` 同类项」两者都存在（如 `padding`/
  `corner_radius`/`background_color`）， **优先控件固有属性**（随控件序列化、可检视）；`Modifier` 同类项
  保留用于「给任意控件套一层」的跨切面场景，绘制时 `Modifier` 在外、固有属性在内，可叠加。
- **为何保留 `Modifier`（AI 友好）**：装饰能力收敛于 **单个 `Modifier` 类型**（约 20 个方法 +
  `Kind` 枚举可枚举），远少于纯包裹控件模型所需的 ~15 个独立 widget 类型（`Padding`/`Container`/
  `DecoratedBox`/`GestureDetector`/`Align`/`Opacity`/`Transform`/`ClipRRect`/`SizedBox`/`Expanded`…）； 扁平 `.modifier`
  链比深层 `Container(GestureDetector(Opacity(...)))` 嵌套更不易生成错位； 且与现有 **扁平** `aurora_api.json` / `diff`/
  `apply_patch` 序列化模型天然契合。结论：不将 `Modifier`
  重构为包裹控件，维持「固有属性 + 正交 `Modifier`」的清晰二层划分（详见 `concepts/CONCEPTS_CORE.md`（核心概念审计：可枚举 UI 原语 #1–#19）、
  `specification/subsystems_api/SUBSYSTEM_API_LAYOUT_ENGINE.md`（§H.13b 布局引擎））。

### 一.11 外观变更只标绘制（不重排）
控件在运行时变更应区分「是否影响布局几何」并调用对应的标脏原语，以配合脏区域追踪（默认开启、
`Window::present_root` 的 layout/paint 分离，见 `architecture/ARCHITECTURE_RUNTIME.md` §5.1（事件驱动帧循环））：

- **仅影响外观、不影响几何**的变更（文本选区高亮、颜色/主题切换、光标位置、滚动偏移等） **必须**调用 `mark_needs_paint()`
  （仅置「绘制脏」），让拖选/主题切换等帧 **跳过整树 `layout`**， 在大窗/最大化场景下收益随面积放大最显著。
- **影响布局几何**的变更（尺寸/约束/子节点增删、结构性 `Modifier` 如 `rotate`/`scale` 重算包围盒） 调用
  `mark_needs_layout()`（置「布局脏 + 绘制脏」），下一帧 `layout + paint`。
- `Widget::on_dirty` 为 `std::function<void(bool)>`（`true`=含布局脏），经 `Window::wire_dirty` 递归接线整棵树； 子控件的
  `State`/`Modifier` 变更通过响应式 `Effect` 自动落到上述两个原语，无需手工标脏。
- 反模式：外观变更误调 `mark_needs_layout()` 会迫使每帧重排，抵消脏追踪的性能收益（拖选卡顿的根因之一）。

### 一.12 控件可定制性契约（主题回退 + 状态反馈 + protected 绘制钩子）
新增 / 改造交互控件必须遵守四条契约（完整定义见 `specification/subsystems_api/SUBSYSTEM_API_WIDGETS.md`（§H.14.1 控件可定制性），配方见 `guideline/GUIDELINE_INTEGRATION.md`（§20 控件样式定制与继承））：

1. **强调色主题回退**：`active_color` 类属性用 `std::optional<Color>`，未设置时绘制期经
   `inherit_theme(ctx).primary` 解析，且未设置不序列化（保留意图）。
2. **状态色统一派生**：hover/pressed 用 `Color::shaded(k)` 乘性调暗，淡色底/选区用 `Color::with_alpha(a)`；
   不手写逐通道乘法。所有交互控件提供 `set_enabled(bool)`：禁用态灰化、吞事件不冒泡、不改值。
3. **绘制分阶段**：`on_paint` 分解为 protected 虚钩子（`paint_background`/`paint_track`/`paint_thumb`/
   `paint_option`/`paint_item`/... · 状态色经 `resolve_*`）；控件成员用 protected 而非 private， 使子类可单点覆盖某阶段而无需重写整个
   `on_paint`。
4. **常量属性化**：行高/盒高/字号/圆角等尺寸不得硬编码，升级为可序列化属性；`corner_radius < 0`
   统一表示「自动」；影响几何的 setter 调 `mark_needs_layout()`，仅外观的调 `mark_needs_paint()`（见 一.11）。

### 三.1 错误可机读 
`Error` 提供 `to_json()`；结构含 `code/message/suggestion/docs/where`，AI 可解析并给出修复。

### 三.2 快照可比对 
渲染输出确定性（相同输入 → 相同像素）；`HeadlessSurface` 可导出 PNG 供 golden test 比对。

### 三.3 降级而非中止 
非法输入/缺失类型产出 `Diagnostics` 并降级到安全默认；
`from_json` 含不可重建控件（`Repeater`/`Canvas`）时返回预期错误而非崩溃。

### 三.4 可观测 
所有输出经 `Logger` 双通道收口，且 `Diagnostics` 汇总「做了什么降级」，使 AI 能直接读取库的运行时行为与降级决策（而非从 stdout 噪声中猜测）。通道划分、前缀格式、输出纪律（禁直接用 `std::cout`/`printf` 等）与 `set_sink`/`set_raw_sink` 重定向见 [`CODING_ERRORS_NAMING.md`](./CODING_ERRORS_NAMING.md) §3.6。

### 三.5 增量编译友好 
头文件只放声明，实现下沉 `src/aurora/*.cpp`；减少 TU 重编范围（见 `architecture/ARCHITECTURE_RUNTIME.md` §4 模块映射）。

**平台后端头必须 pimpl 隔离（硬规则，0.4.0 起全量落地）**：任何依赖平台 SDK 的 `Surface` / 窗口宿主类，
其**公共头不得包含平台重型头**（`<windows.h>`、`<windowsx.h>`、`<GLFW/glfw3.h>`、`<GL/gl.h>`、`<X11/Xlib.h>`、
`wayland-client.h`、AppKit 等），一律以 `std::unique_ptr<Impl> m_pimpl` 形式把细节收进对应 `.cpp`。

- **动机**：① 头依赖收敛——消费者 TU 不因「某后端被开启」而被拉入数万行平台头；② 宏污染隔离——
  `<windows.h>` 的 `min`/`max`/`ERROR`、Xlib 的 `None`/`Bool`/`Status` 不再泄漏到用户命名空间；
  ③ 增量编译——改后端实现只重编 1 个 TU。
- **句柄暴露**：确需向外暴露原生句柄时，返回 **`void*`** 而非平台类型（如 `hwnd() -> void*`），
  调用方在自身已含平台头的 TU 内 `static_cast<HWND>(...)` 还原。此类访问器属「平台逃生舱」，
  其静态类型不计入 API 稳定性承诺。
- **回调归属**：平台 C 回调（GLFW callback、Win32 `WNDPROC`）声明为 `Impl` 的**静态成员**而非文件级自由函数，
  以便直接访问私有 `Impl`；用户指针（`glfwSetWindowUserPointer` / `GWLP_USERDATA`）存 `Impl*`。
- **现状**：`Win32Window`、`GlfwSurface`、`X11Surface`、`WaylandSurface` 均已合规；新增后端须遵循同一形态。

**SIMD 双实现同步修改（硬规则，0.4.2 起）**：凡引入「标量黄金 + SIMD 快路径」双实现的渲染函数（当前：`gradient_*_scanline_*` / `gradient_*_fill`，位于 `aurora::detail`），两条路径必须保持**逐位一致**，且修改任一实现时**必须同步另一份并跑 `test_simd_parity` 全量比对**：

- **动机**：① 双实现随时间演化漂移会产生「SIMD 开启/关闭给出不同像素」的静默正确性 bug；② parity 测试覆盖不到的边界输入（alpha=0/255、非 8 倍数宽、负坐标、裁剪边界）是漂移高发区。
- **约束**：SIMD 路径须沿用标量浮点运算序列（同序、`-ffp-contract=off` 禁 FMA），整型截断统一用 `cvtt`（`_mm_cvttps_epi32` / `_mm256_cvttps_epi32`），sRGB 转换沿用同一 LUT；禁止为「提速」引入标量未做的近似或重排。
- **验收**：每次改动后 `test_simd_parity` 必须 0 failure（G-13 一票否决）；若某函数无法在保持逐位一致前提下向量化，**停下来单独提出，不擅自放宽**。
- **开关**：`AURORA_ENABLE_SIMD` 默认 ON，OFF 时仅编译标量路径（详见 `BUILD_OPTIONS.md` §3.2）；双实现均属 `aurora::detail` 内部，不计入 `aurora_api.json`，新增前须先评估是否值得暴露为公共 API。

### 三.6 错误定位 
错误信息精确到 `文件:行`（`Error::where`）；`AURORA_ASSERT` 附上下文。

### 三.7 局部修复建议 
`Error.suggestion` 直接给出「改哪一行 / 加哪个 include」级别的可执行建议。

### 三.8 无障碍 lint 
不提供对比度/标签缺失/焦点可达性等无障碍 lint（用户明确移出范围）。

### 四.1 Schema 强制 
公共 API 的入参/出参有 Schema 校验；`gen_api_tools` 输出 `aurora_api.json` 即机器可读 Schema。

### 四.2 代码补全 / LSP 
提供 LSP/补全（`aurora-lsp`），消费库 live API（`describe_component` + `known_enums`）对 `au::XxxProps{ .prop = ... }`
等声明式写法做 completion / hover / diagnostics / codeAction。详见 `specification/subsystems_api/SUBSYSTEM_API_TOOLING.md`（§H.17 LSP / §H.19 MCP）。

### 四.3 MCP / CLI stdio JSON-RPC 2.0 MCP Server（`aurora_mcp`，8 个 tools）+ CLI 工具（`aurora_cli`，8 个子命令）， 消费 describe ()
/list_all_schemas ()/validate ()/render_to_png ()/to_code () 等库 API。 LSP 已落地。详见 `specification/subsystems_api/SUBSYSTEM_API_TOOLING.md`
（§H.17 LSP / §H.19 MCP）。

### 四.4 序列化 diff 
`to_json/from_json/diff/apply_patch` 提供树级 diff，便于 AI 推断「改了什么」。

### 四.5 示例文档化 
每个 widget 的最小可编译示例随 API 文档发布（`examples/` + `GUIDELINE.md`）。

### 四.6 零平台魔法 
示例不依赖特定平台事件循环；`HeadlessSurface` 可离线渲染 PNG。

### 四.7 版本稳定 
次要版本只增不删（见第 8 章「版本与变更管理」）；破坏性变更进主版本并写迁移指南。

### 四.8 Playground / REPL 
不提供在线 Playground/REPL（用户明确移出范围）。

### 四.9 组件发现 每个控件实现 `static describe_static()`（注册表 / 工具链消费的完整 `WidgetDescriptor`，含属性 / 事件 / 子节点策略 /
示例）与必写的 `type_name()`；`describe()` 现由 `Widget` 提供默认实现（无富描述控件可省略 override），仅当需要额外
properties/events/children_policy 时才覆写；
`component_schema()` 消费 describe () 输出并增强 `prop_descriptors`/`events`/`children_policy`/`examples` 字段；
`list_all_schemas()` 批量返回全部已注册组件 schema；`aurora_api.json` 自动包含新字段。详见 `specification/subsystems_api/SUBSYSTEM_API_WIDGETS.md`（§H.16 组件发现）。

### 五. 标注与契约表达 
纯函数/非纯、线程安全、是否可重建等契约用文档注释显式标注；
`serialization` 重建约束已在 `include/aurora/widget/serialization.h` 注明。 已为全部 ~80 个公共头文件添加统一契约标注（Thread/Side-effects/Rebuildable），
覆盖 core/、widget/、state/、event/、window/、app/、layout/、animation/、i18n/、 environment/、theming/、inspector/、media/ 全部子系统。
标注格式标准：`@note Thread: ...`、`@note Side-effects: ...`、`@note Rebuildable: ...`。

**同步流程（强制执行）**：

- 新增公共类/结构体时， **必须**在类级别 Doxygen 注释中添加契约标注：
    - `@note Thread: main-thread only` / `thread-safe` / `thread-safe with mutex` / `thread-safe with rwlock`
    - `@note Side-effects: pure` / `none` / `paints` / `mutates layout`
    - `@note Rebuildable: yes, via from_json` / `no`
- 修改现有类的线程安全语义时（如新增 mutex、改变回调线程）， **必须**同步更新 `@note Thread:` 标注
- 修改现有类的副作用行为时（如新增 I/O、网络请求）， **必须**同步更新 `@note Side-effects:` 标注
- 修改序列化支持时（新增/移除 from_json 支持）， **必须**同步更新 `@note Rebuildable:` 标注
- Code Review 中应将契约标注一致性作为审查项
- 读写锁（`std::shared_mutex`）仅用于读多写少场景（如缓存表）， **禁止在控件/Props 等主线程对象中使用**；使用 `shared_mutex`
  的类 **必须**标注 `@note Thread: thread-safe with rwlock` 并在注释中说明为何选择 rwlock 而非普通 mutex

### 六. 评分方法 
以「AI 在有限上下文窗口内能否一次生成可编译、可运行代码」为验收标准。

### 附录 A. 清单 
本文件（第 7 章「AI 友好性」）即采纳清单。

### 附录 B. 变更记录 
标准变更随版本在 `SPECIFICATIONS.md` / `CHANGELOG.json` 维护。

---

