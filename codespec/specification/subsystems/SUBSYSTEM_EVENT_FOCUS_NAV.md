# H.6 事件响应链 + H.7 焦点 + H.8 导航

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.6**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.6 事件与响应链（Response Chain）

核心目标：统一、可预测的事件派发；命中即消费，父链可拦截。

- **`Event`**（基类）：所有事件含 `bool handled` 标志；`handled=true` 表示已被消费，停止向上冒泡。
- **具体事件**：`MouseEvent{action, position, local_position}`（`position` = 全局窗口坐标，由 Surface 后端写入；
  `local_position` = 派发器按命中链 `origin` 本地化后的 **控件本地坐标**，控件在 `on_pointer_event`中应消费此字段；Press /
  Move / Release）、`KeyEvent{key, action, modifiers}`、`ScrollEvent{delta_y}`、`TextInputEvent{text}`。
- **`Widget::on_pointer_event / on_key_event / on_scroll / on_text_input`**：组件处理方法，参数为 **非 const `Event&`**，可写
  `e.handled = true` 消费。
- **`EventDispatcher`**：`dispatch(MouseEvent, FocusManager* = nullptr)` / `dispatch(KeyEvent, FocusManager&)` /
  `dispatch(ScrollEvent)` / `dispatch(TextInputEvent, FocusManager&)`。先经 `Widget::hitTest` 找到最深命中的目标组件，再沿父链向上调用处理方法，直到
  `handled` 或到根。
- **命中测试**：`Widget::hitTest(local, bounds)` 返回命中的子节点（`local` 相对本组件原点，`bounds` 绝对矩形）。MVP 默认命中目标即止，
  `handled` 由组件决定是否消费。
    - **z 序语义（与绘制一致）**：容器（如 `Stack`/`Overlay` 等有重叠子节点的场景）的命中测试 **反向**
      遍历子节点——最后绘制（视觉最上层）的子节点优先命中。即重叠区域中「视觉在上层」的控件优先接收事件，底层控件不会在重叠区抢走本应属于顶层控件的命中（修复「顶层控件被底层控件遮挡」）。非重叠布局（普通
      `Row`/`Column`）各子节点区域互斥，遍历方向不影响结果。
- **坐标契约**：`on_pointer_event(MouseEvent&)` 入参 `e.local_position` 已是相对当前控件左上角的 **本地坐标**——由
  `EventDispatcher` 在命中链冒泡时，按 `hit_test_chain` 携带的控件 `origin`（相对根）换算
  `local_position = position - origin` 后写入；控件 **无需、也无法**再查询自身在树中的绝对位置（`Widget` 已无几何缓存）。
  `e.position` 仍保留为全局窗口坐标供需要时读取。

```cpp
// 写 handled 即可消费事件，阻止向上传递
void MyWidget::on_pointer_event(MouseEvent& e) {
    if (e.action == MouseAction::Press) {
        do_press();
        e.handled = true;     // 命中即消费
    }
}
```

#### #H.7 焦点管理（FocusManager）

核心目标：键盘导航（Tab / Shift+Tab）、焦点高亮、输入控件获焦可预测。

- **`FocusManager`**：持 `m_root` 与 `m_focused`；`setRoot(Widget*)`、`setFocus(Widget*)`、`moveFocus(FocusDirection)`、
  `focused()`。 **焦点控件的生命周期契约**：`FocusManager` 以裸指针记录焦点控件， **不拥有**它；焦点控件常在自身被
  重建/回收后仍留在记录里（如输入框所在页面被 `push_replacement` 换掉）。故内部与 `HitNode` 同构地 附带弱引用守卫（构造时探测是否由
  `shared_ptr` 持有，栈/成员控件回退为裸指针），`focused()` /
  `has_focus()` / `set_focus()` / `move_focus()` 一律经存活视图取用：焦点控件已被回收时 `focused()`
  返回 `nullptr`，键盘/文本派发据此安全返回 `false`，绝不对已释放内存做虚调用。
- **`FocusDirection`**：`Forward / Backward / Up / Down / Left / Right`。
- **组件焦点接口**（`Widget`）：`focusable()`、`setFocusable(bool)`、`tabIndex()`、`setTabIndex(int)`、`isFocused()`、
  `requestFocus()`（读派发期线程局部「当前焦点管理器」请求，控件自身不持久持有 `FocusManager*`）、`onFocusChange(bool)`
  （虚钩子，基类维护 `m_isFocused`）。
- **焦点管理器「随派发可得」**：`EventDispatcher::dispatch(MouseEvent, FocusManager* = nullptr)` 在派发期经线程局部暴露「当前焦点管理器」（
  `current_focus_manager()`），`requestFocus()` 读之，无需在每控件上递归注入；无焦点管理器（`nullptr`）时 `requestFocus` 静默
  no-op。`moveFocus` 按 `tabIndex` 稳定排序后循环取前 / 后一个；Tab / Shift+Tab 由
  `EventDispatcher::dispatch(KeyEvent, FocusManager&)` 识别并转 `moveFocus`。
- **Press 焦点归属与点击失焦（blur）契约**：鼠标 / 触控 `Press` 时，焦点归属为
  **命中链上自最深命中向根找到的第一个可获焦控件**（点到不可获焦的装饰子控件时归属其可获焦祖先，如按钮内图标）；
  **整条链都不可获焦**（点到纯展示容器）或 **命中链为空**（点到根外空白）均视为点击空白——`FocusManager` 清焦点（
  `set_focus(nullptr)`），旧焦点控件收到 `on_focus_change(false)`（`Text` 据此清除选区高亮，`TextInput` 据此隐藏光标）。鼠标与触控（
  `TouchDispatcher`）路径行为一致；`Release` 不切换焦点（避免拖选结束落在别处时选区被清，见 #H.10 指针捕获）。回归：
  `test_text_focus_clear`。
- **`TextInput`** 点击 `requestFocus`，`isFocused()` 控制光标显示。

```cpp
// 构造输入控件；焦点变化经 TextInput::on_focus_change 虚钩子处理（需自定义子类覆写，非可赋值回调）
au::TextInput(au::TextInputProps{ .placeholder = "Name" });
```

#### #H.8 导航（Navigator / Route / Router）

核心目标：声明式页面栈与转场，AI 用"推 / 弹"思维管理界面流。

- **`Route`**：一条路由。构造 `Route(Node root, std::string name = "", RouteTransition transition = {})`；`root()` 返回该页
  UI 子树，`name()` 返回路由名。
- **`Navigator`**：页面栈控制器。`push(Route)`、`pop()`、`pop_replacement(Route)`、`pop_to_root()`、
  `set_on_route_changed(cb)`、`current_root()`；构造 `Navigator(Route)` 以初始路由。新增 **深层链接**：
  `open_uri(const std::string& uri, const std::function<Route(const std::string&)>& build)` 按 `/` 切分名称序列（丢弃空段）后委托
  `restore(names, build)` 重建路由栈；并提供轻量 `RouteRegistry`（
  `std::map<std::string, std::function<Route(const std::string&)>>`），`open_uri(uri, registry)` 直接查表构建（表中缺失的名称段被跳过，不引入
  query/route-args 机制）。
- **`Router`**：路由注册辅助类（位于 `navigation/router.h`；当前未提供 `Router::with` 便捷工厂，请直接构造 `Navigator` 并
  `push`/`pop` `Route`）。
- **`Hero`**（位于 `navigation/hero.h`）：共享元素转场控件，构造 `Hero(std::string tag, Node child)`。同 `tag` 的源/目标
  `Hero` 在 `Navigator` 转场期间被 `NavigatorHost` 配对，于 `TransitionLayer` 覆盖层上按转场进度 `lerp` 矩形 +
  交叉淡变「形变飞入」；常态零开销（仅当 `tag` 处于 morphing 时跳过自绘，由 `NavigatorHost` 经 `Provider<HeroRegistry>`
  注入的注册表驱动）。`NavigatorHost` 持有注册表并在转场期注入子树环境，配对缺失（仅旧页或仅新页有该 tag）时退化为普通淡入淡出。

```cpp
au::Navigator nav{ au::Route{ build_home(), "home" } };
nav.push(au::Route{ build_detail(id), "detail" });  // 跳转
nav.pop();                                            // 返回

// 深层链接：URI 字符串重建整栈（无 query 参数语义）
nav.open_uri("home/detail/42", [](const std::string& name) { return build_route(name); });

// 共享元素转场：两页各放同 tag 的 Hero，转场自动「形变飞入」
au::Hero("logo", au::Text("Aurora"));   // Hero(tag, Node)；Text 直接作为子节点，无需 Container 包裹
```

> 当前为 MVP 页面栈，已支持共享元素转场（`Hero` + `TransitionLayer`，§H.8）与深层链接（`Navigator::open_uri`，§H.8）。
