// ============================================================
// wasm_raf_live_probe — 浏览器 rAF 帧循环真机验收探针（specification/06-app-platform.md §10）
// ------------------------------------------------------------
// 证明对象：`Application::run()` 在真实浏览器中经 `emscripten_request_animation_frame`
// 驱动统一帧循环（无阻塞主线程、每 vsync 推进一帧），并连带验收两条同帧接线的旁路：
//   ① 渲染：Canvas 2D 上屏（脏区重绘随帧可见）；
//   ② 事件：DOM pointer 事件 → on_click → 响应式 State → 下一帧文本更新；
//   ③ 异步：`au::async().then()` 续体——后台任务经 ThreadPool **deferred 模式**在
//      `Application::step_frame()` 帧尾 `pump()` 执行，回投经主线程投递器在下一帧排水。
//
// 无头 CI 无法证明以上任何一条（需要真实 requestAnimationFrame / Canvas / DOM 事件），
// 故本探针不进 CTest，按需人工触发。
//
// 构建（需 Emscripten 工具链，产物为 probe.html + probe.js + probe.wasm）：
//   emcmake cmake -S . -B build-wasm -DAURORA_BACKEND_WASM=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-wasm --target aurora_verify_wasm_raf
//
// 运行（任选其一，须走 HTTP——浏览器拒绝 file:// 下的 wasm 流式编译）：
//   emrun build-wasm/tools/verify/aurora_verify_wasm_raf.html   # 自动起本地服务+开浏览器
//   # 或手动：静态服务器指向产物目录后打开 index（HTML 同名即可）
//
// 逐项期望（人工或浏览器自动化均可判）：
//   1. 页面标题 `aurora-wasm-raf frames = N` 的 N 持续自增（≈60/s）→ rAF 循环活着；
//      标题是最稳定的自动化观测点（document.title 由 on_frame 每帧 EM_ASM 写入）。
//   2. 画布出现计数 UI；点击「+1」按钮 → 「clicks = N」随帧 +1 → 事件接线成立；
//   3. 载入后 1 秒内「async = done」出现 → deferred 线程池帧尾排空 + 续体回投成立；
//   4. 控制台无 Aborted / 未捕获异常。
//
// 退出语义：浏览器页面无正常退出路径，关闭标签页即回收（App::launch 的堆持为
// 页面生命周期语义，见 application.h 注释）。
// ============================================================

#include <emscripten.h>

#include <chrono>
#include <memory>
#include <string>

#include "aurora/aurora.h"

namespace au = aurora;

/// @brief 每帧把帧数写入 document.title：给自动化（及肉眼）一个不依赖像素 OCR 的观测点。
static auto publish_frame_count(int frames) -> void {
    static int last_published = -1;
    if (frames == last_published) {
        return;  // 同一帧内不重复写 DOM
    }
    last_published = frames;
    const std::string title = "aurora-wasm-raf frames = " + std::to_string(frames);
    // EM_ASM 的 `$0` 占位符含 `$` 标识符扩展（-Wpedantic 下告警），属 Emscripten 惯例写法。
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
    // EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
    // clang-format off
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
    // clang-format on
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸（浏览器下即未捕获异常，验收期望 4）
auto main() -> int {
    // ⚠️ WASM rAF 模式下 main 注册帧循环后即返回：所有被帧回调引用的状态必须活在堆上
    // （shared_ptr/static），引用捕获 main 局部变量 = 悬空栈写——本探针首版正是这样翻车的
    // （帧计数别名到他人在用栈内存，出现 1.1e9 的跨运行恒定伪值）。
    auto frames = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"frames = 0"});
    auto frame_count = std::make_shared<int>(0);

    // ② 点击计数：DOM pointer 事件 → on_click → State → 下一帧重绘。
    auto clicks = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"clicks = 0"});
    auto click_count = std::make_shared<int>(0);
    au::Button b_plus{au::ButtonProps{.label = au::LocalizedString{"+1"}}};
    b_plus.on_click = [clicks, click_count]() -> void {
        ++*click_count;
        clicks->set(au::LocalizedString{"clicks = " + std::to_string(*click_count)});
    };

    // ③ 异步续体：任务体在帧尾 pump()（deferred 线程池）执行，then 经主线程投递器回投。
    //    无 sleep——deferred 模式下任务本就跑在主线程，模拟耗时只会卡帧。
    auto async_status = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"async = pending"});
    au::async([]() -> std::string {
        int acc = 0;
        for (int i = 0; i < 1000; ++i) {
            acc += i;
        }
        return acc == 499500 ? "done" : "mismatch";
    }).then([async_status](const au::Result<std::string> &r) -> void {
        async_status->set(au::LocalizedString{r ? "async = " + r.value() : "async = error"});
    });

    au::Node root = au::Column{
        au::Text{au::LocalizedString{"Aurora WASM rAF live probe"}},
        au::Text{au::TextProps{.content = au::Reactive{frames}}},
        au::Row{std::move(b_plus), au::Text{au::TextProps{.content = au::Reactive{clicks}}}},
        au::Text{au::TextProps{.content = au::Reactive{async_status}}},
    };

    // WASM 下 run() 注册 rAF 即返回（实例由 App::launch 页面生命周期堆持）；main 返回后
    // 浏览器事件环继续逐帧驱动帧循环，进程（标签页）关闭即整体回收。
    au::App()
        .title("aurora-wasm-raf")
        .size(640, 480)
        .view(std::move(root))
        .on_frame([frames, frame_count]() -> void {
            ++*frame_count;
            frames->set(au::LocalizedString{"frames = " + std::to_string(*frame_count)});
            publish_frame_count(*frame_count);
        })
        .run();
    return 0;
}
