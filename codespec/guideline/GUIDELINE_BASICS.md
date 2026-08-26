# GUIDELINE_BASICS

> 本文件由 [`GUIDELINE.md`](../GUIDELINE.md) 划分而出（最小界面 / 布局 / 层叠 / 图像 / Canvas / Show / Repeater / 响应式 / 尺寸主题）。
> 返回主线见 [`GUIDELINE.md`](../GUIDELINE.md)。
>
> 片段约定：本文所有片段使用 `namespace au = aurora;`（即 `au::` 前缀）。复制任意单段时，请确保该别名（或 `using namespace aurora;`）已在所处 TU 声明，否则 `au::Xxx` 编译失败。

**本文包含章节：**

- [1. 最小可用界面](#1-最小可用界面)
- [1b. 弹出真实窗口（GLFW 后端）](#1b-弹出真实窗口glfw-后端)
- [2. 布局：纵向 / 横向排列](#2-布局纵向--横向排列)
- [3. 层叠（浮层 / 徽章）](#3-层叠浮层--徽章)
- [4. 图像](#4-图像)
- [5. 自定义绘制（Canvas）](#5-自定义绘制canvas)
- [6. 条件显示（Show）](#6-条件显示show)
- [7. 动态列表（Repeater）](#7-动态列表repeater)
- [8. 响应式状态（细粒度信号）](#8-响应式状态细粒度信号)
- [9. 显式尺寸 / 主题](#9-显式尺寸--主题)

## 1. 最小可用界面

```cpp
#include "aurora/aurora.h"
namespace au = aurora;
int main() {
    au::Node root = au::Text("Hello, Aurora!").font_size(20);
    au::Scene scene{ std::move(root) };
    scene.render_to_png("hello.png", 200, 60);
}
```

---

## 1b. 弹出真实窗口（GLFW 后端）

GLFW 依赖经仓库内置 `third_party/glfw`（3.5.1）源码构建，开启开关即可，无需安装或指定任何路径：

```powershell
cmake -S . -B build -DAURORA_BACKEND_GLFW=ON
```

最小可编译配方（完整版见 `examples/demos/demo_glfw_surface.cpp`）：

```cpp
#include <chrono>

#include "aurora/aurora.h"
namespace au = aurora;
int main() {
    au::enable_dpi_awareness();                 // 必须在建窗前（进程级 DPI 感知）
    au::Node root = au::Text("Hello GLFW!");
    au::FocusManager fm;
    fm.set_root(&root.widget());

    au::GlfwOptions opts;                       // size/title 继承自 WindowOptions，逐字段赋值
    opts.size = au::Size{480.0f, 320.0f};
    opts.title = "Hello";
    // 类型安全工厂强制走 GLFW（绕过 create_native_window 的平台优先级）；
    // 无显示/驱动时 GlfwSurface 构造抛 std::runtime_error，按需 try/catch。
    auto win_res = au::create_window(opts);
    if (!win_res)
        return 1;
    auto win = std::move(win_res.value());
    win->surface().set_event_handler([&](au::Event &e) {
        if (auto *me = dynamic_cast<au::MouseEvent *>(&e))
            au::EventDispatcher::dispatch(root.widget(), *me, &fm);
        else if (auto *ke = dynamic_cast<au::KeyEvent *>(&e))
            au::EventDispatcher::dispatch(root.widget(), *ke, fm);
    });
    win->run([&] {                              // 帧循环：关闭窗口即退出
        const auto t0 = std::chrono::steady_clock::now();
        (void)win->present_root(root);
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        win->set_next_wait(au::compute_wait_timeout(
            win->has_pending_dirty(), /*anim_active=*/false, /*next_deadline_ms=*/-1.0,
            /*frame_budget_ms=*/1000.0 / 60.0, elapsed_ms, win->surface().paces_frames()));
    });                                         // 不设 next_wait 则退化为忙轮询（见 Window::run）
}
```

要点：`GlfwSurface` 渲染仍是软件 `Painter` 光栅，每帧整幅上传为一张 OpenGL 纹理上屏（立即模式，
零 GL 加载器依赖）；鼠标/键盘/滚轮/文本输入由后端翻译为 `aurora::Event` 后统一派发。

---

## 2. 布局：纵向 / 横向排列

```cpp
Node root = Node{ au::Column(au::ColumnProps{.children = {
    Node{ au::Text("标题").font_size(24) },
    Node{ au::Text("副标题") },
    Node{ au::Button(au::ButtonProps{.label = "点我"}) },
}})};

Node row = Node{ au::Row(au::RowProps{.children = {
    Node{ au::Text("左") },
    Node{ au::Spacer() },          // 吸收剩余空间，把两侧推开
    Node{ au::Text("右") },
}})};
```

> 对齐语义：主轴对齐（Center/SpaceBetween 等）只有在父约束强制容器更大时才可见。
> 若想让 `SpaceBetween` 生效，给父容器一个固定尺寸或 `Expand`（见配方 9）。

---

## 3. 层叠（浮层 / 徽章）

```cpp
Node overlay = au::Stack(std::vector<Node>{
    au::ImageView(logo),
    au::Text("NEW"),          // 默认叠在左上角
}, au::Alignment::TopRight);
```

---

## 4. 图像

```cpp
auto img = Image::load("logo.png");   // Result<Image>：PNG/JPG/GIF/TGA/BMP
if (img) {
    Node n = au::ImageView(std::move(img.value()));
}
// 便捷：失败返回占位框
Node n = au::ImageView::from_file("logo.png");
```

---

## 5. 自定义绘制（Canvas）

```cpp
Node chart = au::Canvas(160, 90, [](aurora::Painter& p, Rect b) {
    p.draw_rect(b, au::colors::White);
    p.fill_rect(Rect{Point{b.origin.x, b.origin.y}, Size{40, 40}}, au::colors::Blue);
})};
```

---

## 6. 条件显示（Show）

```cpp
auto visible = std::make_shared<State<bool>>(true);
Node n = au::Show(visible, au::Text("仅条件为真时可见"));
visible->set(false);   // 触发刷新，widget 消失
```

---

## 7. 动态列表（Repeater）

```cpp
auto items = std::make_shared<State<std::vector<std::string>>>({"A","B","C"});
Node list = au::Repeater<std::string>{ items, [](const std::string& s, int i) {
    return au::Text(s);
}};
items->set({"X","Y"});  // 列表自动重建
```

---

## 8. 响应式状态（细粒度信号）

```cpp
// 方式 1：使用内置响应式 widget（Show / Repeater 等接受 State<T>）
auto visible = std::make_shared<State<bool>>(true);
Node n = au::Show(visible, au::Text("条件显示"));
visible->set(false);  // 触发定点刷新，widget 消失

// 方式 2：使用 Effect 手动建立依赖（底层机制）
State<int> count{ 0 };
int observed = 0;
Effect e([&]() -> void { observed = count.get(); });  // 读取信号即登记依赖
e.run();           // 首次运行：observed = 0
count.set(42);     // 触发 Effect 重跑：observed = 42（定点刷新，无全树 diff）
```

> **注意**：仅在构造时读取 `state->get()` 不会建立响应式依赖。必须通过 Effect
> 或使用接受 `State<T>` 的 widget（Show / Repeater）才能实现定点刷新。

```cpp
// 方式 3：用 bind + Subscription 订阅任意信号（析构自动取消，无监听器泄漏）
State<int> count{ 0 };
int seen = 0;
{
  au::Subscription sub = au::bind(count, [&](int v){ seen = v; }); // 立即 seen = 0
  count.set(7);    // → seen = 7
}                   // sub 离开作用域 → 自动取消订阅
```

---

## 9. 显式尺寸 / 主题

```cpp
auto wide = au::Text("宽 200");          // 派生类型变量
wide.width(aurora::px(200));             // width() 返回 Widget&，不能内联进 Node
Node fixed = std::move(wide);            // Node 仅移动派生对象（Widget 拷贝被删除）

auto theme = Theme{ .primary = au::colors::Blue, .background = au::colors::White };
auto themed = au::ThemeProvider{ theme, au::Button(au::ButtonProps{.label = "主题按钮"}) };
```

### 9.1 命名令牌 + StyleProps（T2）

除扁平字段外，可登记命名令牌并以「令牌名或具体值」两态写样式，渲染时经 `Theme` 解析：

```cpp
au::Theme theme = au::Theme::light();
theme.set_token("color.brand", au::Color::red());   // 命名令牌（Color / Font / double 三态）
theme.set_token("space.md", 16.0);                 // dp 尺寸令牌

au::ThemeScope scope{ theme, build_content() };     // 注入（沿用 ThemeProvider 机制）

// 组件绘制前用最近祖先 Theme 解析两态样式：
au::StyleProps sp;
sp.background = "color.brand";   // 令牌名 → 解析为 au::Color::red()
sp.padding    = "space.md";      // 令牌名 → 解析为 16.0
sp.corner_radius = 8.0;          // 具体值 → 直接采用
au::ResolvedStyle r = sp.resolve(nearest_theme);
// r.background == au::Color::red(); r.padding == 16.0; r.corner_radius == 8.0
```

### 9.2 Checkbox 样式定制（控件样式专项）

默认即现代样式（圆角方框 + 抗锯齿勾号 + hover/按下反馈，勾选色跟随主题 `primary`），链式可定制：

```cpp
au::Checkbox cb{ au::Reactive<bool>{ true } };
cb.set_active_color(au::Color{ 46, 160, 67, 255 })  // 勾选填充色（不设则跟随主题 primary）
  .set_check_color(au::colors::White)               // 勾号颜色
  .set_border_color(au::Color{ 140, 140, 146, 255 }) // 未勾选描边色
  .set_size(28.0f)                                  // 方框边长 dp
  .set_corner_radius(9.0f)                          // 圆角（<0 自动 = 边长×0.2）
  .set_border_width(2.0f)                           // 描边宽
  .set_enabled(false);                              // 禁用：灰化且不响应点击
```

---

