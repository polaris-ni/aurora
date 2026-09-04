# Aurora 使用指南

> 面向 AI 编码助手与人类开发者的「复制即用配方集」。每个配方都是最小可编译片段。
> 头文件只需 `#include "aurora/aurora.h"`，命名空间别名 `namespace au = aurora;`。
>
> **片段约定**：本文所有片段统一使用 `au::` 前缀。复制任意单段时，请确保该别名（或 `using namespace aurora;`）已在所处编译单元声明，否则 `au::Xxx` 编译失败。
>
> API 契约见 `specification/` 八份子系统文档；架构见 [`ARCHITECTURE.md`](ARCHITECTURE.md)；状态选择见 [`CONCEPTS.md`](CONCEPTS.md) §2；常见坑见本文 §26；运行时调试见本文 §27。

---

## 1 最小可用界面

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

## 2 弹出真实窗口（GLFW 后端）

GLFW 依赖经仓库内置 `third_party/glfw` 源码构建，开启开关即可，无需安装或指定任何路径：

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
    opts.size = au::Size{480.0F, 320.0F};
    opts.title = "Hello";
    // 类型安全工厂强制走 GLFW（绕过 create_native_window 的平台优先级）
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
    });                                         // 不设 next_wait 则退化为忙轮询
}
```

要点：`GlfwSurface` 渲染仍是软件 `Painter` 光栅，每帧把整幅上传为一张 OpenGL 纹理上屏（立即模式，零 GL 加载器依赖）；鼠标 / 键盘 / 滚轮 / 文本输入由后端翻译为 `aurora::Event` 后统一派发。

---

## 3 布局：纵向 / 横向排列

```cpp
au::Node root = au::Node{ au::Column(au::ColumnProps{ .children = {
    au::Node{ au::Text("标题").font_size(24) },
    au::Node{ au::Text("副标题") },
    au::Node{ au::Button(au::ButtonProps{ .label = "点我" }) },
}}) };

au::Node row = au::Node{ au::Row(au::RowProps{ .children = {
    au::Node{ au::Text("左") },
    au::Node{ au::Spacer() },          // 吸收剩余空间，把两侧推开
    au::Node{ au::Text("右") },
}}) };
```

> 对齐语义：主轴对齐（`Center` / `SpaceBetween` 等）只有在父约束强制容器更大时才可见。若想让 `SpaceBetween` 生效，给父容器一个固定尺寸或 `Expand`（见 §9）。

---

## 4 层叠（浮层 / 徽章）

```cpp
au::Node overlay = au::Stack(std::vector<au::Node>{
    au::ImageView(logo),
    au::Text("NEW"),          // 默认叠在左上角
}, au::Alignment::TopRight);
```

---

## 5 图像

```cpp
auto img = au::Image::load("logo.png");   // Result<Image>
if (img) {
    au::Node n = au::ImageView(std::move(img.value()));
}
// 便捷：失败返回占位框
au::Node n = au::ImageView::from_file("logo.png");
```

---

## 6 自定义绘制（Canvas）

```cpp
au::Node chart = au::Canvas(160, 90, [](au::Painter &p, au::Rect b) {
    p.draw_rect(b, au::colors::AURORA_WHITE);
    p.fill_rect(au::Rect{ au::Point{ b.origin.x, b.origin.y }, au::Size{ 40, 40 } }, au::colors::AURORA_BLUE);
});
```

---

## 7 条件显示（Show）

```cpp
auto visible = std::make_shared<au::State<bool>>(true);
au::Node n = au::Show(visible, au::Text("仅条件为真时可见"));
visible->set(false);   // 触发刷新，widget 消失
```

---

## 8 动态列表（Repeater）

```cpp
auto items = std::make_shared<au::State<std::vector<std::string>>>(std::vector<std::string>{"A","B","C"});
au::Node list = au::Repeater<std::string>{ items, [](const std::string &s, int i) {
    return au::Text(s);
}};
items->set(std::vector<std::string>{"X","Y"});   // 列表自动重建
```

---

## 9 响应式状态（细粒度信号）

```cpp
// 方式 1：使用内置响应式 widget（Show / Repeater 等接受 State<T>）
auto visible = std::make_shared<au::State<bool>>(true);
au::Node n = au::Show(visible, au::Text("条件显示"));
visible->set(false);  // 触发定点刷新，widget 消失

// 方式 2：使用 Effect 手动建立依赖（底层机制）
au::State<int> count{ 0 };
int observed = 0;
au::Effect e([&]() -> void { observed = count.get(); });  // 读取信号即登记依赖
e.run();           // 首次运行：observed = 0
count.set(42);     // 触发 Effect 重跑：observed = 42（定点刷新，无全树 diff）
```

> **注意**：仅在构造时读取 `state->get()` **不会**建立响应式依赖。必须通过 `Effect` 或使用接受 `State<T>` 的 widget（Show / Repeater）才能实现定点刷新。

```cpp
// 方式 3：用 bind + Subscription 订阅任意信号（析构自动取消，无监听器泄漏）
au::State<int> count{ 0 };
int seen = 0;
{
    au::Subscription sub = au::bind(count, [&](int v){ seen = v; });  // 立即 seen = 0
    count.set(7);                                                     // → seen = 7
}                                                                     // sub 离开作用域 → 自动取消订阅
```

`aurora::bind` 必须显式限定命名空间，否则 ADL 可能解析成 `std::bind`。

---

## 10 显式尺寸与主题

```cpp
auto wide = au::Text("宽 200");          // 派生类型变量
wide.width(au::px(200));                 // width() 返回 Widget&，不能内联进 Node
au::Node fixed = std::move(wide);        // Node 仅移动派生对象（Widget 拷贝被删除）

au::Theme theme{ .primary = au::colors::AURORA_BLUE, .background = au::colors::AURORA_WHITE };
auto themed = au::ThemeProvider{ theme, au::Button(au::ButtonProps{ .label = "主题按钮" }) };
```

### 10.1 命名令牌与 StyleProps

除扁平字段外，可登记命名令牌并以「令牌名或具体值」两态写样式，渲染时经 `Theme` 解析：

```cpp
au::Theme theme = au::Theme::light();
theme.set_token("color.brand", au::Color::red());   // 命名令牌（Color / Font / double 三态）
theme.set_token("space.md", 16.0);                  // dp 尺寸令牌

au::ThemeScope scope{ theme, build_content() };     // 注入（沿用 ThemeProvider 机制）

// 组件绘制前用最近祖先 Theme 解析两态样式：
au::StyleProps sp;
sp.background    = "color.brand";   // 令牌名 → 解析为 au::Color::red()
sp.padding       = "space.md";      // 令牌名 → 解析为 16.0
sp.corner_radius = 8.0;             // 具体值 → 直接采用
au::ResolvedStyle r = sp.resolve(nearest_theme);
// r.background == au::Color::red(); r.padding == 16.0; r.corner_radius == 8.0
```

### 10.2 Checkbox 样式定制

默认即现代样式（圆角方框 + 抗锯齿勾号 + hover / 按下反馈，勾选色跟随主题 `primary`），链式可定制：

```cpp
au::Checkbox cb{ au::Reactive<bool>{ true } };
cb.set_active_color(au::Color{ 46, 160, 67, 255 })   // 勾选填充色（不设则跟随主题 primary）
  .set_check_color(au::colors::AURORA_WHITE)                // 勾号颜色
  .set_border_color(au::Color{ 140, 140, 146, 255 }) // 未勾选描边色
  .set_size(28.0F)                                   // 方框边长 dp
  .set_corner_radius(9.0F)                           // 圆角（<0 自动 = 边长 ×0.2）
  .set_border_width(2.0F)                            // 描边宽
  .set_enabled(false);                               // 禁用：灰化且不响应点击
```

> 全部交互控件的可定制性契约见 [`specification/04-widget.md`](specification/04-widget.md) §4。

---

## 11 异步任务

回调式（`au::async` 底层经有界 `aurora::ThreadPool::default_pool()` 执行）：

```cpp
au::async([]() -> int {            // 后台线程执行
    int s = 0; for (int i = 1; i <= 100; ++i) s += i;
    return s;                       // 或返回 Result<int>
}).with_timeout(std::chrono::seconds(5))   // opt-in 超时
 .then([](const au::Result<int> &r) {      // 结果回主线程
    if (r) out->set(r.value());     // 回写 State 触发刷新
    else   au::Diagnostics::report(r.error().message, {}, r.error().code);
});
```

协程式（`co_await` 在后台执行，续体回主线程）：

```cpp
au::CoroTask<void> load() {
    au::Result<int> r = co_await au::co_async([]() -> int {
        int s = 0; for (int i = 1; i <= 100; ++i) s += i;
        return s;
    });
    if (r) out->set(r.value());
}
au::launch(load());                // 启动顶层协程
```

可复用线程池（公开 API）：

```cpp
au::ThreadPool &pool = au::ThreadPool::default_pool();
auto fut = pool.submit([] { return heavy_compute(); });   // 返回 std::future<R>
```

---

## 12 定时任务（Scheduler / Timer）

应用级命令式（由帧循环驱动、主线程触发）：

```cpp
auto win_res = au::create_window(au::Win32Options{ .title = "Timer Demo" });
au::Application app{ build_ui(), win_res ? std::move(win_res.value()) : nullptr };
auto &sched = app.scheduler();
au::TimerHandle poll_h = sched.set_interval(1s, [&] { poll(); });   // 周期
sched.set_timeout(5s, [&] { poll_h.cancel(); });                    // 5s 后停止
app.run();
```

组件级声明式（响应式为主 + 可选回调）：

```cpp
// 时钟：子 UI 经 ticks() 绑定自动刷新
au::Node clock = au::Timer(1s, [](const au::SignalView<int> &tick) {
    return au::Text(au::computed([&] {
        return au::LocalizedString{ "tick: " + std::to_string(tick.get()) }; }));
});

// 自动显现：2s 后 on_tick 置位 shown
auto shown = std::make_shared<au::State<bool>>(false);
au::Node reveal = au::Timer(2s,
    [shown](const au::SignalView<int> &) {
        return au::Show(shown, au::Text(au::LocalizedString{ "revealed!" })); },
    [shown](int) { shown->set(true); });
```

- `set_timeout(d, cb)` 一次性；`set_interval(period, cb)` 周期；返回值 `TimerHandle` 可 `cancel()`、`active()` 查询。
- `Timer` 挂载时向 `Scheduler::current()` 注册，析构自动取消；无运行中 App 时降级（记 `Diagnostics::warn`）不崩溃。

> **`au::LocalizedString`**：`au::LocalizedString{ "revealed!" }` 由 `const char*` 构造，持有字面文本，未启用本地化时直接显示（查表失败也回退为 `text`）。需按 `Locale` 查 `StringTable` 解析时用 `au::LocalizedString::tr("key")`；控件层（如 `Button::label`）统一吃 `LocalizedString`，故 `.content = "Hi"` 可隐式转换。

---

## 13 CPU 节能与硬件加速上屏

帧循环默认已是「事件驱动 + 帧节流」：静态界面 idle 时自动阻塞等待事件（CPU 趋近 0），**无需任何额外代码**。仅在特殊需求时调整：

```cpp
au::WindowOptions opts;
opts.title = "My App";
opts.max_fps = 120;       // 活跃帧（动画/脏区）帧率上限；0 = 不限帧率
opts.power_saving = true; // 默认开；false = 忙轮询（持续自驱动重绘场景才需）
opts.renderer = au::RendererPreference::Auto;  // 有 D3D11 则 GPU 上屏，否则软件 GDI
auto win_res = au::create_window(au::Win32Options{ opts });
au::Application app{ build_ui(), win_res ? std::move(win_res.value()) : nullptr, opts };
app.run();                // idle 均深睡，输入 / 定时器 / 后台回投均可即时唤醒
```

强制 GPU 上屏（需 CMake `-DAURORA_BACKEND_D3D11=ON`）：

```cpp
opts.renderer = au::RendererPreference::GpuD3D11;  // 不可用时 create_window 返回 renderer-unavailable 错误
// 或 RendererPreference::Software 强制软件 GDI
```

- 持续重绘场景（如自定义每帧动画）可 `opts.power_saving = false` 或 `app.window()->enable_dirty_tracking(false)` 退回不限速重绘。
- `RendererPreference` 仅影响「像素如何上屏」；所有绘制仍由软件 `Painter` 完成，widget 层不感知后端。

---

## 14 渲染性能测量（确定性基准）

> 硬规则：**时间类**门槛一律在 `Release + PROFILING=OFF` 下测量；**计数类**门槛（`RenderCounters` 各字段、脏区面积、整帧重绘帧数）在 `Release + PROFILING=ON` 下测量。Debug 默认开 PROFILING，禁止拿 Debug 数据填基线表。

**原语 / 整帧光栅基准**（`tools/bench/bench_render.cpp`，输出 markdown 表到 stdout）：

```bash
cmake --build build --target bench_render
./build/bench_render.exe          # 场景 × 分辨率 × scale 的 ms/帧矩阵
```

**滚动场景基准**（`tools/bench/bench_scroll.cpp`，用 `ScrollBenchHarness` 对业务树确定性采样）：

```bash
cmake --build build --target bench_scroll
# 时间口径（Release + PROFILING=OFF）：独立进程多次取最小，抵消离屏缓冲堆碎片噪声
for i in 1 2 3; do ./build/bench_scroll.exe --scene google_play --format csv; done

# 计数口径（Release + PROFILING=ON）：确定性，可直接锁基线
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=ON
cmake --build build-prof --target bench_scroll
./build-prof/bench_scroll.exe --scene google_play --format csv
```

**在代码里直接用 `ScrollBenchHarness` 做自证采样**（先查 `trustworthy()` 再读性能指标）：

```cpp
#include "aurora/perf/scroll_bench.h"

au::ScrollBenchHarness::Config cfg{};
cfg.name = "my-scroll-tree";
cfg.frames = 300;
cfg.warmup_frames = 30;
cfg.delta_per_frame = 12.0F;   // 单位 dp；harness 运行期标定 dp_per_unit 后换算下发
const auto r = au::ScrollBenchHarness::run(build_my_tree(), au::Size{ 1100, 760 }, cfg);
if (!r.trustworthy()) {
    AURORA_LOG_ERROR("bench", "采样不可信：", r.to_markdown());
    return;
}
// 验收只看三项，不看 avg：
AURORA_LOG_RAW("bench", r.p99_ms(), " ", r.jitter_ms(), " ", r.full_redraw_frames(), "\n");
// 计数器锚点（须 Release + PROFILING=ON 构建）：Scroll 离屏缓冲字节数峰值
AURORA_LOG_RAW("bench", "scroll_buffer_bytes peak=", r.counters_max().scroll_buffer_bytes, "\n");
```

---

## 15 序列化：树 ⇄ JSON

```cpp
au::Json j = au::serialization::to_json(root.widget());   // 结构快照
auto back = au::serialization::from_json(j);               // 重建真实 widget

// 差异补丁
auto patch = au::serialization::diff(a_json, b_json);
au::serialization::apply_patch(a_json, patch);             // a 变为 b
```

---

## 16 树 ⇄ 源码

```cpp
std::string code = au::serialization::to_code(root.widget());
// 输出等效的 au::Column(au::ColumnProps{...}) 源码，可反贴回项目

// 指定风格
std::string step = au::serialization::to_code(j, au::serialization::CodeStyle::StepByStep);
```

---

## 17 树 → YAML

```cpp
// 1) Widget 树 → YAML 字符串
std::string yaml = au::serialization::to_yaml(root.widget());
// 输出示例：
// type: Column
// props:
//   gap: 12
// children:
//   - type: Text
//     props:
//       content: Hello
//       font_size: 24
//   - type: Button
//     props:
//       label: Click

// 2) Json 值 → YAML 字符串（底层 API）
au::Json j = au::serialization::to_json(root.widget());
std::string yaml2 = au::serialization::to_yaml(j);

// 3) CLI 用法
// $ aurora to-yaml tree.json
```

> `to_yaml` 仅输出方向，无 `from_yaml`。内部经 `widget/yaml.h` 的递归下降发射器把 JSON 转为 YAML，与 `to_code` 平行作为输出格式适配层。

---

## 18 生成 API 描述

```bash
cmake --build build --target aurora_api_json   # 推荐：内部跑 gen_api_tools 直写 aurora_api.json
# 或等价手动：gen_api_tools "aurora_api.json"
```

输出包含全部已注册 widget 类型、属性键，以及核心枚举（Color 调色板 / `LengthKind` / `Alignment` / `KeyCode` / `Curve`），供 LSP、文档生成器、设计工具使用。

---

## 19 持久化配置（Preferences）

轻量键值配置，以单个 JSON 文件为后端（对标 `SharedPreferences` / `UserDefaults`）。初始化时**显式指定文件位置**；未指定则仅存内存。`set` 只写内存与响应式 `State`，落盘由 `flush()` 主动触发。

```cpp
#include "aurora/aurora.h"
using namespace aurora;
using namespace aurora::preferences;

// 1) 单例（推荐，应用级全局配置）：初始化时显式指定文件位置，同名全局唯一、线程安全
Preferences &prefs = Preferences::instance("app", Preferences::default_config_dir());
//    或显式绝对路径：Preferences &prefs = Preferences::instance_at("app", "C:/data/settings.json");
//    （普通构造器仍可用：内存模式 Preferences mem; 文件模式 Preferences p{ "C:/data/settings.json" };）

// 2) 响应式绑定到已有控件（Switch 等）
au::Switch sw{ prefs.binding<bool>("dark_mode", false),
    [&prefs](bool v) { prefs.set("dark_mode", v); prefs.flush(); } };

// 3) 读取：类型不匹配或缺失回退默认值
int volume = prefs.get("volume", 5);

// 4) 主动刷新到文件（进程锁 + 原子写）；重新启动后构造即加载，reload 可从文件恢复
if (auto r = prefs.flush(); !r.ok()) { /* r.error() */ }
```

- **单例粒度**：`instance(name)` 按名注册表，每个 `name` 唯一；首次调用按参数创建，后续忽略路径参数。
- **并发安全**：实例内部 `std::shared_mutex` 保护读写；`flush` / `reload` 经跨平台文件锁（`<file>.lock`）+ 原子 `rename` 保证多进程安全。可多线程并发 `set` / `get` / `flush`。
- 支持值类型：`bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象。
- `watch<T>` 返回 `std::shared_ptr<State<T>>`，可手动订阅；`binding<T>` 返回 `Binding<T>`（非拥有，须保持 `Preferences` 存活）。

### 19.1 分组（group）

`group(name)` 返回作用域子视图，把相关配置组织到嵌套 JSON 对象里持久化；根 API 不变，分组与扁平键可共存。

```cpp
// 把界面偏好分组到 "ui"：文件内表现为 {"ui":{"theme":"dark","font_size":14}}
auto ui = prefs.group("ui");
ui.set("theme", std::string("dark"));
ui.set("font_size", 14);

// 控件直接绑定分组内的键（删除走分组墓碑路径）
au::Switch sw{ ui.binding<bool>("dark_mode", false),
    [&prefs](bool v) { prefs.group("ui").set("dark_mode", v); prefs.flush(); } };

// 链式嵌套分组
prefs.group("ui").group("editor").set("font", std::string("Mono"));

// 仅清空 ui 分组子树（不影响其他分组与顶层键）
ui.clear();
```

- 分组键以复合点号路径索引（`"ui.theme"`、`"ui.editor.font"`），旧版扁平文件仍可正确加载（向后兼容）。
- `Group::remove` / `Group::clear` 沿用版本化 LWW + 墓碑 + 清空纪元，随 `flush` 多进程最终一致；`Group` 是轻量视图，须在其所属 `Preferences` 存活期内使用。

---

## 20 视频播放器（VideoPlayer）

`media/` 子系统以抽象 `VideoSource` 为解码扩展点，内置零依赖 `ImageSequenceSource`（图片序列 / 动画）作为自包含样例。播放由 `Application::tick` 自动驱动，控件与 UI 可绑定 `Reactive` 播放状态。

```cpp
#include "aurora/aurora.h"
using namespace au;

// 1) 内置零依赖源：把若干已解码帧当作定帧率动画
auto src = std::make_shared<au::ImageSequenceSource>();
src->set_fps(24.0);
src->set_frames({ au::Image{ 160, 90 }, au::Image{ 160, 90 } });

// 2) 播放器（继承 Container，可叠加控件叠层）；默认带底部 VideoControls
auto player = std::make_unique<au::VideoPlayer>();
player->set_source(src);
player->width(au::px(640));
player->height(au::px(360));

// 3) 播放控制 / 响应式状态
player->toggle_play();
player->seek_fraction(0.5);          // 跳到一半
player->set_volume(0.8);
```

**四类扩展点：**

```cpp
// 4a) 在 on_paint 中读取当前帧并叠加水印
class WatermarkPlayer : public au::VideoPlayer {
  protected:
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) -> void override {
        au::VideoPlayer::on_paint(p, b, ctx);   // 先画默认帧
        p.fill_rect(au::Rect{ { b.origin.x + 8, b.origin.y + 8 }, au::Size{ 120, 24 } },
                    au::Color{ 0, 0, 0, 120 });
        p.draw_text(au::Rect{ { b.origin.x + 12, b.origin.y + 12 }, au::Size{ 80, 18 } },
                    "Aurora", au::Font{ .size_pt = 14 }, au::Color::white());
    }
};

// 4b) 替换默认控件叠层：子类化 VideoPlayer，覆写 create_default_controls()（挂载期生效）
class MinimalPlayer : public au::VideoPlayer {
  protected:
    [[nodiscard]] auto create_default_controls() -> std::unique_ptr<au::Widget> override {
        return std::make_unique<au::VideoControls>(this);   // 或返回自定义控件条
    }
};

// 4c) 换肤 / 重排底部控件条：子类化 VideoControls，访问 play_button()/time_text()/mute_button()
class ThemedControls : public au::VideoControls {
  public:
    explicit ThemedControls(au::VideoController *c) : au::VideoControls(c) {
        if (play_button()) play_button()->color(au::Color::blue());
    }
  protected:
    auto build_children() -> void override {
        au::VideoControls::build_children();        // 先建默认子控件
        if (time_text()) time_text()->color(au::Color::yellow());
    }
};
```

其余扩展点（可插拔源、可插拔手势）与完整 API 见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §9.2。

---

## 21 字体与文本渲染

Aurora 以 **FreeType** 为字体内核（栅格化）、**HarfBuzz** 为文本 shaping（均经 `third_party/` 源码构建编入静态库），默认字体为内置 **Noto Sans（OFL）**，跨平台（含 Headless）确定性。无需任何配置即可使用默认字体；仅在需要自定义 / 嵌入字体时显式注册。

```cpp
#include "aurora/render/font_engine.h"

// 1) 使用内置默认字体（无需注册）
au::Text("Hello, 世界").font_size(16);          // 默认 Noto Sans，CJK 走系统回退链

// 2) 从文件注册（按 family 名引用）
au::FontEngine::register_font("MyBrand", "C:/Fonts/Brand-Regular.ttf");
au::Text("Branded").family("MyBrand");

// 3) 从内存字节注册（跨平台自包含、无外部文件依赖）
std::vector<std::uint8_t> ttf = load_embedded_font();
au::FontEngine::register_font_from_memory("Embedded", ttf);
au::Text("Embedded font").family("Embedded");

// 4) 设为默认 sans-serif（family 留空 "" 即默认链）
au::FontEngine::set_default_font("C:/Fonts/Fallback.ttf");

// 5) 抗锯齿策略（默认 Supersample = FT_RENDER_MODE_NORMAL A8 灰度）
au::FontEngine::set_text_aa_mode(au::TextAAMode::ClearType);
```

- `family` 为空字符串 / `"sans-serif"` / `"Noto Sans"` / `"default"` 均指向内置默认字体。
- 缺字（如未注册 CJK 的 Latin 字体）自动沿系统字体回退链（msyh / simsun / MSGOTHIC 等）查找，避免豆腐块。
- 度量（`measure_width` / `caret_x` / `hit_test_char`）与绘制（`draw_text`）在逻辑 dp 空间一致；`display_*` 系列按物理像素尺寸折算，用于缩放屏下的实显命中校正（见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §8.2）。

---

## 22 Inspector 面板与远程接口

### 22.1 InspectorPanel

`InspectorPanel` 提供类似 Chrome DevTools / Flutter Inspector 的控件树检视能力：左侧树形浏览器展示层级，右侧属性面板展示选中控件的类型与属性值，支持「Export Code」按钮一键导出当前树为 C++ 源码。

```cpp
#include "aurora/aurora.h"
using namespace au;

// 1) 构建目标 UI 树
auto target_tree = []() -> au::Node {
    return au::Node{ au::Column(au::ColumnProps{ .children = {
        au::Node{ au::Text("Hello") },
        au::Node{ au::Button(au::ButtonProps{ .label = "Click" }) },
    } }) };
};

// 2) 创建 InspectorPanel（接受目标树获取函数 + 左侧占比）
au::InspectorPanel inspector{ target_tree, 0.35f };

// 3) 监听选中事件
inspector.on_select_widget = [](au::Widget *w) {
    AURORA_LOG_INFO("app", "选中: ", w->type_name());
};

// 4) 监听导出代码事件
inspector.on_export_code = [](const std::string &code) {
    AURORA_LOG_INFO("app", "导出代码:\n", code);
};

// 5) 手动导出代码
std::string code = inspector.export_code();

// 6) 使用 inspect.h 扩展函数
au::Node root = target_tree();
auto items = au::widget_tree_to_items(root);      // 控件树 → TreeItem 树
auto json  = au::dump_tree_json_full(root);       // 含属性的完整 JSON 快照
auto node  = au::find_node_by_path(root, "0/1");  // 按路径定位节点
auto props = au::get_widget_props(root.widget()); // 属性快照
au::set_widget_prop(root.widget(), "gap", au::Json(12.0F)); // 属性回写
```

### 22.2 InspectorServer

`InspectorServer` 提供 localhost-only HTTP 服务器，暴露 REST 端点供外部工具远程访问运行时控件树。跨平台（Windows 走 Winsock2、Linux / macOS 走 BSD sockets），须开启 CMake 开关 `AURORA_BUILD_INSPECTOR_SERVER`。

```cpp
#include "aurora/aurora.h"
#include "aurora/inspector/inspector_server.h"
using namespace au;

au::InspectorServer server{ target_tree };
if (server.start(6280)) {   // 默认端口 6280，后台线程运行
    AURORA_LOG_INFO("app", "InspectorServer running on port ", server.port());
}

// REST 端点（可用 curl 测试）：
//   GET  http://localhost:6280/api/tree           — 完整控件树 JSON
//   GET  http://localhost:6280/api/widget/0/1     — 路径 0/1 的控件属性
//   PUT  http://localhost:6280/api/widget/0/1/gap — 回写属性（请求体为 JSON 值）
//   GET  http://localhost:6280/api/components     — 全部组件 schema
//   GET  http://localhost:6280/api/yaml           — 当前树的 YAML
//   POST http://localhost:6280/api/to_code        — UI 树 → C++ 代码

server.stop();
```

`root_getter` 回调在 HTTP 工作线程中被调用，内部以 `std::mutex` 保护控件树访问。完整端点清单（含 `/api/debug/*`）见 [`specification/08-tooling.md`](specification/08-tooling.md) §5。

---

## 23 声明式工厂层与滚动容器

### 23.1 `aurora::ui` 工厂

`aurora::ui`（别名 `au::ui`，头 `include/aurora/ui/factories.h`）提供一组**声明式工厂函数**：每个接受父 `Container&` 与内容 / 属性，**自动把新建控件加入父容器**并返回**强类型指针**，免去手动 `add`，压缩 token 与出错面。

```cpp
#include "aurora/ui/factories.h"
using namespace aurora::ui;

au::Column root;
auto *title = label(root, "标题");                         // Text*，自动加为 root 子节点
auto *ok    = button(root, "确定", {}, []{ /* 回调 */ });   // Button*；第四参 on_click 可选
auto *name  = input(root, "默认值");                        // TextInput*
auto *flag  = checkbox(root, au::reactive(true));           // Checkbox*
auto *vol   = slider(root, au::reactive(0.5));              // Slider*
auto *box   = vbox(root);   // Column*；hbox → Row*，stack → Stack*，grid → Grid*，scroll → Scroll*
```

工厂返回的裸指针生命周期由父树 `shared_ptr` 持有，**父树销毁后即失效**，勿长期持有。容器类工厂第二参数为既有 `XxxProps`（默认 `{}`）；`label` / `button` 首参为主文案，覆盖对应 Props 字段。

### 23.2 Scroll

单子垂直滚动容器：固定视口裁切内容、滚轮增量滚动；内容量再大也只缓冲「视口 ×(1 + 2 × overscan)」的有界离屏窗口，纯滚动帧整页仅一次平移合成、不重栅。

```cpp
au::Column list{ au::ColumnProps{ .children = {
    au::Text("行 1"), au::Text("行 2"), /* …长列表… */ } } };

// 方式 1：配置块构造（可设 step / overscan）
au::Scroll scroll{ au::ScrollProps{ .child = std::move(list), .step = 16.0F } };

// 方式 2：便捷构造（取首项为唯一子节点）
au::Node n = au::Scroll{ au::Column{ au::Text("A"), au::Text("B") } };
```

- `ScrollProps` 字段：`.child`（唯一子节点 `Node`）、`.step`（每单位滚轮增量的滚动像素，默认 16）、`.overscan`（视口上下各留 overscan 屏缓冲，默认 1；缓冲内存不随内容量增长）。
- 继承式双模：`.step` 可直接赋值（`scroll.step = 16`），或 `Scroll{ ScrollProps{...} }` 配置块构造。
- 工厂层等价：`au::ui::scroll(parent, ...)`。

> 凡靠自身偏移平移内容的容器（`Scroll` / `LazyList` / `GridView`），**偏移变化必须 `mark_needs_paint()`** 把视口标脏，否则只有自驱动动画控件会跟随滚动。

---

## 24 测试原语（`aurora::test`）

`aurora::test`（头 `include/aurora/test_helpers.h`，**不进 `aurora.h`**）薄封装 `HeadlessSurface` + `TCHECK*` + `EventDispatcher`，用于编写**确定性、可文本验证**的测试。**使用前提：测试编译单元须先 `#include "tests/test_harness.h"`**（提供 `TCHECK*` 断言宏）。

```cpp
#include "tests/test_harness.h"
#include "aurora/aurora.h"
#include "aurora/test_helpers.h"
#include "aurora/ui/factories.h"
using namespace aurora;
using namespace aurora::ui;
using namespace aurora::test;

auto env = init_headless(320, 240);                  // 无头环境（确定性，无需 GUI 后端）
auto *b  = button(*env.root_widget, "Go", {}, [&]{ clicked = true; });
pump(env);                                           // 推进一帧：mount + layout
expect_text(env.root, "Go");                         // 断言树中存在含该文本的控件
expect_tree_contains(env.root, "Button");
expect_count(env.root, "Text", 0);
expect_visible(env.root);
tap(env, *b);                                        // 在控件中心合成 press + release → 触发 on_click
type_text(env, *input_widget, "abc");                // 逐字符喂入已聚焦输入控件
```

富格式文本化：`dump_tree_rich(node)`（`include/aurora/widget/inspect.h`）输出 `#id` / `bounds` / `visible` / `text` / `style` / `listeners` 及 `├─ └─` 树形符，供 AI 断言 / diff / 定位。给节点命名：`node.set_id("my-id")`。

---

## 25 控件样式定制与继承

全部交互控件遵循统一的可定制性契约（主题回退 + 状态反馈 + protected 绘制分阶段钩子），完整定义见 [`specification/04-widget.md`](specification/04-widget.md) §4。

**链式样式定制（组合方式）**：

```cpp
// Outlined 风格按钮：透明背景 + 描边 + 自定义悬停/按下色 + 最小尺寸
auto btn = au::Button("Save");
btn.background(au::colors::AURORA_TRANSPARENT)
   .text_color(au::Color{ 0, 90, 200, 255 })
   .set_border(au::Color{ 0, 90, 200, 255 }, 1.5f)
   .set_hover_color(au::Color{ 0, 90, 200, 24 })   // 不设则由背景色自动调暗
   .set_min_size(96.0F, 36.0F);

// 步进滑块 + 自定义轨道 / 滑块；active_color 不设则跟随主题 primary
auto sl = au::Slider();
sl.set_range(0, 100).set_step(5).set_track_height(8.0F).set_thumb_size(22.0F);

// 密码输入框：掩码 + 限长 + 提交回调
auto pwd = au::TextInput();
pwd.set_placeholder("Password").set_obscure_text(true).set_max_length(32)
   .set_on_submit([](const std::string &v) { /* login */ });
```

**继承方式（单点覆盖绘制阶段）**：

```cpp
// 自定义滑块外观的 Slider：只改滑块，轨道 / 填充沿用基类
class DiamondSlider : public au::Slider {
  protected:
    auto paint_thumb(au::Painter &p, const au::Rect &bounds,
                     const au::Rect &track, au::Color c) -> void override {
        const float cx = track.origin.x + value_fraction() * track.size.width;
        const float cy = bounds.origin.y + bounds.size.height * 0.5f;
        p.fill_rounded_rect({ { cx - 6.0F, cy - 6.0F }, { 12.0F, 12.0F } }, 3.0F, c);
    }
};

// 自定义背景的 Button：只改背景（如渐变），文字 / 边框 / 状态色逻辑不变
class GradientButton : public au::Button {
  protected:
    auto paint_background(au::Painter &p, const au::Rect &b, au::Color bg) -> void override {
        p.draw_linear_gradient(b, b.origin, { b.right(), b.bottom() },
                               { bg, bg.shaded(0.7f) }, { 0.0F, 1.0F });
    }
};
```

**可用钩子速查**：Button `resolve_background` / `resolve_text_color` / `paint_background` / `paint_border` / `paint_label`；Slider `paint_track` / `paint_active_track` / `paint_thumb`；Switch `paint_track` / `paint_thumb`；ProgressIndicator `paint_track` / `paint_fill`；RadioGroup `paint_option`；SpinBox `paint_box` / `paint_value` / `paint_arrows`；Dropdown `paint_box` / `paint_item`；TabBar `paint_tab`；SegmentedControl `paint_segment`；Chip `paint_background` / `paint_content`；TextInput `paint_frame`。

状态色派生用 `Color::shaded(k)`（调暗）与 `Color::with_alpha(a)`（淡色底）。

---

## 26 常见坑

- **链式调用必须用 `std::move`**：`std::move(au::Text("x").font_size(14))`。链式返回基类引用会丢失派生类型，直接进 `Node` 会切片。
- **多数情况无需写 `Node{...}`**：`Node(W&&)` 是非 explicit 转换构造函数，值类型控件（`au::Text{...}`、`au::Column{...}`、`std::move(w)` 等）可隐式转为 `Node`。仅当两分支类型不同的 `?:` 三元、或 `std::make_shared<Widget>(...)` 这类需连续两次用户转换的场景，才显式包 `Node{...}`。
- **`ImageView` 而非 `Image`**：`au::Image` 是解码后的像素数据，`au::ImageView` 是控件（序列化类型名为 `Image`）。
- **强类型尺寸**：`width(au::px(100))` 合法；`width(100)` 编译失败（无 `Length(int)` 隐式转换）。
- **子类新增与基类同名方法会静默 override 基类虚函数**：新增方法前须确认基类中不存在同名虚函数。

> 状态选择指南（State vs Store vs Binding vs Computed）与生命周期速查见 [`CONCEPTS.md`](CONCEPTS.md) §2。

---

## 27 调试能力（真实后端 DEBUG）

真实后端的运行时画面与状态抓取能力，弥补 golden 测试只能覆盖 `HeadlessSurface` 的盲区。全部门控 `AURORA_ENABLE_DEBUG`（Debug / RelWithDebInfo 自动注入，Release / MinSizeRel 不注入）；头经 `aurora/aurora.h` 暴露，消费端调用始终可编译，关闭时 API 返回 `disabled` / `{"available":false,...}`，**零开销**。

开启 `AURORA_BUILD_INSPECTOR_SERVER` 时，下列能力经 `InspectorServer` 的 `/api/debug/*` REST 端点远程暴露（§22.2）。

### 27.1 帧缓冲 / 真实窗口截图

```cpp
au::debug::set_output_directory("./debug_shots");   // 相对文件名落入此目录；空串复位默认 ./aurora_debug
au::debug::capture(surface, "frame.png");           // 软件帧缓冲 → ./debug_shots/frame.png
au::debug::capture(surface, "win.png",
                   au::debug::CaptureSource::OnScreenWindow); // 真实屏幕窗口（含标题栏）
// 显式带目录的路径原样使用（不落入输出目录）
au::debug::capture(surface, "/abs/path/frame.png");
```

`OnScreenWindow` 在 Wayland / Headless 不支持。Release（`AURORA_ENABLE_DEBUG=OFF`）下 `capture` 返回「不支持」错误，不写盘。

### 27.2 可视化调试叠层

```cpp
au::debug::set_flags({
    .layout_guides       = true,  // render box 边框 / 对齐参考
    .relayout_boundaries = true,  // 重排边界框（复用 Widget::is_relayout_boundary()）
    .layer_borders       = true,  // 离屏缓存层（含 cache_layer() 修饰）边框
    .repaint_highlight   = true,  // 本帧实际重绘的控件循环色高亮
    .overdraw            = true,  // 控件粒度过度绘制热力图
});
au::debug::set_flags({});         // 全部复位为 false
```

叠层由 `Window::present_root` 在全树 `paint` 之后统一绘制（**不侵入** `Widget::paint`，避免 Display List replay 漏画）；任一 flag 开启才进入叠加分支。Release 下 `set_flags` 为 no-op、`any_flag_enabled()` 恒 false。

### 27.3 控件拾取

```cpp
auto res = au::debug::widget_picker(root_widget, root_bounds, au::BuildContext{}, au::Point{ x, y });
if (res.hit) {
    for (auto &node : res.chain) { /* node.type_name, node.bounds —— chain[0]=根，末元素=最深命中控件 */ }
}
```

`root_widget` 须为**非 const** `Widget&`（命中测试经 `hit_test_chain` 进入各控件非 const 虚函数链）；`root_bounds` 为根全局盒（通常 `Rect{Point{0,0}, surface.size()}`），`Point{x,y}` 取窗口逻辑 dp。Release 下返回 `{ {}, false }`。

### 27.4 运行时信息导出（JSON）

```cpp
au::Json tree = au::debug::widget_tree(root_node);        // 控件树完整结构
au::Json perf = au::debug::perf_snapshot();               // FPS / 帧时间 / 掉帧 / PerfLog 快照
au::Json tl   = au::debug::frame_phase_timeline(64);      // layout/paint/present 相位均值 + 近帧 + ASCII flamegraph
au::Json why  = au::debug::why_trace(64);                 // 重排 / 重绘因果链（propagated 区分根因与父链传播）
au::Json dg   = au::debug::diagnostics();                 // 运行时诊断只读快照
au::Json st   = au::debug::surface_state(surface);        // Surface 状态
```

上述均为 `aurora::debug` 门面对既有引擎（`Inspector` / `FrameStats` / `PerfLog` / `Diagnostics`）的**薄封装 / 聚合**，生产子系统留原地、公共契约不变。Release 下各 API 返回 `{"available":false, "reason": ...}`。

### 27.5 渲染纯度守卫

`Widget::paint()` 首行自动挂接渲染纯度检查：进入 paint 时 `g_paint_depth` 深度计数器 +1，paint 结束后归零。`current_timestamp()` 等全局可变时钟访问器内建 `AURORA_ASSERT(!g_paint_depth)` 守卫——若控件在 `on_paint` 中读取全局时钟（反模式：让绘制结果依赖每帧时刻，破坏脏区重绘与 Display List 录播一致性），**Debug 构建立即触发断言**。

```cpp
// ❌ 反例（Debug 下断言触发）：在 on_paint 内读全局时钟
auto MyWidget::on_paint(au::Painter &p, const au::Rect &r, const au::BuildContext &ctx) -> void {
    uint64_t now = current_timestamp();   // 绘制结果随每帧时刻变化 → 脏区 / 录播失真
    p.fill_rect(r, color_with_alpha(now % 255));
}

// ✅ 正例：时间源由外部帧驱动，on_paint 只负责绘制
auto MyWidget::on_paint(au::Painter &p, const au::Rect &r, const au::BuildContext &ctx) -> void {
    p.fill_rect(r, ctx.theme().accent);
}
```

纯度机制全程 `AURORA_ENABLE_DEBUG` 门控：Release 构建编译剥离，零运行时开销。

### 27.6 调试 API 目录（自动生成）

`aurora::debug` 命名空间下的全部公共自由函数由 `tools/gen/gen_debug_api.cpp` 从声明源 [`debug_api.toml`](debug_api.toml) 自动生成到 `aurora_api.json` 的 `"debug"` 段（**单一权威目录**）。新增 / 改名调试函数时，**只改 `debug_api.toml`** 再重跑生成器：

```bash
cmake --build build --target gen_debug_api_json   # 读 debug_api.toml → 更新 aurora_api.json 的 debug 段
python tools/check/check_gen_api_merge.py build         # 回归：损坏现有文件不截断、merge 保留其它段
```

生成器为 merge-only：读现有 `aurora_api.json` 的全部其它段（`widgets` / `enums` / `error_codes` / …），仅覆盖 `debug` 段写回；现有文件损坏时直接报错退出、绝不写空对象。`gen_error_codes` / `gen_api_tools` 亦已加固同样的防护并保留 `debug` 段。

---

## 28 自绘标题栏配方（TitleBar）

```cpp
au::WindowOptions opts;
opts.size  = au::Size{ 720, 480 };
opts.title = "文档";
opts.style.decoration = au::DecorationPolicy::Borderless;   // 移除系统标题栏（CSD 接管）

au::TitleBar bar;
bar.set_title("文档")                                       // 主标题
   .set_subtitle("自动保存于 12:00")                         // 副标题（可选）
   .add_action({ "设置", []() -> void { /* … */ } })         // 动作区按钮
   .add_snap_action({ "左半屏", []() -> void { /* … */ } }); // 追加进 Snap 弹窗的自定义项

au::Node root = au::Column{
    au::Node{ std::move(bar) },                  // 标题栏置于首行
    au::Text{ "正文…" },
};
```

控制钮（最小化 / 最大化还原 / 关闭）、拖拽移动与双击最大化经 Environment 注入的 `WindowChrome` 服务自动生效（headless 下安全 no-op）；悬停最大化钮 ≥400ms 触发 Snap 动作弹窗。风格经 `TitleBarStyle` 三预设（`adwaita_dark()` / `adwaita_light()` / `windows_dark()`）或 `Surface::set_title_bar_style()` 调整。

`WindowChrome` 由 `Window::present_root` 在挂载根树时**自动注入根环境**，无需手动提供；子树控件在事件派发栈内经 `ctx.environment<WindowChrome>()` 同步取用：

```cpp
auto *chrome = ctx.environment<au::WindowChrome>();
if (chrome && chrome->valid()) {
    chrome->toggle_maximize();   // 经 Surface 驱动真实窗口动作（Wayland serial 时效约束：仅事件栈内调用）
}
```
