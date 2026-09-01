#include "aurora/app/application.h"

#include <chrono>

namespace aurora {

auto Application::dispatch_click(float x, float y) -> void {
    MouseEvent e;
    e.position = Point{ .x = x, .y = y };
    e.action = MouseAction::Press;
    m_mouse.dispatch_mouse(m_scene.root(), e, &m_focus);
}

auto Application::dispatch_pointer(float x, float y, MouseAction action) -> void {
    MouseEvent e;
    e.position = Point{ .x = x, .y = y };
    e.action = action;
    m_mouse.dispatch_mouse(m_scene.root(), e, &m_focus);
}

auto Application::tick() -> void { m_scene.root().tick(std::chrono::steady_clock::now()); }

auto Application::dispatch_key(KeyEvent e) -> bool {
    // 快捷键优先：匹配到已启用绑定则消费，不再向焦点控件派发（与 dispatch(Event&) 路径一致）。
    if (m_shortcuts.handle(e, m_focus.focused() != nullptr)) {
        return true;
    }
    return EventDispatcher::dispatch(m_scene.root(), e, m_focus);
}

auto Application::dispatch_text(TextInputEvent e) -> bool {
    return EventDispatcher::dispatch(m_scene.root(), e, m_focus);
}

auto Application::dispatch_touch(const TouchEvent &e) -> void {
    // 派发器需要非 const 事件（命中链按 pointer id 缓存、handled 回写），此处构造可变副本。
    TouchEvent copy = e;
    m_touch.dispatch(m_scene.root(), copy, &m_focus);
}

} // namespace aurora
