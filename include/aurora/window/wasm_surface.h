#pragma once
#include "aurora/core/platform.h"

// WASM/Canvas Surface（§10.3）：仅在 defined(AURORA_PLATFORM_WASM) 时提供。
// 渲染到 HTML5 Canvas 的 ImageData。其他平台降级为 HeadlessSurface。
//
// 设计要点：
// - 上屏路径：软件 Painter RGBA 帧缓冲 → EM_ASM 拷贝到 Canvas 2D ImageData → putImageData。
//   无需 WebGL；Canvas 2D 在浏览器中由 GPU 加速合成，性能足够 UI 场景。
// - 事件翻译：Emscripten HTML5 API（emscripten_set_*_callback）翻译鼠标/键盘/触摸/resize。
// - 帧循环：emscripten_request_animation_frame_loop 驱动（浏览器 rAF 对齐 vsync）。
// - 关闭语义：emscripten_set_beforeunload_callback 设置 should_close。

#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)

#include <cstring>
#include <emscripten.h>
#include <memory>
#include <string>

#include <emscripten/html5.h>

#include "aurora/window/surface.h"

namespace aurora {

class WasmSurface : public Surface {
  public:
    WasmSurface(int w, int h, const char *canvas_id = "#canvas") : m_canvas_id(canvas_id), m_w(w), m_h(h) {
        // 注册 Emscripten 事件回调（鼠标/键盘/触摸/resize）
        emscripten_set_mousedown_callback(m_canvas_id.c_str(), this, true, &on_mouse);
        emscripten_set_mouseup_callback(m_canvas_id.c_str(), this, true, &on_mouse);
        emscripten_set_mousemove_callback(m_canvas_id.c_str(), this, true, &on_mouse);
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, true, &on_key);
        emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, true, &on_key);
        emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, &on_resize);
    }

    ~WasmSurface() override {
        emscripten_set_mousedown_callback(m_canvas_id.c_str(), nullptr, true, nullptr);
        emscripten_set_mouseup_callback(m_canvas_id.c_str(), nullptr, true, nullptr);
        emscripten_set_mousemove_callback(m_canvas_id.c_str(), nullptr, true, nullptr);
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, nullptr);
        emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, nullptr);
        emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
    }

    [[nodiscard]] auto begin_frame(int w, int h) -> Result<bool> override {
        m_painter.begin(w, h);
        m_w = w;
        m_h = h;
        return true;
    }
    [[nodiscard]] auto painter() -> Painter & override { return m_painter; }

    [[nodiscard]] auto present() -> Result<bool> override {
        const int w = m_painter.width();
        const int h = m_painter.height();
        if (w <= 0 || h <= 0) return true;
        const std::uint8_t *src = m_painter.data();
        // 通过 EM_ASM 将 WASM 线性内存中的 RGBA 帧缓冲拷贝到 Canvas 2D：
        // 1. 获取 canvas 元素与 2D 上下文
        // 2. 创建 ImageData(w, h)
        // 3. 将 WASM 内存 src 拷贝到 ImageData.data（Uint8ClampedArray）
        // 4. putImageData 到 canvas
        // 注意：Painter 输出 RGBA，Canvas ImageData 也是 RGBA，无需 swizzle。
        EM_ASM(
            {
                const canvas = document.getElementById(UTF8ToString($3));
                if (!canvas) return;
                const ctx = canvas.getContext('2d');
                if (!ctx) return;
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
            src, w, h, m_canvas_id.c_str());
        ++m_frame;
        return true;
    }

    [[nodiscard]] auto size() const -> Size override {
        return Size{ static_cast<float>(m_w), static_cast<float>(m_h) };
    }
    [[nodiscard]] auto should_close() const -> bool override { return m_should_close; }
    [[nodiscard]] auto data() const -> const std::uint8_t * override { return m_painter.data(); }
    [[nodiscard]] auto frame_count() const -> int override { return m_frame; }
    [[nodiscard]] auto clear_color() const -> Color override { return Color{ 245, 245, 247, 255 }; }

    auto set_event_handler(const EventHandler &h) -> void override { m_event_handler = h; }
    auto set_title(const std::string &title) -> void override {
        EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
    }

    /// @brief WASM 下 no-op：浏览器 rAF 驱动帧循环，无需阻塞等待。
    auto wait_events(double /*timeout_ms*/) -> void override {}

  private:
    std::string m_canvas_id;
    Painter m_painter;
    int m_w = 0;
    int m_h = 0;
    int m_frame = 0;
    bool m_should_close = false;
    EventHandler m_event_handler;

    // ---- Emscripten 事件回调 ----
    static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *user_data) {
        auto *self = static_cast<WasmSurface *>(user_data);
        if (!self->m_event_handler) return EM_FALSE;
        MouseEvent ev;
        ev.position = Point{ static_cast<float>(e->target_x), static_cast<float>(e->target_y) };
        ev.button = (e->button == 2) ? MouseButton::Right : (e->button == 1) ? MouseButton::Middle : MouseButton::Left;
        ev.action = (type == EMSCRIPTEN_EVENT_MOUSEDOWN) ? MouseAction::Press
                    : (type == EMSCRIPTEN_EVENT_MOUSEUP) ? MouseAction::Release
                                                         : MouseAction::Move;
        self->m_event_handler(ev);
        return EM_TRUE;
    }

    static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *user_data) {
        auto *self = static_cast<WasmSurface *>(user_data);
        if (!self->m_event_handler) return EM_FALSE;
        KeyEvent ev;
        ev.key = static_cast<int>(e->keyCode);
        ev.action = (type == EMSCRIPTEN_EVENT_KEYDOWN) ? KeyAction::Down : KeyAction::Up;
        if (e->shiftKey) ev.modifiers = ev.modifiers | ModifierKey::Shift;
        if (e->ctrlKey) ev.modifiers = ev.modifiers | ModifierKey::Control;
        if (e->altKey) ev.modifiers = ev.modifiers | ModifierKey::Alt;
        if (e->metaKey) ev.modifiers = ev.modifiers | ModifierKey::Meta;
        self->m_event_handler(ev);
        return EM_TRUE;
    }

    static EM_BOOL on_resize(int /*type*/, const EmscriptenUiEvent * /*e*/, void *user_data) {
        auto *self = static_cast<WasmSurface *>(user_data);
        // 查询 canvas 当前 CSS 尺寸并更新
        double css_w = 0, css_h = 0;
        emscripten_get_element_css_size(self->m_canvas_id.c_str(), &css_w, &css_h);
        if (css_w > 0 && css_h > 0) {
            self->m_w = static_cast<int>(css_w);
            self->m_h = static_cast<int>(css_h);
        }
        return EM_TRUE;
    }
};

} // namespace aurora

#endif // AURORA_BACKEND_WASM / AURORA_PLATFORM_WASM
