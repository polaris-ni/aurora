#pragma once

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/widget/node.h"      // Node 完整定义（析构在 widget.cpp）：使 std::vector<Node> 成员在头文件中可见完整类型
#include "aurora/core/assert.h"      // AURORA_ASSERT：脏标记必达渲染根的 StrictMode 不变量
#include "aurora/core/strict_mode.h" // strict_mode()：同上（仅严格模式下校验）
#include "aurora/core/types.h"
#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/display_list.h" // Display List 缓存成员
#include "aurora/render/painter.h"      // 使 unique_ptr<Painter> 缓存成员可完整析构
#include "aurora/state/effect.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/widget/descriptor.h" // WidgetDescriptor / PropDescriptor（自描述）
#include "aurora/widget/props_io.h"   // Json / lengthToJson / colorToJson（序列化）

#include "aurora/debug/debug_trace.h" // why_trace：DirtyKind + detail::record_dirty（轻量，不 include widget.h）

namespace aurora {

/// @brief widget 树默认最大深度（specification/01-core.md §4.4 有界层深度守卫）。超过此深度的递归展开（如
/// Repeater 嵌套 / 极深容器链）经 `Diagnostics` 降级截断，避免栈溢出 / 渲染雪崩。
inline constexpr std::size_t AURORA_DEFAULT_MAX_WIDGET_DEPTH = 64;

class Painter; // 前向声明（render 模块定义于 render/painter.h）

class Widget; // 前向声明（HitNode 以 std::weak_ptr<Widget> 作为成员；Widget 在下方定义）

// why_trace 热路径埋点：调试帧计数（定义见 debug_paint.cpp；此处仅前向声明，供内联 mark_needs_* 调用）。
namespace debug {
[[nodiscard]] auto current_debug_frame() -> std::uint64_t;
} // namespace debug

/// @brief 命中链节点：携带命中控件及其相对根的全局 origin（用于事件坐标本地化）。
/// 命中链递归下降时，子节点的 `Node::m_bounds.origin` 即其全局 origin，直接带入；
/// 派发器（EventDispatcher）在冒泡到某控件前，以 `e.local_position = e.position - origin`
/// 写入本地坐标，控件无需再查询自身在树中的绝对位置。
///
/// @note 生命周期安全：虚拟列表（LazyList）等会在滚动时回收并销毁子控件，若命中链
/// （悬停链 `m_hover_chain`、指针捕获 `m_pointer_capture`）持有裸指针，回收后即为悬垂指针，
/// 下一次 `update_hover` / 指针事件派发时解引用即触发 use-after-free（访问违规 0xC0000005）。
///
/// 因此 `HitNode` 同时保存裸指针 `ptr` 与弱引用 `guard`：
/// - `guard` 非空（控件由 `shared_ptr` 持有，如 LazyList 复用的子项）时，`get()` 以
///   `guard.lock()` 判活，控件被回收后返回 `nullptr`，派发器据此安全跳过；
/// - `guard` 为空（控件为栈对象 / 成员对象，未被 `shared_ptr` 持有，`weak_from_this()`
///   返回空弱引用）时，其生命周期由持有者保证，`get()` 直接返回 `ptr`。
///
/// 若仅用 `weak_ptr`，栈上构造的控件（测试与大量 demo 的常见写法）`weak_from_this()`
/// 恒为空 → `lock()` 恒失败 → 全部指针事件被静默丢弃。故必须保留裸指针作为可达性来源。
struct HitNode {
    Widget *ptr = nullptr;       ///< 命中链上的控件（root→target 顺序）
    std::weak_ptr<Widget> guard; ///< 生命周期守卫；仅当控件由 shared_ptr 持有时有效
    bool guarded = false;        ///< guard 是否关联控制块（区分「空弱引用」与「已失效弱引用」）
    Point origin{};              ///< 该控件相对根的全局 origin

    HitNode() = default;

    /// 从控件与全局 origin 构造：自动探测该控件是否由 `shared_ptr` 持有。
    /// @param w 命中控件（非空）
    /// @param lifetime_guard lifetime guard
    /// @param global_origin 该控件相对根的全局 origin
    HitNode(Widget *w, std::weak_ptr<Widget> lifetime_guard, const Point &global_origin)
        : ptr(w), guard(std::move(lifetime_guard)), origin(global_origin) {
        // 空弱引用与已失效弱引用的 expired() 都为 true，无法事后区分；
        // 故在构造时（此刻控件必然存活）判定：能 lock 成功即说明由 shared_ptr 持有。
        guarded = guard.lock() != nullptr;
    }

    /// 取存活控件指针；已被回收返回 nullptr。
    ///
    /// @warning 仅用于**不解引用**的用途（如与另一指针比较是否同一控件）。返回值不带
    /// 生命周期保证：`guard.lock()` 产生的临时 `shared_ptr` 在本函数返回时即析构，
    /// 若控件的最后一个强引用就在其中，指针当场悬垂。要调用控件方法请改用 `lock()`。
    [[nodiscard]] auto get() const -> Widget * {
        if (guarded) {
            return guard.lock().get(); // shared_ptr 持有：回收后返回 nullptr
        }
        return ptr; // 栈/成员对象：生命周期由持有者保证
    }

    /// 取存活控件指针，并把强引用写入 `out_keepalive` 以延长其生命周期至调用方作用域结束。
    ///
    /// 派发回调（`on_pointer_event` → 用户 `on_click`）可能销毁控件自身所在的子树
    /// （典型：点击按钮触发 `NavigatorHost::push_replacement`，重建时丢掉该按钮的最后一个
    /// 强引用），而回调返回后 `Widget::on_pointer_event` 仍要写 `m_pressed` 等成员。
    /// 因此凡要解引用命中链节点，都必须在整个调用期间持住强引用。
    /// @param out_keepalive 出参：控件由 `shared_ptr` 持有时写入强引用；栈/成员对象时置空。
    [[nodiscard]] auto lock(std::shared_ptr<Widget> &out_keepalive) const -> Widget * {
        if (guarded) {
            out_keepalive = guard.lock();
            return out_keepalive.get(); // 回收后为 nullptr
        }
        out_keepalive.reset();
        return ptr; // 栈/成员对象：生命周期由持有者保证
    }
};

/**
 * @brief widget 抽象基类：声明布局/绘制/命中/挂载接口与通用属性。
 *
 * 对应 ARCHITECTURE.md §4.4：具体 widget 继承并实现各 `Impl` 虚函数；`modifier`
 * 修饰链在基类的 layout/paint/hit_test 中统一包裹，避免各 widget 重复 props。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Widget : public std::enable_shared_from_this<Widget> {
  public:
    virtual ~Widget() = default;

    Widget() = default;

    Widget(const Widget &) = delete;
    auto operator=(const Widget &) -> Widget & = delete;
    Widget(Widget &&) = default;
    auto operator=(Widget &&) -> Widget & = default;

    /// @brief 测量：应用 modifier 包裹后调用 layoutImpl。
    virtual auto layout(const Constraints &c, const BuildContext &ctx) -> Size;
    /// @brief 绘制：应用 modifier（背景等）后调用 on_paint。
    auto paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void;
    /// @brief 使离屏缓存（`Modifier::cache_layer`）失效，下次绘制重新渲染子树。
    auto invalidate_paint_cache() const -> void;
    /// @brief 命中测试：Clickable 拦截后委托 on_hit_test。
    auto hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget *;

    /// @brief 命中链：返回根→最深命中的完整 widget 路径（`this` 起算，含自身与所有命中的祖先/后代）。
    /// 用于事件自底向上冒泡派发（specification/05-event-navigation.md §3）：派发器从链尾（最深）向链头（根）逐个调用，
    /// 某节点写 `e.handled = true` 即停止。命中即止的 `hit_test` 保留供兼容/纯命中查询。
    auto hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx) -> std::vector<HitNode>;

    /// @brief 挂载：注册响应式依赖并递归挂载子树（由 build 后一次性调用）。
    auto mount(const BuildContext &ctx) -> void;

    /// @brief 收集本 widget 的响应式信号（供基类注册依赖）；子类覆写 push 自身信号。
    /// 默认实现为空：无信号叶控件无需再写空 override（子节点在自身 mount 时自行订阅）。
    virtual auto collect_signals(std::vector<SignalViewBase *> &out) -> void;

    // ---- 通用属性 ----

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Reactive<Modifier> modifier; ///< 修饰链
    Reactive<bool> show{ true }; ///< 可见性（非结构性隐藏）
    // NOLINTEND(*-non-private-member-variables-in-classes)

    /// @brief 显式宽度意图（specification/01-core.md §2.2 / 需求 #20）：默认 `auto`（按内容）。
    /// 用 `au::px(120)` / `au::fill()` / `au::percent(0.5f)` 等强类型设置；
    /// **裸整数编译失败**（无 `Length(int)` 隐式转换）。返回 `Widget&` 以支持链式。
    virtual auto width(Length len) -> Widget & {
        m_width = len;
        mark_needs_layout();
        return *this;
    }
    /// @brief 显式高度意图（语义同 `width`）。
    virtual auto height(Length len) -> Widget & {
        m_height = len;
        mark_needs_layout();
        return *this;
    }
    [[nodiscard]] auto width_spec() const -> const Length & { return m_width; }
    [[nodiscard]] auto height_spec() const -> const Length & { return m_height; }

    /// @brief 溢出策略（参考 CSS overflow）：控制子内容超出本控件边界时的行为。
    /// Visible=溢出可见（默认）；Hidden/Clip/Scroll=裁剪到本控件盒子内。
    /// Hidden 与 Clip 当前行为相同（均裁剪视觉），Clip 保留 hit-test（预留语义）。
    /// Scroll 当前等同 Hidden（滚动预留）。
    virtual auto overflow_strategy(OverflowStrategy strategy) -> Widget & {
        m_overflow = strategy;
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto overflow_strategy() const -> OverflowStrategy { return m_overflow; }

    // ---- 脏标记（specification/04-widget.md §2.4）----
    auto mark_needs_layout() -> void { mark_needs_layout_impl(false); }

    /// @brief 设置布局父节点，供缓存失效与脏标记向上传播。
    ///
    /// @note 通常**无需手工调用**：`Widget::layout()` 会以「当前正在布局的控件」自动登记
    ///       （布局调用天然嵌套，见 widget.cpp 的 `t_layout_parent`），使父链由构造保证完整。
    ///       本接口保留给「不经父容器 `layout()` 入口就地重排子树」的特殊场景与历史调用点。
    auto set_layout_parent(Widget *p) -> void { m_layout_parent = p; }

    /// @brief 布局父节点（未接入树时为 nullptr）。
    [[nodiscard]] auto layout_parent() const -> Widget * { return m_layout_parent; }

    /// @brief 本控件是否可被 Display List 缓存（恒等变换且绘制无副作用、内容不每帧变动）。
    ///        默认 true；绘制时产生副作用（如 Hero 几何注册）或内容每帧变化（转场淡变）的
    ///        控件须覆盖为 false，否则缓存回放会跳过必要的每帧绘制（见 NavigatorHost/Hero/TransitionLayer）。
    [[nodiscard]] virtual auto can_cache_display_list() const -> bool { return true; }

    /// @brief 本控件布局结果是否可被缓存（约束不变 ⇒ on_layout 结果不变、且无非布局副作用）。
    ///        默认 true；与 `can_cache_display_list()` 对称：绘制每帧变动 → 禁 DL 缓存，
    ///        布局非纯函数 → 禁布局缓存。
    ///
    ///        若 `on_layout` 含时间 / 状态依赖的副作用（首屏骨架→真实内容切换、入场动画、
    ///        随外部状态重建子节点等“延期布局”），须覆盖为 false。否则布局缓存（AURORA_LAYOUT_CACHE）
    ///        在约束不变时直接复用缓存尺寸、完全跳过 `on_layout`，使延期逻辑永不触发——
    ///        表现为白屏 / 内容冻结（即 Path B 类 bug）。`mark_needs_layout()` 只能沿“显式标脏”
    ///        路径失效缓存；凡是靠时钟或帧驱动在 `on_paint`/`tick` 中触发重建的控件，必须以此开关
    ///        退出布局缓存，方能保证 `on_layout` 在每个布局 pass 被真正执行。
    ///
    ///        @see Widget::layout（缓存短路条件含 `can_cache_layout()`）
    ///        @see mark_needs_layout（向上失效传播，不覆盖“约束不变但状态已变”的情形）
    [[nodiscard]] virtual auto can_cache_layout() const -> bool { return true; }

    /// @brief 重排边界：自身尺寸由约束决定、不依赖子节点（固定/撑满/百分比宽高）。
    /// 该属性用于布局缓存的语义标识；当前缓存失效策略统一向上传播至根以保证正确性。
    [[nodiscard]] auto is_relayout_boundary() const -> bool {
        return m_is_relayout_boundary || m_width.kind != LengthKind::WrapContent ||
               m_height.kind != LengthKind::WrapContent;
    }
    /// @brief 读取上次成功布局的输入约束（布局缓存键）；present_root 对脏 boundary 子树
    ///        局部重排时据此从正确的约束入口重排。
    [[nodiscard]] auto cached_constraints() const -> const Constraints & { return m_cached_constraints; }
    auto mark_needs_paint() -> void { mark_needs_paint_impl(false); }

#ifdef AURORA_DISPLAY_LIST
    /// @brief 使本控件 Display List 失效并沿布局父链向上传播：子树绘制命令被并入祖先 DL，
    ///        故任一后代内容变化时祖先须重录。已在失效链上则短路避免重复递归。
    auto invalidate_display_list_up() -> void {
        if (m_dl_valid) {
            m_dl_valid = false;
            if (m_layout_parent != nullptr) {
                m_layout_parent->invalidate_display_list_up();
            }
            return;
        }
        // 已失效：可缓存控件此前已向上传播过祖先失效，可短路跳过。
        // 但「内容每帧变化、永不缓存」的控件（can_cache_display_list()==false）m_dl_valid 恒为
        // false，其绘制被并入祖先 DL——若不持续向上失效祖先，祖先缓存会冻结该后代的首帧
        // （如自驱动出场/轮播动画控件永远停在 ent≈0，表现为空白/淡灰）。故对此类控件必须继续上溯。
        if (!can_cache_display_list() && (m_layout_parent != nullptr)) {
            m_layout_parent->invalidate_display_list_up();
        }
    }
#endif
    /// @brief 挂在**本控件自身**上的重绘请求回调；bool=true 表示含布局脏（需重排）。
    /// 供直接持有某控件、需单独观察其标脏的场景（单元测试、自定义驱动）使用，
    /// 不参与树级脏传播；渲染器不再逐控件接线此回调（见 `on_subtree_dirty`）。
    std::function<void(bool)> on_dirty; // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 子树脏汇聚点：由渲染器（`Window`/`Scene`）安装在**根控件**上，全树仅一处。
    /// 任一后代 `request_frame` 时沿布局父链上溯到根，在此回调一次，
    /// `origin` = 最初标脏的控件（渲染器据其 `paint_bounds()` / `is_relayout_boundary()` 决策）。
    ///
    /// 取代旧的「渲染器每帧递归接线整棵树的 `on_dirty`」：接线本质是树结构的一次**快照**，
    /// 而自驱动控件常在 `on_layout` 中动态新建子控件（骨架→真实内容、轮播 banner、卡片），
    /// 这些新控件不在快照内 → 其 `mark_needs_paint` 无人接收 → 自驱动动画冻结在首帧、
    /// 骨架永不切换（表现为白屏/淡灰）；每帧重接又会让链式包装（`prev` 嵌套）无界增长。
    /// 上溯式传播不存在快照，动态新建的子树天然被覆盖。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(Widget &origin, bool layout)> on_subtree_dirty;

    /// @brief 后代标脏通知：`request_frame` 沿布局父链上溯时在**每个祖先**上调用。
    /// 默认空实现。持有离屏内容缓冲的容器（`Scroll`）覆写以置「内容脏」，
    /// 使缓冲仅在后代内容真变化时重录、纯滚动帧只平移合成。
    /// @param origin 最初标脏的后代控件
    /// @param layout 是否含布局脏
    virtual auto on_descendant_dirty(Widget &origin, bool layout) -> void {
        (void)origin;
        (void)layout;
    }

    /// @brief 激活（如点击）：事件派发在命中目标上调用；默认无操作。
    virtual auto activate() -> void {}

    /// @brief 悬停态变化通知（由 `EventDispatcher` 在无捕获 Move 的命中链 diff 时调用）。
    /// 默认仅记录 `m_hover`（不标脏：否则悬停穿过任意控件都会触发父容器整块重绘）；
    /// 需要 hover 视觉反馈的控件（Checkbox 等）覆写并追加 `mark_needs_paint()`。
    virtual auto on_hover_change(bool entered) -> void { m_hover = entered; }

    /// @brief 指针当前是否悬停在本控件上（命中链内即算，含被子控件覆盖的父容器）。
    [[nodiscard]] auto hovered() const -> bool { return m_hover; }

    /// @brief 本控件是否作为点击目标消费指针事件（用于点击/长按互斥与冒泡停止）。
    /// 默认：修饰链含 Clickable 修饰即为点击目标；Button 等自带 on_click 的叶控件覆写返回 true。
    [[nodiscard]] virtual auto wants_click() const -> bool { return modifier.get().has_clickable(); }

    /// @brief 指针事件入口（specification/05-event-navigation.md §3）：在命中目标上调用。
    /// 仅在「先按下、再抬起」且未达长按阈值、未拖拽构成一次完整点击时触发 activate / Clickable 回调；
    /// 悬停移动（Move）不触发点击，但有 `draggable`/`longPress` 修饰时驱动拖拽/长按计时。
    /// `e.handled` 仅在本控件自身消费事件（含 Clickable/Draggable/LongPress/ContextMenu 任一手势）时置位，
    /// 否则保持 false 交由派发器沿命中链向上冒泡给父级。
    virtual auto on_pointer_event(MouseEvent &e) -> void {
        const Modifier &mod = modifier.get();
        const bool consumes = wants_click() || mod.has_gesture() || mod.has_context_menu();
        switch (e.action) {
        case MouseAction::Press:
            // 右键按下：打开上下文菜单
            if (e.button == MouseButton::Right && mod.has_context_menu()) {
                mod.open_context_menu(e.position);
                e.handled = true;
                return;
            }
            m_pressed = true;
            m_press_pos = e.position;
            m_last_drag_pos = e.position;
            m_click_pending = wants_click();
            m_drag_moved = false;
            if (mod.has_gesture()) {
                m_needs_gesture_tick = true; // 开启手势计时，使 tick 递归驱动长按/拖拽阈值
                mod.invoke_drag_start(e.pointer_id);
                mod.press_long_press(std::chrono::steady_clock::now(), e.pointer_id);
            }
            e.handled = consumes;
            break;
        case MouseAction::Release:
            if (m_pressed) {
                const bool fire_click = wants_click() && m_click_pending && !mod.long_press_fired() && !m_drag_moved;
                if (fire_click) {
                    activate();
                    mod.invoke_click();
                }
                if (mod.has_gesture()) {
                    mod.invoke_drag_end(e.pointer_id);
                    mod.cancel_long_press(e.pointer_id);
                }
                m_click_pending = false;
            }
            m_pressed = false;
            e.handled = consumes;
            break;
        case MouseAction::Move:
            if (m_pressed && mod.has_gesture()) {
                const Point delta = e.position - m_last_drag_pos;
                if (std::abs(delta.x) > 1.0f || std::abs(delta.y) > 1.0f) {
                    m_drag_moved = true;
                }
                mod.invoke_drag(delta, e.position, e.pointer_id);
                m_last_drag_pos = e.position;
                e.handled = true; // 拖拽进行中：消费移动，父级不再收到 Move
            }
            break; // 悬停/移动不直接触发点击
        default: break;
        }
    }

    /// @brief 原始多点触摸事件入口：默认仅把完整 `TouchEvent` 交给修饰链（`touch()` / `PinchRecognizer` 等消费），
    ///        不驱动 `Draggable`/`LongPress`/`Clickable`（这些手势由派发器按每个触点合成的 `MouseEvent` 驱动）。
    ///        子类可覆盖以处理多点原始流；务必调用修饰链以免 `touch()` 修饰器失效。
    virtual auto on_pointer_event(TouchEvent &e) -> void { modifier.get().on_pointer_event(e); }

    /// @brief 由 `Application::tick` 周期性调用的公开入口：驱动手势计时（长按阈值检测）。
    /// 内部委派给受保护虚函数 `tick_gestures`；本 widget 与子树经此统一入口递归计时。
    /// 优化：若本 widget 及子树均无需手势计时（`m_needs_gesture_tick == false`），直接跳过。
    virtual auto tick(std::chrono::steady_clock::time_point now) -> void {
        if (!m_needs_gesture_tick) {
            return;
}
        tick_gestures(now);
    }

    /// @brief 键盘事件入口（焦点 widget 上调用）。默认标记为已消费。
    virtual auto on_key_event(KeyEvent &e) -> void { e.handled = true; }

    /// @brief 滚轮事件入口（命中目标上调用）。默认标记为已消费。
    virtual auto on_scroll(ScrollEvent &e) -> void { e.handled = true; }

    /// @brief 文本输入入口（焦点 widget 上调用）。默认标记为已消费。
    virtual auto on_text_input(TextInputEvent &e) -> void { e.handled = true; }

    /// @brief 操作系统文件拖放落在本控件时触发；消费时置 `e.handled` 阻止继续。
    /// 默认不处理（交给命中目标自身）。
    virtual auto on_file_drop(FileDropEvent &e) -> void { (void)e; }

    /// @brief 焦点变更通知（获焦 focus=true / 失焦 focus=false）。
    /// 基类默认维护 `m_isFocused` 以便 `isFocused()` 正确；子类可覆写以更新聚焦态绘制。
    virtual auto on_focus_change(bool focused) -> void { m_is_focused = focused; }

    // ---- 焦点能力（specification/05-event-navigation.md §4）----
    /// @brief 是否可参与焦点序（默认 true）。交互控件保持 true；纯展示控件可设 false。
    [[nodiscard]] auto focusable() const -> bool { return m_focusable; }
    auto set_focusable(bool v) -> Widget & {
        m_focusable = v;
        return *this;
    }

    /// @brief Tab 序权重（默认 0，越小越靠前）；move_focus 按此排序。
    [[nodiscard]] auto tab_index() const -> int { return m_tab_index; }
    auto set_tab_index(int i) -> Widget & {
        m_tab_index = i;
        return *this;
    }

    /// @brief 当前是否持有焦点。
    [[nodiscard]] auto is_focused() const -> bool { return m_is_focused; }

    /// @brief 经派发期间当前焦点管理器主动请求焦点（无管理器时无效）。
    /// @note 定义见 `src/aurora/widget/widget.cpp`；读取 `current_focus_manager()`，无需持有指针。
    auto request_focus() -> void;

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto type_name() const -> const char * = 0;

    /// @brief 运行时自描述（规格附录 B）：返回本控件完整元数据。
    /// 子类以 static describe_static() 提供编译期可访问版本，此虚函数供运行时多态调用。
    /// 默认实现返回 `{ .name = type_name() }`：无富描述控件（叶/简单容器）可省略 override，
    /// 仅当需要额外 properties/events/children_policy 时才覆写。
    [[nodiscard]] virtual auto describe() const -> WidgetDescriptor;

    /// @brief 序列化自有属性到 props JSON（结构快照/工具链用）。
    /// 子类覆写时应先调用基类默认实现以保留通用属性。
    /// @note Rebuildable: yes, via from_json
    virtual auto serialize_props(Json &props) const -> void {
        props["width"] = length_to_json(m_width);
        props["height"] = length_to_json(m_height);
        props["show"] = show.get();
        props["overflow"] = overflow_strategy_to_json(m_overflow);
    }

    /// @brief 从 props JSON 反序列化自有属性（to_json/from_json 闭环）。
    /// 子类覆写时应先调用基类默认实现以恢复通用属性。
    virtual auto deserialize_props(const Json &props) -> void {
        if (props.contains("width")) {
            m_width = json_to_length(props["width"]);
        }
        if (props.contains("height")) {
            m_height = json_to_length(props["height"]);
        }
        if (props.contains("show")) {
            show.set(props["show"].get<bool>());
        }
        if (props.contains("overflow")) {
            overflow_strategy(json_to_overflow_strategy(props["overflow"]));
        }
    }

    /// @brief 验证当前属性值是否满足约束（debug/strict 模式下 deserialize_props 后自动调用）。
    /// @return 成功返回空 Result，失败返回含 ErrorCode 的 Error。
    /// @note Thread: main-thread only
    /// @note Side-effects: none
    [[nodiscard]] virtual auto validate_props() const -> Result<void> { return Result<void>{}; }

    /// @brief 反序列化时接纳子节点列表（Container 覆写；默认无子节点）。
    virtual auto adopt_children(std::vector<Node> && /*kids*/) -> void {} // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)

    /// @brief 遍历直接子节点（结构快照用；默认无子节点）。
    virtual auto for_each_child(const std::function<void(const Widget &)> & /*fn*/) const -> void {}

    /// @brief 返回直接子节点**视图**（引用，零拷贝；introspection/深度守卫用）。默认空。
    /// Container/Repeater/SingleChild 覆写以暴露真实子节点。
    /// @note 以引用返回（而非副本）：若按值返回 `std::vector<Node>`，临时副本析构会触发
    ///       `Node::~Node` 清空子控件的 `m_layout_parent`（树所有权语义），使遍历后
    ///       `request_frame` 沿父链上溯断链、脏标记无法到达渲染根（历史 bug：grid_rows 滚动失效）。
    /// @warning 返回的引用在树重建（子节点增删）期间可能失效，仅限单帧内只读遍历。
    [[nodiscard]] virtual auto child_nodes() const -> const std::vector<Node> & {
        static constexpr std::vector<Node> empty;
        return empty;
    }

    [[nodiscard]] auto size() const -> Size { return m_size; }

  protected:
    /// @brief 子类实现：在给定约束下返回自身尺寸（可写入子节点 bounds）。
    /// @note Side-effects: mutates layout
    virtual auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size = 0;
    /// @brief 子类实现：在 bounds 内绘制自身内容。
    /// @note Side-effects: paints
    virtual auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void = 0;
    /// @brief 子类实现：命中测试（不含 modifier 拦截，已在外层处理）。
    /// @note Side-effects: pure
    virtual auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
        (void)local;
        (void)bounds;
        (void)ctx;
        return nullptr;
    }
    /// @brief 子类实现：返回本 widget 子树内命中的后代链（不含自身）。
    /// 默认无后代（叶控件）；容器/单子控件覆写以递归下降。命中链由 `hit_test_chain` 组装。
    virtual auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> {
        (void)local;
        (void)bounds;
        (void)ctx;
        return {};
    }
    /// @brief 子类可覆写：挂载时额外逻辑（默认递归挂载在 Container 中处理）。
    virtual auto on_mount(const BuildContext &ctx) -> void { (void)ctx; }

    /// @brief 驱动手势计时（长按阈值检测 + Tooltip 延迟检测）。默认处理本 widget 修饰链中的 LongPress/Tooltip；
    /// 容器类覆写以递归子树。由公开入口 `tick` 委派调用。
    virtual auto tick_gestures(std::chrono::steady_clock::time_point now) -> void {
        modifier.get().tick_long_press(now);
        modifier.get().tick_tooltip(now);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    bool m_needs_gesture_tick = false; ///< 本 widget 修饰链是否含需每帧计时的手势（LongPress/Tooltip 等）
    Size m_size;
    bool m_needs_layout = false;
    bool m_needs_paint = false;
    // NOLINTEND(*-non-private-member-variables-in-classes)

    // ---- why_trace 热路径埋点（AURORA_ENABLE_DEBUG）----
    // 私有 impl：公开 mark_needs_layout / mark_needs_paint 委托至此；传播点显式传 propagated=true，
    // 使 why_trace 能区分「业务/状态直接触发的根因」与「引擎沿父链自动冒泡的传播」。
    auto mark_needs_layout_impl([[maybe_unused]] bool propagated) -> void {
        m_needs_layout = true;
#ifdef AURORA_LAYOUT_CACHE
        m_layout_cache_valid = false;
        if ((m_layout_parent != nullptr) && !is_relayout_boundary()) {
            m_layout_parent->mark_needs_layout_impl(true);
        }
#endif
#ifdef AURORA_DISPLAY_LIST
        invalidate_display_list_up();
#endif
        request_frame(true);
#ifdef AURORA_ENABLE_DEBUG
        debug::detail::record_dirty(debug::DirtyKind::Layout, type_name(), debug::current_debug_frame(), propagated);
#endif
    }
    auto mark_needs_paint_impl([[maybe_unused]] bool propagated) -> void {
        m_needs_paint = true;
#ifdef AURORA_DISPLAY_LIST
        invalidate_display_list_up();
#endif
        request_frame(false);
#ifdef AURORA_ENABLE_DEBUG
        debug::detail::record_dirty(debug::DirtyKind::Paint, type_name(), debug::current_debug_frame(), propagated);
#endif
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    // ---- 布局缓存（AURORA_LAYOUT_CACHE）----
    bool m_layout_cache_valid = false;   ///< 当前 m_cached_size 是否对应 m_cached_constraints
    Constraints m_cached_constraints{};  ///< 上一次成功布局时的输入约束（缓存键）
    Size m_cached_size{};                ///< 上一次成功布局得到的尺寸
    Widget *m_layout_parent = nullptr;   ///< 布局父节点（容器在布局入口设置）
    bool m_is_relayout_boundary = false; ///< 显式重排边界声明（见 is_relayout_boundary）
    bool m_mounted = false;          ///< 是否已挂载（mount 幂等保护，避免转场切换复用同一 widget 实例时重复订阅信号）
    bool m_pressed = false;          ///< 指针是否在本控件上按下（用于识别一次完整点击）
    bool m_hover = false;            ///< 指针是否悬停在本控件上（EventDispatcher 命中链 diff 维护）
    bool m_click_pending = false;    ///< 本次按下后待触发点击（松开且未达长按/拖拽阈值时触发）
    bool m_drag_moved = false;       ///< 本次按下后是否发生超过阈值的移动（用于抑制点击）
    Point m_press_pos{ .x=0.0f, .y=0.0f }; ///< 本次按下的绝对坐标（拖拽位移基准）
    Point m_last_drag_pos{ .x=0.0f, .y=0.0f }; ///< 上次 Move 的绝对坐标（计算拖拽增量）

    Length m_width;                                          ///< 显式宽度意图（默认 WrapContent）
    Length m_height;                                         ///< 显式高度意图（默认 WrapContent）
    OverflowStrategy m_overflow = OverflowStrategy::Visible; ///< 溢出策略（默认 Visible）

    Rect m_focus_bounds; ///< 布局后的全局盒（供方向键焦点导航使用，由布局系统写入）

    /// @brief 最近一次 paint 接收的绝对（窗口逻辑 dp）盒；脏区标记据此标记精确几何，
    ///        使 `Window::present_root` 的脏区裁剪绘制（push_clip）命中正确区域，避免整帧重绘。
    Rect m_paint_bounds{};

#ifdef AURORA_ENABLE_DEBUG
    /// @brief 调试叠层（repaint_highlight）用：本控件最近一次实际重绘（render_into 入口）所在的
    ///        调试帧序号。值为 `aurora::debug::current_debug_frame()`；与当前帧相等即代表本帧重绘。
    std::uint64_t m_debug_paint_frame = 0;
#endif

    // NOLINTEND(*-non-private-member-variables-in-classes)

  public:
    /// @brief 设置布局后的全局盒（由父节点/布局系统写入，供方向键焦点导航）。
    auto set_focus_bounds(Rect r) -> void { m_focus_bounds = r; }
    /// @brief 读取布局后的全局盒。
    [[nodiscard]] auto focus_bounds() const -> Rect { return m_focus_bounds; }

    /// @brief 读取最近一次 paint 的绝对（窗口逻辑 dp）盒（脏区标记用）。
    [[nodiscard]] auto paint_bounds() const -> Rect { return m_paint_bounds; }

#ifdef AURORA_ENABLE_DEBUG
    /// @brief 读取最近一次实际重绘所在的调试帧序号（repaint_highlight 用）。
    ///        Release 构建不暴露此成员（见 `m_debug_paint_frame`）。
    [[nodiscard]] auto debug_paint_frame() const -> std::uint64_t { return m_debug_paint_frame; }
#endif

  protected:
    /// @brief 注册一个响应式信号：变化 → markNeedsLayout/Paint。
    auto track(SignalViewBase &sig, std::vector<std::unique_ptr<Effect>> &effects) -> void {
        auto e = std::make_unique<Effect>([this, &sig]() -> void {
            sig.read(); // 在活跃 Effect 下登记依赖
            mark_needs_layout();
            mark_needs_paint();
        });
        e->run();
        effects.push_back(std::move(e));
    }


    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::unique_ptr<Effect>> m_effects;
    bool m_focusable = true;   ///< 是否可参与焦点序（specification/05-event-navigation.md §4）
    int m_tab_index = 0;       ///< Tab 序权重（越小越靠前）
    bool m_is_focused = false; ///< 当前是否持有焦点
    // NOLINTEND(*-non-private-member-variables-in-classes)
    /// @brief 声明本控件为 relayout boundary（尺寸由约束决定、不依赖子节点）。
    ///        虚拟化列表/滚动容器等应在构造时调用，以截断布局脏向上冒泡、避免整树重排。
    auto set_relayout_boundary(bool v) -> void { m_is_relayout_boundary = v; }

  private:
    /// @brief 绘制内容主体（背景 + on_paint + 边框），供直接绘制与离屏合成复用。
    /// @param p painter
    /// @param visual_box  控件视觉矩形（含 Padding/Border 的完整盒子，用于背景/裁剪/边框/阴影）。
    /// @param content_box 内容矩形（已含 Align/Offset/Padding 平移；用于子节点 on_paint）。
    /// @param ctx context
    virtual auto paint_content(Painter &p, const Rect &visual_box, const Rect &content_box,
                               const BuildContext &ctx) -> void;

    /// @brief 把整棵子树渲染到目标缓冲（局部坐标），供直接绘制与离屏缓存复用。
    auto render_into(Painter &dst, const Rect &local, const BuildContext &ctx) -> void;

    /// @brief 离屏缓存（`Modifier::cache_layer`）状态：缓存位图、尺寸与失效标志。
    mutable std::unique_ptr<Painter> m_paint_cache;
    mutable Size m_paint_cache_size{ .width=0.0f, .height=0.0f };
    mutable bool m_paint_cache_valid = false;

#ifdef AURORA_DISPLAY_LIST
    // ---- Display List 缓存（AURORA_DISPLAY_LIST）----
    DisplayList m_display_list; ///< 本控件子树（含后代）的录制命令缓冲
    bool m_dl_valid = false;    ///< 缓存是否有效（内容未变且 bounds 未变）
    Rect m_last_paint_bounds{}; ///< 上次录制时的绘制全局矩形（bounds 变化须重录）
#endif

  protected:
    /// @brief 请求下一帧重绘（不失效自身/祖先 Display List 缓存）。
    /// 子类在「仅内容平移、无需重栅整树」的场景（如滚动容器平移合成）用它替代
    /// `mark_needs_paint`，以避免 `invalidate_display_list_up` 击穿祖先缓存导致整树重录。
    ///
    /// 传播路径（**结构式**，不依赖任何接线快照）：沿 `m_layout_parent` 链上溯，
    ///   ① 在每个祖先上调用 `on_descendant_dirty`（离屏缓冲宿主据此置内容脏）；
    ///   ② 到达链顶（根控件）后调用其 `on_subtree_dirty`，把脏交给渲染器。
    /// 复杂度 O(depth)（层深上限 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`），与既有
    /// `invalidate_display_list_up` / `mark_needs_layout` 的上溯同量级，不引入新数量级。
    auto request_frame(bool layout = false) -> void {
        const auto *top = this;
        for (Widget *p = m_layout_parent; p != nullptr; p = p->m_layout_parent) {
            p->on_descendant_dirty(*this, layout);
            top = p;
        }
        if (top->on_subtree_dirty) {
            top->on_subtree_dirty(*this, layout);
        }
        if (on_dirty) {
            on_dirty(layout); // 直接挂在本控件上的观察回调（单测/自定义驱动）
        }
        // StrictMode 不变量：已接入树（有布局父）却上溯不到任何汇聚点 ⇒ 布局父链断裂，
        // 本次脏标记被静默丢弃（表现为自驱动动画冻结 / 白屏）。属编程错误，严格模式下硬失败。
        if (strict_mode() == StrictMode::On && m_layout_parent != nullptr && !top->on_subtree_dirty && !on_dirty) {
            AURORA_ASSERT(false, "脏标记未上达渲染根：布局父链断裂——某容器未经 Widget::layout() 入口"
                                 "重排其子节点（或就地重排时未 set_layout_parent），后代标脏将被丢弃");
        }
    }
};

/**
 * @brief 容器基类：持有子节点，统一递归挂载/绘制/命中。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Container : public Widget {
  protected:
    std::vector<Node> m_children; // NOLINT(*-non-private-member-variables-in-classes)

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // child.bounds() 存的是“相对本容器内容区”的局部坐标；
        // 绘制前需叠加本容器内容区全局原点（bounds.origin），转为全局坐标。
#ifdef AURORA_OCCLUSION_CULLING
        const Rect clip = p.clip_bounds(); // 当前有效裁剪（视口/圆角容器等）逻辑 dp 全局坐标
#endif
        for (Node &child : m_children) {
            const Rect cb = child.bounds();
            const auto global{ Rect{ .origin=bounds.origin + cb.origin, .size=cb.size } };
#ifdef AURORA_OCCLUSION_CULLING
            // 遮挡剔除：子控件全局盒与裁剪区无交集则整棵子树跳过（保守外接矩形判定）。
            if (!global.intersects(clip)) {
                continue;
            }
#endif
            child.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 反向遍历：与 on_paint 的绘制顺序一致（后者绘制=视觉顶层），
        // 因此重叠子节点中「视觉在最上层」的控件优先命中，避免底层控件在重叠区
        // 抢走本应属于顶层控件的事件（即「顶层控件被底层控件遮挡」问题）。
        for (auto & child : std::views::reverse(m_children)) {
            const Rect cb = child.bounds();
            // local 处于本容器局部坐标系：子节点位置为 cb（相对本容器内容区）。
            if (cb.contains(local)) {
                // 向下传递子节点“全局”盒（本容器全局原点 + 子相对原点），供更深层级本地化。
                const auto global{ Rect{ .origin=bounds.origin + cb.origin, .size=cb.size } };
                Widget *r = child.widget().hit_test(local - cb.origin, global, ctx);
                if (r != nullptr) {
                    return r;
                }
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        // 同样反向遍历，使重叠时视觉顶层控件成为命中链最深（最后派发）目标。
        for (auto & child : std::views::reverse(m_children)) {
            const Rect cb = child.bounds();
            if (cb.contains(local)) {
                // global.origin 即子节点全局 origin，随命中链带入，供派发器本地化坐标。
                const auto global{ Rect{ .origin=bounds.origin + cb.origin, .size=cb.size } };
                std::vector<HitNode> r = child.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        for (Node &child : m_children) {
            child.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now); // 本节点修饰链（LongPress 等）
        for (Node &child : m_children) {
            child.widget().tick(now);
        }
    }

  public:
    /// @brief 公开 tick 入口（覆写基类 public virtual）：递归子树计时，保持对外可见。
    auto tick(std::chrono::steady_clock::time_point now) -> void override {
        if (m_needs_gesture_tick) {
            // 经虚函数 tick_gestures 分发：默认处理 LongPress/Tooltip；
            // 派生类（如 ToastHost 的过期、VideoPlayer 的播放时钟）可扩展自身每帧逻辑。
            tick_gestures(now);
        }
        for (Node &child : m_children) {
            child.widget().tick(now);
        }
    }
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const Node &child : m_children) {
            fn(child.widget());
        }
    }

    auto adopt_children(std::vector<Node> &&kids) -> void override { m_children = std::move(kids); } // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override { return m_children; }

    /// @brief 默认收集子节点信号（遍历 `m_children`）。
    /// 容器子类若有自身信号，覆写时先 push 自身信号再调用 `Container::collect_signals(out)`。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;

    /// @brief 便捷辅助：从扁平初始化列表接管子节点（供 Column/Row 等便捷构造复用，避免重复代码）。
    auto set_children(std::initializer_list<Node> kids) -> void { m_children.assign(kids.begin(), kids.end()); }

    /// @brief 运行时追加子节点（aurora::ui 工厂层与动态增子复用）。尾插并标脏，下一帧重排。
    /// @note 子控件生命周期由父树 `shared_ptr` 持有，返回/持有的裸指针仅在父树存活期间有效。
    auto add(const Node &child) -> void {
        m_children.push_back(child);
        mark_needs_layout();
    }

    /// @brief 运行时访问第 `i` 个子节点（可变，用于设置 `id` / 替换内容等）。越界抛 `std::out_of_range`。
    [[nodiscard]] auto child(size_t i) -> Node & { return m_children.at(i); }
    /// @brief 子节点数量。
    [[nodiscard]] auto child_count() const -> size_t { return m_children.size(); }

    /// @brief 布局入口（AURORA_LAYOUT_CACHE）：先为所有子节点登记布局父节点，
    ///        再走基类布局（命中缓存时整体跳过子树，依赖父链保证安全）。
    auto layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        for (Node &child : m_children) {
            child.widget().set_layout_parent(this);
        }
        return Widget::layout(c, ctx);
    }
};

/**
 * @brief 叶 widget 基类：无子节点，命中即自身。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class LeafWidget : public Widget {
  protected:
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        (void)ctx;
        // local 是相对本 widget 原点的局部坐标，需与“局部矩形”（原点 0）比较；
        // bounds 含绝对 origin，直接用 bounds.contains(local) 会使非原点控件永远命中失败。
        return Rect{ .origin=Point{ .x=0.0f, .y=0.0f }, .size=bounds.size }.contains(local) ? this : nullptr;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        (void)ctx;
        // 叶控件自身即最深命中：返回 [this]（命中链组装时由基类统一前置自身）。
        // 基类 Widget::hit_test_chain 会检测自身是否已在后代链中，避免重复入链。
        // bounds.origin 即本控件全局 origin，随命中链带入，供派发器本地化坐标。
        // weak_from_this() 返回的 weak_ptr 已被安全拷贝进 HitNode（非悬垂）；
        // 此处抑制 GCC 对该标准库惯用法的已知 -Wdangling-pointer 误报。
        return Rect{ .origin=Point{ .x=0.0f, .y=0.0f }, .size=bounds.size }.contains(local)
                   ? std::vector{ HitNode{ this, weak_from_this(), bounds.origin } }
                   : std::vector<HitNode>{};
    }
#pragma GCC diagnostic pop
};

/**
 * @brief 单子 widget 基类（Provider / 装饰器使用）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class SingleChild : public Widget {
  protected:
    SingleChild() = default;
    explicit SingleChild(Node child) : m_child(std::move(child)) {}

    /// @brief 运行时替换唯一子节点（aurora::ui 工厂层复用）。标脏，下一帧重排。
    auto set_child(Node child) -> void {
        m_child = std::move(child);
        m_child_view_valid = false;
        mark_needs_layout();
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Node m_child;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储；set_child 时置失效）。
    mutable std::vector<Node> m_child_view;
    mutable bool m_child_view_valid = false;
    // NOLINTEND(*-non-private-member-variables-in-classes)

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        m_child.widget().paint(p, bounds, ctx);
    }
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        return m_child.widget().hit_test(local, bounds, ctx);
    }
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        return m_child.widget().hit_test_chain(local, bounds, ctx);
    }
    auto on_mount(const BuildContext &ctx) -> void override { m_child.widget().mount(ctx); }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now); // 本节点修饰链
        m_child.widget().tick(now);
    }

  public:
    /// @brief 公开 tick 入口（覆写基类 public virtual）：递归子控件计时，保持对外可见。
    auto tick(std::chrono::steady_clock::time_point now) -> void override {
        if (m_needs_gesture_tick) {
            // 经虚函数 tick_gestures 分发：默认处理 LongPress/Tooltip；
            // 派生类（如 ToastHost 的过期）可扩展自身每帧逻辑。
            tick_gestures(now);
        }
        m_child.widget().tick(now);
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override { fn(m_child.widget()); }

    /// @brief 单子节点视图（惰性重建缓存：m_child 变化时经 set_child 置失效，避免每次拷贝）。
    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        if (!m_child_view_valid) {
            m_child_view.clear();
            if (m_child) {
                m_child_view.push_back(m_child);
}
            m_child_view_valid = true;
        }
        return m_child_view;
    }

    /// @brief 布局入口（AURORA_LAYOUT_CACHE）：为子节点登记布局父节点，再走基类布局。
    auto layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        m_child.widget().set_layout_parent(this);
        return Widget::layout(c, ctx);
    }
};

} // namespace aurora
