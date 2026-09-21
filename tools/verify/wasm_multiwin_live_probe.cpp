// ============================================================
// wasm_multiwin_live_probe — 浏览器多窗口事件路由真机验收探针（specification/06-app-platform.md §2.4）
// ------------------------------------------------------------
// 证明对象（`WasmSurface` 多窗口路由三通道 + 一条字符输入折算 + 层叠/标题两通道）：
//   ① 键盘路由：document 级单一分发器按「当前焦点 Surface」转发——点击 win-b 后敲字符/
//      Enter 只落 win-b 的输入框，win-a 计数不动（反之亦然）；
//   ② resize 广播：window 级 resize 经分发器遍历全部实例刷新各自 CSS 尺寸，两窗口尺寸
//      观测值随浏览器窗口缩放同步变化（canvas CSS 宽度为视口百分比，见 shell）；
//   ③ 焦点语义：构造即接管路由（末建窗口 win-b 天然先聚焦，无需点击）；
//   ④ 字符折算：KeyDown 恒发 KeyEvent，单字符可打印键**另发** TextInputEvent（X11 同口径）
//      ——缺它则 WASM 任何文本框打不进字符，① 也就无从验证；
//   ⑤ raise() 层叠：浏览器无 z 序 API，实现是「把本 canvas 移到父节点末子」，故判据取
//      交叠中心的 `document.elementFromPoint` 命中者（只看 DOM 序字符串会被 z-index 蒙混，
//      这里要求 DOM 序与真实命中一致才算证明）；且 raise 不得改变激活状态；
//   ⑥ set_title 焦点语义：`document.title` 是页面单值，只有焦点窗口的标题有权写；非焦点
//      窗口改标题只进各自缓存（`ta=`/`tb=` 观测），待它成为焦点时自动重播；
//   ⑦ 焦点易主四接线的标题跟随：`focus_window()` / 鼠标按下 / 构造接管 / 焦点窗口析构回落
//      四处都要把页面标题换成新焦点窗口的缓存——最后那处（关一个改过标题的窗口）漏接就
//      会在标签页上留下幽灵标题。
//
// 观测通道（**两条，且必须分开**）：
//   * `window.__mwState`：探针每帧发布的状态串（路由焦点、两窗口输入文本/提交计数/尺寸、
//     两窗口标题缓存，末尾由 JS 侧补 DOM 层叠序、交叠命中者与页面标题）。
//   * `document.title`：本探针的**被测量**，只有被测代码（`WasmSurface::set_title` 与焦点
//     接管链路）有权写——旧版把它当观测通道自用，那样 ⑥⑦ 两条判据恒真、等于没测。
//   自动化经 CDP `Runtime.evaluate` 读这两条，不依赖像素 OCR。
// 注入通道：`window.__mwCmd`（字符串队列，探针每帧排空）。raise/改标题/关窗三类动作没有
//   对应的 DOM 事件可派发，故沿用 ARIA 桥 `window.__auroraAriaQueue` 的队列口径。
//   支持命令：`raiseA` `raiseB` `focusA` `focusB` `titleA:<文本>` `titleB:<文本>` `closeB`。
//
// 无头 CI 无法证明以上任何一条（需要真实 DOM 事件路由、窗口 resize、层叠命中与页面标题），
// 故本探针不进 CTest，按需人工触发。
//
// 构建（需 Emscripten 工具链，产物为 html + js + wasm）：
//   cmake -B build-wasm -DAURORA_BACKEND_WASM=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-wasm --target aurora_verify_wasm_multiwin
//
// 运行（须走 HTTP——浏览器拒绝 file:// 下的 wasm 流式编译）：
//   静态服务器指向产物目录后打开 aurora_verify_wasm_multiwin.html（或 emrun）。
//
// 逐项期望（1–5 读 `__mwState` 的 C++ 段，6–10 读其中的 DOM 段与 `document.title`）：
//   1. 初始含 `foc=win-b`（末建窗口接管路由，判据 ③）；
//   2. 点击 win-a 输入区 → `foc=win-a`；键入 "ab" → `A[ab/0]`，B 不动（判据 ①④）；
//      按 Enter → `A[.. /1]`（KeyEvent 通道的 on_submit）；
//   3. 点击 win-b 输入区 → 键入 "cd" → `B[cd/0]` 且 A 不变（判据 ① 反向）；
//   4. 缩放浏览器窗口 → `szA=/szB=` 宽度分量随视口宽度同步变化（判据 ②）；
//   5. 控制台无 Aborted / 未捕获异常；
//   6. 初始 `document.title == mw-B`（焦点窗口的标题即页面标题；win-a 的 `mw-A` 只留在
//      `ta=` 缓存里——判据 ⑥，且它替换掉了壳里的初值 `mw-shell`）；
//   7. 点 win-a → `document.title == mw-A`（鼠标按下这一处的标题跟随，判据 ⑦）；
//   8. 发 `titleA:改名A` → `document.title == 改名A` 且 `ta=改名A`（焦点窗口改名即时生效）；
//   9. 发 `titleB:待切B` → `document.title` **仍是** 改名A 而 `tb=待切B`（非焦点只缓存）；
//      随后发 `focusB` → `document.title == 待切B`（易主重播；⑥⑦ 合起来才是完整证据）；
//  10. 发 `raiseA` → `order=` 末位为 win-a **且** `zTop=win-a`（层叠真的变了），而 `foc=`
//      与 `document.title` 一律不动（raise 不改激活）；发 `closeB` → `B[closed]`、
//      `foc=win-a`、`document.title == 改名A`（析构回落这一处）。
//
// 退出语义：浏览器页面无正常退出路径，关闭标签页即回收（页面级 beforeunload 置位
// should_close 为申报语义，见 wasm_surface.h 类头）。
// ============================================================

#include <emscripten.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
    au::Application *app = nullptr;  ///< 关窗命令的执行者（`close_window` 按 id 请求）。
    au::Window *wa = nullptr;
    au::Window *wb = nullptr;  ///< 帧末 reap 真死后由存活判定置空。
    au::WindowId id_b = au::AURORA_INVALID_WINDOW_ID;  ///< win-b 的宿主 id（0 = 未开窗）。
    std::string note;          ///< 最近一条命令的执行回执，便于排障。
};

// 页面侧数据搬运一律走 EM_JS：**不**用 `emscripten/val.h`——embind/emval 在本代 Emscripten
// 已改造成 port，未 `--use-port=embind` 链接时整组 `_emval_*` 符号不存在，表现为 wasm-ld 报
// undefined symbol（实测）。探针为一个观测串去改链接口径不划算。
// EM_JS 形参为具名 C/C++ 参数（无 `$` 占位符），故不触发 -Wdollar-in-identifier-extension，
// 无需诊断守卫（与 `wasm_aria.cpp` 同法）；JS 里的空串写 `""` 而非 `''`——片段仍经 C++ 词法
// 分析，`''` 会被判为「空字符常量」而报 -Winvalid-pp-token。

/// @brief 命令队列当前长度（`window.__mwCmd`，未定义即空）。
EM_JS(int, cmd_count_js, (), {
    const q = window.__mwCmd;
    return q && q.length ? q.length : 0;
});

/// @brief 把第 idx 条命令以 UTF-8 落进 wasm 内存（NUL 结尾；超出容量即截断）。
EM_JS(void, cmd_at_js, (char *dst, int cap, int idx), {
    const s = String((window.__mwCmd || [])[idx] || "");
    const enc = new TextEncoder().encode(s);
    const n = Math.min(enc.length, cap - 1);
    for (let k = 0; k < n; k++) {
        HEAPU8[dst + k] = enc[k];
    }
    HEAPU8[dst + n] = 0;
});

/// @brief 排空命令队列（命令只执行一次）。
EM_JS(void, cmd_clear_js, (), { window.__mwCmd = []; });

auto cmd_count() -> int { return cmd_count_js(); }

auto cmd_at(int i) -> std::string {
    char buf[256] = {};
    cmd_at_js(buf, static_cast<int>(sizeof(buf)), i);
    return std::string{buf};
}

auto cmd_clear() -> void { cmd_clear_js(); }

/// @brief 把状态串发布到 `window.__mwState`，并由 JS 侧就地补齐三条 DOM 观测。
/// @note DOM 三段（`order=` 层叠序 / `zTop=` 交叠命中者 / `dt=` 页面标题）必须在 JS 里读，
///       C++ 侧没有 DOM；`dt=` 只是给排障看的镜像，判据仍以自动化直读 `document.title` 为准。
EM_JS(void, publish_state_js, (const char *base), {
    const a = document.getElementById("win-a");
    const b = document.getElementById("win-b");
    let order = "";
    for (const el of document.body.children) {
        if (el.tagName !== "CANVAS") {
            continue;
        }
        order += (order ? "," : "") + el.id;
    }
    let zTop = "no-canvas";
    if (a && b) {
        const ra = a.getBoundingClientRect();
        const rb = b.getBoundingClientRect();
        const x = (Math.max(ra.left, rb.left) + Math.min(ra.right, rb.right)) / 2;
        const y = (Math.max(ra.top, rb.top) + Math.min(ra.bottom, rb.bottom)) / 2;
        const hit = document.elementFromPoint(x, y);
        zTop = hit ? (hit.id || hit.tagName) : "none";
    }
    window.__mwState = UTF8ToString(base) + " order=" + order + " zTop=" + zTop + " dt=" + document.title;
});

auto publish_state(const std::string &state) -> void { publish_state_js(state.c_str()); }

/// @brief 焦点窗口标题缓存的只读观测（经 `Window::surface()` 下转；后端即 WasmSurface）。
auto cached_title(au::Window *w) -> std::string {
    if (w == nullptr) {
        return "-";
    }
    return static_cast<au::WasmSurface &>(w->surface()).title();
}

auto apply_command(Obs &o, const std::string &cmd) -> void {
    auto surface_of = [&o](char which) -> au::Surface * {
        au::Window *w = (which == 'A') ? o.wa : o.wb;
        return w == nullptr ? nullptr : &w->surface();
    };
    if (cmd == "raiseA" || cmd == "raiseB") {
        if (auto *s = surface_of(cmd.back()); s != nullptr) {
            s->raise();
            o.note = cmd;
        }
        return;
    }
    if (cmd == "focusA" || cmd == "focusB") {
        if (auto *s = surface_of(cmd.back()); s != nullptr) {
            s->focus_window();
            o.note = cmd;
        }
        return;
    }
    if (cmd == "closeB") {
        // 只「请求关闭」：宿主在帧末 reap 才真死，故存活判定另按在册数量走（判据 10）。
        if (o.app != nullptr && o.id_b != au::AURORA_INVALID_WINDOW_ID) {
            o.app->close_window(o.id_b);
            o.note = "closeB";
        }
        return;
    }
    constexpr std::string_view prefix_a{"titleA:"};
    constexpr std::string_view prefix_b{"titleB:"};
    if (cmd.rfind(prefix_a, 0) == 0) {
        if (o.wa != nullptr) {
            o.wa->set_title(cmd.substr(prefix_a.size()));
            o.note = "titleA";
        }
    } else if (cmd.rfind(prefix_b, 0) == 0) {
        if (o.wb != nullptr) {
            o.wb->set_title(cmd.substr(prefix_b.size()));
            o.note = "titleB";
        }
    } else {
        o.note = "unknown:" + cmd;
    }
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
        publish_state("mw PROBE-ERROR create_window failed");
        return 1;
    }
    obs->wa = ra.value().get();
    obs->wb = rb.value().get();

    // ⚠️ rAF 契约：run() 注册后即返回、main 随即结束——`Application` 若活在 main 栈上，
    // 析构摘除 raf_owner_ 守卫令蹦床下一拍自停（本探针首版正是如此零帧翻车）。与
    // App::launch 同法：函数级 static unique_ptr 堆持至页面生命周期。
    static std::unique_ptr<au::Application> app_keep_alive;
    app_keep_alive = std::make_unique<au::Application>(au::Scene{std::move(root_a)}, std::move(ra.value()), oa);
    obs->app = app_keep_alive.get();
    obs->id_b = app_keep_alive->open_window(std::move(rb.value()), au::Scene{std::move(root_b)}, ob);

    // 每帧发布观测状态（帧循环本身活着即 rAF 接线回归，见 wasm_raf_live_probe）。
    app_keep_alive->set_on_frame([obs]() -> void {
        const int n = cmd_count();
        for (int i = 0; i < n; ++i) {
            apply_command(*obs, cmd_at(i));
        }
        if (n > 0) {
            cmd_clear();
        }
        // 关窗是「帧末回收」：命令发出到 host 真死之间有帧间隙，故存活判定按实际在册数量，
        // 一旦 B 不再在册即置空指针——否则后续帧解引用悬垂 Window。
        if (obs->wb != nullptr && obs->app->window_count() < 2) {
            obs->wb = nullptr;
        }
        const au::Size sa = obs->wa->size();
        const std::string foc = au::WasmSurface::focused_canvas_id();
        std::string state = "mw foc=" + (foc.empty() ? std::string{"-"} : foc);
        state += " A[" + obs->a_text + "/" + std::to_string(obs->a_sub) + "]";
        if (obs->wb != nullptr) {
            const au::Size sb = obs->wb->size();
            state += " B[" + obs->b_text + "/" + std::to_string(obs->b_sub) + "]";
            state += " szB=" + std::to_string(static_cast<int>(sb.width)) + "x"  //
                   + std::to_string(static_cast<int>(sb.height));
            state += " tb=" + cached_title(obs->wb);
        } else {
            state += " B[closed]";
        }
        state += " szA=" + std::to_string(static_cast<int>(sa.width)) + "x"  //
               + std::to_string(static_cast<int>(sa.height));
        state += " ta=" + cached_title(obs->wa);
        state += " cmd=" + obs->note;
        publish_state(state);
    });
    app_keep_alive->run();
    return 0;
}
