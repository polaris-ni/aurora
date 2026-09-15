#pragma once
#include "aurora/core/platform.h"  // NOLINT

// WASM/Canvas Surface（ARCHITECTURE.md §8.4）：仅在 defined(AURORA_PLATFORM_WASM) 时提供。
// 渲染到 HTML5 Canvas 的 ImageData。其他平台降级为 HeadlessSurface。
//
// 设计要点：
// - 上屏路径：软件 Painter RGBA 帧缓冲 → EM_ASM 拷贝到 Canvas 2D ImageData → putImageData。
//   无需 WebGL；Canvas 2D 在浏览器中由 GPU 加速合成，性能足够 UI 场景。
// - 事件翻译：Emscripten HTML5 API（emscripten_set_*_callback）翻译鼠标/键盘/触摸/resize。
// - 帧循环：emscripten_request_animation_frame_loop 驱动（浏览器 rAF 对齐 vsync）。
// - 关闭语义：emscripten_set_beforeunload_callback 设置 should_close。

#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)

#include <emscripten.h>
#include <emscripten/html5.h>

#include <string>
#include <unordered_set>

#include "aurora/window/surface.h"

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
    WasmSurface(int w, int h, const char *canvas_id = "#canvas") : canvas_id_(canvas_id), w_(w), h_(h) {
        // 鼠标事件按 canvas 元素注册（多窗口各 canvas 独立，天然可用）。
        emscripten_set_mousedown_callback(canvas_id_.c_str(), this, true, &on_mouse);
        emscripten_set_mouseup_callback(canvas_id_.c_str(), this, true, &on_mouse);
        emscripten_set_mousemove_callback(canvas_id_.c_str(), this, true, &on_mouse);
        // 键盘 / resize 注册在 document / window 级：多窗口下必须「单一分发器 + 按焦点路由」，
        // 否则各 Surface 各自注册 document 级键盘回调会被后者覆盖，仅最后创建的窗口能收到。
        // 故全局仅注册一次，由全局回调按 focused_surface_ 路由键盘、按实例集合广播 resize。
        instances_.insert(this);
        if (!global_handlers_registered_) {
            emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, &on_key_dispatch);
            emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, &on_key_dispatch);
            emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, &on_resize_dispatch);
            global_handlers_registered_ = true;
        }
    }

    ~WasmSurface() override {
        // 仅注销本 canvas 的鼠标回调；document/window 级全局分发器常驻（进程生命周期内
        // 有效，实例清空后遍历自然跳过，无副作用）。
        emscripten_set_mousedown_callback(canvas_id_.c_str(), nullptr, true, nullptr);
        emscripten_set_mouseup_callback(canvas_id_.c_str(), nullptr, true, nullptr);
        emscripten_set_mousemove_callback(canvas_id_.c_str(), nullptr, true, nullptr);
        instances_.erase(this);
        if (focused_surface_ == this) {
            focused_surface_ = nullptr;
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
    [[nodiscard]] auto should_close() const -> bool override { return should_close_; }
    [[nodiscard]] auto data() const -> const std::uint8_t * override { return painter_.data(); }
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    auto set_event_handler(const EventHandler &h) -> void override { event_handler_ = h; }
    auto set_title(const std::string &title) -> void override {
        EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
    }

    /// @brief WASM 下 no-op：浏览器 rAF 驱动帧循环，无需阻塞等待。
    auto wait_events(double /*timeout_ms*/) -> void override {}

  private:
    // 多窗口事件路由支撑（document/window 级单一分发器）：全局仅注册一次回调，
    // 键盘按 focused_surface_ 路由、resize 按实例集合广播。Wasm 为单线程 JS 环境，
    // 实例集合与焦点指针的读写均发生在主线程事件回调内，无需加锁。
    inline static std::unordered_set<WasmSurface *> instances_;
    inline static WasmSurface *focused_surface_ = nullptr;
    inline static bool global_handlers_registered_ = false;

    std::string canvas_id_;
    Painter painter_;
    int w_ = 0;
    int h_ = 0;
    int frame_ = 0;
    bool should_close_ = false;
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
        // 多窗口：最近交互（按下）的 canvas 成为键盘/事件焦点。
        if (ev.action == MouseAction::Press) {
            focused_surface_ = self;
        }
        self->event_handler_(ev);
        return EM_TRUE;
    }

    // 把 Emscripten 键盘事件翻译为 Aurora KeyEvent 并派发到指定 Surface（focused_surface_ 或测试桩）。
    static EM_BOOL dispatch_key_to(WasmSurface *target, int type, const EmscriptenKeyboardEvent *e) {
        if (!target || !target->event_handler_) {
            return EM_FALSE;
        }
        KeyEvent ev;
        ev.key = static_cast<int>(e->keyCode);
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
        return EM_TRUE;
    }

    // 全局 document 级键盘分发器：仅注册一次，按当前焦点 Surface 路由（多窗口语义）。
    static EM_BOOL on_key_dispatch(int type, const EmscriptenKeyboardEvent *e, void * /*user_data*/) {
        return dispatch_key_to(focused_surface_, type, e);
    }

    // 查询本 canvas 当前 CSS 尺寸并更新（window resize 时各实例自行刷新）。
    auto update_css_size() -> void {
        double css_w = 0, css_h = 0;
        emscripten_get_element_css_size(canvas_id_.c_str(), &css_w, &css_h);
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
};

}  // namespace aurora

#ifdef AURORA_COMPILER_CLANG
#pragma clang diagnostic pop
#endif

#endif  // AURORA_BACKEND_WASM / AURORA_PLATFORM_WASM
