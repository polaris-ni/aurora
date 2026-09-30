# 事件、手势、动画与导航（event / animation / navigation）

> 覆盖 `include/aurora/event/`（6 个头）、`animation/`（4 个头）、`navigation/`（6 个头）。
> 本文件是事件模型、命中测试、焦点、手势、动画与页面栈的**唯一权威**。
> 命中链节点的生命周期契约见 [`04-widget.md`](04-widget.md) §6.1；帧循环与指针捕获见 [`06-app-platform.md`](06-app-platform.md)。

---

## 1 模块范围

| 关注点 | 头文件 |
|:---|:---|
| 事件定义 | `event/event.h`、`event/keycode.h` |
| 派发器 | `event/dispatcher.h` |
| 焦点 | `event/focus.h` |
| 手势 | `event/gesture.h` |
| 拖拽 | `event/drag_drop.h` |
| 动画 | `animation/animator.h`、`animation/easing.h`、`animation/spring.h`、`animation/timeline.h` |
| 导航 | `navigation/navigator.h`、`navigation/route.h`、`navigation/router.h`、`navigation/hero.h`、`navigation/transition_layer.h`、`navigation/navigator_host.h` |

---

## 2 事件模型

### 2.1 基类

```cpp
struct Event {
    bool is_handled = false;   // 是否已被消费（停止冒泡）
};
```

所有事件携带 `is_handled` 标志。组件处理方法接收**非 const** 事件引用，可写 `e.is_handled = true` 消费事件，阻止继续向上冒泡。

### 2.2 具体事件

| 事件 | 字段 | 位置 |
|:---|:---|:---|
| `MouseEvent` | `position`（全局窗口逻辑坐标，由 Surface 后端写入）、`local_position`（相对当前控件的本地坐标，由 `EventDispatcher` 写入）、`action`、`button` | `event.h` |
| `KeyEvent` | `key`、`action`（`KeyAction::Down` / `Up`）、`modifiers` | `event.h` |
| `ScrollEvent` | `position`、`delta_x`（右为正）、`delta_y`（**上为正**）、`remaining_y`（消费后未用尽的垂直余量，与 `delta_y` 同单位同号，默认 0） | `event.h` |
| `TextInputEvent` | `text`（UTF-8 文本片段） | `event.h` |
| `TextCompositionEvent` | `preedit`（UTF-8 预编辑串，空 = 组合结束/取消）、`cursor_index`（组合光标，**preedit 内码点下标**）、`sel_start` / `sel_end`（待转换选区，含尾；无选区时 `sel_end == AURORA_NO_SELECTION`）、`committed`（本次上屏文本，UTF-8） | `event.h` |
| `FileDropEvent` | `position` 与拖放文件信息 | `event.h` |
| `TouchEvent` | `TouchPoint{id, position, prev_position, is_active}` 集合 | `event.h` |

枚举：`MouseButton{Left, Right, Middle}`、`MouseAction`（`event.h`）、`KeyAction{Down, Up}`、`ModifierKey`（`event.h）、`KeyCode`（`keycode.h`）。

**修饰键语义（`KeyEvent::modifiers`）= 队列相对，而非派发时刻的物理读数**：宿主按键盘消息流推进一份修饰位状态（Win32 侧为 `src/aurora/window/detail/win32_modifiers.h` 的 `ModifierKeyTracker`），`KeyEvent` 携带的是「该键在队列里被处理的那一刻」的修饰态。取此口径的理由：在派发时刻异步采样（`GetAsyncKeyState`）会让热键命中与否取决于「消息被泵到之前修饰键是否仍按着」，于是 UI 卡顿或人手 chord 短于一帧（约 21–30 ms）时按键**静默**丢失（同载体同形态实测 0/10，见 `manual-test/21-debug.md` TC-DEBUG-004 备注）。三条配套边界：

- **左右归并**：`VK_LSHIFT` / `VK_RSHIFT` / `VK_SHIFT` 折到同一位，使不配对的变体（注入或布局按下给 L 变体、抬起给通用码）不残留幻影；代价是同位上任一变体抬起即清位。
- **失焦清空、激活播种**：`WM_ACTIVATE` 转非激活与 `WM_KILLFOCUS` 整体清零（未送达的抬起消息不可追），重新取得激活时用一次异步读数播种基线（覆盖「Alt+Tab 切进来时 Alt 已按下」）。因此修饰键**只在持有键盘焦点的那个窗口内计数**——非前台窗口上的物理 Ctrl 不再被别的窗口借读，这恰是 Windows 自身的键盘语义。
- **`WM_SYSKEY*` 只跟态、不派发**：按住 Alt 期间的按键仍交 `DefWindowProc`（`Alt+F4`、菜单助记键归系统），但 Alt 自身的按下/抬起计入跟踪器，否则 Alt 松开后的普通按键会错报带 Alt。副作用是 Alt 组合在库侧仍不派发（既有边界）。

指针类事件（`MouseEvent` / `ScrollEvent`）不携带修饰位。

**滚动方向约定**：`ScrollEvent::delta_y` 正方向为「向上滚动」（应露出上方内容、offset 减小）。所有滚动控件统一用 `offset_ - e.delta_y * step`；误用 `+` 会导致方向相反。

**滚动增量单位**：`delta_y` 是**设备无关增量**（滚轮格数口径），不是 dp；控件按自己的 `step`（dp/增量单位）换算位移。回传余量 `remaining_y` **与 `delta_y` 同单位同号**（未吃尽的增量数），派发器把它原样作为下一跳的 `delta_y`，故跨控件嵌套时各层按自己的 `step` 折算——内层 `step=16`、外层 `step=1` 也不会串味。

### 2.3 坐标契约

`on_pointer_event(MouseEvent&)` 的入参 `e.local_position` 已是相对当前控件左上角的**本地坐标**——由 `EventDispatcher` 在命中链冒泡时，按 `hit_test_chain` 携带的控件 `origin`（相对根）换算 `local_position = position - origin` 后写入。控件**无需、也无法**再查询自身在树中的绝对位置（`Widget` 已无几何缓存）。`e.position` 仍保留为全局窗口坐标供需要时读取。

```cpp
void MyWidget::on_pointer_event(MouseEvent &e) {
    if (e.action == MouseAction::Press) {
        do_press();
        e.is_handled = true;     // 命中即消费
    }
}
```

### 2.4 输入法组合（IME）契约

`TextCompositionEvent` 是 **CJK 输入的唯一入口**：后端桥把平台输入法状态**同步折算**成契约口径后投出，控件据此渲染 preedit（预编辑串）。「上屏」与「组合中」严格分离：

| 约定 | 内容 |
|:---|:---|
| 下标口径 | `preedit` 为 UTF-8；`cursor_index` / `sel_start` / `sel_end` 一律是 **preedit 内码点下标**（非字节、非 UTF-16 单元）。代理对由折算层夹紧，绝不切半字符 |
| 选区 | `sel_end` **含尾**；无待转换选区时 `sel_end == AURORA_NO_SELECTION`（`sel_end < sel_start` 时控件退化为单点选区，不报错） |
| 结束/取消 | `preedit` 为空即组合结束或取消；`committed` 只在本次确有文本上屏时非空 |
| 数据模型纯净 | **preedit 不进 `value()`**。`TextInput` / `RichTextEdit` 只在绘制与测量期把 preedit 插到光标处（`composed_text()`），故 `value()`、序列化与 golden 始终不含半截拼音 |
| 路由 | 只派发给**当前焦点控件**，不冒泡、不经命中链（§3.1）；控件覆写 `on_text_composition`（`widget/widget.h`），默认实现直接消费 |
| 失焦 | 失焦即取消未上屏的组合（平台惯例，避免 preedit 残留在旧控件里） |

典型序列（微软拼音输入「你好」）：`preedit="nihao",cursor=5` → `preedit="你好",cursor=2,sel=[0,2)` → `preedit="",committed="你好"`。

**候选窗定位**：输入法候选列表必须落在插入点旁，而非屏幕左上角。控件侧钩子为 `Widget::composition_caret_bounds()`（`widget/widget.h`，默认返回 `focus_bounds_`，即自身焦点框；文本控件覆写为 **preedit 光标处的零宽竖盒**），宿主经 `Surface::set_composition_caret_provider()`（`window/surface.h`）把它交给后端，桥内按 DPI 缩放换算成像素并映射到平台坐标系（Win32：`ClientToScreen`；X11：客户窗物理 px 写 `XNSpotLocation`；Wayland：表面本地物理 px 经 `set_cursor_rectangle`）。该盒坐标为**窗口逻辑 dp**。

> ⚠️ `composition_caret_bounds()` 的返回值在**首帧绘制之后**才有效——`focus_bounds_` 由 `Widget::paint` 写入（控件无几何缓存，见 §2.3）。空树/未绘制时返回退化矩形，后端据此退化为「不移动候选窗」。

**平台接线状态**：

| 后端 | 状态 |
|:---|:---|
| `Win32Surface`（GDI）/ `D3D11Surface` | ✅ **IMM32 桥已接**（`src/aurora/window/detail/win32_ime.h`）。二者共用同一 `Win32Host` 宿主与同一份桥，故一条路径覆盖两路 |
| `X11Surface` | ✅ **XIM 桥已接**（`src/aurora/window/x11_surface.cpp` 内联）：`XIMPreeditCallbacks` 风格协商 + draw/caret 回调回推，PreeditNothing 逐级降级 |
| `WaylandSurface` | ✅ **text-input-unstable-v3 桥已接**（`src/aurora/window/wayland_surface.cpp` 内联，门 `AURORA_HAVE_WL_TEXT_INPUT`）：enable 判据 = 焦点 ∧ provider 非零盒；组合内容/上屏/回删全部折算成契约事件 |
| `HeadlessSurface` | 无输入法概念；组合事件由测试直接构造并派发（`tests/unit/utest_ime_composition.cpp`） |
| GLFW / Wasm / macOS | ⬜ **契约级**：事件与控件侧行为已完备，缺平台桥（macOS `NSTextInputClient`、Wasm DOM `composition*`）。须有真实桌面/浏览器输入法环境后补，无头 CI 无法完成 |

各平台的取舍与实现细节（IMM32 而非 TSF、`WM_IME_CHAR` 吞字纪律、XIM 风格协商与溢出取字、v3 enable 判据与去重）见 [`06-app-platform.md`](06-app-platform.md) §8.5；真机验收见 `tools/verify/win32_ime_live_probe.cpp` / `x11_ime_live_probe.cpp` / `wayland_ime_live_probe.cpp`。

---

## 3 派发与命中测试

### 3.1 EventDispatcher

`EventDispatcher`（`event/dispatcher.h`）提供 **6 个静态 `dispatch(Widget &root, …)` 重载** + **1 个实例级鼠标入口**；所有派发入口首参均为派发起点根 widget `root`，命中测试与命中链局限于该子树：

| 入口 | 说明 |
|:---|:---|
| `static dispatch(Widget& root, MouseEvent&, FocusManager* = nullptr) -> bool` | 指针事件；委托进程内持久 `EventDispatcher` 单例，故同样保留跨事件指针捕获；`FocusManager*` 可选，非空时派发期暴露为「当前焦点管理器」 |
| `dispatch_mouse(Widget& root, MouseEvent&, FocusManager* = nullptr) -> bool`（实体方法） | 带指针捕获的鼠标派发：Press 命中后缓存命中链，后续 Move/Release 即使命中失败也持续派发给按下时目标，直到 Release 解除捕获（`Application` 走此路径） |
| `static dispatch(Widget& root, KeyEvent&, FocusManager&) -> bool` | 键盘事件；识别 Tab / Shift+Tab 并转为 `move_focus`，方向键与激活键各有一个控件级 opt-in 前置投递（`wants_navigation_keys()` / `wants_activation_keys()`，见 §4.2）；`root` 为统一重载签名而保留，键盘不经命中链 |
| `static dispatch(Widget& root, ScrollEvent&) -> bool` | 滚动事件：沿命中链**自最深向根**找 `wants_scroll()` 者逐个派发，余量经 `remaining_y` 上冒（§3.3） |
| `static dispatch(Widget& root, FileDropEvent&) -> bool` | 文件拖放事件（不冒泡，仅交给命中目标） |
| `static dispatch(Widget& root, TextInputEvent&, FocusManager&) -> bool` | 文本输入（只路由到焦点控件；无焦点返回 `false`） |
| `static dispatch(Widget& root, TextCompositionEvent&, FocusManager&) -> bool` | IME 组合态同步推送（`event/dispatcher.h`）：与文本输入同径——**只路由到当前焦点控件**的 `on_text_composition`，不经命中链、不冒泡（组合串属于正在输入的编辑器，上冒只会让容器误吞）；无焦点返回 `false` |

派发流程：先经 `Widget::hit_test` 找到最深命中的目标组件，再沿父链向上调用处理方法，直到 `handled` 为真或到达根。

`TouchDispatcher`（`dispatcher.h`）处理触控路径：实体方法 `dispatch(Widget& root, TouchEvent&, FocusManager* = nullptr) -> bool`，按 `TouchPoint::id` 做指针捕获，原始多点流全链广播 + 合成 `MouseEvent` 手势流冒泡，焦点行为与鼠标路径一致。

### 3.2 命中测试

`Widget::hit_test(local, bounds, ctx)` 返回命中的子节点：`local` 相对本组件原点，`bounds` 为绝对矩形，`ctx` 为构建上下文。

**z 序语义（与绘制一致）**：有重叠子节点的容器（如 `Stack`）的命中测试**反向**遍历子节点——最后绘制（视觉最上层）的子节点优先命中。即重叠区域中「视觉在上层」的控件优先接收事件，底层控件不会在重叠区抢走本应属于顶层控件的命中。非重叠布局（`Row` / `Column`）各子节点区域互斥，遍历方向不影响结果。

**可滚动容器自动进入命中链**：`Widget::hit_test_chain` 在「无命中的后代、自身不可点击」时仍会把 `wants_scroll()` 为真且命中点落在内容盒内的控件自身纳入链尾——内容全非可点击时滚动容器也必须在链内，否则滚轮落空。容器**无需**再覆写 `on_hit_test` 返回 `this`。

**自带拖拽区的容器必须把自身显式入链**：基类 `Widget::hit_test_chain`（`src/aurora/widget/widget.cpp`）只在「后代链非空 / `wants_click()` / `wants_scroll()` / 带 Input 修饰」之一成立时才把控件自身追加进链尾。因此靠 `on_pointer_event` 自绘拖拽带、又不 `wants_click()` 的容器（典型为 `Splitter` 的分隔条）若在其拖拽带上返回**空**后代链，就会整条链为空，被派发器判为「点在空白处」而根本不投递 Press——拖拽在任何后端都失效，且与 `on_hit_test`（点命中）口径不一致而难以察觉。此类控件须在拖拽带内返回 `std::vector{HitNode{this, weak_from_this(), bounds.origin}}`，并在带内无交互后代的空白点同样自入链，否则拖拽过程中指针一旦越出拖拽带，后续 Move 会因链空被丢弃。契约由 `utest_splitter` 的 `hit_chain_at_divider_includes_splitter` 与 `divider_drag_via_dispatcher_updates_ratio_and_clamps` 守护。

### 3.3 嵌套滚动协调（滚轮余量上冒）

`dispatch(Widget&, ScrollEvent&)`（`event/dispatcher.cpp`）把滚轮判给**最近可滚动祖先**，并在内层吃到端点后把余量交给外层：

1. 取命中链，自**最深**（链尾）向根遍历，跳过 `wants_scroll()` 为假的控件——可点击子控件（`Button` 等）不拦截滚轮，嵌套时最深滚动者优先。
2. 每一跳先把 `e.remaining_y` 归零再调 `on_scroll`：**不写余量的 handler 视为全量消费**（默认 0），与旧「一次性消费、不冒泡」约定逐位兼容，既有自定义 `on_scroll` 无需改动。
3. 该跳回传非零余量（clamp 后没吃尽的增量，与 `delta_y` 同单位同号）⇒ 令 `e.delta_y = e.remaining_y`，继续交给更浅一层可滚动祖先。
4. 链上存在可滚动者但全部吃完仍有剩 ⇒ 止步（返回 `true`）；链上**无可滚动者** ⇒ 兜底回落点命中目标直接派发一次、不再冒泡（保持历史行为）。

典型接线：`PullToRefresh` 包住 `Scroll`（外层，`wants_scroll()` 恒真且仅当子树在顶部时吃余量下拉）、`Scroll` 内嵌 `LazyList`（内层先滚，到顶/到底才把余量让给外层）。内层到端点、外层接手的端到端行为由 `utest_dispatcher` 的 `wheel_margin_bubbles_from_inner_scroll_to_pull_to_refresh` 钉住（全量冒 / 零冒 / 部分冒 / 反方向门控四种组合）。

---

## 4 焦点管理

### 4.1 FocusManager

`FocusManager`（`event/focus.h`）持 `root_` 与 `focused_`，接口：`set_root(Widget*)`、`set_focus(Widget*, FocusDirection)`、`request_focus(Widget*)`、`has_focus(const Widget*)`、`clear()`、`move_focus(FocusDirection)`、`focused()`、`set_on_change(cb)`、`push_scope(Widget*)`、`pop_scope()`、`scope_depth()`。

`FocusDirection`（`focus.h`）取值 `Forward` `Backward` `Up` `Down` `Left` `Right`。

**焦点作用域栈（模态焦点陷阱）**：`push_scope(subtree)` 把 `move_focus` 的候选集限定在 `subtree` 子树内（Tab 循环不逃出，scope 内自然回卷），并自动把焦点移入子树内首个可聚焦控件（无候选则保持原焦点），同时快照压栈前的焦点供 `pop_scope` 恢复；`pop_scope()` 弹出栈顶作用域并恢复压入前焦点（已回收则清除焦点），空栈为 no-op。供模态弹层（Dialog / Popup / Drawer）打开时配对调用；可嵌套（多层弹层各自 push / pop，恢复顺序与压栈相反）。`scope_depth()` 返回当前作用域深度（0 = 无作用域，Tab 遍历整棵根树）。

**焦点控件的生命周期契约**：`FocusManager` 以裸指针记录焦点控件，**不拥有**它；焦点控件常在自身被重建 / 回收后仍留在记录里（如输入框所在页面被 `push_replacement` 换掉）。故内部与 `HitNode` 同构地附带弱引用守卫（构造时探测是否由 `shared_ptr` 持有，栈 / 成员控件回退为裸指针）。`focused()` / `has_focus()` / `set_focus()` / `move_focus()` 一律经存活视图取用：焦点控件已被回收时 `focused()` 返回 `nullptr`，键盘与文本派发据此安全返回 `false`，**绝不对已释放内存做虚调用**。

### 4.2 组件焦点接口

`Widget` 提供：`focusable()`、`set_focusable(bool)`、`wants_focus()`（虚钩子，Tab 停点意愿，见下）、`tab_index()`、`set_tab_index(int)`、`is_focused()`、`request_focus()`、`on_focus_change(bool)`（虚钩子，基类维护 `is_focused_` 并标脏重绘）、`wants_focus_ring()`（虚钩子，基类统一焦点环开关，见 §4.4）。

**焦点管理器「随派发可得」**：`EventDispatcher::dispatch(Widget&, MouseEvent&, FocusManager*)` 在派发期经线程局部暴露「当前焦点管理器」（`current_focus_manager()`），`request_focus()` 读之，无需在每控件上递归注入。无焦点管理器（`nullptr`）时 `request_focus` 静默 no-op。

`move_focus` 按 `tab_index` 稳定排序后循环取前 / 后一个；Tab / Shift+Tab 由 `dispatch(Widget&, KeyEvent&, FocusManager&)` 识别并转 `move_focus`。

**激活键（Enter / Space）路由**：`Enter` 与 `Space` 归为「激活键」，默认由派发器直接调用焦点控件的 `activate()`（按钮等「按下即激活」语义），控件本身观察不到这两个按键。需要观察 Enter 的文本录入类控件（`TextInput` / `RichTextEdit`）覆写 `Widget::wants_activation_keys()` 返回 true：派发器先投递 `on_key_event`，其消费（`is_handled`）即止；未消费才回落 `activate()`。`TextInput::on_submit`（Enter 提交）即由此路径可达——若只依赖激活语义，Enter 会被在焦点路由前消费掉而永远到不了控件。

**方向键（↑ / ↓ / ← / →）路由**：与激活键同构的「控件优先、宿主兜底」约定。方向键默认归 `FocusManager::move_focus(dir)` 做几何焦点导航，控件观察不到；覆写 `Widget::wants_navigation_keys()` 返回 true 的控件先收到 `on_key_event`，消费即止、焦点不动，未消费才回落几何焦点导航。认领方向键的两类控件：文本录入类（`TextInput` 与 `RichTextEdit` 的 ←/→ 移光标与 Shift 扩选）与键盘重排类（`ReorderableList`）。**文本录入控件必须在此认领**：单行输入框左右键若落到几何焦点导航，`move_focus(Left/Right)` 一旦命中候选就把焦点移走，此后所有按键（含退格）都发给新焦点控件，输入框当场失焦且再也无法用键盘编辑——这正是无人认领时的错误形态。未被认领的方向（如单行框的 ↑/↓）仍走焦点导航，默认 `false` 对既有非文本控件逐字节不变。两处谓词都只在**焦点控件**上探测，不经冒泡（键盘路由本就不冒泡）。

**端点键（Home / End）语义**：`Home` / `End` 不属派发器的 Tab / 方向键 / 激活键任一类别，因此**无需认领**——未匹配全局快捷键时天然经焦点路由直达焦点控件，控件消费即止。两个文本录入控件都把它实现为「文本 / 文档两端」（单行框本无行首行尾之别；`RichTextEdit` 的行内定位属未实现项，当前按整篇文档首尾处理），且与 ←/→ 共用各自的选区模型：非 Shift 时跳光标并清选区，Shift 时从光标扩到对应端点。`TextInput` 额外约束：光标本已在端点且原无选区时，Shift+`Home`/`End` 的扩选结果为空，不得伪造一个单字符选区（否则一次退格会删掉无辜字符）。

**Tab 停点的分级默认**：一个控件进入 Tab 序的条件是 `focusable() && wants_focus()`，两个谓词分工不同，不可混用：

- `focusable()` 是**宿主侧的显式开关**（默认 `true`）：`false` 一票否决，`true` 只代表「宿主没否决」，不代表该停点存在。
- `wants_focus()` 是**控件类型侧的意愿**（`Widget` 基类默认 `true`）：由类型自己声明「我是不是一个交互停点」，宿主无需逐个关闭。

覆写为「有输入语义才入序」的只有两类：**纯布局容器**（`Container` / `SingleChild` 基类——`Column`/`Row`/`Stack`/`Grid`/`Wrap` 与 `Show`/`Provider`/`Lifecycle`/`Badge` 等，它们本身不接受输入）与**纯展示件**（`Text`/`Divider`/`Spacer`/`Placeholder`/`RichText`/`Progress`/`Skeleton`/`ImageView`/`Canvas`/`BreakpointBuilder`/`LayoutBuilder`）。判定统一走 `Widget::has_input_semantics()`（protected）：`wants_click() || modifier.has_gesture() || modifier.has_context_menu() || wants_scroll() || wants_navigation_keys() || wants_activation_keys()`——任一成立即认为该控件确实需要键盘到达，于是同一个 `Text` 挂上 `.clickable(...)` 后自动回到 Tab 序，宿主不必再改焦点属性。因此该覆写是**减法而非一刀切**：自带滚动语义的容器（`Scroll`/`LazyList`/`LazyRow`/`GridView`/`ReorderableList`/`PullToRefresh`）与自带点击的 `ExpansionPanel` 仍是停点。**交互控件无需任何声明**（基类默认 `true` 已覆盖）。

分级只作用于 `move_focus` 的候选集，**不影响**指针 Press 的焦点归属（§4.3）与 `set_focus` / `request_focus` 的显式聚焦——二者仍只看 `focusable()`，因此点击一个纯展示容器依然不会误清焦点，而业务代码强制聚焦任何未被否决的控件都仍然成功。

### 4.3 Press 焦点归属与点击失焦契约

鼠标 / 触控 `Press` 时，焦点归属为**命中链上自最深命中向根找到的第一个可获焦控件**（点到不可获焦的装饰子控件时归属其可获焦祖先，如按钮内的图标）。

**整条链都不可获焦**（点到纯展示容器）或**命中链为空**（点到根外空白）均视为点击空白——`FocusManager` 清焦点（`set_focus(nullptr)`），旧焦点控件收到 `on_focus_change(false)`（`Text` 据此清除选区高亮，`TextInput` 据此隐藏光标）。

鼠标与触控路径行为一致。`Release` **不**切换焦点，避免拖选结束落在别处时选区被清。

`TextInput` 点击时 `request_focus`，`is_focused()` 控制光标显示。

### 4.4 焦点可视化：基类统一焦点环

焦点态必须在像素上可判定，否则人工侧只能靠猜（`Button` 这类控件自身不画任何聚焦外观）。为此 `Widget::paint_content` 末尾由**基类统一绘制焦点环**：控件持有焦点（`is_focused()`）且 `wants_focus_ring()` 为 `true` 时，在其视觉盒外画一圈主题色（`inherit_theme(ctx).primary`）圆角边框。

- 几何：环带严格落在盒外 **2–4 dp** 的带宽内（间距 2 dp + 环宽 2 dp，圆角 4 dp 小于常见控件圆角故不与边框弧线相交）。**不得压在控件自身边缘像素上**——否则会把 `RichTextEdit` 之类贴着边缘绘制的-caret / 边框像素覆盖掉。这三项常量是 `Widget` 的 `AURORA_FOCUS_RING_GAP` / `AURORA_FOCUS_RING_THICKNESS` / `AURORA_FOCUS_RING_RADIUS`，绘制与标脏共用同一份（不得各写一份，否则脏区与像素会漂移）。
- 开关：`wants_focus_ring()`（`Widget` 基类默认 `true`，虚钩子）已自带聚焦态外观的控件覆写为 `false`，以免双环。当前唯一覆写者是 `TextInput` 与 `RichTextEdit`（它们画 Fluent 式主题色加粗聚焦边框，`paint_frame`）。
- 重绘：`on_focus_change(bool)` 基实现在维护 `is_focused_` 后调用 `mark_needs_paint()`，故焦点变化必然进下一帧，环不会「逻辑上聚焦、像素上无环」。
- **脏区（关键契约）**：环画在控件自身盒**外**，而 `paint_bounds()` 只含自身盒，因此标脏**不得**直接用 `paint_bounds()`——必须用 `Widget::dirty_bounds()`（= 自身绘制盒 ∪ 盒外装饰）。该契约有两个方向，缺一即错：
  - 获焦帧：脏区不含环带 → 环落在 `push_clip` 之外被裁掉，「逻辑上聚焦、像素上无环」（要靠别的控件恰好同帧标脏扩大合并框才偶发出现）。
  - 失焦帧（或尺寸变化后）：脏区不含环带 → 上一帧的环像素无人重绘，残留成「盒外一圈主题色，看起来比控件自身盒大」。
  汇聚点共两处，都要走 `dirty_bounds()`：`Window::install_dirty_sink`（普通窗口坐标）与 `Scroll::on_descendant_dirty`（离屏内容缓冲的内容坐标系，脏带若漏环则残留固化在缓冲里随视口反复上屏）。回归见 `tests/integration/itest_dirty_clip_paint.cpp` 的 `focus_ring_band_repainted_when_focus_changes` / `focus_ring_band_inside_scroll_repainted`（不变量：部分脏区重绘与整帧重绘逐位一致）。
- 可观测性：环是无条件的像素证据，配合树快照里持有焦点节点的 `focused: true`（见 [`08-tooling.md`](08-tooling.md)），Inspector 与人工验收都能在不依赖控件自觉的前提下确认焦点落点。
- 取舍：未采用 `:focus-visible`（仅键盘触发环）方案——它需要给 `FocusDirection` 增「非导航来源」并让派发器透传来源信息，且会使「指针点击后的焦点」在像素上重新不可判定，与本条要解决的问题相反。

---

## 5 手势与拖拽

### 5.1 手势识别器

`event/gesture.h` 提供识别器：

| 识别器 | 说明 |
|:---|:---|
| `PinchRecognizer` | 双指捏合缩放；锁定两个 pointer id，跟踪 `initial_distance_` / `current_distance_` |
| `RotationRecognizer` | 双指旋转；锁定两个 pointer id，跟踪 `initial_angle_` / `current_angle_` |
| `DragRecognizer` | 单指拖动（pointer-agnostic）：超 `slop`（默认 8 逻辑 dp）起拖，按 pointer_id 锁定首按点（中途其他指插入不换锁），起拖瞬间按 \|dx\|\>\|dy\| 锁主轴（`DragAxis`）；`delta()` 起拖后仅输出锁定主轴分量（未起拖或未锁轴时为原始位移）。`on_mouse` / `on_touch` 双入口（触摸取首个活跃点，单指语义）；纯识别器，不接触动画、不持有 State |

`DragRecognizer` 的动画消费方 `DragToDismiss`（跟手 + spring 接管）见 §6.7。

`Modifier` 层提供 `.draggable(...)` 与 `.long_press(...)` 两个手势修饰节点（单指），由 `Draggable` / `LongPress` 驱动（见 [`07-environment-modifier.md`](07-environment-modifier.md)）。

### 5.2 拖拽

| 类型 | 说明 | 位置 |
|:---|:---|:---|
| `DragData` | 拖拽载荷，含 `mime_type`（`"text/plain"`、`"aurora/widget"` 或自定义） | `drag_drop.h` |
| `DragSession` | 一次拖拽会话，跟踪 `origin_` 与 `active_` | `drag_drop.h` |
| `DropTargetCallbacks` | 放置目标回调集 | `drag_drop.h` |

---

## 6 动画

### 6.1 核心类型

| 类型 | 说明 | 头文件 |
|:---|:---|:---|
| `AnimationController` | 驱动一条归一化进度（0→1）。`forward(from = -1)` 正向、`reverse()` 反向、`reset(t = 0)` 复位、`value()` 取进度。时长在构造 `AnimationController(duration_seconds, value)` 时确定，**无 `set_duration`** | `animator.h` |
| `AnimationStatus` | 动画状态枚举 `Dismissed`（进度 0）/ `Forward`（正向播放中）/ `Reverse`（反向播放中）/ `Completed`（进度 1）。`AnimationController::status()` 与 `AnimatedValue::status()` 均提供状态查询；其中 `is_completed()` 两者都有，而 `is_dismissed()` / `is_animating()` 仅属 `AnimationController`（`AnimatedValue` 只暴露 `status()` 与 `is_completed()`） | `animator.h` |
| `Tween<T>` | 补间函数。`Tween<T>(a, b, curve)`，`value(t)` 按曲线在 a→b 间插值；支持 `int` / `float` / `Size` / `Point` / `Color` 等 | `timeline.h` |
| `Keyframes<T>` | 关键帧序列。`Keyframes<T>(stops)`，每帧 `Stop{time, value}`，**插值严格线性**；`value(t)` 在分段间插值 | `timeline.h` |
| `Curve` / `Curves` | 缓动曲线。`Curves::linear()` / `ease_in()` / `ease_out()` / `ease_in_out()` / `ease_in_out_cubic()` 等（无 `steps` 工厂） | `easing.h` |
| `SpringDescription` | 弹簧物理参数（`stiffness` / `damping` / `mass`），配合 `SpringSimulation` 用于物理感动画 | `spring.h` |
| `Animator` | 帧循环驱动器 | `animator.h` |
| `AnimatedValue<T>` | 「`State<T>` + `Tween` + 控制器」三合一句柄 | `animator.h` |
| `TweenAnimation<T>` | 自包含动画值：拥有自己的 `State<T>`，可独立推进。`animate_to(target, duration_s[, curve])` 起步、`tick(dt)` 每帧推进、`get()` 取当前值、`is_animating()`、`as_signal()` 交出内部 `State<T>` 供响应式绑定 | `animator.h` |
| `TimelineInterval` | 归一化子区间 [begin, end]（对应 Flutter `Interval`）；`contains(t)` 判归属，`local(t)` 把总进度映射为区间局部进度（区间外夹取端点：t ≤ begin → 0，t ≥ end → 1） | `timeline.h` |
| `TimelineSpec` / `TimelineResolved` | 时间轴构建器与求值结果（纯值）：`sequence()` / `parallel()` / `staggered(item, gap, count)` 组合子方法链声明子段，`add(时长秒)` / `add(子组)` 追加；`build()` 展开为槽位区间表 + 总时长。见 §6.6 | `timeline.h` |
| `TimelinePlayer` | 时间轴播放器：单主控制器驱动全部轨道（槽位 + `Tween<T>` + 目标 `State<T>`）；`forward` / `reverse` / `stop` 中断续播，`track<T>` 类型安全绑轨，`attach(Animator)` 接入帧循环。见 §6.6 | `animator.h` |

`timeline.h` 实际只提供 `lerp` 重载族（算术 / `Point` / `Size` / `Color` / `EdgeInsets` / `Rect`）、`Tween<T>`、`Keyframes<T>` 与时间轴编排（`TimelineSpec` / `TimelineResolved`，见 §6.6）。多段编排优先用 `TimelineSpec` + `TimelinePlayer`；单条多关键帧曲线仍可用 `Keyframes<T>` 的 `Stop{time, value}` 序列表达，或经 `Animator::add_binding` 追加帧回调按 `AnimationController::status()` / `value()` 分段驱动。

### 6.2 Animator 与生命周期

`Animator::bind(controller, tween, state)` 把控制器的进度写入目标 `State<T>`；每帧 `tick(dt)` 推进并触发刷新；`add_binding(fn)` 追加任意帧回调（在控制器推进后、清除脏标记前执行）。

**`drive()` / `bind()` 登记的是非拥有裸指针**，`Animator` 不延长控制器与目标 `State` 的生命周期。因此：

> 凡「控制器是某控件的成员、却注册进 `Application` 全局 `Animator`」的场景（典型如 `NavigatorHost`），**必须**在析构函数中调用 `remove(const AnimationController&)` 注销该控制器及其全部绑定，否则控件析构后下一帧 `tick` 就会写入已释放内存（use-after-free）。对未登记过的控制器调用是无操作，可重复调用。

### 6.3 AnimatedValue

`AnimatedValue<T>` 把 `State<T>`、一条 `Tween` 与控制器收拢一处，**内部以 `shared_ptr` 持有驱动载荷（pimpl）**，因此句柄可按值返回、自由拷贝，且 `attach(animator)` 后即使原句柄离开作用域，帧循环仍安全持有驱动载荷（不悬垂）。

接口：`forward(from)`、`tick(dt)`（自驱动一帧）、`progress()`、`current()`（目标 `State` 当前值）、`is_completed()`、`on_completed(cb)`（到达终点的一次性回调）、`attach(Animator&)`（接入帧循环）。

### 6.4 统一入口 animate

自由函数 `animate(target, tween, duration_s[, animator])` 返回**已起步**（`forward(0)`）的 `AnimatedValue<T>` 句柄，两个重载：

1. 不接 `Animator`——调用方自行每帧 `handle.tick(dt)`；
2. 额外传入 `Animator&`——自动 `attach` 接入其帧循环。

既有 `AnimationController` / `AnimatedValue` 的直接构造方式保留。

```cpp
au::State<float> opacity{0.0F};

// 形态一：统一入口 + 自动接入帧循环
au::Animator anim;
auto handle = au::animate(opacity, au::Tween<float>(0.0F, 1.0F, au::Curves::ease_in_out()), 0.3, anim);
handle.on_completed([](){ /* 动画结束 */ });
// 每帧：anim.tick(dt);  →  opacity 从 0 渐变到 1，组件自动重绘

// 形态二：无 Animator 时手动自驱动
auto h2 = au::animate(opacity, au::Tween<float>(1.0F, 0.0F, au::Curves::linear()), 0.3);
// 每帧：h2.tick(dt);
```

### 6.5 设计约束

动画只改变 `State<T>`（视觉属性如 opacity / transform），**不改变布局盒模型**；布局快照在动画前后一致（与 [`03-layout-render.md`](03-layout-render.md) §10.2 规则 7 同源）。

**自驱动动画**：在 `on_paint` 末尾 `mark_needs_paint()` 自调度下一帧、并用 `std::chrono::steady_clock` 算 `dt` 手动缓动的控件，必须覆写 `can_cache_display_list()` 返回 `false`（[`04-widget.md`](04-widget.md) §2.4）。

### 6.6 Timeline 编排（TimelineSpec / TimelinePlayer）

声明式时间轴编排原语：构建器自动计算各子段归一化区间，消除 Flutter staggered 模式「手算 `Interval` 端点」的痛点。

**区间树（纯值层，`timeline.h`）**——三种组合子覆盖编排形态，可嵌套（子组随父组规则展开）：

- `sequence()`：子段首尾相接（游标推进）；
- `parallel()`：子段同起点，组长 = max(子时长)，组尾允许间隙（同 Flutter `Interval` 语义）；
- `staggered(item_duration, gap, count)`：交错糖，第 i 叶子区间 `[i×(item+gap), i×(item+gap)+item]`。

区间计算在 `build()` 一次完成：叶子段按深度优先序获得槽位 0..n-1（与 `add` 调用序一致）；叶子时长非正值夹取 1e-6。`TimelineResolved` 保存槽位区间表 + 总时长（拷贝廉价），`interval(slot)` 越界防御返回全区间。

**播放器（`animator.h`）**——单主 `AnimationController`（时长 = spec 总时长）驱动全部轨道，每轨道 = (槽位区间, `Tween<T>`, 目标 `State<T>`)：

```cpp
au::State<double> slide{0.0}, fade{0.0};
auto tl = au::TimelineSpec::sequence()
              .add(0.3)                                    // 槽位 0
              .add(au::TimelineSpec::staggered(0.2, 0.05, 3))  // 槽位 1..3
              .build();
au::TimelinePlayer player{tl};
player.track<double>(0, au::Tween<double>{0.0, 1.0}, slide);
player.track<double>(1, au::Tween<double>{0.0, 1.0, au::Curves::ease_out()}, fade);
player.forward();
player.attach(app.animator());
```

关键语义：

- **反向镜像**：`reverse()` 沿同一归一化进度倒放（主进度标量倒退），区间映射天然对称——交错序列严格逆序呈现，零额外实现；
- **中断续播**：`stop()` / `reverse()` / `forward(-1)` 都不动主进度，任何方向切换从当前进度续（跟手接管的基础，见 §6.7）；
- **区间外保值**：未开始的子段保持 Tween begin 值、已完成的保持 end 值；起播瞬间（`forward(0)`）统一把全部轨道初始化到 begin 值，消除「未播轨道保持旧值」的不确定；
- **reduce_motion**：主控制器短路直接落端点 → 全轨道一步到位，继承 `AnimationController` 的无障碍语义，编排层零特判；
- **生命周期**：沿用 `AnimatedValue` 模式——载荷 `shared_ptr` 自持，`attach` 后句柄可离开作用域不悬垂；轨道目标 `State<T>` 为非拥有引用，必须比播放器存活更久；
- **类型擦除边界**：轨道存储在播放器内部以 `shared_ptr<void>` + apply 函数擦除；公共 API（`track<T>`）全程类型安全，擦除不经接口泄漏；
- **写序**：同帧多轨道按绑定序写入，无相互依赖的 State 间无顺序假设。

### 6.7 手势驱动动画（DragToDismiss / Dismissible）

单指拖动跟手 + 松手 spring 裁决的 drag-to-dismiss 语义（pointer-agnostic，鼠标 / 触摸统一走 `pointer_id` 抽象流）。

**`DragToDismiss`（`event/gesture.h`）**：拖动消除驱动器，内部持有 `DragRecognizer`。

- **跟手 = 直接操作而非动画**：drag 期间 `progress` 逐事件写为「主轴位移 / `travel`（消除行程，逻辑 dp）」，1:1 映射；`reduce_motion` 不干预跟手（无障碍语义：直接操作保持 1:1 响应）。负方向夹取 0，超行程夹取 1。
- **松手裁决**：`on_release()` 按 `progress ≥ threshold`（默认 0.5）判落点——≥ 阈值 spring 到 1，收敛后触发 `on_dismissed`；否则 spring 回 0（静默，不触发回调）。拖动速度（最近事件位移 / 固定 60Hz 采样假设，主轴分量）作 spring 初速度：方向与裁决一致保留、相反丢弃。
- **reduce_motion**：spring 阶段单帧直接落端点（与 `AnimationController::tick` 的短路语义同源）。
- **帧推进**：spring 阶段由使用方每帧 `tick(dt)` 推进（`attach` 之外的独立路径）；跟手阶段 `tick` 为 no-op。

**`Dismissible`（`widget/dismissible.h`）**：拖动消除任意子树的容器控件（对标 Flutter `Dismissible`），内部持有 `DragToDismiss`。

- 位移映射：`offset = progress × travel`（首次布局后按主轴向尺寸校准行程）；透明度联动 `1 − 0.5 × progress`（飞出途中渐隐）。位移与渐隐在 `paint` 期平移实现，**不动布局盒**（§6.5 约束）。
- 事件消费：拖动 / spring 进行中消费指针事件（父级不响应点击）；spring 阶段经控件 `tick_gestures` 帧驱动（需挂入 `Application` 的 gesture tick）。
- 摘除策略：spring 飞出完成后**默认从最近 `Container` 祖先摘除自身**（触发重排）；`on_dismissed(cb)` 注册自定义回调后覆盖默认摘除（如列表数据删除后由数据层重建子树）。
- 序列化：手势进度与回调为运行时态，工厂注册仅收录自描述元数据（`Rebuildable: no`）。

**与 TimelinePlayer 的配合**（编排侧接管）：drag 中 `player.stop()`；松手回位走 `player.reverse()` 从中断处续播——即 §6.6 中断续播语义的消费示例。`DragToDismiss` 自带 spring，不依赖 Timeline。

---

## 7 导航

### 7.1 Route

`Route`（`navigation/route.h`）是一条路由。构造 `Route(Node root, std::string name = "", RouteTransition transition = {})`；`root()` 返回该页 UI 子树，`name()` 返回路由名。

### 7.2 Navigator

`Navigator`（`navigation/navigator.h`）是页面栈控制器。构造 `Navigator(Route)` 指定初始路由。

| 方法 | 说明 |
|:---|:---|
| `push(Route)` | 入栈 |
| `pop()` | 出栈；仅剩根路由时拒绝并返回 `false` |
| `push_replacement(Route)` | 替换当前页（原地换栈顶，深度不变） |
| `pop_to_root()` | 回到根页 |
| `current()` | 当前路由引用（const / 非 const 重载） |
| `current_root()` | 当前页子树（无路由返回空 `Node`） |
| `depth()` | 当前栈深 |
| `can_pop()` | 是否可出栈（等价「栈深 > 1」） |
| `stack()` | 整个路由栈的只读视图（`const std::vector<Route>&`） |
| `path()` | 导出当前路由栈名序列（deep linking 用） |
| `max_depth()` / `set_max_depth(d)` | 读 / 写最大栈深上限（默认 `AURORA_DEFAULT_MAX_NAV_DEPTH`） |
| `set_on_route_changed(cb)` | 路由变化回调 |
| `open_uri(uri, build)` | 深层链接：按 `/` 切分名称序列（丢弃空段）后委托 `restore(names, build)` 重建路由栈 |
| `restore(names, build)` | 按名称序列重建整栈 |
| `open_uri(uri, registry)` | 经 `RouteRegistry` 查表构建（表中缺失的名称段被跳过） |

`RouteRegistry` 是轻量注册表：`std::map<std::string, std::function<Route(const std::string&)>>`。

**栈深上限守卫**：默认上限 `AURORA_DEFAULT_MAX_NAV_DEPTH = 32`（`navigator.h`，`inline constexpr std::size_t`），可经 `set_max_depth` 调整；`push` / `restore` 超过上限时经 `Diagnostics` 降级拒绝，避免无限深栈导致栈溢出 / 渲染雪崩。

**深层链接无 query 参数语义**：不引入 route-args 机制，名称段即全部信息。

### 7.3 Router

`Router`（`navigation/router.h`）是路由注册辅助类，支持三种等价建表写法：

| 方法 | 说明 |
|:---|:---|
| `register_route(name, builder)` | 登记命名路由：`name` → 构建 `Route` 的工厂（`RouteBuilder`）；同名覆盖 |
| `with(name, builder) const` | 便捷工厂：返回**新表** = 本表 + 这一条（本表不变，对标 `Environment::with`），供链式建表 |
| `with({{name, builder}, ...}) const` | 便捷工厂：一次并入一批 `Entry`（`{name, builder}`），表形态写法 |
| `has(name)` | 是否已登记该名称 |
| `build(name)` | 按名称构建 `Route`；未登记返回 `nullopt` |
| `build_root(name)` | 便捷：构建并取根节点 `Node`；未登记返回空 `Node` |

`with` 每次调用复制整张表（`unordered_map` 浅拷贝，成本与路由条数成正比），适合「建一次、到处读」的 `const` 路由表；高频增量登记仍用 `register_route`。

### 7.4 Hero 共享元素转场

`Hero`（`navigation/hero.h`）构造 `Hero(std::string tag, Node child)`。

同 `tag` 的源 / 目标 `Hero` 在 `Navigator` 转场期间被 `NavigatorHost` 配对，于 `TransitionLayer` 覆盖层上按转场进度 `lerp` 矩形 + 交叉淡变，形成「形变飞入」效果。

- **常态零开销**：仅当 `tag` 处于 morphing 时跳过自绘，由 `NavigatorHost` 经 `Provider<HeroRegistry>` 注入的注册表驱动。
- `NavigatorHost` 持有注册表并在转场期注入子树环境。
- **换页自带布局级标脏**：`NavigatorHost` 每次重建展示层（`push` / `pop` / `pop_to_root` / `push_replacement` / `open_uri`）都发一次含布局脏的重绘请求，非动画路径（未带 `RouteTransition` 的 `push`、`open_uri` 深链）同样自标脏；消费者无需再经 `set_on_route_changed` 手工请求重绘。
- **配对缺失**（仅旧页或仅新页有该 tag）时退化为普通淡入淡出。

```cpp
au::Navigator nav{ au::Route{ build_home(), "home" } };
nav.push(au::Route{ build_detail(id), "detail" });   // 跳转
nav.pop();                                           // 返回

// 深层链接：URI 字符串重建整栈（无 query 参数语义）
nav.open_uri("home/detail/42", [](const std::string& name) { return build_route(name); });

// 共享元素转场：两页各放同 tag 的 Hero，转场自动「形变飞入」
au::Hero("logo", au::Text("Aurora"));   // Hero(tag, Node)；Text 直接作为子节点，无需 Container 包裹
```

---

## 8 需求规格

### 8.1 SPEC.API.EXPLICIT-FIRST.001 显式优于隐式（含样式继承）

**核心目标：** AI 无理解盲区。

```cpp
// ❌ 隐式 —— AI 不知道这里发生了什么
window.show();  // 内部自动创建渲染上下文、事件循环、平台窗口

// ✅ 显式 —— AI 能完整理解控制流
// 无头便捷构造 Application(Scene, width, height)：不持有 Window，供程序化派发与 render_to_png
auto app = au::Application{ au::Scene{ build_ui() }, 800, 600 };
app.dispatch_click(x, y);                    // 显式注入输入
app.tick();                                  // 显式推进一帧（内部驱动布局/绘制/上屏）
```

**关键约束：**

- 没有「魔法」全局状态。
- 没有隐藏的初始化顺序依赖。
- 生命周期显式管理（RAII + 明确的 create / destroy）。
- **主题、字体、颜色的继承链必须是显式参数或显式 scope 传递**，不能依赖上下文隐式获取。

```cpp
// ❌ 隐式样式继承
// 父组件设了 font_size=20，子组件「自动继承」

// ✅ 显式样式传递
au::ThemeProvider{ my_theme, au::Column(au::ColumnProps{
    .children = {
        // 此子树内所有组件可经 ctx.environment<Theme>() 读取 my_theme
    },
}) };

// 或显式参数（不经隐式继承）
au::Text("Hello").font_size(20);
```

**验收标准：** 主题、字体、颜色的来源在代码中可见；不存在「父级设值、子级自动生效」的隐式继承路径。环境注入的显式 scope 机制见 [`07-environment-modifier.md`](07-environment-modifier.md)。
