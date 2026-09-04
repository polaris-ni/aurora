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
    bool handled = false;   // 是否已被消费（停止冒泡）
};
```

所有事件携带 `handled` 标志。组件处理方法接收**非 const** 事件引用，可写 `e.handled = true` 消费事件，阻止继续向上冒泡。

### 2.2 具体事件

| 事件 | 字段 | 位置 |
|:---|:---|:---|
| `MouseEvent` | `position`（全局窗口逻辑坐标，由 Surface 后端写入）、`local_position`（相对当前控件的本地坐标，由 `EventDispatcher` 写入）、`action`、`button` | `event.h:67` |
| `KeyEvent` | `key`、`action`（`KeyAction::Down` / `Up`）、`modifiers` | `event.h:79` |
| `ScrollEvent` | `position`、`delta_x`（右为正）、`delta_y`（**上为正**） | `event.h:86` |
| `TextInputEvent` | `text`（UTF-8 文本片段） | `event.h:93` |
| `FileDropEvent` | `position` 与拖放文件信息 | `event.h:98` |
| `TouchEvent` | `TouchPoint{id, position, prev_position, active}` 集合 | `event.h:112` |

枚举：`MouseButton{Left, Right, Middle}`、`MouseAction`（`event.h:20`）、`KeyAction{Down, Up}`、`ModifierKey`（`event.h:30）、`KeyCode`（`keycode.h:12`）。

**滚动方向约定**：`ScrollEvent::delta_y` 正方向为「向上滚动」（应露出上方内容、offset 减小）。所有滚动控件统一用 `m_offset - e.delta_y * step`；误用 `+` 会导致方向相反。

### 2.3 坐标契约

`on_pointer_event(MouseEvent&)` 的入参 `e.local_position` 已是相对当前控件左上角的**本地坐标**——由 `EventDispatcher` 在命中链冒泡时，按 `hit_test_chain` 携带的控件 `origin`（相对根）换算 `local_position = position - origin` 后写入。控件**无需、也无法**再查询自身在树中的绝对位置（`Widget` 已无几何缓存）。`e.position` 仍保留为全局窗口坐标供需要时读取。

```cpp
void MyWidget::on_pointer_event(MouseEvent &e) {
    if (e.action == MouseAction::Press) {
        do_press();
        e.handled = true;     // 命中即消费
    }
}
```

---

## 3 派发与命中测试

### 3.1 EventDispatcher

`EventDispatcher`（`event/dispatcher.h:29`）提供 **5 个静态 `dispatch(Widget &root, …)` 重载** + **1 个实例级鼠标入口**；所有派发入口首参均为派发起点根 widget `root`，命中测试与命中链局限于该子树：

| 入口 | 说明 |
|:---|:---|
| `static dispatch(Widget& root, MouseEvent&, FocusManager* = nullptr) -> bool` | 指针事件；委托进程内持久 `EventDispatcher` 单例，故同样保留跨事件指针捕获；`FocusManager*` 可选，非空时派发期暴露为「当前焦点管理器」 |
| `dispatch_mouse(Widget& root, MouseEvent&, FocusManager* = nullptr) -> bool`（实体方法） | 带指针捕获的鼠标派发：Press 命中后缓存命中链，后续 Move/Release 即使命中失败也持续派发给按下时目标，直到 Release 解除捕获（`Application` 走此路径） |
| `static dispatch(Widget& root, KeyEvent&, FocusManager&) -> bool` | 键盘事件；识别 Tab / Shift+Tab 并转为 `move_focus`；`root` 为统一重载签名而保留，键盘不经命中链 |
| `static dispatch(Widget& root, ScrollEvent&) -> bool` | 滚动事件（不冒泡，仅交给命中链最深叶） |
| `static dispatch(Widget& root, FileDropEvent&) -> bool` | 文件拖放事件（不冒泡，仅交给命中目标） |
| `static dispatch(Widget& root, TextInputEvent&, FocusManager&) -> bool` | 文本输入（只路由到焦点控件；无焦点返回 `false`） |

派发流程：先经 `Widget::hit_test` 找到最深命中的目标组件，再沿父链向上调用处理方法，直到 `handled` 为真或到达根。

`TouchDispatcher`（`dispatcher.h:115`）处理触控路径：实体方法 `dispatch(Widget& root, TouchEvent&, FocusManager* = nullptr) -> bool`，按 `TouchPoint::id` 做指针捕获，原始多点流全链广播 + 合成 `MouseEvent` 手势流冒泡，焦点行为与鼠标路径一致。

### 3.2 命中测试

`Widget::hit_test(local, bounds, ctx)` 返回命中的子节点：`local` 相对本组件原点，`bounds` 为绝对矩形，`ctx` 为构建上下文。

**z 序语义（与绘制一致）**：有重叠子节点的容器（如 `Stack`）的命中测试**反向**遍历子节点——最后绘制（视觉最上层）的子节点优先命中。即重叠区域中「视觉在上层」的控件优先接收事件，底层控件不会在重叠区抢走本应属于顶层控件的命中。非重叠布局（`Row` / `Column`）各子节点区域互斥，遍历方向不影响结果。

**可滚动容器须覆盖 `on_hit_test` 返回 `this`**：`dispatch(Widget&, ScrollEvent&)` 只调用命中链最深叶的 `on_scroll`，不冒泡。

---

## 4 焦点管理

### 4.1 FocusManager

`FocusManager`（`event/focus.h:32`）持 `m_root` 与 `m_focused`，接口：`set_root(Widget*)`、`set_focus(Widget*, FocusDirection)`、`move_focus(FocusDirection)`、`focused()`。

`FocusDirection`（`focus.h:17`）取值 `Forward` `Backward` `Up` `Down` `Left` `Right`。

**焦点控件的生命周期契约**：`FocusManager` 以裸指针记录焦点控件，**不拥有**它；焦点控件常在自身被重建 / 回收后仍留在记录里（如输入框所在页面被 `push_replacement` 换掉）。故内部与 `HitNode` 同构地附带弱引用守卫（构造时探测是否由 `shared_ptr` 持有，栈 / 成员控件回退为裸指针）。`focused()` / `has_focus()` / `set_focus()` / `move_focus()` 一律经存活视图取用：焦点控件已被回收时 `focused()` 返回 `nullptr`，键盘与文本派发据此安全返回 `false`，**绝不对已释放内存做虚调用**。

### 4.2 组件焦点接口

`Widget` 提供：`focusable()`、`set_focusable(bool)`、`tab_index()`、`set_tab_index(int)`、`is_focused()`、`request_focus()`、`on_focus_change(bool)`（虚钩子，基类维护 `m_is_focused`）。

**焦点管理器「随派发可得」**：`EventDispatcher::dispatch(Widget&, MouseEvent&, FocusManager*)` 在派发期经线程局部暴露「当前焦点管理器」（`current_focus_manager()`），`request_focus()` 读之，无需在每控件上递归注入。无焦点管理器（`nullptr`）时 `request_focus` 静默 no-op。

`move_focus` 按 `tab_index` 稳定排序后循环取前 / 后一个；Tab / Shift+Tab 由 `dispatch(Widget&, KeyEvent&, FocusManager&)` 识别并转 `move_focus`。

### 4.3 Press 焦点归属与点击失焦契约

鼠标 / 触控 `Press` 时，焦点归属为**命中链上自最深命中向根找到的第一个可获焦控件**（点到不可获焦的装饰子控件时归属其可获焦祖先，如按钮内的图标）。

**整条链都不可获焦**（点到纯展示容器）或**命中链为空**（点到根外空白）均视为点击空白——`FocusManager` 清焦点（`set_focus(nullptr)`），旧焦点控件收到 `on_focus_change(false)`（`Text` 据此清除选区高亮，`TextInput` 据此隐藏光标）。

鼠标与触控路径行为一致。`Release` **不**切换焦点，避免拖选结束落在别处时选区被清。

`TextInput` 点击时 `request_focus`，`is_focused()` 控制光标显示。

---

## 5 手势与拖拽

### 5.1 手势识别器

`event/gesture.h` 提供多指识别器：

| 识别器 | 说明 |
|:---|:---|
| `PinchRecognizer` | 双指捏合缩放；锁定两个 pointer id，跟踪 `m_initial_distance` / `m_current_distance` |
| `RotationRecognizer` | 双指旋转；锁定两个 pointer id，跟踪 `m_initial_angle` / `m_current_angle` |

`Modifier` 层提供 `.draggable(...)` 与 `.long_press(...)` 两个手势修饰节点（单指），由 `Draggable` / `LongPress` 驱动（见 [`07-environment-modifier.md`](07-environment-modifier.md)）。

### 5.2 拖拽

| 类型 | 说明 | 位置 |
|:---|:---|:---|
| `DragData` | 拖拽载荷，含 `mime_type`（`"text/plain"`、`"aurora/widget"` 或自定义） | `drag_drop.h:16` |
| `DragSession` | 一次拖拽会话，跟踪 `m_origin` 与 `m_active` | `drag_drop.h:39` |
| `DropTargetCallbacks` | 放置目标回调集 | `drag_drop.h:77` |

---

## 6 动画

### 6.1 核心类型

| 类型 | 说明 | 头文件 |
|:---|:---|:---|
| `AnimationController` | 驱动一条归一化进度（0→1）。`forward(from = -1)` 正向、`reverse()` 反向、`reset(t = 0)` 复位、`value()` 取进度。时长在构造 `AnimationController(duration_seconds, value)` 时确定，**无 `set_duration`** | `animator.h` |
| `AnimationStatus` | 动画状态枚举 `Dismissed`（进度 0）/ `Forward`（正向播放中）/ `Reverse`（反向播放中）/ `Completed`（进度 1）。`AnimationController::status()` 与 `AnimatedValue::status()` 提供，配套 `is_dismissed()` / `is_completed()` / `is_animating()` | `animator.h` |
| `Tween<T>` | 补间函数。`Tween<T>(a, b, curve)`，`value(t)` 按曲线在 a→b 间插值；支持 `int` / `float` / `Size` / `Point` / `Color` 等 | `timeline.h` |
| `Keyframes<T>` | 关键帧序列。`Keyframes<T>(stops)`，每帧 `Stop{time, value}`，**插值严格线性**；`value(t)` 在分段间插值 | `timeline.h` |
| `Curve` / `Curves` | 缓动曲线。`Curves::linear()` / `ease_in()` / `ease_out()` / `ease_in_out()` / `ease_in_out_cubic()` 等（无 `steps` 工厂） | `easing.h` |
| `SpringDescription` | 弹簧物理参数（`stiffness` / `damping` / `mass`），配合 `SpringSimulation` 用于物理感动画 | `spring.h` |
| `Animator` | 帧循环驱动器 | `animator.h` |
| `AnimatedValue<T>` | 「`State<T>` + `Tween` + 控制器」三合一句柄 | `animator.h` |
| `TweenAnimation<T>` | 自包含动画值：拥有自己的 `State<T>`，可独立推进。`animate_to(target, duration_s[, curve])` 起步、`tick(dt)` 每帧推进、`get()` 取当前值、`is_animating()`、`as_signal()` 交出内部 `State<T>` 供响应式绑定 | `animator.h` |

`timeline.h` 实际只提供 `lerp` 重载族（算术 / `Point` / `Size` / `Color` / `EdgeInsets` / `Rect`）、`Tween<T>` 与 `Keyframes<T>`；**「时间轴编排」类型（如 `Timeline`）为规划中（未实现）**。当前多段时间轴编排以 `Animator` + `Tween`/`Keyframes` 组合实现：用单条 `Keyframes<T>` 的 `Stop{time, value}` 序列表达多关键帧段，或经 `Animator::add_binding` 追加帧回调按 `AnimationController::status()` / `value()` 分段驱动。

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

**栈深上限守卫**：默认上限 `AURORA_DEFAULT_MAX_NAV_DEPTH = 32`（`navigator.h:23`，`inline constexpr std::size_t`），可经 `set_max_depth` 调整；`push` / `restore` 超过上限时经 `Diagnostics` 降级拒绝，避免无限深栈导致栈溢出 / 渲染雪崩。

**深层链接无 query 参数语义**：不引入 route-args 机制，名称段即全部信息。

### 7.3 Router

`Router`（`navigation/router.h`）是路由注册辅助类。当前未提供 `Router::with` 便捷工厂，请直接构造 `Navigator` 并 `push` / `pop` `Route`。

### 7.4 Hero 共享元素转场

`Hero`（`navigation/hero.h`）构造 `Hero(std::string tag, Node child)`。

同 `tag` 的源 / 目标 `Hero` 在 `Navigator` 转场期间被 `NavigatorHost` 配对，于 `TransitionLayer` 覆盖层上按转场进度 `lerp` 矩形 + 交叉淡变，形成「形变飞入」效果。

- **常态零开销**：仅当 `tag` 处于 morphing 时跳过自绘，由 `NavigatorHost` 经 `Provider<HeroRegistry>` 注入的注册表驱动。
- `NavigatorHost` 持有注册表并在转场期注入子树环境。
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

### 8.1 #8 显式优于隐式（含样式继承）

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
