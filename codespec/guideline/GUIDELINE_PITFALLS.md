# GUIDELINE_PITFALLS

> 本文件由 [`GUIDELINE.md`](../GUIDELINE.md) 划分而出（常见坑 / 状态选择指南 / 调试能力 / 自绘标题栏）。
> 返回主线见 [`GUIDELINE.md`](../GUIDELINE.md)。
>
> 片段约定：本文所有片段使用 `namespace au = aurora;`（即 `au::` 前缀）。复制任意单段时，请确保该别名（或 `using namespace aurora;`）已在所处 TU 声明，否则 `au::Xxx` 编译失败。

**本文包含章节：**

- [常见坑](#常见坑)
- [21. 状态选择指南（State vs Store vs Binding vs Computed）](#21-状态选择指南state-vs-store-vs-binding-vs-computed)
- [22. 调试能力（真实后端 DEBUG，AURORA_ENABLE_DEBUG）](#22-调试能力真实后端-debugaurora_enable_debug)
- [23. 自绘标题栏配方（TitleBar）](#23-自绘标题栏配方titlebar)

## 常见坑

- **链式调用必须用 `std::move`**：`std::move(au::Text("x").font_size(14))`。
  链式返回基类引用会丢失派生类型，直接进 `Node` 会切片。
- **多数情况无需写 `Node{...}`**：`Node(W&&)` 是非 explicit 转换构造函数，值类型控件
  （`au::Text{...}`、`au::Column{...}`、`std::move(w)` 等）可隐式转为 `Node`。仅当两分支类型
  不同的 `?:` 三元、或 `std::make_shared<Widget>(...)` 这类需连续两次用户转换的场景，才显式包 `Node{...}`。
- **`ImageView` 而非 `Image`**：`core::Image` 是解码后的像素数据，`au::ImageView` 是 widget。
- **强类型尺寸**：`width(px(100))` 合法；`width(100)` 编译失败（无 `Length(int)` 隐式转换）。

---

## 21. 状态选择指南（State vs Store vs Binding vs Computed）

> **决策树**（500 token 内选出正确模式）：

```
值的使用范围？
├─ 仅本控件 ──────────► State<T>
├─ 父子/兄弟共享 ──────► 提升到公共祖先 State<T> + Binding<T> 下发
├─ 跨不相关子树 ───────► Store<S> + Environment 注入
└─ 纯派生值 ───────────► Computed<T>
```

### 21.1 组件内 State

```cpp
// Checkbox 自持 checked，外部无需感知
auto checked = std::make_shared<au::State<bool>>(false);
au::Checkbox cb{ au::Reactive<bool>{ checked } };
```

### 21.2 状态提升 + Binding

```cpp
// 父组件持有 State，两个子 Checkbox 共享
auto flag = std::make_shared<au::State<bool>>(true);
au::Checkbox a{ au::Binding<bool>{ *flag } };
au::Checkbox b{ au::Binding<bool>{ *flag } };
flag->set(false); // a、b 同时刷新
```

### 21.3 Store 集中管理

```cpp
struct Cart { std::vector<std::string> items; };
auto cart = au::make_store<Cart>(Cart{},
    [](const Cart &s, const au::Action &a) -> Cart {
        Cart n = s;
        if (a.type == "add")
            if (auto *v = a.payload_as<std::string>()) n.items.push_back(*v);
        return n;
    });
cart->dispatch(au::Action{ "add", std::string("Book") });
// 任意子树经 cart->as_signal() 订阅
```

### 21.4 Environment 注入

```cpp
auto theme = au::Theme::light();
theme.primary = au::colors::Blue;
Node root = au::ThemeProvider{ theme,
    au::Button(au::ButtonProps{ .label = "按钮" }) };
// 子组件内：const Theme* t = ctx.environment<Theme>();
```

`WindowChrome` 是另一类经 Environment 注入的服务（窗口 chrome：移动 / 缩放 / 最小化 / 最大化 / 全屏 / 关闭，自绘标题栏消费方）。**它由 `Window::present_root` 在挂载根树时自动注入根环境**（`window.h`：`m_root_env.set<WindowChrome>(WindowChrome{ &*m_surface })`），无需手动提供；子树控件在事件派发栈内经 `ctx.environment<WindowChrome>()` 同步取用：

```cpp
// 自定义标题栏按钮：消费注入的 WindowChrome（headless 下 env 为空 → 安全 no-op）
auto* chrome = ctx.environment<WindowChrome>();
if (chrome && chrome->valid()) {
    chrome->toggle_maximize();   // 经 Surface 驱动真实窗口动作（Wayland serial 时效约束：仅事件栈内调用）
}
```

> 跨不相关子树的共享状态（如购物车 `Store`、主题、`WindowChrome`）统一走 Environment 注入 + `ctx.environment<T>()` 取用；注入方须保证 `T` 的生命周期长于消费方（见 §21 决策树）。

### 21.5 Computed 派生

```cpp
auto src = std::make_shared<au::State<std::vector<std::string>>>(
    std::vector<std::string>{"Apple", "Banana", "Avocado"});
auto kw = std::make_shared<au::State<std::string>>(std::string("A"));
auto filtered = std::make_shared<au::Computed<std::vector<std::string>>>(
    [src, kw]() -> std::vector<std::string> {
        std::vector<std::string> r;
        for (const auto &s : src->get())
            if (s.find(kw->get()) != std::string::npos) r.push_back(s);
        return r;
    });
// kw->set("B"); → filtered 自动重算
```

### 生命周期速查

| 类型          | 所有权            | 清理                 |
|---------------|-------------------|----------------------|
| `State<T>`    | `shared_ptr` 或栈 | 随引用计数 / 作用域  |
| `Store<S>`    | `shared_ptr` 全局 | 进程退出             |
| `Binding<T>`  | **非拥有**        | 上游须更长存活       |
| `Computed<T>` | 自管理            | 依赖源析构后自动摘除 |

> **注意**：`Binding` 不拥有上游。若子组件生命周期可能超过父 `State`，改用 `shared_ptr<State<T>>` 共享所有权。

---

## 22. 调试能力（真实后端 DEBUG，AURORA_ENABLE_DEBUG）

> 真实后端的运行时画面与状态抓取能力（设计取舍见 `../architecture/ARCHITECTURE_PERF.md` §10.7，API 契约见 `SPECIFICATIONS.md` §H.10c 与 `BUILD_OPTIONS.md` `AURORA_ENABLE_DEBUG`），弥补 golden 测试只能覆盖 `HeadlessSurface` 的盲区。
> 全部门控 `AURORA_ENABLE_DEBUG`（Debug / RelWithDebInfo 自动注入，Release / MinSizeRel 不注入）；头经 `aurora/aurora.h` 暴露，消费端调用始终可编译，关闭时 API 返回 `disabled` / `{"available":false,...}`， **零开销**。
> 开启 `AURORA_BUILD_INSPECTOR_SERVER`（跨平台）时，下列能力经 `InspectorServer` 的 `/api/debug/*` REST 端点远程暴露（见 §17b）。

### 22.1 帧缓冲 / 真实窗口截图

```cpp
au::debug::set_output_directory("./debug_shots");   // 相对文件名落入此目录；空串复位默认 ./aurora_debug
au::debug::capture(surface, "frame.png");           // 软件帧缓冲 → ./debug_shots/frame.png
au::debug::capture(surface, "win.png",
                   au::debug::CaptureSource::OnScreenWindow); // 真实屏幕窗口（含标题栏；Wayland/Headless 不支持）
// 显式带目录的路径原样使用（不落入输出目录）：
au::debug::capture(surface, "/abs/path/frame.png");
```

> Release（`AURORA_ENABLE_DEBUG=OFF`）下 `capture` 返回 `Error{GeneralNotSupported}`，不写盘。像素与 `Painter` 输出逐位一致（Headless 下可由 golden 比对验证）。

### 22.2 可视化调试叠层（DebugPaintFlags）

```cpp
au::debug::set_flags({
    .layout_guides       = true,  // render box 边框 / 对齐参考（对齐 Flutter debugPaintSizeEnabled）
    .relayout_boundaries = true,  // 重排边界框（复用 Widget::is_relayout_boundary()）
    .layer_borders       = true,  // 离屏缓存层（含 cache_layer() 修饰）边框
    .repaint_highlight   = true,  // 本帧实际重绘的控件循环色高亮（rainbow）
    .overdraw            = true,  // 控件粒度过度绘制热力图（paint_bounds 半透明叠加）
});
// present_root 下一帧即在「全树绘制后」独立遍历叠加；如需关闭：
au::debug::set_flags({});  // 全部复位为 false
```

> 叠层由 `Window::present_root` 在全树 `paint` 之后统一绘制（ **不侵入** `Widget::paint`，避免 DisplayList replay 漏画）；任一 flag 开启才进入叠加分支。Release 下 `set_flags` 为 no-op、`any_flag_enabled()` 恒 false，paint 路径零开销。

### 22.3 控件拾取（点击选 widget）

```cpp
auto res = au::debug::widget_picker(root_widget, root_bounds, au::BuildContext{}, au::Point{x, y});
if (res.hit) {
    for (auto &node : res.chain) { /* node.type_name, node.bounds —— chain[0]=根，末元素=最深命中控件 */ }
}
```

> `root_widget` 须为 **非 const** `Widget&`（命中测试经 `hit_test_chain` 进入各控件非 const 虚函数链）；`root_bounds` 为根全局盒（通常 `Rect{Point{0,0}, surface.size()}`），`Point{x,y}` 取窗口逻辑 dp。Release 下返回 `{ {}, false }`。

### 22.4 运行时信息导出（JSON）

```cpp
au::Json tree = au::debug::widget_tree(root_node);        // Widget 树完整结构（type + props + children）
au::Json perf = au::debug::perf_snapshot();               // FPS / 帧时间 / 掉帧 / PerfLog 快照
au::Json tl   = au::debug::frame_phase_timeline(64);      // layout/paint/present 相位均值 + 近帧 + ASCII flamegraph
au::Json why  = au::debug::why_trace(64);                 // 重排/重绘因果链（propagated 区分根因与父链传播）
au::Json dg   = au::debug::diagnostics();                 // 运行时诊断只读快照（severity/category/message/...）
au::Json st   = au::debug::surface_state(surface);        // Surface 状态（size / scale_factor / frame_count / ...）
```

> 上述均为 `aurora::debug` 门面对既有引擎（`Inspector` / `FrameStats` / `PerfLog` / `Diagnostics`）的 **薄封装 / 聚合**，生产子系统留原地、公共契约不变。Release 下各 API 返回 `{"available":false, "reason": ...}`。

### 22.5 渲染纯度守卫（捕获「绘制中读全局时钟」）

`Widget::paint()` 首行自动挂接渲染纯度检查：进入 paint 时 `g_paint_depth` 深度计数器 +1（用深度而非 bool，以正确覆盖 `drawer.cpp` 内部递归 paint 与直接 `paint()` 的测试），paint 结束后归零。`current_timestamp()` 等全局可变时钟访问器内建 `AURORA_ASSERT(!g_paint_depth)` 守卫——若控件在 `on_paint` 中读取全局时钟（反模式：让绘制结果依赖每帧时刻，破坏脏区重绘与 Display List 录播一致性）， **Debug 构建立即触发断言**。

```cpp
// ❌ 反例（Debug 下 AURORA_ASSERT(!g_paint_depth) 触发）：在 on_paint 内读全局时钟
auto MyWidget::on_paint(Painter &p, const Rect &r, const BuildContext &ctx) -> void {
    uint64_t now = current_timestamp();   // 绘制结果随每帧时刻变化 → 脏区/录播失真
    p.fill_rect(r, color_with_alpha(now % 255));
}

// ✅ 正例：时间源由外部帧驱动 / 注入 BuildContext，on_paint 只负责绘制
auto MyWidget::on_paint(Painter &p, const Rect &r, const BuildContext &ctx) -> void {
    p.fill_rect(r, ctx.theme().accent);   // 不读全局时钟，绘制结果确定
}
```

> 纯度机制全程 `AURORA_ENABLE_DEBUG` 门控：Release 构建编译剥离、`g_paint_depth` / `PaintPurityGuard` / `check_render_purity()` 仅 Debug 可见， **零运行时开销**。

### 22.6 调试 API 目录（自动生成，勿手改）

`aurora::debug` 命名空间下的全部公共自由函数由 `tools/gen_debug_api.cpp` 从声明源 `codespec/debug_api.toml` 自动生成到 `aurora_api.json` 的 `"debug"` 段（ **单一权威目录**）。新增 / 改名调试函数时， **只改 `debug_api.toml`**，再重跑生成器即可，无需在本章手列：

```bash
cmake --build build --target gen_debug_api_json   # 读 debug_api.toml → 更新 aurora_api.json 的 debug 段
python tools/check_gen_api_merge.py build         # 回归：损坏现有文件不截断、merge 保留其它段
```

生成器为 merge-only：读现有 `aurora_api.json` 的全部其它段（`widgets` / `enums` / `error_codes` / ...），仅覆盖 `debug` 段写回；现有文件 **损坏时直接报错退出、绝不写空对象**，避免截断丢失其它段（历史陷阱）。`gen_error_codes` / `gen_api_tools` 亦已加固同样的防护并保留 `debug` 段。

> `aurora_api.json` 的 `debug` 段条目格式：`{ name, since, gated, signature, summary, header }`，供工具链 / LSP / 文档消费。

## 23. 自绘标题栏配方（TitleBar）

```cpp
au::WindowOptions opts;
opts.size  = au::Size{ 720, 480 };
opts.title = "文档";
opts.style.decoration = au::DecorationPolicy::Borderless;   // 移除系统标题栏（CSD 接管）

au::TitleBar bar;
bar.set_title("文档")                            // 主标题
   .set_subtitle("自动保存于 12:00")              // 副标题（可选）
   .add_action({ "设置", []() -> void { /* … */ } })         // 动作区按钮
   .add_snap_action({ "左半屏", []() -> void { /* … */ } }); // 追加进 Snap 弹窗的自定义项

au::Node root = au::Column{
    au::Node{ std::move(bar) },                  // 标题栏置于首行
    au::Text{ "正文…" },
};
// create_native_window(opts) 后正常挂载 root 即可。
```

控制钮（最小化/最大化还原/关闭）、拖拽移动与双击最大化经 Environment 注入的 `WindowChrome` 服务自动生效
（headless 下安全 no-op）；悬停最大化钮 ≥400ms 触发 Snap 动作弹窗。风格经 `TitleBarStyle` 三预设
（`adwaita_dark()` / `adwaita_light()` / `windows_dark()`）或 `Surface::set_title_bar_style()` 调整。

