# H.1–H.2 响应式信号系统 + Modifier 修饰系统

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.1–H.2**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

### H. 核心子系统 API 规范

> 本章为核心子系统 API 规范，与 `specification/features/FEATURE_API_DESIGN.md` §2（命名一致性 + 扁平命名空间）、`specification/features/FEATURE_ENGINEERING.md` §24（Token 效率）一致。所有示例以真实头文件为准；属性为 `snake_case` 且无 `m_`前缀，类型为
> `CamelCase`。

#### #H.1 响应式信号系统（细粒度）

核心目标：让"状态 → UI"成为自动且定点刷新的纯映射，AI 无需管理订阅 / 重渲染。

- **`State<T>`**：可变状态源。`get()` 在 `Effect` / 渲染作用域内自动登记依赖；`set()` 通知依赖者重跑。
- **`Reactive<T>`**：属性容器。可持有"常量"或"信号引用"；`get()` 在作用域内同样登记依赖。所有可被信号驱动的属性均为
  `Reactive<T>`（如 `Text.content`、`Button.label`）。
- **`Binding<T>`**：`State<T>` 的双向绑定包装（读写都同步回源）。
- **`Computed<T>`**：派生信号；`get()` 时按当前依赖重算，只读、可缓存。构造：`au::computed(lambda)` 工厂（T 由 lambda 返回类型推导）或 `Computed<T>{ lambda }`（T 显式指定），两种形式等价。
- **`Effect`**：响应式副作用作用域。构造时立即运行一次，期间 `get()` 的信号会登记为依赖；任一依赖 `set()` 时自动重跑。
- **`SignalView<T>`**：`State<T>` / `Computed<T>` 共用的只读信号视图（`get()`）。

```cpp
au::State<int> count{0};

au::Text label("count = ");          // 常量（LocalizedString）
au::Text value(au::Computed<au::LocalizedString>{ [&] {    // 派生信号（Computed<T>{ fn } 构造）
    return au::LocalizedString{ std::to_string(count.get()) }; } });
auto is_even = au::Computed<bool>{ [&] { return count.get() % 2 == 0; } };   // 派生信号

au::Effect watch{ [&]{ Diagnostics::warn("count = " + std::to_string(count.get())); } };

count.set(1);   // → value 重绘、watch 重跑；label 不受影响（未读 count）
```

设计要点：树只构建一次（组件 `paint` 读取信号时登记依赖），信号变化定点刷新依赖它的局部计算， **无虚拟 DOM、无 key、无 diff**（与
§20 布局确定性互补）。组件通过 `Widget::collect_signals` 把属性里的信号登记到所属信号作用域，信号变化时仅重绘该组件。

> 可选的类 Redux `Store<S>`（内部持有 `State<S>`，提供 `dispatch(Action)` + 纯函数 `Reducer`，并 `as_signal()` 暴露为信号）见
> §6。

- **`Subscription`（RAII 订阅句柄）**：包装 `State`/`Reactive`/`Computed`/`Store` 订阅返回的取消句柄；析构自动取消，杜绝监听器泄漏。AI
  生成代码无需手动保存/调用取消句柄——把返回值留在作用域即可。
- **`bind(src, fn)` / `bind(store, fn)`**：把响应式信号或 `Store` 接到回调（每次变化调用 `fn(最新值)`，首次立即应用当前值），返回
  `Subscription`。`src` 可为 `State<T>`/`Reactive<T>`/`Computed<T>`（均继承 `SignalView<T>`）。

```cpp
au::State<int> count{0};
int seen = 0;
{
  au::Subscription sub = au::bind(count, [&](int v){ seen = v; }); // 立即 seen = 0
  count.set(5);   // → seen = 5
}                 // sub 析构 → 自动取消订阅
```

> **生命周期安全（T1b）**：响应式内核以 `Connection`（`weak_ptr` 锚点）维护「信号源↔Effect」观察者图，`notify()` 在遍历时探测并
> **惰性摘除失效边**，因此 **任一侧先析构都不会再解引用失效对象**。`Subscription` / `Effect` / `Computed` / widget `track`
> 内部的 `Effect` 销毁后，信号源继续 `set()` 是安全的（不会重跑已死的观察者，也不会崩溃）。该保证由 T1b 落实（实现见
> `state/state.h` / `state/effect.h`，行为由 `tests/test_reactive_lifecycle.cpp` 覆盖）。

#### #H.2 Modifier 修饰系统

核心目标：用一组正交、可组合的修饰符表达内边距、背景、可点击、尺寸、边框、裁剪、对齐、偏移、拖拽等横切能力，替代继承爆炸。

`Modifier` 是不可变值：每个方法返回新副本，可链式组合并赋值给任意组件的 `.modifier` 属性。

```cpp
auto save_btn = au::Button(au::ButtonProps{ .label = "保存" });
save_btn.modifier = au::Modifier{}
    .padding(12)                     // 内边距（float 或 EdgeInsets）
    .background(au::colors::Blue)    // 背景填充
    .clickable([&]{ save(); })       // 点击回调
    .border(1, au::colors::Gray)     // 边框（线宽, 颜色）
    .align(au::Alignment::Center)
    .fill_max_width();
```

常用工厂（全部返回 `Modifier`，可链式）：

| 工厂                                                            | 作用                                                |
|:----------------------------------------------------------------|:----------------------------------------------------|
| `.padding(float)` / `.padding(EdgeInsets)`                      | 内边距                                              |
| `.background(Color, float radius=0)`                            | 背景填充（可选圆角）                                |
| `.clickable(std::function<void()>)`                             | 点击回调（命中即消费事件）                          |
| `.opacity(float a)`                                             | 透明度（`a∈[0,1]`，复用 `Painter::set_alpha`）      |
| `.size(w, h)` / `.width(float)` / `.height(float)`              | 固定尺寸（Length 强类型宽度走 `Widget::width(Length)`）|
| `.fill_max_width()` / `.fill_max_height()` / `.fill_max_size()` | 撑满父约束                                          |
| `.border(float, Color)`                                         | 边框                                                |
| `.clip()` / `.clip_rounded(float)`                              | 矩形 / 圆角裁剪                                     |
| `.align(Alignment)`                                             | 在父约束内的对齐                                    |
| `.offset(float dx, float dy)`                                   | 绘制期平移（Transform 切片）                        |
| `.rotate(float degrees)`                                        | 绕内容中心旋转（Transform 切片，仿射矩阵）          |
| `.scale(float sx, float sy)` / `.scale(float s)`                | 绕内容中心缩放（Transform 切片）                    |
| `.transform(Matrix2D)`                                          | 应用任意 2×3 仿射矩阵（Transform 切片，绕内容中心） |
| `.expand(float weight=1)`                                       | 在 Flex 主轴占权重                                  |
| `.draggable(...)` / `.long_press(...)`                          | 手势（已发布；单指，由 `Draggable`/`LongPress` 修饰节点驱动）|

修饰节点分为四类切片，按固定顺序作用于布局 / 绘制 / 输入：`Layout`（Padding/Size/Expand/FlexWeight）、`Paint`
（Background/Border/Clip/Opacity）、`Input`（Clickable/Draggable/LongPress）、`Transform`（Align/Offset/Rotate/Scale/Transform）。
`Transform` 切片累积为单个 `Matrix2D`（绕内容中心构造），绘制时整树离屏合成、命中测试用逆矩阵映射本地坐标；`Opacity` 复用
`Painter::set_alpha`。`Widget::paint` 三段式修饰绘制： **先绘阴影 / 背景毛玻璃 (backdrop)**（须在圆角裁剪之前，以免外扩羽化被切），
**再压栈裁剪（Clip / ClipRounded，与既有裁剪栈取交集）**， **随后绘背景 / 渐变背景**——因圆角裁剪已生效，背景随 `clip_rounded`
圆角呈圆角（符合 Flutter `ClipRRect` 语义：圆角裁剪作用于控件绘制的一切内容，含自身背景）。Paint
切片（背景、边框、阴影、裁剪、后效）统一作用于控件完整视觉盒子 `visual_box`；子节点 `on_paint` 与内容后效则限定在 `content_box`
（已扣除 Padding/Align 等布局内边距）。内容后弹栈并绘制边框。旧实现曾把背景先于裁剪当作直角矩形填色，也曾把 Paint 修饰限制在
content_box 导致 padding 区域露白；回归用例 `tests/test_clip_rounded_background.cpp` 覆盖这两种情况。
