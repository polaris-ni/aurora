# GUIDELINE_ASYNC_SERIAL

> 本文件由 [`GUIDELINE.md`](../GUIDELINE.md) 划分而出（异步任务 / 定时任务 / CPU 节能 / 渲染性能测量 / 序列化 / YAML / API 描述）。
> 返回主线见 [`GUIDELINE.md`](../GUIDELINE.md)。
>
> 片段约定：本文所有片段使用 `namespace au = aurora;`（即 `au::` 前缀）。复制任意单段时，请确保该别名（或 `using namespace aurora;`）已在所处 TU 声明，否则 `au::Xxx` 编译失败。

**本文包含章节：**

- [10. 异步任务（#19）](#10-异步任务19)
- [10b. 定时任务（Scheduler / Timer）（#H.10b）](#10b-定时任务scheduler--timerh10b)
- [10c. CPU 节能与硬件加速上屏（CPU 性能专项）](#10c-cpu-节能与硬件加速上屏cpu-性能专项)
- [10d. 渲染性能测量（确定性基准）](#10d-渲染性能测量确定性基准)
- [11. 序列化：树 ⇄ JSON（#13）](#11-序列化树--json13)
- [12. 树 ⇄ 源码（#22）](#12-树--源码22)
- [12b. 树 → YAML（to_yaml）](#12b-树--yamlto_yaml)
- [13. 生成 API 描述（#12）](#13-生成-api-描述12)

## 10. 异步任务（#19）

回调式（`au::async` 底层经有界 `aurora::ThreadPool::default_pool()` 执行，不再裸起 detached 线程）：

```cpp
au::async([]() -> int {            // 后台线程执行
    int s = 0; for (int i=1;i<=100;++i) s += i;
    return s;                       // 或返回 Result<int>
}).with_timeout(std::chrono::seconds(5))   // opt-in 超时
 .then([](const Result<int>& r) {  // 结果回主线程
    if (r) out->set(r.value());     // 回写 State 触发刷新
    else   Diagnostics::error(r.error().message); // 含 async-timeout
});
```

协程式（`co_await` 在后台执行，续体回主线程）：

```cpp
au::CoroTask<void> load() {
    au::Result<int> r = co_await au::co_async([]() -> int {
        int s = 0; for (int i=1;i<=100;++i) s += i;
        return s;
    });
    if (r) out->set(r.value());
}
au::launch(load());                // 启动顶层协程
```

可复用线程池（公开 API）：

```cpp
aurora::ThreadPool& pool = aurora::ThreadPool::default_pool();
auto fut = pool.submit([] { return heavy_compute(); });  // 返回 std::future<R>
```

---

## 10b. 定时任务（Scheduler / Timer）（#H.10b）

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
Node clock = au::Timer(1s, [](const au::SignalView<int> &tick) {
    return au::Text(au::computed([&] { return "tick: " + std::to_string(tick.get()); }));
});

// 自动显现：2s 后 on_tick 置位 shown
auto shown = std::make_shared<au::State<bool>>(false);
Node reveal = au::Timer(2s,
    [shown](const au::SignalView<int> &) { return au::Show(shown, au::Text(au::LocalizedString{ "revealed!" })); },
    [shown](int) { shown->set(true); });
```

- `set_timeout(d, cb)` 一次性；`set_interval(period, cb)` 周期；返回值 `TimerHandle` 可 `cancel()`、`active()` 查询。
- `Timer` 挂载时向 `Scheduler::current()` 注册，析构自动取消；无运行中 App 时降级（记 `Diagnostics::warn`）不崩溃。

> **`au::LocalizedString`**：可本地化字符串（i18n 运行时，规格 §8）。`au::LocalizedString{ "revealed!" }` 由 `const char*` 构造，持有字面文本，未启用本地化时直接显示（查表失败也回退为 `text`）。需按 `Locale` 查 `StringTable` 解析时用 `LocalizedString::tr("key", {args...})`（`localize = true`）；控件层（如 `Button::label`）统一吃 `LocalizedString`，故 `Text{ .content = "Hi" }` 可隐式转换。

---

## 10c. CPU 节能与硬件加速上屏（CPU 性能专项）

帧循环默认已是「事件驱动 + 帧节流」：静态界面 idle 时自动阻塞等待事件（CPU 趋近 0），**无需任何额外代码**。仅在特殊需求时调整：

```cpp
au::WindowOptions opts;
opts.title = "My App";
opts.max_fps = 120;     // 活跃帧（动画/脏区）帧率上限；0 = 不限帧率
opts.power_saving = true; // 默认开；false = 忙轮询（持续自驱动重绘场景才需）
opts.renderer = au::RendererPreference::Auto; // 默认：有 D3D11 则 GPU 上屏，否则软件 GDI
auto win_res = au::create_window(au::Win32Options{ opts });
au::Application app{ build_ui(), win_res ? std::move(win_res.value()) : nullptr, opts };
app.run(); // idle 均深睡，输入/定时器/后台回投均可即时唤醒
```

开启 GPU 上屏（需 CMake `-DAURORA_BACKEND_D3D11=ON`）：

```cpp
opts.renderer = au::RendererPreference::GpuD3D11; // 强制 D3D11；不可用时 create_window 返回 renderer-unavailable 错误
// 或 RendererPreference::Software 强制软件 GDI；未开启时 Auto 自动走软件
```

- 静态界面实测稳态 CPU 0.0%（`tools/bench_idle_cpu.exe`）；持续重绘场景（如自定义每帧动画）可 `opts.power_saving = false` 或 `app.window()->enable_dirty_tracking(false)` 退回不限速重绘。
- `RendererPreference` 仅影响「像素如何上屏」；所有绘制仍由软件 `Painter` 完成，widget 层不感知后端。

## 10d. 渲染性能测量（确定性基准）

> 硬规则：**时间类**门槛一律在 `Release + PROFILING=OFF` 下测量（`build/`）；**计数类**门槛（`RenderCounters` 各字段、脏区面积、整帧重绘帧数）在 `Release + PROFILING=ON` 下测量（`build-prof/`）。Debug 默认开 PROFILING，禁止拿 Debug 数据填基线表。

**原语 / 整帧光栅基准**（`tools/bench_render.cpp`，输出 markdown 表到 stdout）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BUILD_DEMOS=OFF
cmake --build build --target bench_render
./build/bench_render.exe          # 场景 × 分辨率 × scale 的 ms/帧矩阵
```

**滚动场景基准**（`tools/bench_scroll.cpp`，用 `ScrollBenchHarness` 对业务树确定性采样）：

```bash
cmake --build build --target bench_scroll
# 时间口径（Release+OFF）：独立进程多次取最小，抵消离屏缓冲堆碎片噪声
for i in 1 2 3; do ./build/bench_scroll.exe --scene google_play --format csv; done
# 计数口径（Release+ON）：确定性，可直接锁基线
cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_ENABLE_PROFILING=ON -DAURORA_BUILD_DEMOS=OFF
cmake --build build-prof --target bench_scroll
./build-prof/bench_scroll.exe --scene google_play --format csv
```

**在代码里直接用 `ScrollBenchHarness` 做自证采样**（先查 `trustworthy()` 再读性能指标）：

```cpp
#include "aurora/perf/scroll_bench.h"
using namespace au;
auto cfg = ScrollBenchHarness::Config{};
cfg.name = "my-scroll-tree"; cfg.frames = 300; cfg.warmup_frames = 30;
cfg.delta_per_frame = 12.0f;  // 单位 dp；harness 运行期标定 dp_per_unit 后换算下发
const auto r = ScrollBenchHarness::run(build_my_tree(), Size{1100, 760}, cfg);
if (!r.trustworthy()) { AURORA_LOG_ERROR("bench", "采样不可信：", r.to_markdown()); return; }
// 验收只看三项，不看 avg：
AURORA_LOG_RAW("bench", r.p99_ms(), " ", r.jitter_ms(), " ", r.full_redraw_frames(), "\n");
// 计数器锚点（须 Release+ON 构建）：Scroll 离屏缓冲字节数峰值
AURORA_LOG_RAW("bench", "scroll_buffer_bytes peak=", r.counters_max().scroll_buffer_bytes, "\n");
```

---

## 11. 序列化：树 ⇄ JSON（#13）

```cpp
Json j = au::serialization::to_json(root.widget());        // 结构快照
auto back = au::serialization::from_json(j);                // 重建真实 widget
// 差异补丁
auto patch = au::serialization::diff(aJson, bJson);
au::serialization::apply_patch(aJson, patch);               // a 变为 b
```

---

## 12. 树 ⇄ 源码（#22）

```cpp
std::string code = au::serialization::to_code(root.widget());
// 输出等效的 au::Column(au::ColumnProps{...}) 源码，可反贴回项目
```

---

## 12b. 树 → YAML（to_yaml）

```cpp
#include "aurora/aurora.h"
using namespace aurora;

// 1) Widget 树 → YAML 字符串
std::string yaml = au::serialization::to_yaml(root.widget());
// 输出示例：
// type: Column
// gap: 12
// children:
//   - type: Text
//     text: Hello
//     font_size: 24
//   - type: Button
//     label: Click

// 2) Json 值 → YAML 字符串（底层 API）
Json j = au::serialization::to_json(root.widget());
std::string yaml2 = au::serialization::to_yaml(j);

// 3) CLI 用法
// $ aurora to-yaml tree.json
```

> 注：`to_yaml` 仅输出方向，无 `from_yaml`。内部经 `yaml.h` 的递归下降 YAML 发射器将 JSON 转为 YAML，与 `to_code` 平行作为输出格式适配层。

## 13. 生成 API 描述（#12）

```bash
cmake --build build --target aurora_api_json   # 推荐：内部跑 gen_api_tools 直写 aurora_api.json
# 或等价手动：gen_api_tools "aurora_api.json"
```

输出包含全部已注册 widget 类型、属性键，以及核心枚举（Color 调色板 / LengthKind /
Alignment / KeyCode / Curve），供 LSP、文档生成器、设计工具使用。

---

