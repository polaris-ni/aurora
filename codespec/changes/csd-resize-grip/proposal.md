# CHG-076 无边框窗口的拖边缩放手势

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-076 |
| 提出日期 | 2026-11-14 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | 新增公共控件 `WindowResizeGrip`（新头 `window_resize_grip.h`）：单子容器，把「按下窗口边缘」翻译成 `WindowChrome::begin_resize(edge)`；`WindowChrome` 一侧零改动（它早已备好转发口，本批只是补上第一个调用方）；命中面新增八档边缘 / 角落判定与对应光标形状；`aurora_api.json` 与 `TitleBar` 的 `window_controls` 面零改动（本控件与 `NavigatorHost` / `Scaffold` 同类，只持有子树、不进序列化注册表，故 schema 的控件数不变）；影响判据面：新增 `utest_window_resize_grip`、`tests/golden/` 无变化（新控件自身不绘制任何像素）、`codespec/specification/04-widget.md` 的控件表与说明段、`tools/check/umbrella_manifest.txt` 的公共头分类、`CHANGELOG.md` 的 `Added` 条目。 |

## 动机

1. **`begin_resize` 全仓无调用方，无边框窗口因此只能拖不能缩。** `WindowChrome` 的五个动作里，`begin_move` / `minimize` / `toggle_maximize` / `set_fullscreen` / `close` 都由 `TitleBar` 消费；唯独 `begin_resize` 没有任何控件调用。用户把窗口设成 `Borderless` 后能看到自绘标题栏、能拖动、能点三钮，却抓不到边框改尺寸——这是 CSD 链路上唯一断掉的一环。

2. **这不是「框架该自动做」的事。** 边缘属于谁，取决于应用怎么排布自己的 chrome：有的应用整窗自绘、有的只在标题栏上方留一条、有的希望边缘带比平台默认更宽。把它塞进 `Window` 或 `Application` 会强制一种几何；做成控件则由应用声明一次即可，与 `TitleBar` 同一分工。

3. **手势必须在按下的派发栈内同步发起。** Wayland 的 `xdg_toplevel.resize` 要带 serial，过期即被合成器丢弃。因此不能「记录按下坐标、等 Move 再判断」，只能在 Press 事件里当场判定边档并调 `begin_resize`——这与 `TitleBar` 空白区发起移动是同一约束、同一写法。

4. **光标反馈不能靠单一形状。** 八个边档对应四种光标（`ResizeNS` / `ResizeEW` / `ResizeNWSE` / `ResizeNESW`），而 `Widget::cursor_shape()` 无位置参数。派发器的悬停光标解析对「链上没有形状变化的情况」不会重复下发，故不能靠 `Move` 里手工推光标（离开时会留下粘住的光标）。可行解法是让边档在**命中测试**阶段定档：命中测试发生在悬停光标解析之前，档位一变、`cursor_shape()` 的返回值随之变，派发器自然下发新形状。

## 变更内容

### 1 新控件 `WindowResizeGrip`

`include/aurora/widget/` 下新增 `window_resize_grip.h`，`class WindowResizeGrip : public Widget`：

- 构造 `WindowResizeGrip(Node child, float band = AURORA_WINDOW_RESIZE_BAND)`；`set_band(float)` 链式可改，`band()` 读回。`band` 是边缘带厚度（dp），默认 6——Windows 10/11 的不可见缩放边框约 4–8 dp、GNOME 的 `gtk-window-decoration` 取 6，取中位数并写进注释。
- 单子容器：`describe().children_policy == "single"`，`for_each_child` 只交出 `child`；`type_name()` 返回 `"WindowResizeGrip"`。
- 布局：`on_layout` 以满约束布局子件，自身尺寸取 `c.constrain(c.max)`（与 `NavigatorHost` 同口径，不向父级多要空间）。子件布局父节点登记与 `NavigatorHost` 一致。
- 绘制：不画任何像素，`on_paint` 只委托子件；`can_cache_display_list()` 返回 true（本控件自身无绘制，缓存与否只影响子件）。
- 属性面只有 `band` 一行（`float`，默认 `6.0`，`min_value = "0"`）；无枚举型属性，故不进 `known_enums.h`。控件**不注册**进 `WidgetRegistry`（与 `NavigatorHost` / `TransitionLayer` / `Scaffold` 同口径：构造需要子树，无法从静态 JSON 重建），故 `aurora_api.json` 的控件数与属性条目零变化。

### 2 边档几何与命中

- 角落优先：四角各取边长 `2 × band` 的正方形（默认 12 dp），落在角内优先判对角档（`TopLeft` / `TopRight` / `BottomLeft` / `BottomRight`），其余边缘带按 `band` 厚度判 `Top` / `Bottom` / `Left` / `Right`。角落放大到两倍带宽是为了让斜向缩放可命中——角上只有 `band × band` 时指针必须落在 6×6 的格子里，实测过小。
- 窗口过窄时收缩：`band = clamp(band, 0, min(w, h) / 4)`，避免小窗被边缘带吃满、内容无法点击；`band <= 0` 时整条退出（不命中、不发起缩放），等价于关闭该手势。角落边长同样取 `min(2 × band, min(w, h) / 2)`。
- 命中：`on_hit_test_chain` 在带内只返回自身（不下降），带外交给子件的链（前置自身节点）。这样带内按下不会漏给内容（否则拖动边缘会误触内容里的按钮），带外一切照旧。
- 最大化 / 全屏下不发起缩放：`env->get<WindowMode>()` 非 `Normal` 时边缘带不命中（与 `TitleBar` 读同一个环境值），避免最大化窗口上出现「拖边没反应却能挡住内容点击」的死带。

### 3 手势与光标

- 按下（左键）且命中边档：`env->get<WindowChrome>()->begin_resize(edge)`，并置 `is_handled`。`WindowChrome` 未注入时同样置 `is_handled`（与本仓 `TitleBar` 在无头环境下的既有口径一致：命中分支执行、不做动作），保证行为可预测。
- 只响应 `Press`，不处理 Move / Release：缩放由 OS 接管拖动循环，随后的 Move / Release 落在谁的命中链上由平台决定（X11 走 `_NET_WM_MOVERESIZE`、Win32 走 `SC_SIZE` 模态循环），框架侧不追。
- 光标：命中测试阶段把当前边档写进成员，`cursor_shape()` 按档返回 `ResizeNS` / `ResizeEW` / `ResizeNWSE` / `ResizeNESW`；不在带内返回 `nullopt`，把决定权交还给命中链更深处（内容控件）与祖先。命中测试先于悬停光标解析发生，故指针沿边框移动时形状即刻跟随，不存在「改档后要等下一个 Move」的滞后。

### 4 与既有 chrome 链的关系

- 不改 `WindowChrome`、不改 `Surface`、不改任何后端：`Win32` / `X11` / `Wayland` / `GLFW` / `macOS` 的 `begin_window_resize` 早已各自实现（D3D11 与 wgpu 面按既有缺口清单不转发，本批不擅自补）。
- 不改 `TitleBar`：它与本控件可叠用（典型形态是 `WindowResizeGrip` 包住整列内容，`TitleBar` 在其内），互不侵占——`TitleBar` 的命中盒在内容区内，边缘带在内容区外。
- 不新增开关：不提供「只允许横向缩放」一类掩码。当前没有提出该需求的消费方，`WindowResizeEdge` 已能表达八档；若后续需要，属单纯扩属性行。

## 验收判据

1. 八档边档与四档光标逐档命中：在 200×120 的宿主上依次把指针置于四边中线与四角（角落按 `2 × band` 判定），命中的边档与 `cursor_shape()` 返回值逐档对上；同一指针位置在 `band = 0` 时全部落到内容（不得命中控件自身）。
2. 带内按下发起且仅发起一次缩放：注入 `Press(Left)` 于左边缘，`begin_resize` 被调一次且参数为 `Left`；同一位置注入 `Press(Right)` 与 `Release` 不发起；带外的 `Press(Left)` 落到内容控件而非本控件（用一个记录自身被按下的探针叶控件验证）。
3. 角落优先于边缘：指针落在左上角 `2 × band` 区域内时档位是 `TopLeft` 而非 `Top` / `Left`；把 `band` 调大到 `min(w, h) / 4` 的钳制值时，角落边长仍不超过 `min(w, h) / 2`，且边缘带不覆盖中心（中心区域仍能命中内容）。
4. 最大化不发起：环境注入 `WindowMode::Maximized` 时四边与四角的 `Press` 均落到内容，`begin_resize` 调用次数为 0；切回 `Normal` 后同一坐标恢复命中（证明判据读的是环境值而不是一次性缓存）。
5. 光标无粘滞：把指针从左边缘移到中心（同一条命中链内的不同档），再移出控件，`cursor_log` 依次出现 `ResizeEW` 与 `Arrow`（无头表面记录序列），不得出现「离开后仍停在 ResizeEW」。该条同时钉住「命中测试定档」这一实现选择，改回「Move 里推光标」会转红。
6. 子件行为不变：子件（探针叶控件）的布局尺寸与不带本控件时逐值相同，绘制像素逐像素相同（本控件不绘制、不吃掉像素）；`can_cache_display_list()` 为 true 且不影响子件的缓存判定。
7. 判据非空转须真注入变异：① 把角落判定挪到边缘判定之后 ⇒ 判据 1 的角落档与判据 3 同时转红；② 令 `cursor_shape()` 恒返回 `ResizeEW` ⇒ 判据 1 与 5 转红；③ 把 `Press` 分支改成不调 `begin_resize` ⇒ 判据 2 与 4 转红；④ 去掉 `band` 的「最短边 / 4」钳制 ⇒ 窄窗夹具（离上边 12dp 应落回内容）转红。每处记录「改了什么、哪条转红、还原后是否回绿」。

    - 判据 7 变异自证记录（逐一真注入，还原即回绿）：四处都改 `include/aurora/widget/window_resize_grip.h`，本控件是纯头实现，故用独立探针（自建 `HeadlessSurface` 子类记录 `begin_window_resize`）逐轮编译验证，不走整包重建。
      - ① 把四角判定四段移到四条边判定之后：右上角 (198,2) 由 `TopRight` 变成 `Top`，探针报 `cursor at (198,2) mismatch` 与 `begin_resize at (198,2) not issued`；还原后 PASS。
      - ② `cursor_shape()` 开头插 `return CursorShape::ResizeEW;`：右上角光标不再是对角线，探针报 `cursor at (198,2) mismatch`；还原后 PASS。
      - ③ 把 `chrome->begin_resize(edge)` 换成 `(void)edge;`：两处按下都不再发起缩放，探针报两条 `begin_resize ... not issued`；还原后 PASS。
      - ④ 把 `band` 的 `std::clamp(band_, 0.0F, min_side * 0.25F)` 换成 `std::max(band_, 0.0F)`：200×40 窗口上 `band = 100` 不再收缩，离上边 12dp 的点被当成边缘，探针报 `oversized band is not clamped`；还原后 PASS。

## 回写落点

- `codespec/specification/04-widget.md`：容器族新增 `WindowResizeGrip` 一段（单子容器、边缘带几何、四档光标、按下一发即交 OS），属性表补 `band` 一行。
- 后端侧的 CSD 口径不动：`codespec/specification/06-app-platform.md` 记的是各后端的 `begin_window_resize` 实现与能力位，不记控件消费方，本批无改动。
- `codespec/specification/05-event-navigation.md`：§3.2 自入链契约补「边缘带内只交自身、不下降」这一形态；悬停光标解析段补「命中测试阶段定档」的用法。
- `codespec/SPECIFICATIONS.md`：`SPEC.FEAT.APP.MULTI-WINDOW.001` 的「能力」列补无边框窗口拖边缩放，「落点」列补 `codespec/specification/04-widget.md` 的对应锚点；特性行总数不变。
- 根目录 CHANGELOG.md：`Added` 记新增控件与 `band` 属性。

## 实施记录

- 落码：`include/aurora/widget/window_resize_grip.h`（控件本体）、伞头与 `tools/check/umbrella_manifest.txt` 各补一行、伞头 `include/aurora/aurora.h` 的分类不变。
- 用例：`utest_window_resize_grip` 七例（八档边档与四档光标、带内按下发起且带外透传、`band = 0` 关闭、非 `Normal` 窗口模式不命中并可恢复、光标无粘滞、窄窗钳制、自描述）。
- 文档：`codespec/specification/04-widget.md` §3.6 控件表与说明段、`codespec/specification/05-event-navigation.md` §3.2 的「边缘带只交自身」与「按位置变化的光标档」两段、`codespec/SPECIFICATIONS.md` 的 `SPEC.FEAT.APP.MULTI-WINDOW.001` 能力列、根目录 CHANGELOG 的 `Added`。
- 未做：D3D11 与 wgpu 两个面仍不转发 `begin_window_resize`（既有缺口，不在本批）；滚动条式的「边框可视化」不做——边缘带是纯命中区，不绘制任何像素。
