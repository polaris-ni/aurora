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
// 方式 3：用 connect + Subscription 订阅任意信号（析构自动取消，无监听器泄漏）
au::State<int> count{ 0 };
int seen = 0;
{
    au::Subscription sub = au::connect(count, [&](int v){ seen = v; });  // 立即 seen = 0
    count.set(7);                                                        // → seen = 7
}                                                                        // sub 离开作用域 → 自动取消订阅
```

**为何不是 `bind`**：`State` 基类链上的 `std::enable_shared_from_this` 使 `std` 进入实参的 ADL 关联命名空间集，无限定 `bind(state, fn)` 会被变参转发的 `std::bind` 吸走——编译通过但回调永不执行。故本 API 定名 `connect`（原 `bind` 已于 1.0.0-alpha.1 改名），与 `std` 名字无重叠。

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

### 13.1 GPU 栅格（GLFW 窗口，整条管线 GPU 化）

上一节的 D3D11 偏置只加速「上屏」，绘制仍走软件 `Painter`。若要**栅格管线本身**走 GPU（帧级 DisplayList 经 OpenGL 3.3 core 批渲染进 MSAA 帧缓冲，跳过每帧全屏像素上传），用 GLFW 窗口的 GPU 模式（需 CMake `-DAURORA_BACKEND_GLFW=ON -DAURORA_ENABLE_GLFW_GPU_GL=ON`）：

```cpp
au::GlfwOptions opts;
opts.size = au::Size{.width = 560.0F, .height = 380.0F};
opts.title = "GPU raster";
opts.gpu = true;  // GPU 栅格模式；初始化失败（驱动过老 / 无 GL）自动回退软件纹理路径

auto win_res = au::create_window(opts);
if (win_res) {
    auto win = std::move(win_res.value());
    // win->surface().gpu_backend() 非空即 GPU 路径生效（name() 恒 "gpu-gl"）；
    // nullptr 表示未编译 GPU_GL 或已回退软件纹理路径。
}
```

- widget 层与绘制代码**完全无感知**——同一棵控件树无需任何改动即可在 GPU / 软件路径间切换；GPU 实现与软件路径逐公式对齐（渐变 LUT / PMA 图像 / 字形图集共用软件光栅化 / 效果 pass）。
- 完整契约（管线模型、语义同源承诺、初始化失败链）见 `specification/03-layout-render.md` §8.7；可运行示例见 `examples/demos/demo_gpu.cpp`。

### 13.2 GPU 栅格（wgpu 窗口，跨平台 GPU 主力路径）

§13.1 的 GLFW 模式依赖 OpenGL 3.3 上下文；若要走**同一套 WGSL 管线覆盖 Vulkan / D3D12 / Metal / GLES** 的跨平台 GPU 栅格，用 wgpu 窗口（需 CMake `-DAURORA_BACKEND_GPU_WGPU=ON -DAURORA_BACKEND_WIN32=ON`，构建期需 Rust 工具链 + libclang，见 `BUILD_OPTIONS.md` §3.8；宿主当前为 Win32）：

```cpp
au::WgpuOptions opts;
opts.size = au::Size{.width = 560.0F, .height = 380.0F};
opts.title = "wgpu raster";
opts.vsync = true;  // FIFO 呈现节拍

auto win_res = au::create_window(opts);
if (!win_res) {
    // 无可用 adapter / 开窗失败：返回 renderer-unavailable 错误——该工厂不静默降级
}
// win->surface().gpu_backend() 非空且 name() 恒 "gpu-wgpu" = GPU 栅格路径挂点就绪；
// 首帧运行期失效后整窗永久回退 GDI 上传路径（WgpuSurface::gpu_active() 转 false）。
```

也可经通用 Win32 工厂强制路由：`Win32Options{}.renderer = au::RendererPreference::GpuWgpu`（`Auto` 优先序不含 wgpu，避免改变既有默认行为）。widget 层同样零感知——同一棵控件树在 GPU / 软件回退路径间切换，语义与 §8.7 同源。完整契约见 `specification/03-layout-render.md` §8.8；真机验收探针 `aurora_verify_win32_wgpu`。

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

`aurora::test` 薄封装（头 `tests/support/test_helpers.h`，**仓库私有设施**：不进 `include/`、不进 `aurora.h`、不构成公共 API 兼容承诺）提供 `HeadlessSurface` 驱动的无头测试环境与 `expect_*` 断言家族，用于编写**确定性、可文本验证**的测试。断言宏 `AURORA_TEST_CHECK*` 由框架唯一入口 `tests/framework/aurora_test.h` 提供（注册式 runner 用法见 [`CODING_STANDARDS.md`](CODING_STANDARDS.md) §3.1）；`tests/` 已在测试目标包含路径上，两个头均按裸名包含。

```cpp
#include "framework/aurora_test.h"   // 断言宏 + 用例注册（测试 TU 唯一框架入口）
#include "support/test_helpers.h"    // aurora::test 无头环境与 expect_* 家族
#include "aurora/aurora.h"
#include "aurora/ui/factories.h"

namespace aurora::test_cases::utest_demo {

AURORA_TEST_CASE(button_click_smoke) {
    bool clicked = false;
    auto env = test::init_headless(320, 240);            // 无头环境（确定性，无需 GUI 后端）
    auto *b  = button(*env.root_widget, "Go", {}, [&]{ clicked = true; });
    test::pump(env);                                     // 推进一帧：mount + layout
    test::expect_text(env.root, "Go");                   // 断言树中存在含该文本的控件
    test::expect_tree_contains(env.root, "Button");
    test::expect_count(env.root, "Text", 0);
    test::expect_visible(env.root);
    test::tap(env, *b);                                  // 在控件中心合成 press + release → 触发 on_click
    AURORA_TEST_CHECK_TRUE(clicked);
    // type_text(env, <已聚焦的 TextInput>, "abc");       // 逐字符喂入已聚焦输入控件
}

}  // namespace aurora::test_cases::utest_demo
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

### 27.7 运行时 feature 宏查询

编译期 feature 宏（后端 / 优化 / SIMD / 插桩 / 编解码）的取值已单点收口为运行时查询，**应用代码零 `#ifdef`**（需求 #14）：

```cpp
au::debug::FeatureFlags flags = au::debug::feature_flags();
if (flags.backend_glfw) { /* GLFW 后端已编译进库 */ }

au::Json j = au::debug::feature_flags_json();
// {"AURORA_BACKEND_HEADLESS": true, "AURORA_ENABLE_SIMD": true, ...}  键 = 完整宏名
```

全配置可用（不受 `AURORA_ENABLE_DEBUG` 门控）；结果反映链接进来的 aurora 静态库的编译期取值。禁止再散写 `#ifdef AURORA_*` 探测能力——宏镜像只在 `src/aurora/debug/feature_flags.cpp` 单点维护。

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

---

## 29 Timeline 编排（多段交错动画）

`TimelineSpec` 声明区间树（`sequence()` 子段首尾相接 / `parallel()` 同起点 / `staggered(时长, 间隔, 数量)` 交错糖），`build()` 自动计算各槽位归一化区间——无需手算 Flutter `Interval` 端点；`TimelinePlayer` 用单主控制器驱动全部轨道（槽位 → `Tween` → 目标 `State`）：

```cpp
au::State<double> slide{0.0}, fade{0.0};

auto tl = au::TimelineSpec::sequence()
              .add(0.3)                                        // 槽位 0：先滑入 0.3s
              .add(au::TimelineSpec::staggered(0.2, 0.05, 3))  // 槽位 1..3：3 项 0.2s 交错入场
              .build();                                        // 总时长 0.75s

au::TimelinePlayer player{tl};
player.track<double>(0, au::Tween<double>{0.0, 1.0}, slide);
player.track<double>(1, au::Tween<double>{0.0, 1.0, au::Curves::ease_out()}, fade);
player.attach(app.animator());   // app 为 au::Application（建窗见配方 2）；attach 后句柄可离开作用域
player.forward();                // 正放；到达终点触发一次 on_completed
player.on_completed([]() -> void { /* … */ });
```

要点：`reverse()` 从当前进度沿同一时间轴严格逆序镜像倒放；`stop()` / 方向切换都从当前进度续播（中断续播，手势接管的基础）；`reduce_motion` 开启时全轨道一步落端点（编排层零特判）；同槽位重复 `track` 覆盖前值；轨道目标 `State<T>` 为非拥有引用，必须比播放器存活更久。

## 30 拖动消除（Dismissible）

`Dismissible` 包住任意子树：水平 / 垂直拖出消除（跟手 1:1 映射 + 途中渐隐），松手按阈值（默认 0.5）裁决——过半程 spring 飞出并从树上摘除，未过半程 spring 回位（对标 Flutter `Dismissible`）：

```cpp
auto make_card = [](const char *label, bool custom_cb) -> au::Node {
    au::Text title{label};
    title.modifier.set(au::Modifier{}.size(280.0F, 56.0F));
    auto dis = std::make_shared<au::Dismissible>(au::Node{std::move(title)});
    if (custom_cb) {
        dis->on_dismissed([]() -> void { /* 覆盖默认摘除：删除列表数据后重建子树 */ });
    }
    return au::Node{dis};   // 未注册回调时：飞出后自动从最近 Container 祖先摘除并重排
};

au::Node root = au::Node{ au::Column(au::ColumnProps{ .children = {
    make_card("swipe me ->", false),
    make_card("custom callback", true),
} }) };

au::Scene scene{ std::move(root) };
au::Application app{ std::move(scene), 520, 420 };  // 无头便捷构造；真实窗口见配方 2
app.run();  // spring 阶段由 Application 帧循环的 gesture tick 推进（跟手由指针事件驱动）
```

要点：构造第二参选主轴 `au::DragAxis::Vertical` 可纵向拖出；行程默认 200 逻辑 dp，首次布局后自动按主轴向尺寸校准（`travel_distance` 字段可手动覆写）；拖动 / 飞出进行中该子树消费指针事件，父级不响应点击；`progress()` / `is_animating()` 供诊断与联动绑定。可编译样例见 `examples/demos/demo_dismissible.cpp`。

---

## 31 命令系统（命令注册 + 三源统一 + 命令面板）

`CommandRegistry`（`commands.h`）是命令的**唯一真源**：`bind_shortcuts()` 把它投影到快捷键表、`to_menu_items()` 投影到菜单数据层，`CommandPalette`（`widget/command_palette.h`）是第三个消费方。同一动作只定义一次，启用条件与「无动作体」判定单点生效。

```cpp
#include "aurora/aurora.h"

using namespace au;

au::Command greet;
greet.id = "demo.greet";
greet.title = "Say Hello";
greet.icon = "chat";
greet.category = "Demo";
greet.default_binding = au::KeyCombo{au::ModifierKey::Control, au::KeyCode::H};  // 默认快捷键（可选）
greet.action = []() -> void { /* 执行体 */ };

au::Command unavailable;
unavailable.id = "demo.unavailable";
unavailable.title = "Unavailable Action";
unavailable.when_label = "never";                                    // 仅展示 / 序列化标签，不参与求值
unavailable.enabled = []() -> bool { return false; };                // 运行期启用条件（空 = 恒启用）

app.commands().add(std::move(greet));
app.commands().add(std::move(unavailable));
app.commands().bind_shortcuts(app.shortcuts());  // ① 默认快捷键接入（必须显式调用一次）

// ② 菜单投影：交给 MenuBar 的数据层
for (const au::MenuItem &item : app.commands().to_menu_items()) {
    // item.label / item.icon / item.shortcut_text / item.enabled / item.on_click
}

// ③ 命令面板：Ctrl+K 唤出，模糊检索 + ↑/↓ 导航 + Enter 执行
auto palette = std::make_shared<au::CommandPalette>(&app.commands());
app.shortcuts().add(au::KeyCombo{au::ModifierKey::Control, au::KeyCode::K},
                    [palette]() -> void { palette->toggle(); });
// 面板须进 widget 树（例如 `Stack{ content, palette }` + `StackFit::Expand`）才会渲染与参与命中。
```

要点：`bind_shortcuts()` 必须**显式**调用（`run()` 不会自动绑定，否则 `run()` 之后注册的命令会静默失效），且面板的 Esc / ↑ / ↓ 键位依赖它——未接线时面板仍可鼠标操作与 Enter 执行，但键盘导航不可用并会记一条 WARN。`invoke(id)` 求值启用条件后才执行，未启用 / 无 `action` 时返回 `false` 而不静默成功；`remove(id)` / `clear()` 会**连带撤销**对应的快捷键绑定。检索用 `command_fuzzy_score`（不区分大小写的子序列匹配，词首命中与连续命中加权），与 MCP `list_commands` 工具共用同一打分与排序，故 AI 侧检索次序与面板一致。`to_json()` 产出 `{"commands":[…]}` 自描述信封供 AI 工具面枚举。可编译样例见 `examples/demos/demo_command_palette.cpp`。

---

## 32 多窗口

一个 `Application` 可同时驱动多个窗口：每个窗口是一个 `WindowHost`（自带 `Scene` + `FocusManager` + 帧统计），由**单一帧循环**统一驱动，**焦点与捕获不跨窗口**。

```cpp
#include "aurora/aurora.h"

using namespace au;

auto main() -> int {
    enable_dpi_awareness();  // 必须在建窗前

    auto pings = std::make_shared<State<int>>(0);  // 共享状态：多窗口共用一个 State 即可
    auto root = Column{ColumnProps{.children = {
        Text{"Main window"},
        Text{TextProps{.content = Reactive{pings}}},
        Button{ButtonProps{.label = "Broadcast"}},
    }}};

    WindowOptions opts;
    opts.size = Size{.width = 800.0F, .height = 600.0F};
    opts.title = "Main";
    auto win = create_native_window(opts);
    if (!win) {
        return -1;
    }

    Application app{Scene{std::move(root)}, std::move(win.value()), opts};

    // 跨窗通信：`bus()` 承载**一次性通知/命令**；共享**状态**直接用共享 State/Store。
    auto sub = app.bus().on<std::string>([](const std::string &msg, WindowId from) -> void {
        AURORA_LOG_INFO("demo", "msg from window ", from, ": ", msg);
    });
    (void)sub;  // RAII：保持存活即订阅，离开作用域自动取消

    // 追加独立顶层窗口（Auxiliary：关闭只影响自己）。
    WindowOptions aux_opts;
    aux_opts.size = Size{.width = 480.0F, .height = 320.0F};
    aux_opts.title = "Auxiliary";
    aux_opts.role = WindowRole::Auxiliary;
    if (auto aux = create_native_window(aux_opts)) {
        Node aux_root = Column{ColumnProps{.children = {
            Text{"Auxiliary window"},
            Text{TextProps{.content = Reactive{pings}}},
        }}};
        (void)app.open_window(std::move(aux.value()), Scene{std::move(aux_root)}, aux_opts);
    }

    app.bus().post(std::string{"hello"}, app.main_window());
    app.run();  // 默认 ExitPolicy::LastWindowClosed：关掉最后一个窗口即退出
    return 0;
}
```

要点：

- **角色与退出策略**：`WindowRole{Main, Auxiliary, Transient}` 决定连带关闭（`Transient` 随 `owner` 关闭）；`ExitPolicy{LastWindowClosed（默认）, MainWindowClosed, ExplicitOnly}` 决定何时结束 `run()`，`quit()` 在任何策略下都生效。
- **模态与父子**：`WindowOptions` 的 `owner` + `modal = true` → 打开时建立 OS 层从属关系并**屏蔽 owner 输入**，关闭后自动恢复；无需焦点陷阱（焦点本就不跨窗）。
- **每窗口 DPI / 显示器 / z 序**：`WindowHost::display_id()` / `move_to_display(id)` / `raise()` / `focus_window()`；DPI 变化由后端上报并触发整帧重排重绘。
- **几何持久化**：`preferences::Preferences` + `WindowOptions::persist_id`，`app.set_window_geometry_store(&prefs)` 之后**打开自动恢复、关闭自动保存**（非法/越界几何自动回退默认布局）；窗口组用 `prefs.group("windows")` 分组建键。
- **调试枚举**：`InspectorServer::set_window_ids_getter(...)` 注册后 `GET /api/windows` 可枚举全部窗口 id。
- 可编译样例见 `examples/demos/demo_multi_window.cpp`。

---

## 33 图表控件

五图（`BarChart` / `LineChart` / `Sparkline` / `PieChart` / `ScatterChart`）均为「`XxxProps` 纯值属性 + `class Xxx : public LeafWidget, public XxxProps` + `defaults()`」形态；数据进序列化面，AI 可经 `aurora_api.json` schema + `from_json` 生成带真实数据的图表。

```cpp
#include "aurora/aurora.h"

using namespace au;

auto main() -> int {
    BarChart chart{BarChartProps{
        .series = {
            ChartSeries{.name = "cpu", .values = {12.0, 18.0, 9.0, 24.0}, .color = Color{0x2D, 0x9B, 0xFD, 255}},
            ChartSeries{.name = "mem", .values = {8.0, 10.0, 14.0, 12.0}},
        },
        .categories = {"Mon", "Tue", "Wed", "Thu"},
        .legend = ChartLegendSpec{.visible = true, .position = LegendPosition::Top},
    }};
    chart.set_stacked(true);  // 分组并排 / 堆叠切换（链式 setter）
    chart.on_point_tapped = [](int si, int pi) -> void {  // 回调旁挂，不进序列化面
        AURORA_LOG_INFO("chart", "tapped series ", si, " point ", pi);
    };

    PieChart donut{PieChartProps{
        .sections = {PieSection{.name = "mobile", .value = 45.0},
                     PieSection{.name = "desktop", .value = 30.0},
                     PieSection{.name = "tablet", .value = 15.0}},
        .center_space_ratio = 0.45F,  // > 0 ⇒ 环形
        .show_percentage_labels = true,
    }};

    LineChart trend{LineChartProps{
        .series = {ChartSeries{.name = "latency", .values = {4.0, 6.0, 3.0, 8.0, 5.0}}},
        .show_dots = true,
    }};

    ScatterChart pts{ScatterChartProps{
        .series = {ScatterSeries{.name = "samples",
                                 .points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 3.0, .y = 4.0}}}},
    }};

    Sparkline spark{SparklineProps{.values = {4.0, 6.0, 3.0, 8.0, 5.0}, .line_width = 2.0F}};

    Application app{Scene{Column{ColumnProps{.children = {Node{chart}, Node{donut}, Node{trend},
                                                              Node{pts}, Node{spark}}}}}};
    return app.run();
}
```

要点：

- **数据进序列化面**：`series` / `sections` / `values` / `points` 都是属性键（值经 `to_json` / `from_json` 往返，畸形元素逐项跳过、绝不抛异常）；交互回调（`on_point_tapped` / `on_section_tapped`）是 `std::function` 成员，**不进序列化面**，Inspector PUT 与 `from_json` 都不会重建它们。
- **取色三级优先**：系列显式 `color` > `Theme` 命名令牌 `chart.palette.N`（N = 系列序号取模 8）> 内置 Material 风 8 色板（`chart_palette(i)`）。
- **轴域与命中同源**：渲染、刻度、hover 命中都消费同一份 `LinearScale` / `BandScale`，不要各算一遍；`axis_x`/`axis_y` 的 `min`/`max` 显式指定即锁定域，否则按数据 nice 化（含 0 基线由 `include_zero` 控制）。
- **绘制不得越出 `bounds`**：`Widget::paint_bounds_` 决定脏区，越界像素不会被擦除（残影）；悬浮值框 / 十字准线 / 百分比标签都按可用区夹取或翻转。
- **无头渲染 golden**：`render_to_png` 无 `Application` ⇒ `Animator::current() == nullptr` ⇒ grow-in 动画进度恒为 1（终态），故 golden 基线稳定可复现。
- 可编译样例见 `examples/demos/demo_bar_chart.cpp` / `demo_line_chart.cpp` / `demo_pie_chart.cpp` / `demo_scatter_chart.cpp` / `demo_sparkline.cpp`；控件契约见 `specification/04-widget.md` §3.8，矢量原语（`Polyline` / `Sector`）见 `specification/03-layout-render.md` §8.1。

---

## 34 基线对齐（CrossAxisAlignment::Baseline）

`Row` 内各子项按**首行文本基线**对齐：不同字号的文字、图标、按钮同行时视觉下沿不再参差。几何契约见 [`specification/03-layout-render.md`](specification/03-layout-render.md) §3.8。

```cpp
#include "aurora/aurora.h"

using namespace au;

auto main() -> int {
    // ① 不同字号：小字号整体下移，首行基线共线。
    Row mixed;
    mixed.add(Node{Text{"12pt"}.font_size(12.0F)});
    mixed.add(Node{Text{"28pt"}.font_size(28.0F)});
    mixed.set_cross_axis_alignment(CrossAxisAlignment::Baseline);
    mixed.set_gap(12.0F);

    // ② 无基线子项（如图标：任何未覆写 baseline_distance 的控件）按 CSS 式合成基线 =
    //    自身交叉轴底边参与对齐，不会 assert 崩溃。
    Column icon;  // Column 无基线钩子 ⇒ nullopt
    icon.modifier.set(Modifier{}.size(20.0F, 20.0F).background(Color{37, 99, 235, 255}));

    // ③ 内边距与按钮：Modifier 的内边距把内容盒下移，容器会补入该位移；
    //    Button 的钩子与 paint_label 同源（标签居中偏移 + ascent）。
    Row with_button;
    auto padded = Text{"padded"};
    padded.modifier.set(Modifier{}.padding(10.0F));
    with_button.add(Node{padded});
    with_button.add(Node{Button{"OK"}});
    with_button.set_cross_axis_alignment(CrossAxisAlignment::Baseline);

    Application app{Scene{Column{ColumnProps{.children = {Node{mixed}, Node{icon}, Node{with_button}}}}}};
    return app.run();
}
```

要点：

- **仅水平主轴有意义**：`Column` 的交叉轴是水平的，`Baseline` 在 `Column` 上按 `Start` 处理，并每实例发一次 `Diagnostics::degraded`（不逐帧刷屏）。
- **无基线子项宽容降级**：钩子返回 `std::nullopt` 即走合成基线（自身底边）；全部子项都无基线时整体退化为 `End` 对齐。
- **自定义控件接入**：覆写 `Widget::baseline_distance(const BuildContext &) const -> std::optional<float>`，返回**内容盒顶 → 首行基线**的距离（含自身内边距/居中偏移）；**不要**在其中做整像素 `floor` snap——容器按 `max_above - baseline` 定位后，绘制侧 pen_y 的 ascent 与之相消，各子项实绘基线才能像素一致。
- **可编译样例**：`examples/demos/demo_baseline.cpp`。

---

## 35 滚动位置保存/恢复

滚动容器的位置默认随控件销毁而消失（标签页切换、详情页返回、热重载、进程重启）。给容器一个 `restore_key` 即接入 `app::ScrollStorage`：重建或重启后回到原位置。

```cpp
#include "aurora/app/scroll_storage.h"
#include "aurora/preferences/preferences.h"
#include "aurora/aurora.h"

using namespace au;

auto main() -> int {
    // ① 绑定持久化后端（文件模式 = 跨进程；内存模式 = 仅会话内）。
    auto &prefs = preferences::Preferences::instance("app");
    ScrollStorage::instance().attach(prefs);

    // ② 容器声明 restore_key：同 key 的新实例会恢复位置（不同 key 互不干扰）。
    auto list = std::make_shared<LazyList>(
        1000, [](int i) -> Node { return Node{Text(std::to_string(i))}; }, 48.0F);
    list->set_restore_key("feed");

    // ③ 位置变化（滚轮 / 拖拽 / set_scroll_offset）自动写回注册表。
    Application app{Scene{Node{list}}};
    const int rc = app.run();

    // ④ 退出前批量落盘（运行中亦可随时 sync + flush）。
    ScrollStorage::instance().sync();
    (void)prefs.flush();
    ScrollStorage::instance().detach();
    return rc;
}
```

要点：

- **四个控件同一契约**：`Scroll` / `LazyList` / `LazyRow` / `GridView` 都支持 `restore_key`（空 = 不参与）；恢复只在**首次可滚动布局**生效一次，用户主动滚动不会再被回拉。程序化跳转另有 `Scroll::set_offset(offset) -> bool`。
- **写回是内存操作**：滚动帧只更新内存 map（零 JSON / 零磁盘），`sync()` 批量写穿 `Preferences` 内存，落盘时机由 `flush()` 决定——与窗口几何持久化同一哲学。
- **多窗口不串味**：`WindowHost::render_frame` 每帧建立本窗口作用域（键 = `scope + 0x1f + key`）；无头 / 无宿主路径退化为全局单桶。
- **重复键**：同一 `restore_key` 被两个控件认领时按「后写覆盖」工作并在认领路径提示一次（不在滚动热路径刷屏）；确需两处独立记忆请用不同 key。
- **可编译样例**：`examples/demos/demo_scroll_restore.cpp`。

---

## 36 列表拖拽重排

需要用户手动排序的列表（任务、播放队列、仪表盘卡片）用 `ReorderableList<T>`：按住条目拖动换位，跟手 + 其余项实时让位 + 松手弹簧落位，数据顺序由控件自己改写。

```cpp
#include "aurora/widget/reorderable_list.h"
#include "aurora/aurora.h"

using namespace au;

auto main() -> int {
    auto items = std::make_shared<State<std::vector<std::string>>>(
        std::vector<std::string>{"需求评审", "接口联调", "写回归用例"});

    auto list = std::make_shared<ReorderableList<std::string>>(
        items,
        [](const std::string &value, int index) -> Node {
            auto row = std::make_shared<Row>();                 // 条目自带背景与内边距
            row->modifier.set(Modifier{}.background(pal::AURORA_SURFACE, 6.0F).padding(10.0F));
            row->add(Node{Text(value)});
            return Node{std::shared_ptr<Widget>(row)};
        },
        8.0F);
    list->set_drag_handle(true);   // 条目含可点击控件时**必须**：手柄带留给列表（见下）

    // 数据已由控件改写，这里只做持久化 / 派生状态刷新。
    list->set_on_reorder([items](int from, int to) -> void {
        save_order(items->get());   // from → to（下标均为改写后的语义）
    });

    auto viewport = std::make_shared<Column>();                 // 给明确高度 ⇒ 列表按父约束取视口
    viewport->modifier.set(Modifier{}.size(360.0F, 420.0F));
    viewport->add(Node{std::shared_ptr<Widget>(list)});

    Application app{Scene{Node{viewport}}};
    return app.run();
}
```

要点：

- **控件改写数据**：松手落位后控件把 `State<std::vector<T>>` 旋转到新顺序再触发 `on_reorder(from, new_index)`；宿主要复用顺序请读 `items->get()`，别在回调里再改一次（会重复换位）。程序化重排用 `list->reorder(from, to)`，`to` 是**落位后的最终下标**。
- **手柄带 vs 整项拖**：条目若自带 `Clickable` / `Button`，其 Press 会被子项消费（冒泡 stop-on-handled），列表收不到按下 ⇒ 只能 `set_drag_handle(true)`（右侧 48dp 手柄带由列表自留）；条目是纯展示（`Row`/`Text`，无点击）时整项可拖（默认）。
- **落位与让位**：让位是**绘制期偏移**（`Node::bounds` 不动，命中链同步补偿），松手 spring 落位后才提交数据；`reduce_motion` 下直接落位。换位判定带 ±2dp 滞回。
- **近边缘自动滚动**：被拖项进入视口上下 `auto_scroll_threshold`（默认 48dp）带内按比例滚动，且滚动量吃进跟手位移（被拖项屏幕位置守恒）；拖拽期间滚轮被吞。
- **虚拟化列表不重排**：长列表请用 `LazyList`（只读滚动）；重排是全量实例化（适合 <500 项）。
- **可编译样例**：`examples/demos/demo_reorderable_list.cpp`。

---

## 37 无障碍：让读屏读出你的界面

Aurora 的无障碍**默认在跑**——你不需要为「让 NVDA 读出界面」写任何接线代码。桥在首个读屏查询到达时才构造，无读屏在线时零开销（不建语义树、不发平台事件）。

### 37.1 开箱即得的默认行为

```cpp
au::Scene build_ui() {
    return au::Column{
        au::Text{au::TextProps{ .content = "订单总额" }},
        au::Button{au::ButtonProps{ .label = "提交" }},
        au::TextInput{au::TextInputProps{ .value = "abc", .placeholder = "请输入" }},
        au::Slider{au::Reactive{volume}},
        au::Checkbox{au::Reactive{agreed}},
    };
}

au::WindowOptions wopts;
wopts.title = "我的应用";
wopts.size = au::Size{ .width = 520.0F, .height = 420.0F };
auto win = au::create_native_window(wopts);
au::Application app{ build_ui(), std::move(win.value()) };
app.run();          // 读屏（讲述人 / NVDA / Inspect.exe）此时已能读出控件
```

跑起来后打开 Windows 自带的「讲述人」或 Inspect.exe，即可看到树形结构与各控件的 Name / ControlType / 可用 pattern。**角色、状态、取值域与文本都从控件自身读出**：

| 你用的控件 | 读屏看到 |
|:---|:---|
| `Button` | ControlType=Button，Name=标签文本，可用 `Invoke` 动作（读屏可「按下」它，走真实点击路径） |
| `TextInput` | ControlType=Edit，Name=placeholder，Value=当前文本，可用 `Value` 动作；选区 / 逐字导航经 TextPattern |
| `Slider` | ControlType=Slider，可用 `RangeValue`（可读写当前值、最小/最大/步长）与 `Value` |
| `Checkbox` / `Switch` | ControlType=CheckBox，状态位 `checkable` + `checked`，可用 `Toggle` |
| `Text` | ControlType=Text，Name=内容文本 |
| 图表族（`BarChart` 等） | ControlType=Image |
| `Scroll` | `Scroll` / `ScrollItem` pattern（含滚动到指定偏移） |

### 37.2 补一个可访问名

若控件的可见文字不是它的合适读法（或控件本身没有文字，如纯图形按钮），覆写 `accessibility_label()`：

```cpp
class IconButton : public au::LeafWidget {
  public:
    [[nodiscard]] auto accessibility_label() const -> std::string override { return "关闭"; }
    // ...
};
```

**名称回退链**（不覆写时的自动推导顺序）：

1. `accessibility_label()` 返回非空 → 用它；
2. 角色是 `Text` / `TextInput` → 用控件文本（`accessibility_text()`），再退到 `accessibility_value()`；
3. 否则看**唯一文本子节点**：子树里恰有 1 个 `Text` / `RichText` / `Label` 子节点时取它的标签。

> ⚠️ **兄弟标签不被自动关联**。`au::Checkbox` 与 `au::Slider` 是叶子控件、没有内建 label；本库里它们的标签通常是**兄弟** `Text` 而不是子节点，第 3 条回退因此取不到。目前须显式覆写 `accessibility_label()`（子类化），或在设计上接受该控件无读屏名。是否引入 `aria-labelledby` 式的标签关联 API 尚未裁决。

### 37.3 描述状态与取值（自定义控件）

```cpp
class Rating : public au::LeafWidget {
  public:
    [[nodiscard]] auto accessibility_role() const -> au::AccessibilityRole override {
        return au::AccessibilityRole::Slider;   // 借用平台既有语义
    }
    [[nodiscard]] auto accessibility_label() const -> std::string override { return "评分"; }
    [[nodiscard]] auto accessibility_state() const -> au::AccessibilityState override {
        auto s = au::Widget::accessibility_state();   // 先取基类（含 focused）
        s.read_only = false;
        return s;
    }
    [[nodiscard]] auto accessibility_range() const -> std::optional<au::AccessibilityRange> override {
        return au::AccessibilityRange{ .min = 0.0, .max = 5.0, .step = 1.0, .value = value_ };
    }
    auto perform_accessibility_action(const au::AccessibilityActionRequest &req) -> bool override {
        if (req.action == au::AccessibilityAction::Value) {
            value_ = std::clamp(req.number, 0.0, 5.0);
            mark_needs_paint();
            return true;
        }
        return au::Widget::perform_accessibility_action(req);   // 其余交基类默认路由
    }
  private:
    double value_ = 0.0;
};
```

`perform_accessibility_action` 的**基类默认实现**把动作路由到真实事件路径（聚焦 / 点击 / 调用 / 滚动经 `EventDispatcher`），所以「读屏按下按钮」与「鼠标按下按钮」走同一条码路——不覆写即正确。

### 37.4 动态播报（toast / 异步结果）

焦点不变、取值不变的临时文本只会被忽略。用 `announce` 显式请求朗读：

```cpp
save_button.set_on_click([&] {
    const auto ok = do_save();
    save_button.announce(ok ? "已保存" : "保存失败，请重试");
});

// 无控件归属的全局提示（如后台任务完成）
au::notify_accessibility_announcement("导出完成，共 128 条", nullptr);
```

播报走**独立事件通道**，不经语义树 diff，因此不受「无焦点/取值变化」限制。对应平台映射：UIA `UiaRaiseNotificationEvent`（老系统回退 `LiveRegionChanged`）、macOS `announcementRequested`、AT-SPI2 `object:announcement`、Wasm `aria-live`。

### 37.5 排除装饰性控件

纯装饰的图形 / 分隔线不该被读屏逐个念出来。覆写 `accessibility_is_semantic()` 返回 `false` 即把它（及其标记含义）从语义树里摘掉：

```cpp
class Divider : public au::LeafWidget {
  public:
    [[nodiscard]] auto accessibility_is_semantic() const -> bool override { return false; }
};
```

无 label 的 `Image` 与「无名 + 无交互」的纯布局容器无需你动手：共享层已按同一规则把前者标记为非控件、把后者排除出内容视图（避免读屏不停念「pane」）。

### 37.6 想验证时用什么

| 手段 | 看什么 |
|:---|:---|
| **讲述人**（Win+Ctrl+Enter） | 端到端朗读体验；Tab 走焦点、空格/回车激活 |
| **Inspect.exe**（Windows SDK） | UIA 元素树、属性清单、可用 pattern；`FrameworkId` 应显示 `Aurora` |
| `tools/verify/win32_ua_live_probe` | 真机自动化验收：以 COM UIA 客户端（与读屏同路径）遍历控件视图并与期望表比对（见 [`specification/08-tooling.md`](specification/08-tooling.md) §7.5） |
| 单元测试 | 平台中立层：快照 / diff / 偏移映射 / 动作路由；`utest_a11y_*` 四个套件 |

### 37.7 常见坑

- **只读钩子别有副作用**：`accessibility_*()` 会在任意平台查询时刻被调用（可能远多于你的绘制次数），其间不要改控件状态、不要发布局请求、不要触发事件。
- **别把语义树当性能热点**：重建是**拉取式**的（平台查询到达才做全量重投影），不进帧循环；但若你有上千节点的大列表，请优先用虚拟化容器，别让读屏一次拉全量。
- **`runtime_id` 不是 `Node::id`**：前者是进程级原子自增的无障碍身份（构造时分配、生命周期恒定），后者是树内定位标识。桥用前者保证「同一控件跨事件可比」。

---

## 38 滚动增强：吸附分页 / 下拉刷新 / 吸顶头部 / 滚动驱动动画

轮播与卡片流要**整页对齐**、分组列表要**头部钉顶**、信息流要**下拉刷新**、视差与进度条要**跟着滚动偏移走**。这四件事共用同一套滚动内核（`ScrollSnap` + `ScrollGlide` + `ScrollEvent::remaining_y` + `offset_signal()`），不需要第三方组件。

### 38.1 整页翻页与条目吸附

```cpp
#include "aurora/aurora.h"

using namespace au;

// 整页：周期取视口高，配 Start / Center / End 对齐方位
au::Scroll pager{au::ScrollProps{
    .child = au::Node{build_pages()},
    .snap = ScrollSnap::page(ScrollSnapAlignment::Start),
}};

// 条目吸附：按固定周期（= 行高 / 格高）收位，居中展示
auto list = std::make_shared<LazyList>(200, [](int i) -> Node { return Node{Text(std::to_string(i))}; },
                                       48.0F);
list->set_snap(ScrollSnap{.extent = 48.0F, .alignment = ScrollSnapAlignment::Center});
```

收位不是跳变：滚轮落点后 150ms easeOutCubic 短滑动收敛到对齐点（自驱动 `tick_gestures`，不占 `Animator`）。程序化滚动用 `scroll_to(offset, animate)`（`LazyList` 另有 `scroll_to_item(index, animate)`），`animate = false` 即时落位；`set_offset` / `set_scroll_offset` 会**作废进行中的滑动**。观测点 `is_gliding()`。

### 38.2 下拉刷新

```cpp
auto scroll = std::make_shared<Scroll>(ScrollProps{.child = Node{build_feed()}});
auto pull = std::make_shared<PullToRefresh>(
    PullToRefreshProps{.child = Node{std::move(scroll)}, .threshold = 64.0F, .max_pull = 128.0F});

std::weak_ptr<PullToRefresh> weak_pull = pull;   // 回调可能晚于控件析构
pull->on_refresh([weak_pull]() -> void {
    fetch_remote([weak_pull]() -> void {
        if (auto p = weak_pull.lock()) {
            p->finish_refresh();                 // 收拢指示器；不调用则 spinner 常驻
        }
    });
});
```

指示器是覆盖层（顶部半透明带 + 弧线 spinner），不改布局盒、不推挤内容。`state()` / `pull_distance()` / `progress()` 供宿主联动（例如把 `progress()` 绑到文案或图标旋转）。

### 38.3 吸顶分组头部

```cpp
au::Scroll grouped{au::ScrollProps{.child = au::Node{au::Column{au::ColumnProps{
    .children = {
        au::Node{au::StickyHeader{au::Node{au::Text{"今天"}}}},
        au::Node{message_row(1)},  // ...
        au::Node{au::StickyHeader{au::Node{au::Text{"昨天"}}}},
        au::Node{message_row(2)},  // ...
    }}}}}};
```

`StickyHeader` 正常参与布局（占自身高度、随内容滚），滚过视口顶部后由宿主按 pin 位压顶重绘；下一条头部逼近时把本条**顶出**（CSS `position: sticky` 同语义）。宿主为 `Scroll`（多个头部依次堆叠）或 `LazyList`（粘性项作为普通行实例化）。

### 38.4 滚动驱动动画与嵌套滚动

```cpp
auto scroll = std::make_shared<Scroll>(ScrollProps{.child = Node{build_hero()}});

// 偏移信号：任何滚动通道（滚轮 / 拖拽 / 收位滑动帧 / 程序化）落位即发布
auto fade = std::make_shared<State<float>>(1.0F);
au::Effect drive([&]() -> void {
    const float offset = scroll->offset_signal().get();   // Effect 作用域内读取即订阅
    fade->set(std::clamp(1.0F - offset / 200.0F, 0.0F, 1.0F));
});
```

嵌套滚动无需接线：滚轮判给**最近可滚动祖先**，内层滚到端点后未消费的量经 `ScrollEvent::remaining_y` 上冒给外层（故 `PullToRefresh{Scroll{...}}` 到顶即下拉、外层 `Scroll` 内嵌 `LazyList` 内层先滚）。路由细节见 [`specification/05-event-navigation.md`](specification/05-event-navigation.md) §3.3。

要点：

- **吸附三属性同一套键**：`snap_extent` / `snap_paging` / `snap_alignment` 在 `Scroll` / `LazyList` / `GridView` 上语义与序列化一致（`LazyRow` 暂无吸附）；`snap_extent <= 0` 且非分页 = 关闭。
- **分页末尾停在整页对齐点**：剩余内容不足一整页时结果夹到 `max_scroll_offset`，不会停在半页。
- **`reduce_motion` 全程短路**：收位滑动、`scroll_to(…, true)`、下拉回弹在系统「减少动态效果」下直落端点，不产生中间帧（与 `Dismissible` / `ReorderableList` 一致）。
- **回调属运行时接线**：`on_refresh` 不进序列化，UI 重建 / 热重载后须重挂（`threshold` / `max_pull` 会随属性还原）。
- **可编译样例**：`examples/demos/demo_scroll_snap.cpp`、`examples/demos/demo_pull_to_refresh.cpp`、`examples/demos/demo_sticky_header.cpp`。

---

## 39 输入法组合输入（CJK / IME）

拼音 / 五笔 / 假名输入的本质是「有一段**还没定下来**的文字要显示，但不能进数据模型」。Aurora 把它收敛成一条事件 `TextCompositionEvent`：`preedit` 管显示，`committed` 管落字，`value()` 全程干净。

### 39.1 用现成文本控件：零接线

```cpp
#include "aurora/aurora.h"

using namespace au;

au::TextInput field{au::TextInputProps{.placeholder = "请输入姓名"}};
field.on_changed([](const std::string &s) -> void { /* 只收到已上屏文本 */ });
```

`TextInput` / `RichTextEdit` 已内置：preedit 下划线 + 待转换选区高亮、组合光标落在候选插入点、preedit 参与宽度测量（组合中的中文会撑开输入框，不被裁切）、失焦自动取消未上屏的组合。**应用侧一行都不用写。**

### 39.2 自定义文本控件：两个钩子

```cpp
class MyEditor : public Widget {
  public:
    // ① 收组合态：先落 committed，再更新 preedit 显示态
    auto on_text_composition(TextCompositionEvent &e) -> void override {
        if (!e.committed.empty()) {
            insert_at_caret(e.committed);          // 上屏：唯一写数据模型的时机
        }
        preedit_ = e.preedit;                      // 空串 = 组合结束 / 取消
        preedit_cursor_ = e.cursor_index;          // preedit 内「码点」下标
        mark_dirty();
    }

    // ② 候选窗定位盒：preedit 光标处的零宽竖盒（窗口逻辑 dp）
    [[nodiscard]] auto composition_caret_bounds() const -> Rect override {
        return caret_box_in_window_coords(preedit_cursor_);
    }
};
```

只读 / 禁用的编辑器应**吞掉**事件（不落字、不显示 preedit），别让它半进半退。

### 39.3 无头测试里模拟一段拼音

```cpp
TextCompositionEvent e;
e.preedit = "你好";
e.cursor_index = 2;              // 码点，不是字节
e.sel_start = 0;
e.sel_end = 2;                   // 含尾；AURORA_NO_SELECTION = 无待转换选区
ti.on_text_composition(e);       // 组合期：preedit 显示，value() 不变
AURORA_TEST_CHECK_EQ(ti.preedit(), std::string{"你好"});
AURORA_TEST_CHECK_EQ(ti.value(), std::string{""});

e.preedit.clear();
e.committed = "你好";            // 上屏：value() 才落字
ti.on_text_composition(e);
AURORA_TEST_CHECK_EQ(ti.value(), std::string{"你好"});
```

完整三段序列（preedit → 候选替换 → 上屏）、`max_length` 截断、只读吞输入、失焦取消、preedit 参与测量均由 `tests/unit/utest_text_input.cpp` 守住。

### 39.4 Win32 真机验收

```powershell
cmake --preset ninja                     # 如未配置
cmake -S . -B build -DAURORA_BUILD_VERIFY_TOOLS=ON   # 探针不在默认构建、也不进 CTest
cmake --build build --target aurora_verify_win32_ime
build\aurora_verify_win32_ime.exe --interactive
```

用微软拼音输入「你好世界」，目视：① preedit 带下划线且随拼音更新；② 候选窗出现在插入点旁而非屏幕左上角；③ 选字后只上一次屏；④ Esc 取消后无残留；⑤ 切走焦点再回来无半截拼音。

要点：

- **preedit 绝不进 `value()`**：数据模型、序列化与 golden 因此始终不含半截拼音；显示与测量走 `composed_text()`。
- **下标是码点**：`cursor_index` / `sel_*` 按码点折算，代理对不会被切半。
- **组合事件只到焦点控件**：不经命中链、不冒泡，故容器不会误吞。
- **只有 `Application` 驱动才有 IME**：裸 `Window` + `present_root` 没有任何事件处理器（连 Tab 焦点都不通），详见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §8.5。
- **非 Win32 后端目前只有契约**：macOS / X11 / Wayland / Wasm 缺平台桥，写了钩子也收不到事件；接线状态见 [`specification/05-event-navigation.md`](specification/05-event-navigation.md) §2.4。

---

## 40 命名记录仓储（Storage）

命名记录仓储：`id → Json / 原生二进制`，后端可插拔（Memory / Filesystem / SQLite），门面负责信封化、类型化、异步卸载与变更通知。

```cpp
#include "aurora/aurora.h"
using namespace aurora;
using namespace aurora::storage;

// 1) 默认文件系统后端（零依赖，始终可用）：每记录一个 JSON 信封文件
auto s = Storage::create(FilesystemOptions{.root = "C:/data/myapp_store"});
if (!s) { /* s.error() = StorageBackendUnavailable */ }
Storage &store = s.value();

// 2) JSON 记录 + 二进制记录（sidecar `<id>.bin`，零 base64 膨胀）
store.put("profile", Json{{"name", "ada"}, {"age", 36}});
auto profile = store.get("profile");        // Result<Json>
store.put("avatar", StorageBytes{std::byte{0x89}, std::byte{0x50}});
auto bytes = store.get_bytes("avatar");     // Result<StorageBytes>

// 3) SQLite 后端（需 CMake 开启 AURORA_ENABLE_STORAGE_SQLITE，默认 OFF）：真事务 + 内联 BLOB
#ifdef AURORA_ENABLE_STORAGE_SQLITE
auto db = Storage::create(SqliteOptions{.path = "C:/data/myapp.db"});  // 或 {.in_memory = true}
db.value().transaction([](Storage &tx) -> Result<void> {
    tx.put("k1", Json{1});
    tx.put("k2", Json{2});
    return {};                              // 返回错误 → 整体 ROLLBACK
});
#endif

// 4) 变更通知（主线程发射），Subscription RAII 退订
auto sub = store.on_change([](const StorageChange &ch) { /* ch.op / ch.id */ });
```

- **三后端契约统一**：Memory（内存，测试用，`Storage::create(std::make_unique<MemoryBackend>())`）/ Filesystem（默认）/ SQLite（可选编译），后端只认 `StorageRecord` 信封，见 [`specification/06-app-platform.md`](specification/06-app-platform.md) §9.2。
- **事务语义随后端**：SQLite 是真事务（BEGIN IMMEDIATE/COMMIT/ROLLBACK，嵌套并入外层）；Filesystem 顺序执行尽力而为，单条失败不撤销已完成的写。
- **类型化与迁移**：实现 `to_storage_json` / `from_storage_json`（或二进制版）的 `StorageStorable` 类型可直接 `put<T>` / `get<T>`，`version` 落后时自动走 `migrate_storage`。
- **异步**：`async_put` / `async_get` / `async_list` 等经 worker 线程卸载，返回 `Task<T>`。

---
