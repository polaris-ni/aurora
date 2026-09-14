#include "aurora/app/application.h"

#include <chrono>

namespace aurora {

auto Application::dispatch_click(float x, float y) -> void {
    MouseEvent e;
    e.position = Point{.x = x, .y = y};
    e.action = MouseAction::Press;
    mouse_.dispatch_mouse(scene_.root(), e, &focus_);
}

auto Application::dispatch_pointer(float x, float y, MouseAction action) -> void {
    MouseEvent e;
    e.position = Point{.x = x, .y = y};
    e.action = action;
    mouse_.dispatch_mouse(scene_.root(), e, &focus_);
}

auto Application::tick() -> void { scene_.root().tick(std::chrono::steady_clock::now()); }

auto Application::dispatch_key(KeyEvent e) -> bool {
    // 派发上下文：快捷键动作内同样可取到当前焦点管理器（与 dispatch(Event&) 路径一致）。
    FocusManager *const prev_fm = current_focus_manager();
    set_current_focus_manager(&focus_);
    // 快捷键优先：匹配到已启用绑定则消费，不再向焦点控件派发（与 dispatch(Event&) 路径一致）。
    if (shortcuts_.handle(e, focus_.focused() != nullptr)) {
        set_current_focus_manager(prev_fm);
        return true;
    }
    const bool result = EventDispatcher::dispatch(scene_.root(), e, focus_);
    set_current_focus_manager(prev_fm);
    return result;
}

auto Application::dispatch_text(TextInputEvent e) -> bool {
    return EventDispatcher::dispatch(scene_.root(), e, focus_);
}

auto Application::dispatch_touch(const TouchEvent &e) -> void {
    // 派发器需要非 const 事件（命中链按 pointer id 缓存、is_handled_ 回写），此处构造可变副本。
    TouchEvent copy = e;
    touch_.dispatch(scene_.root(), copy, &focus_);
}

}  // namespace aurora
