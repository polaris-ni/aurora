#pragma once
#include "aurora/core/platform.h"  // NOLINT

// WASM/Canvas Surface（ARCHITECTURE.md §8.4）：仅在 defined(AURORA_PLATFORM_WASM) 时提供。
// 渲染到 HTML5 Canvas 的 ImageData。其他平台降级为 HeadlessSurface。
//
// 设计要点：
// - 上屏路径：软件 Painter RGBA 帧缓冲 → EM_ASM 拷贝到 Canvas 2D ImageData → putImageData。
//   无需 WebGL；Canvas 2D 在浏览器中由 GPU 加速合成，性能足够 UI 场景。
// - 事件翻译：Emscripten HTML5 API（emscripten_set_*_callback）翻译鼠标/键盘/触摸/resize。
// - 线程模型：无 pthreads 构建下 ThreadPool 为 deferred 排空模式（见 thread_pool.h 类头），
//   排空点是 `Application::step_frame()` 的帧尾（步骤 7）而非本 Surface 的 `present()`——
//   空闲帧被脏区决策跳过就没有 present，挂在这儿会饿死 `au::async` / 协程续体（同下方 ARIA 条）。
//   以 `-pthread` 构建（`AURORA_ENABLE_WASM_PTHREADS`）则回到普通 worker 池真并行，但浏览器要求
//   宿主页面先跨源隔离（`crossOriginIsolated`），故该构建选项默认关闭、服务器头由宿主自担。
// - 帧循环：浏览器主线程不可阻塞，`Application::run()` 经 `emscripten_request_animation_frame_loop`
//   把统一帧循环挂到 rAF/vsync（蹦床 `Application::raf_tick`，见 application.h）；本 Surface 的
//   `wait_events` 因此无需实现（保持空体，rAF 模式下帧循环根本不调用它）。
// - 关闭语义：window 级 beforeunload 回调（全局注册一次）置位页面级关闭请求，所有
//   Surface 的 `should_close()` 同时为真——浏览器整页卸载即全部窗口关闭，语义天然一致。
// - 多窗口路由（specification/06-app-platform.md §2.4 已知限制收口）：新建窗口即接管键盘
//   焦点（同桌面「新建即激活」）；`focus_window()` 切路由指针；`raise()` 经「把本 canvas 移到
//   其父节点末子」实现——浏览器无 z 序 API，**DOM 顺序即层叠顺序**（末位在上）。
// - 页面标题（同 §2.4）：`document.title` 是**页面单值**，而每窗口各有一份标题，故按桌面
//   口径折算——「焦点窗口的标题即页面标题」：`set_title` 先落本窗口缓存，仅当自己是焦点
//   窗口时才写 DOM；焦点易主（构造接管 / `focus_window()` / 鼠标按下 / 焦点窗口析构回落）
//   即把新焦点窗口的缓存重播到 `document.title`。从未声明过标题的窗口不动页面标题
//   （宿主自己的 `<title>` 不归库管），声明过空串则如实写空——「声明」与否才分水岭。
//   ⚠️ 一处**如实申报的契约偏差**：基类 `focus_window()` 的桌面语义是「置顶 + 取键盘焦点」，
//   WASM 只做后者。因为「置顶」在此只能靠改写宿主 DOM 顺序实现，隐式重排别人家的节点是
//   越权行为（正常流下会让画布换位跳动），故层序变更只在宿主**显式**调 `raise()` 时发生。
// - 无障碍（ARIA 镜像桥）：首帧 `set_accessibility_root` 即构造并激活 `WasmAriaBridge`
//   （浏览器无读屏探测面，D14 惰性激活的既定例外，见 wasm_aria.h 申报）；镜像同步
//   （D9 拉取式重投影）与读屏反向动作排水由桥**自持的 rAF 自驱拍**每帧执行——不经
//   `present()` 帧尾，因静止页面没有脏帧就没有 present，反向通道会被饿死（见 wasm_aria.h）。

#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)

#include <emscripten.h>
#include <emscripten/html5.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "aurora/event/keycode.h"
#include "aurora/window/surface.h"
#include "aurora/window/wasm_aria.h"

// EM_ASM 的 JS 片段以 `$0`/`$1` 作参数占位符（Emscripten 宏契约，见 em_asm.h）。
// `-Wpedantic` 下 clang 在词法阶段即把这些 token 判为「标识符含 $」扩展并报
// -Wdollar-in-identifier-extension；片段经 `#code` 字符串化、不参与 C++ 求值，属误报。
// 该守卫必须位于首个 `$` token 之前（诊断按文件位置生效），且与文件末尾的 pop
// 同处一个平台守卫内——否则条件为假时 pop 会与未展开的 push 失配。
#ifdef AURORA_COMPILER_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif

namespace aurora {

class WasmSurface : public Surface {
  public:
    WasmSurface(int w, int h, const char *canvas_id = "#canvas") : w_(w), h_(h) {
        // 入参统一按 DOM id 理解（可带 '#' 前缀，两种写法等价）。派生两种形态分开使用：
        // `present()` 上屏走 getElementById（须裸 id），事件注册/尺寸查询走 Emscripten 的
        // querySelector（须 CSS 选择器）——同一字符串两用必坏一边（裸 id 不是合法选择器，
        // 注册静默失败；带 '#' 又查不到元素），历史默认值 "#canvas" 正踩中此坑。
        std::string id = canvas_id;
        if (!id.empty() && id.front() == '#') {
            id.erase(0, 1);
        }
        canvas_id_ = id;
        canvas_selector_ = "#" + id;
        // 鼠标事件按 canvas 元素注册（多窗口各 canvas 独立，天然可用）。
        emscripten_set_mousedown_callback(canvas_selector_.c_str(), this, true, &on_mouse);
        emscripten_set_mouseup_callback(canvas_selector_.c_str(), this, true, &on_mouse);
        emscripten_set_mousemove_callback(canvas_selector_.c_str(), this, true, &on_mouse);
        // 键盘 / resize 注册在 document / window 级：多窗口下必须「单一分发器 + 按焦点路由」，
        // 否则各 Surface 各自注册 document 级键盘回调会被后者覆盖，仅最后创建的窗口能收到。
        // 故全局仅注册一次，由全局回调按 focused_surface_ 路由键盘、按实例集合广播 resize。
        instances_.insert(this);
        // 新建窗口即接管键盘焦点（同桌面平台「新建窗口被激活」语义）；首个窗口因此天然
        // 有焦点，无需等第一次鼠标点击。此刻本窗口尚无标题（工厂随后才 `set_title`），
        // 故 take_focus 不会写 DOM，页面标题留到那次声明时由本窗口接管。
        take_focus(this);
        if (!global_handlers_registered_) {
            emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, &on_key_dispatch);
            emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, &on_key_dispatch);
            emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, &on_resize_dispatch);
            // 页面卸载 → 置位关闭请求（返回 false = 不弹确认框，放行卸载）。整页关闭即全部
            // 窗口关闭，故为页面级单标志、所有实例共享读取。
            emscripten_set_beforeunload_callback(nullptr, &on_before_unload);
            global_handlers_registered_ = true;
        }
    }

    ~WasmSurface() override {
        // 仅注销本 canvas 的鼠标回调；document/window 级全局分发器常驻（进程生命周期内
        // 有效，实例清空后遍历自然跳过，无副作用）。
        emscripten_set_mousedown_callback(canvas_selector_.c_str(), nullptr, true, nullptr);
        emscripten_set_mouseup_callback(canvas_selector_.c_str(), nullptr, true, nullptr);
        emscripten_set_mousemove_callback(canvas_selector_.c_str(), nullptr, true, nullptr);
        // 桥先于实例表清算：析构内 deactivate 会注销广播表并移除本页镜像容器（防孤儿树）。
        aria_bridge_.reset();
        instances_.erase(this);
        if (focused_surface_ == this) {
            // 焦点窗口销毁：路由指针回落到任一存活实例（浏览器无「下一个激活窗口」概念，
            // 取集合首元素即确定性回落），空集则归 nullptr。回落即「焦点易主」，页面标题
            // 须跟着新焦点窗口走——否则关闭一个改过标题的窗口后，标签页上挂着的是幽灵标题。
            take_focus(instances_.empty() ? nullptr : *instances_.begin());
        }
    }

    [[nodiscard]] auto begin_frame(int w, int h) -> Result<bool> override {
        painter_.begin(w, h);
        w_ = w;
        h_ = h;
        return true;
    }
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }

    [[nodiscard]] auto present() -> Result<bool> override {
        const int w = painter_.width();
        const int h = painter_.height();
        if (w <= 0 || h <= 0) {
            return true;
        }
        const std::uint8_t *src = painter_.data();
        // 通过 EM_ASM 将 WASM 线性内存中的 RGBA 帧缓冲拷贝到 Canvas 2D：
        // 1. 获取 canvas 元素与 2D 上下文
        // 2. 创建 ImageData(w, h)
        // 3. 将 WASM 内存 src 拷贝到 ImageData.data（Uint8ClampedArray）
        // 4. putImageData 到 canvas
        // 注意：Painter 输出 RGBA，Canvas ImageData 也是 RGBA，无需 swizzle。
        EM_ASM(
            {
                const canvas = document.getElementById(UTF8ToString($3));
                if (!canvas) {
                    return;
                }
                const ctx = canvas.getContext('2d');
                if (!ctx) {
                    return;
                }
                // 确保 canvas 尺寸匹配帧缓冲
                if (canvas.width != $1 || canvas.height != $2) {
                    canvas.width = $1;
                    canvas.height = $2;
                }
                const img = ctx.createImageData($1, $2);
                // 从 WASM 内存拷贝 RGBA 数据到 ImageData.data
                for (let i = 0; i < $1 * $2 * 4; i++) {
                    img.data[i] = HEAPU8[$0 + i];
                }
                ctx.putImageData(img, 0, 0);
            },
            src, w, h, canvas_id_.c_str());
        ++frame_;
        return true;
    }

    [[nodiscard]] auto size() const -> Size override { return Size{static_cast<float>(w_), static_cast<float>(h_)}; }
    [[nodiscard]] auto should_close() const -> bool override { return close_requested_; }

    /// @brief 把键盘事件路由指针切到本窗口并重播页面标题（页面级焦点无法用 JS 之外的方式
    ///        抢占，路由即 WASM 上「激活窗口」的忠实映射；鼠标按下同一效果）。
    /// @note 基类契约里的「置顶」一半在浏览器里**故意不做**：置顶只能靠改写宿主 DOM 顺序
    ///       实现，隐式重排别人的节点属越权（正常流下画布会换位跳动），故需要层序提升请显式
    ///       调 `raise()`。类头「页面标题」条同步申报此偏差。
    auto focus_window() -> void override { take_focus(this); }

    /// @brief 把本窗口提到页面层叠顶部——浏览器无 z 序 API，**DOM 顺序即层叠顺序**，
    ///        故实现是把本 canvas 移到其父节点的末子位（末位压前位）。
    /// @note 同一元素 `appendChild` 是**移动**而非重建：节点身份保留，故已注册的
    ///       Emscripten 鼠标回调、Canvas 2D 上下文与 ARIA 镜像容器（挂在 body 上、按
    ///       canvas id 索引）全部随迁不失效，无需重注册。
    /// @note 不改变激活状态（与基类契约一致）：路由指针与 `document.title` 一律不动。
    auto raise() -> void override {
        EM_ASM(
            {
                const canvas = document.getElementById(UTF8ToString($0));
                // 节点已不在文档里（宿主删了画布）⇒ 无从提升，静默返回。
                if (canvas && canvas.parentNode) {
                    canvas.parentNode.appendChild(canvas);
                }
            },
            canvas_id_.c_str());
    }

    /// @brief 当前键盘路由目标的 canvas id（无焦点窗口时为空串）——多窗口路由的只读观测口，
    ///        供真机探针/自动化断言「点击/ focus_window 后路由确实切换」。
    [[nodiscard]] static auto focused_canvas_id() -> std::string {
        return focused_surface_ != nullptr ? focused_surface_->canvas_id_ : std::string{};
    }
    [[nodiscard]] auto data() const -> const std::uint8_t * override { return painter_.data(); }
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    auto set_event_handler(const EventHandler &h) -> void override { event_handler_ = h; }

    /// @brief 声明本窗口标题：**先落每窗口缓存**，仅当本窗口是焦点窗口时才写 `document.title`
    ///        （页面标题是单值，多窗口下由焦点窗口代表——同桌面「活动窗口标题在标题栏」）。
    /// @note 非焦点窗口改标题只更新缓存，等它下次成为焦点窗口时自动重播，无需宿主协调。
    auto set_title(const std::string &title) -> void override {
        title_ = title;
        title_declared_ = true;  // 「声明过」才是分水岭：空串也是宿主的显式要求
        if (focused_surface_ == this) {
            publish_focused_title();
        }
    }

    /// @brief 本窗口缓存的标题（多窗口观测口：非焦点窗口的标题只活在这里，不上页面）。
    [[nodiscard]] auto title() const -> const std::string & { return title_; }

    /// @brief WASM 下 no-op：浏览器 rAF 驱动帧循环，无需阻塞等待。
    auto wait_events(double /*timeout_ms*/) -> void override {}

    /// @brief 本窗口的 ARIA 镜像桥（首帧根注入前为 nullptr —— 桥随 `present_root` 诞生）。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override {
        return aria_bridge_.get();
    }

    /// @brief 语义树根注入（`Window::present_root` 每帧调用）：首次构造桥（容器 id 按
    ///        canvas id 隔离，多窗口各挂各的镜像树），随后交由桥做幂等判定。
    auto set_accessibility_root(Widget *root) -> void override {
        if (aria_bridge_ == nullptr) {
            aria_bridge_ = std::make_unique<WasmAriaBridge>("aurora-a11y-" + canvas_id_);
        }
        aria_bridge_->set_root(root);
    }

  private:
    // 多窗口事件路由支撑（document/window 级单一分发器）：全局仅注册一次回调，
    // 键盘按 focused_surface_ 路由、resize 按实例集合广播。Wasm 为单线程 JS 环境，
    // 实例集合与焦点指针的读写均发生在主线程事件回调内，无需加锁。
    inline static std::unordered_set<WasmSurface *> instances_;
    inline static WasmSurface *focused_surface_ = nullptr;
    inline static bool global_handlers_registered_ = false;
    inline static bool close_requested_ = false;  ///< beforeunload 置位；页面级=全部窗口关闭。

    /// @brief 焦点接管的两件事一起做完：路由指针 + 页面标题跟随。四处调用点（构造、
    ///        `focus_window()`、鼠标按下、焦点窗口析构回落）共用，防漏其一。
    static auto take_focus(WasmSurface *s) -> void {
        focused_surface_ = s;
        publish_focused_title();
    }

    /// @brief 把「焦点窗口的标题缓存」重播到 `document.title`（页面单值的唯一写入口）。
    /// @note 无焦点窗口、或它从未声明标题 ⇒ **不动** DOM：页面标题可能是宿主自己写的，
    ///       库无权清空；`set_title("")` 属显式声明，会如实写空串。
    static auto publish_focused_title() -> void {
        if (focused_surface_ == nullptr || !focused_surface_->title_declared_) {
            return;
        }
        EM_ASM({ document.title = UTF8ToString($0); }, focused_surface_->title_.c_str());
    }

    std::string canvas_id_;       ///< 裸 DOM id（getElementById 上屏用）。
    std::string canvas_selector_; ///< CSS 选择器形态（Emscripten 事件注册/querySelector 用）。
    std::string title_;           ///< 本窗口标题缓存（页面标题由焦点窗口的这份代表）。
    bool title_declared_ = false; ///< 宿主是否声明过标题（决定易主时要不要重播 DOM）。
    std::unique_ptr<WasmAriaBridge> aria_bridge_;  ///< ARIA 镜像桥（首帧根注入时构造；见 wasm_aria.h）
    Painter painter_;
    int w_ = 0;
    int h_ = 0;
    int frame_ = 0;
    EventHandler event_handler_;

    // ---- Emscripten 事件回调 ----
    static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *user_data) {
        auto *self = static_cast<WasmSurface *>(user_data);
        if (!self->event_handler_) {
            return EM_FALSE;
        }
        MouseEvent ev;
        // EmscriptenMouseEvent 字段为驼峰（html5.h 现行 ABI；snake_case 旧拼写已随 SDK 移除）。
        ev.position = Point{static_cast<float>(e->targetX), static_cast<float>(e->targetY)};
        ev.button = (e->button == 2) ? MouseButton::Right : (e->button == 1) ? MouseButton::Middle : MouseButton::Left;
        ev.action = (type == EMSCRIPTEN_EVENT_MOUSEDOWN) ? MouseAction::Press
                    : (type == EMSCRIPTEN_EVENT_MOUSEUP) ? MouseAction::Release
                                                         : MouseAction::Move;
        // 多窗口：最近交互（按下）的 canvas 成为键盘/事件焦点，页面标题随之跟随。
        if (ev.action == MouseAction::Press) {
            take_focus(self);
        }
        self->event_handler_(ev);
        return EM_TRUE;
    }

    // DOM `KeyboardEvent.key` 名 → 库 `KeyCode` 折算（specification/05-event-navigation.md §2.2）。
    // 不可直接取 `keyCode`：那是 DOM 数字码（Enter=13），与 Aurora 自有稠密枚举（Enter=57）
    // 完全错位——字母恰与 VK 同值掩盖了这一点，控制键在 WASM 上实则整条通道失效
    // （Enter 提交/退格删除/方向键移光标全部打不动，多窗口探针实测坐实）。`key` 名是
    // DOM 规范稳定值，按名折算才是正解。
    static auto dom_key_to_code(const char *k) -> KeyCode {
        using enum KeyCode;
        const std::string_view s{k};
        if (s.size() == 1) {
            const char c = s[0];
            if (c >= 'a' && c <= 'z') {
                return static_cast<KeyCode>(static_cast<int>(A) + (c - 'a'));
            }
            if (c >= 'A' && c <= 'Z') {
                return static_cast<KeyCode>(static_cast<int>(A) + (c - 'A'));
            }
            if (c >= '0' && c <= '9') {
                return static_cast<KeyCode>(static_cast<int>(D0) + (c - '0'));
            }
            switch (c) {
                case ' ': return Space;
                case '-': return Minus;
                case '=': return Equal;
                case '[': return LeftBracket;
                case ']': return RightBracket;
                case '\\': return Backslash;
                case ';': return Semicolon;
                case '\'': return Quote;
                case ',': return Comma;
                case '.': return Period;
                case '/': return Slash;
                case '`': return Backquote;
                default: break;
            }
        }
        if (s == "Enter") { return Enter; }
        if (s == "Tab") { return Tab; }
        if (s == "Backspace") { return Backspace; }
        if (s == "Delete") { return Delete; }
        if (s == "Escape") { return Escape; }
        if (s == "ArrowLeft") { return ArrowLeft; }
        if (s == "ArrowRight") { return ArrowRight; }
        if (s == "ArrowUp") { return ArrowUp; }
        if (s == "ArrowDown") { return ArrowDown; }
        if (s == "Home") { return Home; }
        if (s == "End") { return End; }
        if (s == "PageUp") { return PageUp; }
        if (s == "PageDown") { return PageDown; }
        if (s == "Shift") { return Shift; }
        if (s == "Control") { return Control; }
        if (s == "Alt") { return Alt; }
        if (s == "Meta") { return Meta; }
        if (s.size() >= 2 && s.size() <= 3 && s[0] == 'F' && s[1] >= '1' && s[1] <= '9') {
            const int n = (s.size() == 2) ? (s[1] - '0') : (s[1] - '0') * 10 + (s[2] - '0');
            if (n >= 1 && n <= 12) {
                return static_cast<KeyCode>(static_cast<int>(F1) + (n - 1));
            }
        }
        return Unknown;
    }

    // 把 Emscripten 键盘事件翻译为 Aurora 事件并派发到指定 Surface（focused_surface_ 或测试桩）。
    // 折算口径与 X11 路一致：KeyDown 恒发 KeyEvent；`key` 串为单字符可打印（DOM 规范：字符键
    // 给出该字符，控制键给出 "Enter"/"ArrowLeft" 等名字）时**另发** TextInputEvent 落字——
    // 缺这一步则 WASM 上任何文本框都打不进字符（旧实现只发 KeyEvent）。
    static EM_BOOL dispatch_key_to(WasmSurface *target, int type, const EmscriptenKeyboardEvent *e) {
        if (!target || !target->event_handler_) {
            return EM_FALSE;
        }
        KeyEvent ev;
        ev.key = static_cast<int>(dom_key_to_code(e->key));
        ev.action = (type == EMSCRIPTEN_EVENT_KEYDOWN) ? KeyAction::Down : KeyAction::Up;
        if (e->shiftKey) {
            ev.modifiers = ev.modifiers | ModifierKey::Shift;
        }
        if (e->ctrlKey) {
            ev.modifiers = ev.modifiers | ModifierKey::Control;
        }
        if (e->altKey) {
            ev.modifiers = ev.modifiers | ModifierKey::Alt;
        }
        if (e->metaKey) {
            ev.modifiers = ev.modifiers | ModifierKey::Meta;
        }
        target->event_handler_(ev);
        if (type == EMSCRIPTEN_EVENT_KEYDOWN) {
            const char c = e->key[0];
            if (c != '\0' && e->key[1] == '\0' && static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
                TextInputEvent te;
                te.text.assign(1, c);
                target->event_handler_(te);
            }
        }
        return EM_TRUE;
    }

    // 全局 document 级键盘分发器：仅注册一次，按当前焦点 Surface 路由（多窗口语义）。
    static EM_BOOL on_key_dispatch(int type, const EmscriptenKeyboardEvent *e, void * /*user_data*/) {
        return dispatch_key_to(focused_surface_, type, e);
    }

    // 查询本 canvas 当前 CSS 尺寸并更新（window resize 时各实例自行刷新）。
    auto update_css_size() -> void {
        double css_w = 0, css_h = 0;
        emscripten_get_element_css_size(canvas_selector_.c_str(), &css_w, &css_h);
        if (css_w > 0 && css_h > 0) {
            w_ = static_cast<int>(css_w);
            h_ = static_cast<int>(css_h);
        }
    }

    // 全局 window 级 resize 分发器：仅注册一次，遍历所有实例刷新各自 canvas 尺寸
    // （浏览器窗口 resize 影响全部 canvas 布局，应广播而非只给焦点窗口）。
    static EM_BOOL on_resize_dispatch(int /*type*/, const EmscriptenUiEvent * /*e*/, void * /*user_data*/) {
        for (auto *s : instances_) {
            s->update_css_size();
        }
        return EM_TRUE;
    }

    // 页面卸载回调：置位关闭请求后返回 nullptr（不弹离开确认框，放行卸载）。
    static const char *on_before_unload(int /*type*/, const void * /*reserved*/, void * /*user_data*/) {
        close_requested_ = true;
        return nullptr;
    }
};

}  // namespace aurora

#ifdef AURORA_COMPILER_CLANG
#pragma clang diagnostic pop
#endif

#endif  // AURORA_BACKEND_WASM / AURORA_PLATFORM_WASM
