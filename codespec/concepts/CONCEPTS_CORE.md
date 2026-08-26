# CONCEPTS_CORE

> 本文件由 [`CONCEPTS.md`](../CONCEPTS.md) 划分而出（核心概念审计：全部可枚举 UI 原语 #1–#19 统一枚举）。章节编号保持原样。
> 返回主线见 [`CONCEPTS.md`](../CONCEPTS.md)。

**本文包含章节：**

- [一、核心概念审计](#一核心概念审计)

## 一、核心概念审计

目标：Aurora 的全部 UI 原语可枚举、可命名、可映射，统一审计，便于 AI 检索与迁移训练。 以下为当前库的真实原语盘点与归类（头文件见
`include/aurora/`）。

> **枚举说明**：下表 #1–#19 为 Aurora 全部可枚举 UI 原语

| #  | 概念                             | 成员（类名） | 头文件 |
|----|----------------------------------|--------------|--------|
| 1  | **Widget（原子控件）**           | `Text` `TextInput` `Button` `Image`(`ImageView`) `Checkbox` `Switch` `ProgressIndicator` `Slider` `Canvas` `Skeleton` `VideoPlayer` `VideoControls` `BottomNavBar` | `widget/text.h` `widget/text_input.h` `widget/button.h` `widget/image_widget.h`(`ImageView`) `widget/checkbox.h` `widget/switch.h` `widget/progress.h` `widget/slider.h` `widget/canvas.h` `widget/skeleton.h` `widget/bottom_nav_bar.h` |
| 2  | **Container（多子布局）**        | `Column` `Row` `Stack` `Grid` `GridView` `Scroll` `Spacer` `Repeater` `LazyList` `LazyRow` | `widget/containers.h` `widget/scroll.h` `widget/spacer.h` `widget/grid_view.h` `widget/repeater.h` `widget/lazy_list.h` `widget/lazy_row.h` |
| 3  | **Modifier（声明式装饰）**       | `Padding` `Background` `Border` `Clip` `Opacity` `SizeModifier` `FlexWeight` `Clickable` `Align` `Offset` `Blur`(blur/backdrop_filter/shadow) `BlendMode` `ShaderMask` `CacheLayer` `Draggable` `LongPress` `Rotate` `Scale` `Transform` | `modifier/modifier.h` `render/blend.h` |
| 4  | **Control Flow（组合/条件）**    | `Show` `Timer`（定时任务） | `widget/show.h` `widget/provider.h` `environment/*.h` `widget/timer.h` |
| 5  | **State（响应式状态）**          | `State<T>` `Binding<T>` `Store<T>` `Reactive<T>`(响应式包装，供 `ui::` 工厂双模入参) | `state/state.h` `state/binding.h` `state/store.h` |
| 6  | **Derived（派生值）**            | `Computed<T>` `Effect` | `state/computed.h` `state/effect.h` |
| 7  | **Signal（细粒度订阅）**         | `Signal` / `Subscription`（`SignalViewBase`） | `state/signal_view.h` |
| 8  | **Theme（主题）**                | `Theme` `ThemeProvider` `colors` | `theming/theme.h` `widget/provider.h` |
| 9  | **Environment（跨树注入）**      | `Environment` `Provider<T>` `LocaleProvider` `MediaQueryProvider` `MediaQuery` | `environment/*.h` |
| 10 | **Navigation（路由）**           | `Navigator` `Route` `RouteRegistry` `Transition` `RouteTransition` `Hero`（共享元素转场） `open_uri`（深层链接） | `navigation/navigator.h` `navigation/route.h` `navigation/hero.h` |
| 11 | **Layout（布局引擎）**           | `Layout` `Constraints` `EdgeInsets` flex/grid 求解（含 `RelayoutBoundary` 重排边界属性 `Widget::is_relayout_boundary()`；paint 侧孪生 `RepaintBoundary` ≈ `CacheLayer` 修饰） | `layout/*.h` `core/types.h` |
| 12 | **Event（事件）**                | `MouseEvent` `KeyEvent` `ScrollEvent` `TextInputEvent` `EventDispatcher` `FocusManager` | `event/event.h` `event/dispatcher.h` `event/focus.h` |
| 13 | **Animation（动画）**            | `Tween<T>` `Keyframes<T>` `Curve` `Animator` `SpringSimulation` `AnimationController` | `animation/*.h` |
| 14 | **Platform Shell（平台 Shell）** | `FileDialog`(open_file/save_file/open_folder) `SystemTray`(show/hide/set_icon/show_balloon/on_activate) `Clipboard`(set_text/get_text/set_image/get_image) `FileDropEvent`+`Widget::on_file_drop`(OS 文件拖放) `Display`+`app::list_displays`/`primary_display`/`display_containing`/`move_window_to_display`(多显示器枚举与窗口迁移) | `app/file_dialog.h` `app/system_tray.h` `app/clipboard.h` `app/display.h` `event/event.h`(`FileDropEvent`)；Win32 经 `file_dialog_win32.cpp`/`system_tray_win32.cpp`/`display_win32.cpp` + `window/win32_window.cpp`(`WM_DROPFILES`) 真实实现；非 Win32/Headless 为 no-op/headless 钩子 |
| 15 | **Accessibility（无障碍）**      | `AccessibilityNode` `AccessibilityRole` `AccessibilityAction` `infer_accessibility_role` `build_accessibility_tree` | `core/accessibility.h` |
| 16 | **DevTools（开发工具）**         | `HotReload` `generate_ui` `validate_ui` `Diagnostics`(report/warn/degraded) `inspect`(dump_tree/query/get_state) `Logger`(日志设施) | `app/hot_reload.h` `app/generate_ui.h` `app/validate_ui.h` `core/diagnostics.h` `widget/inspect.h` `core/log.h` |
| 17 | **Render（渲染）**               | `Painter` `Surface` `HeadlessSurface` `Win32Surface` `GlfwSurface` `X11Surface` `WaylandSurface` `MacOSSurface` `WasmSurface` `create_window`(工厂) `auto_detect_surface` | `render/painter.h` `window/*.h` `window/window.h` |
| 18 | **Result/Error（错误）**         | `Result<T>` `Error` | `core/result.h` `core/error.h` `core/log.h` |
| 19 | **Lifecycle / Window（生命周期）** | `Lifecycle`(控件挂载/卸载副作用) `WindowState`(Visible/Occluded/Hidden) `WindowMode`(Normal/Maximized/Minimized/FullScreen) | `widget/lifecycle.h` `window/window_state.h` |

> **注册状态说明**：`LazyList` / `LazyRow` / `BottomNavBar` 已接入 `register_core_widgets()`（JSON 工厂名 `"LazyList"`/`"LazyRow"`/`"BottomNavBar"`，C++ 便利构造器 `ui::lazy_list(...)`/`ui::lazy_row(...)`/`ui::bottom_nav_bar(...)`）；`Skeleton`（行 1）与 `GridView`（行 2）已有头文件与类型，但 **尚未接入 `register_core_widgets()`
序列化工厂**，暂不可经 JSON 反序列化还原（GridView 渲染越界崩溃已修复：新增 `push_clip(bounds)`/`pop_clip()`，回归用例 `tests/test_grid_view.cpp`）；`X11Surface` / `WaylandSurface`（行 17）的 Linux 桌面双后端
> **已完整实现并经真实开窗验证**（`AURORA_BACKEND_X11` 需 libX11；`AURORA_BACKEND_WAYLAND` 需
wayland-client/xkbcommon/wayland-protocols；默认均 `OFF`，可单开或同时开启，同时开启时按会话类型运行期择优）；
> `MacOSSurface` / `WasmSurface`（行 17）的 **CMake 开关已接入**（默认 `OFF`）：`AURORA_BACKEND_MACOS` / `AURORA_BACKEND_WASM`
> 已加入 CMake 并激活 `native_surfaces.h` 的 `#ifdef`：`WasmSurface::present()` 的 `<canvas>` 像素写回**已实现**（`wasm_surface.h` 内嵌 EM_ASM
`putImageData` 胶水 + 事件翻译），仅 Emscripten 构建/链接验证待补全；`MacOSSurface` 的 Cocoa 窗口实现仍待平台工具链补全（roadmap）。`D3D11Surface` 已通过 `AURORA_BACKEND_D3D11` 开关可用。

> **重叠能力优先级判定**：当同一视觉能力同时存在于「固有属性」与「Modifier」时，判定边界如下——
> - 控件**自身身份语义**的能力（如 `Button` 的 `background_color`、`corner_radius`、`padding`）→ 用**固有属性**优先：随控件序列化、可被 Inspector 枚举、参与 diff。
> - 给**任意控件临时套一层**通用装饰（如给 `Image` 加 `Padding`、给 `Text` 加 `Background`）→ 用 **Modifier**：正交、可叠加、可 `Reactive` 变化，且不污染控件身份。
> - 若控件固有属性已提供该能力，则 **Modifier 同类项不再重复写**，仅作为「跨控件通用兜底」。

| 重叠能力 | 固有属性（控件内） | Modifier（正交链） | 判定边界 |
|----------|--------------------|--------------------|----------|
| `padding` | `XxxProps::padding`（随控件序列化） | `Padding` 修饰 | 控件自带留白用固有；跨控件统一留白用 Modifier |
| `corner_radius` | `XxxProps::corner_radius` | `Clip`/`Border` 修饰 | 控件圆角用固有；给任意矩形切圆角用 Modifier |
| `background_color` | `XxxProps::background_color` | `Background` 修饰 | 控件底色用固有；叠加高亮/状态色用 Modifier |

### 状态作用域决策树（AI 消费规则）

> 面向 AI 编码助手的快速决策规则： **在 500 token 内正确选择 State vs Store vs Binding vs Computed**。

```
问：该值的使用范围？
│
├─ 仅本控件内部使用 ──────────────────► State<T>（组件内状态）
│
├─ 父子 / 兄弟共享 ───────────────────► 提升到最近公共祖先的 State<T> + 经 Binding<T> 下发给子组件
│
├─ 跨不相关子树（全局 / 跨页面） ─────► Store<S> + Environment 注入
│
└─ 纯派生值（可由其他状态计算） ──────► Computed<T>（不存储，纯函数计算）
```

#### 生命周期规则

| 类型          | 所有权                               | 生命周期                | 清理时机                                  |
|---------------|--------------------------------------|-------------------------|-------------------------------------------|
| `State<T>`    | 通常 `shared_ptr` 或栈上             | 随创建者管理            | 栈上随作用域；`shared_ptr` 随引用计数归零 |
| `Store<S>`    | 通常 `shared_ptr` 全局单例           | 进程级（或应用级）      | 进程退出 / `shared_ptr` 释放              |
| `Binding<T>`  | **非拥有**（裸指针指向上游 `State`） | 上游 `State` 须更长存活 | 无自身清理；上游析构后不可再访问          |
| `Computed<T>` | 自管理（内部持有 `Effect`）          | 依赖源全部存活即可      | 依赖源析构后 Effect 自动惰性摘除          |

> **关键约束**：`Binding` 不拥有上游——传递给子组件时， **父组件须保证 `State` 的存活期 ≥ 子组件**。
> 若子组件可能独立于父组件销毁，应改用 `shared_ptr<State<T>>` + `State::shared()` 共享所有权。

#### 反例：Binding 跨子树独立存活（补充 F13）

> 当子组件可能独立于父组件销毁（如经 `Navigator` push 的页面、或异步加载的面板），裸 `Binding<T>` 借用的上游 `State` 会悬空。此场景应改用 `shared_ptr<State<T>>` 共享所有权：

```cpp
// 错误：子页面关闭后 flag 随父析构，Binding 悬空
// au::Binding<bool>{ *parent_flag }   // parent_flag 为栈上/短生命周期 State

// 正确：以 shared_ptr 共享所有权，引用计数保活
auto flag = std::make_shared<au::State<bool>>(true);
// 经 Environment 注入或参数下发 shared_ptr；子组件用 Reactive 包装
push_route(au::Checkbox{ au::Reactive<bool>{ flag } }); // flag 存活期 = 最长引用者
```

#### 示例 1：组件内 State —— Checkbox 的 checked 状态

```cpp
// C++：Checkbox 内部自持 checked 状态，外部无需感知
auto checked = std::make_shared<au::State<bool>>(false);
au::Checkbox cb{ au::Reactive<bool>{ checked } };
// checked 随父组件析构自动释放
```

```json
{
  "type": "Checkbox",
  "checked": false
}
```

#### 示例 2：状态提升 + Binding —— 父组件持有 State，两个子组件共享

```cpp
// C++：父组件持有 State，两个子 Checkbox 通过 Binding 共享同一值
auto shared_flag = std::make_shared<au::State<bool>>(true);
// 子组件 A：经 Binding 读取/写回
au::Checkbox cb_a{ au::Binding<bool>{ *shared_flag } };
// 子组件 B：同样绑定到同一上游
au::Checkbox cb_b{ au::Binding<bool>{ *shared_flag } };
shared_flag->set(false); // cb_a 与 cb_b 同时刷新
```

```json
{
  "type": "Column",
  "children": [
    {
      "type": "Checkbox",
      "checked": true
    },
    {
      "type": "Checkbox",
      "checked": true
    }
  ]
}
```

#### 示例 3：Store 集中管理 —— 购物车状态

```cpp
// C++：Redux 式 Store，跨不相关子树共享
struct Cart { std::vector<std::string> items; };
auto cart = au::make_store<Cart>(
    Cart{},
    [](const Cart &s, const au::Action &a) -> Cart {
        Cart next = s;
        if (a.type == "add") {
            if (auto *name = a.payload_as<std::string>()) next.items.push_back(*name);
        } else if (a.type == "clear") {
            next.items.clear();
        }
        return next;
    }
);
cart->dispatch(au::Action{ "add", std::string("Aurora Book") });
// 任意子树经 cart->as_signal() 订阅变化
```

```json
{
  "type": "Store<Cart>",
  "state": {
    "items": [
      "Aurora Book"
    ]
  }
}
```

#### 示例 4：Environment 注入 —— 主题色

```cpp
// C++：主题经 Provider 注入子树，子组件经 BuildContext 读取
auto theme = au::Theme::light();
theme.primary = au::colors::Blue;
Node root = au::ThemeProvider{ theme,
    au::Button(au::ButtonProps{ .label = "主题按钮" })
};
// 子组件内部：const Theme* t = ctx.environment<Theme>(); // 最近祖先优先
```

```json
{
  "type": "ThemeProvider",
  "theme": {
    "primary": "#0000FF"
  },
  "child": {
    "type": "Button",
    "label": "主题按钮"
  }
}
```

#### 示例 5：Computed 派生 —— 过滤后的列表

```cpp
// C++：Computed 自动追踪依赖，source 或 keyword 变化时重算
auto source = std::make_shared<au::State<std::vector<std::string>>>(
    std::vector<std::string>{"Apple", "Banana", "Avocado", "Cherry"});
auto keyword = std::make_shared<au::State<std::string>>(std::string("A"));
auto filtered = std::make_shared<au::Computed<std::vector<std::string>>>(
    [source, keyword]() -> std::vector<std::string> {
        std::vector<std::string> result;
        const auto &kw = keyword->get();
        for (const auto &s : source->get()) {
            if (s.find(kw) != std::string::npos) result.push_back(s);
        }
        return result;
    }
);
// filtered->get() == {"Apple", "Avocado"}
// keyword->set("Ch"); → filtered 自动重算 → {"Cherry"}
```

```json
{
  "type": "Computed",
  "value": [
    "Apple",
    "Avocado"
  ]
}
```

---
