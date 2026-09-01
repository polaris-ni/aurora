#include "aurora/event/dispatcher.h"

#include <algorithm>
#include <optional>
#include <ranges>

#include "aurora/event/keycode.h"

namespace aurora {

auto EventDispatcher::dispatch(Widget &root, MouseEvent &e, FocusManager *fm) -> bool {
    // 便捷封装：委托给「进程内持久」的实例，从而保留跨事件的指针捕获
    // （按下后即使光标移出窗口/落入重叠兄弟控件，Move/Release 仍持续派发给同一目标）。
    // 指针捕获表仍以「实例成员」形式存在（非全局静态），避免控件按值/栈管理时缓存悬空 Widget*；
    // 持久单例仅在进程内共享，单线程 UI 下安全。需要独立隔离的派发域时仍可用实体实例 dispatch_mouse。
    static EventDispatcher s_inst;
    return s_inst.dispatch_mouse(root, e, fm);
}

namespace {
// 沿命中链自最深（链尾）向根（链头）派发；全局坐标换算为各控件本地坐标。
auto deliver_chain(std::vector<HitNode> &chain, MouseEvent &e) -> void {
    const Point global = e.position;
    for (auto &it : std::views::reverse(chain)) {
        // 控件可能已被虚拟列表回收（命中链持有生命周期守卫）：失效则安全跳过。
        // keepalive 必须活到 on_pointer_event 返回之后：用户 on_click 回调可能重建
        // 页面并丢掉该控件的最后一个强引用，而返回后控件还要写自己的 m_pressed 等成员。
        std::shared_ptr<Widget> keepalive;
        Widget *sp = it.lock(keepalive);
        if (sp == nullptr) {
            continue;
        }
        e.local_position = global - it.origin;
        sp->on_pointer_event(e);
        if (e.handled) {
            break;
        }
    }
}

// Press 时的焦点归属：自最深命中向根找第一个可获焦控件（点到不可获焦的装饰子控件
// 时归属其可获焦祖先，如按钮内的图标）；整条链都不可获焦返回 nullptr。
[[nodiscard]] auto focus_target_of(const std::vector<HitNode> &chain) -> Widget * {
    for (const auto &it : std::views::reverse(chain)) {
        // focusable() 是对控件的解引用调用，须在调用期间持住强引用（见 HitNode::lock）。
        std::shared_ptr<Widget> keepalive;
        Widget *sp = it.lock(keepalive);
        if (sp == nullptr) {
            continue;
        }
        if (sp->focusable()) {
            return sp;
        }
    }
    return nullptr;
}
} // namespace

auto EventDispatcher::update_hover(const std::vector<HitNode> &chain) -> void {
    // 命中链 diff：旧链有、新链无 → 离开；新链有、旧链无 → 进入。链长度通常 ≤ 几十，
    // 线性查找足够；先发离开再发进入，与主流框架 enter/leave 顺序一致。
    // 控件以弱引用持有：若某控件已在两次 hover 之间被虚拟列表回收（如 LazyList 滚动复用
    // 子项），其弱引用失效，lock 返回空即跳过，绝不解引用悬垂指针（避免 use-after-free）。
    auto contains = [](const std::vector<HitNode> &c, const Widget *w) -> bool {
        if (w == nullptr) {
            return false;
        }
        return std::ranges::any_of(c, [w](const HitNode &n) -> bool { return n.get() == w; });
    };
    for (const HitNode &old_n : m_hover_chain) {
        Widget *sp = old_n.get();
        if (sp == nullptr) {
            continue; // 已回收，跳过离开通知
        }
        if (!contains(chain, sp)) {
            sp->on_hover_change(false);
        }
    }
    for (const HitNode &new_n : chain) {
        Widget *sp = new_n.get();
        if (sp == nullptr) {
            continue;
        }
        if (!contains(m_hover_chain, sp)) {
            sp->on_hover_change(true);
        }
    }
    m_hover_chain = chain;
}

auto EventDispatcher::dispatch_mouse(Widget &root, MouseEvent &e, FocusManager *fm) -> bool {
    FocusManager *prev = current_focus_manager();
    set_current_focus_manager(fm);
    const int key = e.pointer_id.has_value() ? e.pointer_id.value() : AURORA_MOUSE_CAPTURE_KEY;
    const Rect root_rect{ .origin = Point(), .size = root.size() };

    // Press：命中后缓存命中链（指针捕获），后续 Move/Release 即使命中失败也复用。
    if (e.action == MouseAction::Press) {
        auto chain = root.hit_test_chain(e.position, root_rect, BuildContext{});
        if (chain.empty()) {
            // 点击空白：标准 UI 惯例为清除当前焦点（blur）——旧焦点控件收到
            // on_focus_change(false)，Text 据此清除选区高亮。
            if (fm != nullptr) {
                fm->clear();
            }
            set_current_focus_manager(prev);
            return false;
        }
        if (fm != nullptr) {
            // 最近可获焦目标获焦；整条链都不可获焦则清焦点（set_focus(nullptr)），
            // 与点击空白同义——否则点到纯展示容器时旧焦点/选区残留不消。
            fm->set_focus(focus_target_of(chain));
        }
        deliver_chain(chain, e);
        m_pointer_capture[key] = std::move(chain);
        set_current_focus_manager(prev);
        return true;
    }

    // Move / Release：已对当前指针建立捕获则复用缓存命中链（无视实时命中测试），
    // 使光标移出根/窗口外、或落入重叠兄弟控件时仍连续派发给按下时的目标。
    auto cit = m_pointer_capture.find(key);
    if (cit != m_pointer_capture.end()) {
        std::vector<HitNode> chain = cit->second; // 复制：Release 会擦除原链
        if (e.action == MouseAction::Release) {
            m_pointer_capture.erase(cit);
        }
        deliver_chain(chain, e);
        set_current_focus_manager(prev);
        return true;
    }

    // 无捕获（悬停 Move、或窗口外 Release 而无前置 Press）：走常规命中测试。
    // 注意：鼠标 Release 不在此切换焦点（与原 EventDispatcher 行为一致，焦点仅在
    // Press 时转移），否则拖选结束落在邻行/窗口外时焦点被抢走、选区被清空。
    auto chain = root.hit_test_chain(e.position, root_rect, BuildContext{});
    if (e.action == MouseAction::Move) {
        update_hover(chain); // 悬停追踪：空链也要 diff（光标移到空白/窗外 → 清除旧悬停）
    }
    if (chain.empty()) {
        set_current_focus_manager(prev);
        return false;
    }
    deliver_chain(chain, e);
    set_current_focus_manager(prev);
    return true;
}

// ---- 多点触控派发：拆为「① 命中链解析 / ② 原始流广播 / ③ 手势流冒泡」三步 ----
namespace {

/// @brief 单触点的路由结果：本帧命中链 + 合成手势动作。
struct TouchRoute {
    std::vector<HitNode> chain;
    MouseAction action = MouseAction::Move;
};

// ① 命中链解析 + 指针捕获维护：按下时命中并缓存整条链，活跃期复用，抬起即清除。
[[nodiscard]] auto route_touch_point(Widget &root, const Rect &root_rect,
                                     std::unordered_map<int, std::vector<HitNode>> &capture, const TouchPoint &p)
    -> TouchRoute {
    TouchRoute r;
    const auto cit = capture.find(p.id);
    const bool was_down = (cit != capture.end());
    if (p.active) {
        if (was_down) {
            r.chain = cit->second; // 活跃期复用缓存链
            r.action = MouseAction::Move;
            return r;
        }
        // 首次按下：命中测试并缓存整条命中链（指针捕获）。
        r.chain = root.hit_test_chain(p.position, root_rect, BuildContext{});
        r.action = MouseAction::Press;
        if (!r.chain.empty()) {
            capture[p.id] = r.chain;
        }
        return r;
    }
    // 已抬起：取出并清除捕获链（抬起即清，避免悬空）。
    if (was_down) {
        r.chain = std::move(cit->second);
        capture.erase(cit);
    }
    r.action = MouseAction::Release;
    return r;
}

// ② 原始多点流：把完整 TouchEvent 交给整条命中链（touch() 修饰器 / PinchRecognizer 消费），全链广播不截断。
auto broadcast_touch(const std::vector<HitNode> &chain, TouchEvent &e) -> void {
    for (const auto &it : std::views::reverse(chain)) {
        std::shared_ptr<Widget> keepalive; // 回调可能销毁该控件，须持强引用跨越调用
        Widget *sp = it.lock(keepalive);
        if (sp == nullptr) {
            continue; // 控件已被回收，安全跳过
        }
        sp->on_pointer_event(e);
    }
}

// ③ 手势流：按当前触点合成 MouseEvent（携带 pointer_id）驱动 Draggable/LongPress/Clickable，冒泡至被消费。
auto deliver_synthesized(const std::vector<HitNode> &chain, const TouchPoint &p, MouseAction action) -> void {
    MouseEvent me;
    me.action = action;
    me.position = p.position;
    me.button = MouseButton::Left;
    me.pointer_id = p.id;
    for (const auto &it : std::views::reverse(chain)) {
        std::shared_ptr<Widget> keepalive; // 同 ②：回调可能销毁该控件，须持强引用跨越调用
        Widget *sp = it.lock(keepalive);
        if (sp == nullptr) {
            continue; // 控件已被回收，安全跳过
        }
        me.local_position = p.position - it.origin;
        sp->on_pointer_event(me);
        if (me.handled) {
            break; // 某级消费即停止冒泡
        }
    }
}

} // namespace

auto TouchDispatcher::dispatch(Widget &root, TouchEvent &e, FocusManager *fm) -> bool {
    FocusManager *prev = current_focus_manager();
    set_current_focus_manager(fm);
    bool any = false;
    const Rect root_rect{ .origin = Point(), .size = root.size() };

    for (const TouchPoint &p : e.points) {
        const TouchRoute route = route_touch_point(root, root_rect, m_pointer_capture, p);
        if (route.chain.empty()) {
            continue; // 抬起的悬空点 / 完全未命中
        }
        any = true;

        // Press 时把焦点交给命中链上最近的「可聚焦」控件（与 MouseEvent 路径一致）；
        // 整条链不可获焦则清焦点（点到纯展示区域时旧选区不残留）。
        if (route.action == MouseAction::Press && fm != nullptr) {
            fm->set_focus(focus_target_of(route.chain));
        }
        broadcast_touch(route.chain, e);
        deliver_synthesized(route.chain, p, route.action);
    }

    set_current_focus_manager(prev);
    return any;
}

// ---- 键盘事件派发：拆为「按键分类 / 修饰键合并 / 快捷键匹配 / 焦点路由」四步 ----
namespace {

// ① 按键分类：把 aurora 逻辑键归入少数语义类别，供后续步骤分流。
enum class KeyCategory : std::uint8_t { Other, Tab, Arrow, Activate };

[[nodiscard]] auto classify_key(const KeyEvent &e) -> KeyCategory {
    switch (static_cast<KeyCode>(e.key)) {
    case KeyCode::Tab: return KeyCategory::Tab;
    case KeyCode::ArrowUp:
    case KeyCode::ArrowDown:
    case KeyCode::ArrowLeft:
    case KeyCode::ArrowRight: return KeyCategory::Arrow;
    case KeyCode::Enter:
    case KeyCode::Space: return KeyCategory::Activate;
    default: return KeyCategory::Other;
    }
}

// ② 修饰键合并：把位掩码修饰键归并为命名标志，供快捷键匹配派生导航意图（如 Shift+Tab 后退）。
struct MergedModifiers {
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;
};

[[nodiscard]] auto merge_modifiers(const KeyEvent &e) -> MergedModifiers {
    return MergedModifiers{
        .shift = (static_cast<unsigned>(e.modifiers & ModifierKey::Shift) != 0u),
        .control = (static_cast<unsigned>(e.modifiers & ModifierKey::Control) != 0u),
        .alt = (static_cast<unsigned>(e.modifiers & ModifierKey::Alt) != 0u),
        .meta = (static_cast<unsigned>(e.modifiers & ModifierKey::Meta) != 0u),
    };
}

// ③ 快捷键匹配：Tab / 方向键 / Enter·Space 的全局快捷键。命中并消费返回 handled 结果；
// 否则返回 nullopt，交由焦点路由把事件交给当前焦点控件（如文本框内部光标/选区）。
[[nodiscard]] auto match_shortcut(KeyEvent &e, FocusManager &fm, KeyCategory cat, const MergedModifiers &mods)
    -> std::optional<bool> {
    if (e.action != KeyAction::Down) {
        return std::nullopt; // 仅按下阶段匹配快捷键；释放等交给焦点控件
    }
    switch (cat) {
    case KeyCategory::Tab: // Tab 序导航：Shift+Tab 后退，否则前进（specification/05-event-navigation.md §4）
        fm.move_focus(mods.shift ? FocusDirection::Backward : FocusDirection::Forward);
        e.handled = true;
        return true;
    case KeyCategory::Arrow: {
        FocusDirection dir = FocusDirection::Forward;
        switch (static_cast<KeyCode>(e.key)) {
        case KeyCode::ArrowUp: dir = FocusDirection::Up; break;
        case KeyCode::ArrowDown: dir = FocusDirection::Down; break;
        case KeyCode::ArrowLeft: dir = FocusDirection::Left; break;
        case KeyCode::ArrowRight: dir = FocusDirection::Right; break;
        default: return std::nullopt;
        }
        if (fm.move_focus(dir)) {
            e.handled = true;
            return true;
        }
        // 方向键但焦点未能移动：继续交给焦点控件（如文本框内部光标移动）
        return std::nullopt;
    }
    case KeyCategory::Activate: { // Enter/Space 激活当前焦点控件（触发 click）
        Widget *focused = fm.focused();
        if (focused == nullptr) {
            return std::nullopt; // 无焦点控件则不消费
        }
        focused->activate();
        e.handled = true;
        return true;
    }
    default: return std::nullopt;
    }
}

// ④ 焦点路由：无匹配快捷键时把事件交给当前焦点控件处理（文本输入等）。
[[nodiscard]] auto route_to_focused(KeyEvent &e, Widget *focused) -> bool {
    if (focused == nullptr) {
        return false;
    }
    focused->on_key_event(e);
    return e.handled;
}

} // namespace

auto EventDispatcher::dispatch(Widget & /*root*/, KeyEvent &e, FocusManager &fm) -> bool {
    FocusManager *prev = current_focus_manager();
    set_current_focus_manager(&fm);

    // ① 按键分类
    const KeyCategory cat = classify_key(e);
    // ② 修饰键合并
    const MergedModifiers mods = merge_modifiers(e);
    // ③ 快捷键匹配：Tab/方向/激活；命中并消费则直接返回。
    if (const auto handled = match_shortcut(e, fm, cat, mods); handled.has_value()) {
        set_current_focus_manager(prev);
        return handled.value();
    }
    // ④ 焦点路由：无匹配快捷键则交给当前焦点控件。
    const bool result = route_to_focused(e, fm.focused());
    set_current_focus_manager(prev);
    return result;
}

auto EventDispatcher::dispatch(Widget &root, ScrollEvent &e) -> bool {
    Widget *target = hit_test(root, e.position);
    if (target == nullptr) {
        return false;
    }
    target->on_scroll(e);
    return true;
}

auto EventDispatcher::dispatch(Widget &root, FileDropEvent &e) -> bool {
    Widget *target = hit_test(root, e.position);
    if (target == nullptr) {
        return false;
    }
    target->on_file_drop(e);
    return e.handled;
}

auto EventDispatcher::dispatch(Widget & /*root*/, TextInputEvent &e, FocusManager &fm) -> bool {
    FocusManager *prev = current_focus_manager();
    set_current_focus_manager(&fm);
    Widget *focused = fm.focused();
    if (focused == nullptr) {
        set_current_focus_manager(prev); // 与其余出口一致：不得把 &fm 泄漏到调用者作用域之外
        return false;
    }
    focused->on_text_input(e);
    set_current_focus_manager(prev);
    return e.handled;
}

} // namespace aurora
