// ============================================================
// wasm_multiwin_live_probe — 浏览器多窗口事件路由真机验收探针（specification/06-app-platform.md §2.4）
// ------------------------------------------------------------
// 证明对象（`WasmSurface` 多窗口路由三通道 + 一条字符输入折算）：
//   ① 键盘路由：document 级单一分发器按「当前焦点 Surface」转发——点击 win-b 后敲字符/
//      Enter 只落 win-b 的输入框，win-a 计数不动（反之亦然）；
//   ② resize 广播：window 级 resize 经分发器遍历全部实例刷新各自 CSS 尺寸，两窗口尺寸
//      观测值随浏览器窗口缩放同步变化（canvas CSS 宽度为视口百分比，见 shell）；
//   ③ 焦点语义：构造即接管路由（末建窗口 win-b 天然先聚焦，无需点击）；
//   ④ 字符折算：KeyDown 恒发 KeyEvent，单字符可打印键**另发** TextInputEvent（X11 同口径）
//      ——缺它则 WASM 任何文本框打不进字符，① 也就无从验证。
//
// 观测通道：document.title 每帧发布（foc= 路由指针 + 两窗口输入文本/提交计数/尺寸），
// 自动化经 CDP `Runtime.evaluate document.title` 读取，不依赖像素 OCR。
//
// 无头 CI 无法证明以上任何一条（需要真实 DOM 事件路由与窗口 resize），故本探针不进
// CTest，按需人工触发。
//
// 构建（需 Emscripten 工具链，产物为 html + js + wasm）：
//   cmake -B build-wasm -DAURORA_BACKEND_WASM=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-wasm --target aurora_verify_wasm_multiwin
//
// 运行（须走 HTTP——浏览器拒绝 file:// 下的 wasm 流式编译）：
//   静态服务器指向产物目录后打开 aurora_verify_wasm_multiwin.html（或 emrun）。
//
// 逐项期望：
//   1. 初始标题含 `foc=win-b`（末建窗口接管路由，判据 ③）；
//   2. 点击 win-a 输入区 → 标题 `foc=win-a`；键入 "ab" → `A[ab/0]`，B 不动（判据 ①④）；
//      按 Enter → `A[.. /1]`（KeyEvent 通道的 on_submit）；
//   3. 点击 win-b 输入区 → 键入 "cd" → `B[cd/0]` 且 A 不变（判据 ① 反向）；
//   4. 缩放浏览器窗口 → 标题 `szA=/szB=` 宽度分量随视口宽度同步变化（判据 ②）；
//   5. 控制台无 Aborted / 未捕获异常。
//
// 退出语义：浏览器页面无正常退出路径，关闭标签页即回收（页面级 beforeunload 置位
// should_close 为申报语义，见 wasm_surface.h 类头）。
// ============================================================

#include <emscripten.h>

#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/window/wasm_surface.h"

namespace au = aurora;

namespace {

/// @brief 探针观测聚合：仅主线程（浏览器单线程事件环）读写，无需加锁。
/// ⚠️ 帧回调引用态必须堆持（shared_ptr）——rAF 模式 main 注册后即返回，捕获栈变量即悬空。
struct Obs {
    std::string a_text;  ///< win-a 输入框当前值（TextInputEvent 通道）。
    std::string b_text;
    int a_sub = 0;       ///< Enter 提交次数（KeyEvent 通道）。
    int b_sub = 0;
};

auto publish_title(const std::string &title) -> void {
    static std::string last;
    if (title == last) {
        return;  // 同一内容不重复写 DOM
    }
    last = title;
    // EM_ASM 的 `$0` 占位符含 `$` 标识符扩展（-Wpedantic 下告警），属 Emscripten 惯例写法。
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸（浏览器下即未捕获异常，验收期望 5）
auto main() -> int {
    auto obs = std::make_shared<Obs>();

    // 两窗口各一个 TextInput：on_changed 证字符折算（④），on_submit 证控制键（① KeyEvent 路）。
    au::TextInput ta;
    ta.set_on_changed([obs](const std::string &v) -> void { obs->a_text = v; })
        .set_on_submit([obs](const std::string &) -> void { ++obs->a_sub; });
    au::TextInput tb;
    tb.set_on_changed([obs](const std::string &v) -> void { obs->b_text = v; })
        .set_on_submit([obs](const std::string &) -> void { ++obs->b_sub; });

    au::Node root_a = au::Column{
        au::Text{au::LocalizedString{"window A (canvas win-a)"}},
        std::move(ta),
    };
    au::Node root_b = au::Column{
        au::Text{au::LocalizedString{"window B (canvas win-b)"}},
        std::move(tb),
    };

    au::WasmOptions oa;
    oa.size = au::Size{.width = 640.0F, .height = 260.0F};
    oa.canvas_id = "win-a";
    oa.title = "mw-A";
    au::WasmOptions ob = oa;
    ob.canvas_id = "win-b";
    ob.title = "mw-B";

    auto ra = au::create_window(oa);
    auto rb = au::create_window(ob);
    if (!ra || !rb) {
        publish_title("mw PROBE-ERROR create_window failed");
        return 1;
    }
    au::Window *wa = ra.value().get();
    auto *wb = rb.value().get();

    // ⚠️ rAF 契约：run() 注册后即返回、main 随即结束——`Application` 若活在 main 栈上，
    // 析构摘除 raf_owner_ 守卫令蹦床下一拍自停（本探针首版正是如此零帧翻车）。与
    // App::launch 同法：函数级 static unique_ptr 堆持至页面生命周期。
    static std::unique_ptr<au::Application> app_keep_alive;
    app_keep_alive = std::make_unique<au::Application>(au::Scene{std::move(root_a)}, std::move(ra.value()), oa);
    app_keep_alive->open_window(std::move(rb.value()), au::Scene{std::move(root_b)}, ob);

    // 每帧发布观测标题（帧循环本身活着即 rAF 接线回归，见 wasm_raf_live_probe）。
    app_keep_alive->set_on_frame([obs, wa, wb]() -> void {
        const au::Size sa = wa->size();
        const au::Size sb = wb->size();
        const std::string foc = au::WasmSurface::focused_canvas_id();
        publish_title("mw foc=" + (foc.empty() ? std::string{"-"} : foc)  //
                      + " A[" + obs->a_text + "/" + std::to_string(obs->a_sub) + "] "        //
                      + "B[" + obs->b_text + "/" + std::to_string(obs->b_sub) + "] "          //
                      + "szA=" + std::to_string(static_cast<int>(sa.width)) + "x"             //
                      + std::to_string(static_cast<int>(sa.height)) + " "                     //
                      + "szB=" + std::to_string(static_cast<int>(sb.width)) + "x"             //
                      + std::to_string(static_cast<int>(sb.height)));
    });
    app_keep_alive->run();
    return 0;
}
