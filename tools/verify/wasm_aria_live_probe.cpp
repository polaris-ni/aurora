// ============================================================
// wasm_aria_live_probe — Wasm ARIA 镜像桥真机验收探针（specification/06-app-platform.md §2.4 / ARCHITECTURE.md §8.4）
// ------------------------------------------------------------
// 证明对象（`WasmAriaBridge` 的三面一栈，全部须经真实浏览器无障碍栈才算数）：
//   ① 镜像面进 AX 树：隐藏镜像容器（clip 隐藏、非 display:none）内的 role/aria-* 元素
//      被浏览器无障碍引擎采纳——CDP `Accessibility.getFullAXTree` 能读到 button/checkbox/
//      textbox/slider 角色与名称（读屏视角的终局证据，无头单测只能证折算 JSON）；
//   ② 反向动作闭环：JS 侧 click 镜像元素 → 页面级队列 → 帧尾 `pump_actions()` →
//      `Widget::perform_accessibility_action`（Invoke/Toggle）→ 控件回调真实执行；
//   ③ 播报通道：`announce_accessibility` → 桥 `on_announcement` → 容器内
//      `aria-live="polite"` 区文本落 DOM；
//   ④ 增量 ops：首帧全量 applyFull 之后，状态变更只发 applyOps（DOM 恒等性/性能面）。
//
// 观测通道：document.title 每帧发布（两按钮计数 + 勾选态 + 输入框现值），镜像 DOM 与
// AX 树由 CDP 读取；全部触发（点击镜像、插桩计数）从 CDP 侧发起，探针自身无自驱动时序，
// 判定与执行顺序无关。
//
// 构建（需 Emscripten 工具链，产物 html + js + wasm，须走 HTTP 打开）：
//   cmake -B build-wasm -G Ninja -DAURORA_BACKEND_WASM=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-wasm --target aurora_verify_wasm_aria
//
// 逐项期望（CDP 断言口径）：
//   1. 标题以 `aria ` 开头且 c/a 计数在位（帧循环活、桥随首帧根注入诞生）；
//   2. `#aurora-a11y-win-a` 内按 role 可查得 button×2 / checkbox / textbox / slider，
//      checkbox 带 aria-checked、slider 带 aria-valuemin/max/now；
//   3. CDP AX 树含上述角色节点（①）；
//   4. 点击「确定」镜像按钮 → 标题 `c=1`、镜像按钮文本变 `确定·1`、textbox 值变 `clk1`（②）；
//   5. 点击 checkbox 镜像元素 → 标题 `cb=1` 且 DOM `aria-checked="true"`（② Toggle）；
//   6. 点击「播报」镜像按钮 → live 区文本 `aurora-live-1` 且带 aria-live=polite（③）；
//   7. 插桩计数：上述全部动作发生后 full==1、ops>=3（首帧一次全量、其余皆增量，④）；
//   8. 控制台无未捕获异常 / Aborted。
//
// 退出语义：浏览器页面无正常退出路径，关闭标签页即回收（申报口径同 wasm_raf_live_probe）。
// ============================================================

#include <emscripten.h>

#include <memory>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/accessibility.h"
#include "aurora/window/wasm_surface.h"

namespace au = aurora;

namespace {

/// @brief 探针观测聚合：仅主线程（浏览器单线程事件环）读写，无需加锁。
/// ⚠️ 帧回调引用态必须堆持（shared_ptr）——rAF 模式 main 注册后即返回，捕获栈变量即悬空。
struct Obs {
    int clicks = 0;  ///< 「确定」按钮经无障碍动作通道被执行次数（② Invoke 闭环）。
    int announces = 0;  ///< 播报按钮触发次数（③ 通道路过证明）。
    bool cb_checked = false;  ///< Checkbox 现值（on_changed 回写，② Toggle 闭环）。
    std::string entry_value;  ///< TextInput 现值（点击联动 clk-N）。
};

auto publish_title(const std::string &title) -> void {
    static std::string last;
    if (title == last) {
        return;  // 同一内容不重复写 DOM
    }
    last = title;
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

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸（浏览器下即未捕获异常，验收期望 8）
auto main() -> int {
    auto obs = std::make_shared<Obs>();

    // ---- 控件树：叶节点先建 Node 取裸指针（Node 以 shared_ptr 持 widget，拷贝同实例），
    //      回调再经指针闭包——点击「确定」同时改按钮标签与输入框值，令一次反向动作携带
    //      两处状态变更，供 ②（回调执行）与 ④（增量 ops）同源取证。
    // 下行目标类型由紧邻上一行构造的控件锁定（Button/TextInput/Checkbox/Slider 各自建 Node），
    // dynamic_cast 徒增 RTTI 依赖且把「构造即已知」的确定性换成运行期查找。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
    au::Node btn_node{au::Button{au::ButtonProps{.label = au::LocalizedString{"确定"}}}};
    auto *btn = static_cast<au::Button *>(&btn_node.widget());
    au::Node ann_node{au::Button{au::ButtonProps{.label = au::LocalizedString{"播报"}}}};
    auto *ann = static_cast<au::Button *>(&ann_node.widget());
    au::Node entry_node{au::TextInput{au::TextInputProps{.value = "abc"}}};
    auto *entry = static_cast<au::TextInput *>(&entry_node.widget());
    au::Node cb_node{au::Checkbox{au::Reactive<bool>{false}}};
    auto *cb = static_cast<au::Checkbox *>(&cb_node.widget());
    au::Node slider_node{au::Slider{au::Reactive<double>{25.0}}};
    auto *slider = static_cast<au::Slider *>(&slider_node.widget());
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)

    btn->set_on_click([obs, btn, entry]() -> void {
        ++obs->clicks;
        btn->set_label("确定·" + std::to_string(obs->clicks));
        entry->set_value("clk" + std::to_string(obs->clicks));  // ValueChanged 事件 ⇒ 桥置脏
        obs->entry_value = entry->value();
    });
    ann->set_on_click([obs, entry]() -> void {
        ++obs->announces;
        // 播报直发（不经 diff）：target=entry ⇒ live 区带 data-aurora-target 归属观测。
        aurora::announce_accessibility("aurora-live-" + std::to_string(obs->announces), entry);
    });
    cb->set_on_changed([obs](bool v) -> void { obs->cb_checked = v; });
    slider->set_range(0.0, 100.0).set_step(1.0);

    au::Node root = au::Column{
        au::Text{au::LocalizedString{"aria mirror probe"}},
        std::move(btn_node),
        std::move(ann_node),
        std::move(entry_node),
        std::move(cb_node),
        std::move(slider_node),
    };

    au::WasmOptions opts;
    opts.size = au::Size{.width = 640.0F, .height = 360.0F};
    opts.canvas_id = "win-a";
    opts.title = "aria";
    auto made = au::create_window(opts);
    if (!made) {
        publish_title("aria PROBE-ERROR create_window failed");
        return 1;
    }

    // ⚠️ rAF 契约：run() 注册后即返回——Application 须堆持（同 wasm_multiwin_live_probe）。
    static std::unique_ptr<au::Application> app_keep_alive;
    app_keep_alive = std::make_unique<au::Application>(au::Scene{std::move(root)}, std::move(made.value()), opts);

    // 每帧发布观测标题；桥的 sync/pump 由 present() 帧尾自动执行，探针不触碰。
    app_keep_alive->set_on_frame([obs]() -> void {
        publish_title("aria c=" + std::to_string(obs->clicks)  //
                      + " a=" + std::to_string(obs->announces)  //
                      + " cb=" + (obs->cb_checked ? "1" : "0")  //
                      + " ev=" + (obs->entry_value.empty() ? "-" : obs->entry_value));
    });
    app_keep_alive->run();
    return 0;
}
