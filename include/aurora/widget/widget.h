#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/a11y_types.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/aurora_assert.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/core/strict_mode.h"
#include "aurora/core/types.h"
#include "aurora/debug/debug_paint.h"
#include "aurora/debug/debug_trace.h"
#include "aurora/event/event.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/state/effect.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/node.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll_viewport.h"

namespace aurora {

/// @brief widget 树默认最大深度（specification/01-core.md §4.4 有界层深度守卫）。超过此深度的递归展开（如
/// Repeater 嵌套 / 极深容器链）经 `Diagnostics` 降级截断，避免栈溢出 / 渲染雪崩。
inline constexpr std::size_t AURORA_DEFAULT_MAX_WIDGET_DEPTH = 64;

class Painter;  // 前向声明（render 模块定义于 render/painter.h）

class Widget;  // 前向声明（HitNode 以 std::weak_ptr<Widget> 作为成员；Widget 在下方定义）

/// @brief 上报**焦点变化**到无障碍事件通道（`AccessibilityEventKind::FocusChanged`）。
///
/// 定义在 `src/aurora/widget/widget.cpp`（与无障碍事件通道实现同处一个 TU，头内只留声明）。
/// @param target 焦点发生变化的控件
/// @note Thread: main-thread only
/// @note Side-effects: invokes accessibility event handler
auto notify_accessibility_focus_changed(const Widget *target) -> void;

/// @brief 上报**结构变化**到无障碍事件通道（`AccessibilityEventKind::StructureChanged`）。
/// 定义位置同 `notify_accessibility_focus_changed`。
/// @param host 子节点发生增删/替换的容器（可为 nullptr：宿主未知时的合法取值，§4.6）
/// @note Thread: main-thread only
/// @note Side-effects: invokes accessibility event handler
auto notify_accessibility_structure_changed(const Widget *host) -> void;

/// @brief 上报**动态播报**（Live Region / Announcement）到无障碍事件通道。
///
/// 与 `Widget::announce(text)` 的区别：本入口不绑定控件（`target` 可空），供 toast /
/// 异步结果等无控件归属的临时文本使用；控件级播报用 `Widget::announce`。
/// 定义位置同 `notify_accessibility_focus_changed`。
/// @param text 待朗读文本（UTF-8；空串不上报）
/// @param target 关联控件（可为 nullptr）
/// @note Thread: main-thread only
/// @note Side-effects: invokes accessibility event handler
auto notify_accessibility_announcement(const std::string &text, const Widget *target) -> void;

/// @brief 命中链节点：携带命中控件及其相对根的全局 origin（用于事件坐标本地化）。
/// 命中链递归下降时，子节点的 `Node::bounds_.origin` 即其全局 origin，直接带入；
/// 派发器（EventDispatcher）在冒泡到某控件前，以 `e.local_position = e.position - origin`
/// 写入本地坐标，控件无需再查询自身在树中的绝对位置。
///
/// 生命周期安全：虚拟列表（LazyList）等会在滚动时回收并销毁子控件，若命中链（悬停链
/// `hover_chain_`、指针捕获 `pointer_capture_`）持有裸指针，回收后即为悬垂指针，下一次
/// `update_hover` / 指针事件派发时解引用即触发 use-after-free（访问违规 0xC0000005）。
/// 因此 `HitNode` 同时保存裸指针 `ptr` 与弱引用 `guard`：`guard` 非空（控件由 `shared_ptr`
/// 持有，如 LazyList 复用的子项）时 `get()` 以 `guard.lock()` 判活，控件被回收后返回
/// `nullptr`、派发器据此安全跳过；`guard` 为空（控件为栈对象 / 成员对象，`weak_from_this()`
/// 返回空弱引用）时其生命周期由持有者保证，`get()` 直接返回 `ptr`。
///
/// 若仅用 `weak_ptr`，栈上构造的控件（测试与大量 demo 的常见写法）`weak_from_this()`
/// 恒为空 → `lock()` 恒失败 → 全部指针事件被静默丢弃。故必须保留裸指针作为可达性来源。
///
/// @note 双轨判活：`guarded` 只能在构造时（此刻控件必然存活）以 `guard.lock()` 是否成功
///       确定，事后无法区分「空弱引用」与「已失效弱引用」。
struct HitNode {
    Widget *ptr = nullptr;  ///< 命中链上的控件（root→target 顺序）
    std::weak_ptr<Widget> guard;  ///< 生命周期守卫；仅当控件由 shared_ptr 持有时有效
    bool guarded = false;  ///< guard 是否关联控制块（区分「空弱引用」与「已失效弱引用」）
    Point origin{};  ///< 该控件相对根的全局 origin

    /// @brief 默认构造：得到全空占位节点（`ptr` 为空、`guard` 为空、`guarded` 为 false、`origin` 为零盒）。
    ///        保留它是为让 `std::vector<HitNode>` 一类容器可默认构造与重置；空节点即「未命中」。
    HitNode() = default;

    /// @brief 从控件与全局 origin 构造命中链节点：自动探测该控件是否由 `shared_ptr` 持有。
    /// @param w 命中控件（非空），派发器据此取方法入口。
    /// @param lifetime_guard 该控件的弱引用守卫，由调用方以 `weak_from_this()` 传入（可空）。
    /// @param global_origin 该控件相对根的全局 origin，用于事件坐标本地化。
    HitNode(Widget *w, std::weak_ptr<Widget> lifetime_guard, const Point &global_origin)
        : ptr(w), guard(std::move(lifetime_guard)), origin(global_origin) {
        // 空弱引用与已失效弱引用的 expired() 都为 true，无法事后区分；
        // 故在构造时（此刻控件必然存活）判定：能 lock 成功即说明由 shared_ptr 持有。
        guarded = guard.lock() != nullptr;
    }

    /// @brief 取存活控件指针；已被回收返回 nullptr。
    ///
    /// 仅用于**不解引用**的用途（如与另一指针比较是否同一控件）。返回值不带生命周期
    /// 保证：`guard.lock()` 产生的临时 `shared_ptr` 在本函数返回时即析构，若控件的最后一个
    /// 强引用就在其中，指针当场悬垂——要调用控件方法请改用 `lock()`。
    /// @return 守卫有效时返回 `guard.lock()` 的裸指针（控件已回收则为 nullptr）；未被
    ///         `shared_ptr` 持有（栈 / 成员对象）时直接返回 `ptr`。
    /// @warning 需要解引用或跨调用持住生命周期时，必须改用 `lock()`。
    [[nodiscard]] auto get() const -> Widget * {
        if (guarded) {
            return guard.lock().get();  // shared_ptr 持有：回收后返回 nullptr
        }
        return ptr;  // 栈/成员对象：生命周期由持有者保证
    }

    /// @brief 取存活控件指针，并把强引用写入 `out_keepalive` 以延长其生命周期至调用方作用域结束。
    ///
    /// 派发回调（`on_pointer_event` → 用户 `on_click`）可能销毁控件自身所在的子树
    /// （典型：点击按钮触发 `NavigatorHost::push_replacement`，重建时丢掉该按钮的最后一个
    /// 强引用），而回调返回后 `Widget::on_pointer_event` 仍要写 `pressed_` 等成员。
    /// 因此凡要解引用命中链节点，都必须在整个调用期间持住强引用。
    /// @param out_keepalive 出参：控件由 `shared_ptr` 持有时写入强引用；栈 / 成员对象时置空。
    /// @return 存活控件指针；`guarded` 且控件已被回收时为 nullptr（调用方据此跳过派发）。
    [[nodiscard]] auto lock(std::shared_ptr<Widget> &out_keepalive) const -> Widget * {
        if (guarded) {
            out_keepalive = guard.lock();
            return out_keepalive.get();  // 回收后为 nullptr
        }
        out_keepalive.reset();
        return ptr;  // 栈/成员对象：生命周期由持有者保证
    }
};

/// @brief widget 抽象基类：声明布局/绘制/命中/挂载接口与通用属性。
///
/// 对应 ARCHITECTURE.md §4.4：具体 widget 继承并实现各 `Impl` 虚函数；`modifier`
/// 修饰链在基类的 layout/paint/hit_test 中统一包裹，避免各 widget 重复 props。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class Widget : public std::enable_shared_from_this<Widget> {
  public:
    /// @brief 虚析构：控件一律以基类指针（`shared_ptr<Widget>`）销毁，故析构函数必须为虚。
    virtual ~Widget() = default;

    /// @brief 默认构造：分配进程级唯一运行时身份（见 `runtime_id()`），脏标记与布局盒取初始值。
    Widget() : runtime_id_(next_runtime_id()) {}

    /// @brief 禁止拷贝构造：拷贝会复制 `runtime_id_`，令两个实例共用同一身份、破坏语义树与平台桥寻址。
    Widget(const Widget &) = delete;
    /// @brief 禁止拷贝赋值（同拷贝构造：共用运行时身份会破坏 diff 键的唯一性前提）。
    /// @return 该重载恒为 `= delete`，任何拷贝赋值尝试止于编译期。
    auto operator=(const Widget &) -> Widget & = delete;
    /// @brief 移动构造保留默认实现（成员众多且含不可移动项）：**移动源与移动目标共享 runtime_id**。
    ///
    /// Widget 通常由 `Node` 以 shared_ptr 就地构造、不做移动，被移动后的源对象随即析构，
    /// 故此共享在实践中不可观测；若将来出现「移动后仍使用源对象」的形态须显式改派 id。
    Widget(Widget &&) = default;
    /// @brief 移动赋值保留默认实现（语义同移动构造：运行时身份随成员一并搬移，不重新分配）。
    /// @return 赋值后的自身引用。
    auto operator=(Widget &&) -> Widget & = default;

    /// @brief 进程级唯一运行时身份（原子自增，自 1 起；0 保留为无效）。
    ///
    /// 用途：语义树节点身份（`AccessibilityNode::id`）与平台桥的 diff 键。
    /// 构造时分配、实例生命周期内恒定；非序列化属性。
    /// @return 构造时分配的运行时身份 id（自 1 起；0 保留为无效）。
    /// @note Side-effects: pure
    [[nodiscard]] auto runtime_id() const noexcept -> std::uint64_t { return runtime_id_; }

    /// @brief 测量：应用 modifier 包裹后调用 layoutImpl。
    /// @param c 父级下发的尺寸约束（固定 / 撑满 / 百分比意图在该轴上被夹成等式）。
    /// @param ctx 构建上下文（主题 / 环境等读取入口）。
    /// @return 本控件的布局尺寸（写入 `size()`；位置由父节点经 `Node::set_bounds` 决定）。
    virtual auto layout(const Constraints &c, const BuildContext &ctx) -> Size;
    /// @brief 绘制：应用 modifier（背景等）后调用 on_paint。
    /// @param p 目标画笔（软件光栅 `Painter`，可为离屏缓冲或 Display List 录制器）。
    /// @param bounds 本控件的绝对（窗口逻辑 dp）盒，入口即记入 `paint_bounds()` / `focus_bounds()`。
    /// @param ctx 构建上下文（基类焦点环从中取主题色）。
    auto paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void;
    /// @brief 使离屏缓存（`Modifier::cache_layer`）失效，下次绘制重新渲染子树。
    auto invalidate_paint_cache() const -> void;
    /// @brief 命中测试：Clickable 拦截后委托 on_hit_test。
    /// @param local 相对本控件原点的局部坐标（先经修饰链平移 / 逆仿射映射）。
    /// @param bounds 本控件的绝对盒（其原点用于向子节点本地化坐标）。
    /// @param ctx 构建上下文。
    /// @return 命中的控件指针；`show == false` 或未命中时为 nullptr。
    auto hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget *;

    /// @brief 命中链：返回根→最深命中的完整 widget 路径（`this` 起算，含自身与所有命中的祖先/后代）。
    /// 用于事件自底向上冒泡派发（specification/05-event-navigation.md §3）：派发器从链尾（最深）向链头（根）逐个调用，
    /// 某节点写 `e.is_handled = true` 即停止。命中即止的 `hit_test` 保留供兼容/纯命中查询。
    /// @param local 相对本控件原点的局部坐标（与 `hit_test` 同一套变换）。
    /// @param bounds 本控件的绝对盒，其原点随命中链带入每个 `HitNode::origin`。
    /// @param ctx 构建上下文。
    /// @return 根→最深命中的 `HitNode` 链；自身与后代均未命中时为空链。
    auto hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx) -> std::vector<HitNode>;

    /// @brief 挂载：注册响应式依赖并递归挂载子树（由 build 后一次性调用）。
    /// @param ctx 构建上下文，透传给 `on_mount` 供子类读取主题 / 环境。
    auto mount(const BuildContext &ctx) -> void;

    /// @brief 收集本 widget 的响应式信号（供基类注册依赖）；子类覆写 push 自身信号。
    /// 默认实现为空：无信号叶控件无需再写空 override（子节点在自身 mount 时自行订阅）。
    /// @param out 输出向量：本控件自身信号的 `SignalViewBase *` 依次 push 至尾部（裸指针，非拥有）。
    virtual auto collect_signals(std::vector<SignalViewBase *> &out) -> void;

    // ---- 通用属性 ----

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    /// @brief 修饰链：本控件的 `Modifier` 节点序列（背景 / 边框 / 裁剪 / 变换 / 手势等）。
    ///
    /// 链上节点在 `layout` / `paint` / `hit_test` 入口按序包裹生效，控件自身无需重复处理；
    /// 作为响应式信号被 `mount` 订阅，链变化即标脏布局与绘制。
    Reactive<Modifier> modifier;  ///< 修饰链
    Reactive<bool> show{true};  ///< 可见性（非结构性隐藏）
    // NOLINTEND(*-non-private-member-variables-in-classes)

    /// @brief 显式宽度意图（specification/01-core.md §2.2 / 需求 SPEC.QUALITY.LAYOUT.ALGEBRA.20）：默认
    /// `auto`（按内容）。 用 `au::px(120)` / `au::fill()` / `au::percent(0.5f)` 等强类型设置；
    /// **裸整数编译失败**（无 `Length(int)` 隐式转换）。
    /// @param len 宽度意图：`Fixed` / `Fraction` / `Expand` 在布局时把该轴夹成等式。
    /// @return 自身引用，供链式继续设置其它属性。
    virtual auto width(Length len) -> Widget & {
        width_ = len;
        mark_needs_layout();  // 尺寸意图变更影响测量结果：标脏布局
        return *this;
    }
    /// @brief 显式高度意图（语义同 `width`）。
    /// @param len 高度意图（强类型 `Length`，裸整数不可编译）。
    /// @return 自身引用，供链式继续设置其它属性。
    virtual auto height(Length len) -> Widget & {
        height_ = len;
        mark_needs_layout();  // 尺寸意图变更影响测量结果：标脏布局
        return *this;
    }
    /// @brief 读取当前宽度意图（设定值，非测量结果）。
    /// @return 宽度 `Length` 的引用（生命周期同本控件）。
    [[nodiscard]] auto width_spec() const -> const Length & { return width_; }
    /// @brief 读取当前高度意图（语义同 `width_spec()`）。
    /// @return 高度 `Length` 的引用（生命周期同本控件）。
    [[nodiscard]] auto height_spec() const -> const Length & { return height_; }

    /// @brief 溢出策略（参考 CSS overflow）：控制子内容超出本控件边界时的行为。
    /// Visible=溢出可见（默认）；Hidden/Clip=裁剪到本控件盒子内（Clip 保留 hit-test，预留语义）；
    /// Scroll=裁剪 + **滚轮滚动**（落地）：内容按滚轮增量垂直平移，经共享 `ScrollViewport`
    /// 内核夹取；滚轮沿命中链路由到最近可滚动祖先（`wants_scroll`），可点击子控件不拦截。
    /// 轻量实现不建离屏缓冲，重内容/长列表请用 Scroll / LazyList。
    /// @param strategy 溢出策略；`Scroll` 同时使控件成为滚轮派发目标（见 `wants_scroll()`）。
    /// @return 自身引用，供链式继续设置其它属性。
    virtual auto overflow_strategy(OverflowStrategy strategy) -> Widget & {
        overflow_ = strategy;
        mark_needs_layout();  // 裁剪盒与内容高变化影响测量：标脏布局
        mark_needs_paint();  // 裁剪 / 平移需重录绘制结果：标脏绘制
        return *this;
    }
    /// @brief 读取当前溢出策略。
    /// @return 最近一次 `overflow_strategy(...)` 设定的策略（默认 `Visible`）。
    [[nodiscard]] auto overflow_strategy() const -> OverflowStrategy { return overflow_; }

    /// @brief 当前滚动偏移（仅 OverflowStrategy::Scroll 有意义；0 = 顶部）。
    /// @return 内容纵向滚动偏移（dp）；取自共享视口 `ScrollViewport::offset_y`，未滚动时为 0。
    [[nodiscard]] auto scroll_offset_y() const -> float { return scroll_viewport_.offset_y; }
    /// @brief 程序化滚动（供测试/无障碍/外部控制器驱动），语义同滚轮：delta_y 正方向为向上滚动。
    ///
    /// 设为虚：真实滚动控件（`Scroll`）需以同名同签名覆写，否则派生版本会**隐藏**本非虚函数
    /// （同名仅返回类型不同不构成重载），静态分析据此报警且经基类引用调用时语义不一致。
    /// @param delta_y 滚动增量，以「滚轮单位」计（经 `ScrollViewport::step` 换算为 dp）。
    /// @return 滚动偏移是否实际变化（到达端点后再滚返回 false）。
    virtual auto scroll_by(float delta_y) -> bool {
        ScrollEvent e;  // 合成的滚轮事件：只填 delta_y，交由 on_scroll 走共享视口内核
        e.delta_y = delta_y;
        const bool changed = [&] {
            const float before = scroll_viewport_.offset_y;
            on_scroll(e);  // 经虚函数派发：派生滚动控件在此改写自身视口
            return scroll_viewport_.offset_y != before;
        }();
        return changed;
    }

    // ---- 脏标记（specification/04-widget.md §2.4）----
    /// @brief 标记本控件需要重新布局，并沿布局父链向上失效传播（业务 / 状态直接触发的根因）。
    auto mark_needs_layout() -> void { mark_needs_layout_impl(false); }

    /// @brief 设置布局父节点，供缓存失效与脏标记向上传播。
    /// @param p 新布局父节点（裸指针、非拥有；nullptr = 脱离父链，本控件的脏标记将不再向上溯达）。
    ///
    /// @note 通常**无需手工调用**：`Widget::layout()` 会以「当前正在布局的控件」自动登记
    ///       （布局调用天然嵌套，见 widget.cpp 的 `t_layout_parent`），使父链由构造保证完整。
    ///       本接口保留给「不经父容器 `layout()` 入口就地重排子树」的特殊场景与历史调用点。
    auto set_layout_parent(Widget *p) -> void { layout_parent_ = p; }

    /// @brief 布局父节点（未接入树时为 nullptr）。
    /// @return 布局父指针（非拥有）；由容器 `layout()` 入口自动登记，`request_frame` / 脏传播沿此链上溯。
    [[nodiscard]] auto layout_parent() const -> Widget * { return layout_parent_; }

    /// @brief 本控件是否可被 Display List 缓存（恒等变换且绘制无副作用、内容不每帧变动）。
    /// @return 是否允许把本控件子树的绘制命令并入祖先 Display List 缓存并回放。
    ///        默认 true；绘制时产生副作用（如 Hero 几何注册）或内容每帧变化（转场淡变）的
    ///        控件须覆盖为 false，否则缓存回放会跳过必要的每帧绘制（见 NavigatorHost/Hero/TransitionLayer）。
    ///        `OverflowStrategy::Scroll` 例外——滚动偏移是每帧可变的绘制输入（同 Scroll
    ///        组件禁自身 DL 缓存的同理），声明滚动的控件不缓存自身 DL。
    [[nodiscard]] virtual auto can_cache_display_list() const -> bool { return overflow_ != OverflowStrategy::Scroll; }

    /// @brief 本控件布局结果是否可被缓存（约束不变 ⇒ on_layout 结果不变、且无非布局副作用）。
    /// @return 是否允许在输入约束未变时复用上次布局得到的尺寸。
    ///        默认 true；与 `can_cache_display_list()` 对称：绘制每帧变动 → 禁 DL 缓存，
    ///        布局非纯函数 → 禁布局缓存。若 `on_layout` 含时间 / 状态依赖的副作用（首屏骨架→真实内容切换、入场动画、
    ///        随外部状态重建子节点等“延期布局”），须覆盖为 false。否则布局缓存（AURORA_ENABLE_LAYOUT_CACHE）
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
    /// @return 是否构成重排边界：显式声明过（`set_relayout_boundary`）或任一轴设了非 WrapContent 尺寸意图即为 true。
    [[nodiscard]] auto is_relayout_boundary() const -> bool {
        return is_relayout_boundary_ || width_.kind != LengthKind::WrapContent ||
               height_.kind != LengthKind::WrapContent;
    }
    /// @brief 读取上次成功布局的输入约束（布局缓存键）；present_root 对脏 boundary 子树
    ///        局部重排时据此从正确的约束入口重排。
    /// @return 上次布局记录的输入约束（引用，生命周期同本控件）；从未成功布局时为默认构造值。
    [[nodiscard]] auto cached_constraints() const -> const Constraints & { return cached_constraints_; }
    /// @brief 标记本控件需要重绘：置绘制脏位、沿布局父链向上失效 Display List 缓存，并请求一次重绘帧。
    ///        实现体委托 `mark_needs_paint_impl(false)`（propagated=false = 业务/状态直接触发）。
    auto mark_needs_paint() -> void { mark_needs_paint_impl(false); }

#ifdef AURORA_ENABLE_DISPLAY_LIST
    /// @brief 使本控件 Display List 失效并沿布局父链向上传播：子树绘制命令被并入祖先 DL，
    ///        故任一后代内容变化时祖先须重录。已在失效链上则短路避免重复递归。
    auto invalidate_display_list_up() -> void {
        if (dl_valid_) {
            dl_valid_ = false;
            if (layout_parent_ != nullptr) {
                layout_parent_->invalidate_display_list_up();
            }
            return;
        }
        // 已失效：可缓存控件此前已向上传播过祖先失效，可短路跳过。
        // 但「内容每帧变化、永不缓存」的控件（can_cache_display_list()==false）dl_valid_ 恒为
        // false，其绘制被并入祖先 DL——若不持续向上失效祖先，祖先缓存会冻结该后代的首帧
        // （如自驱动出场/轮播动画控件永远停在 ent≈0，表现为空白/淡灰）。故对此类控件必须继续上溯。
        if (!can_cache_display_list() && (layout_parent_ != nullptr)) {
            layout_parent_->invalidate_display_list_up();
        }
    }
#endif
    /// @brief 挂在**本控件自身**上的重绘请求回调；bool=true 表示含布局脏（需重排）。
    ///
    /// 供直接持有某控件、需单独观察其标脏的场景（单元测试、自定义驱动）使用，
    /// 不参与树级脏传播；渲染器不再逐控件接线此回调（见 `on_subtree_dirty`）。
    /// @return 回调对象本身（未接线即为空，可直接判空跳过）；调用它返回 void，脏信息由 bool 实参传出。
    std::function<void(bool)> on_dirty;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 子树脏汇聚点：由渲染器（`Window`/`Scene`）安装在**根控件**上，全树仅一处。
    /// 任一后代 `request_frame` 时沿布局父链上溯到根，在此回调一次，
    /// `origin` = 最初标脏的控件（渲染器据其 `paint_bounds()` / `is_relayout_boundary()` 决策）。
    ///
    /// 取代旧的「渲染器每帧递归接线整棵树的 `on_dirty`」：接线本质是树结构的一次**快照**，
    /// 而自驱动控件常在 `on_layout` 中动态新建子控件（骨架→真实内容、轮播 banner、卡片），
    /// 这些新控件不在快照内 → 其 `mark_needs_paint` 无人接收 → 自驱动动画冻结在首帧、
    /// 骨架永不切换（表现为白屏/淡灰）；每帧重接又会让链式包装（`prev` 嵌套）无界增长。
    /// 上溯式传播不存在快照，动态新建的子树天然被覆盖。
    /// @param origin 回调入参：最初标脏的后代控件（渲染器据其 `paint_bounds()` / `is_relayout_boundary()` 决策）。
    /// @param layout 回调入参：本次脏是否含布局脏（需重排）。
    /// @return 回调对象本身：仅被渲染器接线的根控件非空，其余控件为空；调用它返回 void。
    std::function<void(Widget &origin, bool layout)>
        on_subtree_dirty;  // NOLINT(*-non-private-member-variables-in-classes)

    /// @brief 后代标脏通知：`request_frame` 沿布局父链上溯时在**每个祖先**上调用。
    /// 默认空实现。持有离屏内容缓冲的容器（`Scroll`）覆写以置「内容脏」，
    /// 使缓冲仅在后代内容真变化时重录、纯滚动帧只平移合成。
    /// @param origin 最初标脏的后代控件（引用，非拥有）。
    /// @param layout 本次脏是否含布局脏（需重排）。
    virtual auto on_descendant_dirty(Widget &origin, bool layout) -> void {
        (void)origin;
        (void)layout;
    }

    /// @brief 激活（如点击）：事件派发在命中目标上调用；默认无操作。
    virtual auto activate() -> void {}

    /// @brief 悬停态变化通知（由 `EventDispatcher` 在无捕获 Move 的命中链 diff 时调用）。
    /// 默认仅记录 `hover_`（不标脏：否则悬停穿过任意控件都会触发父容器整块重绘）；
    /// 需要 hover 视觉反馈的控件（Checkbox 等）覆写并追加 `mark_needs_paint()`。
    /// @param entered true = 指针进入本控件（命中链 diff 判定）；false = 离开。
    virtual auto on_hover_change(bool entered) -> void { hover_ = entered; }

    /// @brief 指针当前是否悬停在本控件上（命中链内即算，含被子控件覆盖的父容器）。
    /// @return 悬停状态位 `hover_`；由 `EventDispatcher` 在无捕获 Move 时经 `on_hover_change` 维护。
    [[nodiscard]] auto hovered() const -> bool { return hover_; }

    /// @brief 本控件是否作为点击目标消费指针事件（用于点击/长按互斥与冒泡停止）。
    /// 默认：修饰链含 Clickable 修饰即为点击目标；Button 等自带 on_click 的叶控件覆写返回 true。
    /// @return 是否为点击目标：基类按 `modifier.get().has_clickable()` 判定。
    [[nodiscard]] virtual auto wants_click() const -> bool { return modifier.get().has_clickable(); }

    /// @brief 指针事件入口（specification/05-event-navigation.md §3）：在命中目标上调用。
    /// 仅在「先按下、再抬起」且未达长按阈值、未拖拽构成一次完整点击时触发 activate / Clickable 回调；
    /// 悬停移动（Move）不触发点击，但有 `draggable`/`longPress` 修饰时驱动拖拽/长按计时。
    /// `e.is_handled` 仅在本控件自身消费事件（含 Clickable/Draggable/LongPress/ContextMenu 任一手势）时置位，
    /// 否则保持 false 交由派发器沿命中链向上冒泡给父级。
    /// @param e 鼠标事件（就地派发：实现读写其 `action` / `button` / `position` / `pointer_id` / `is_handled` 字段）。
    virtual auto on_pointer_event(MouseEvent &e) -> void {
        const Modifier &mod = modifier.get();
        const bool consumes = wants_click() || mod.has_gesture() || mod.has_context_menu();
        switch (e.action) {
            case MouseAction::Press:
                // 右键按下：打开上下文菜单
                if (e.button == MouseButton::Right && mod.has_context_menu()) {
                    mod.open_context_menu(e.position);
                    e.is_handled = true;
                    return;
                }
                pressed_ = true;
                press_pos_ = e.position;
                last_drag_pos_ = e.position;
                click_pending_ = wants_click();
                drag_moved_ = false;
                if (mod.has_gesture()) {
                    needs_gesture_tick_ = true;  // 开启手势计时，使 tick 递归驱动长按/拖拽阈值
                    mod.invoke_drag_start(e.pointer_id);
                    mod.press_long_press(std::chrono::steady_clock::now(), e.pointer_id);
                }
                e.is_handled = consumes;
                break;
            case MouseAction::Release:
                if (pressed_) {
                    const bool fire_click = wants_click() && click_pending_ && !mod.long_press_fired() && !drag_moved_;
                    if (fire_click) {
                        activate();
                        mod.invoke_click();
                    }
                    if (mod.has_gesture()) {
                        mod.invoke_drag_end(e.pointer_id);
                        mod.cancel_long_press(e.pointer_id);
                    }
                    click_pending_ = false;
                }
                pressed_ = false;
                e.is_handled = consumes;
                break;
            case MouseAction::Move:
                if (pressed_ && mod.has_gesture()) {
                    const Point delta = e.position - last_drag_pos_;
                    if (std::abs(delta.x) > 1.0F || std::abs(delta.y) > 1.0F) {
                        drag_moved_ = true;
                    }
                    mod.invoke_drag(delta, e.position, e.pointer_id);
                    last_drag_pos_ = e.position;
                    e.is_handled = true;  // 拖拽进行中：消费移动，父级不再收到 Move
                }
                break;  // 悬停/移动不直接触发点击
            default:
                break;
        }
    }

    /// @brief 原始多点触摸事件入口：默认仅把完整 `TouchEvent` 交给修饰链（`touch()` / `PinchRecognizer` 等消费），
    ///        不驱动 `Draggable`/`LongPress`/`Clickable`（这些手势由派发器按每个触点合成的 `MouseEvent` 驱动）。
    ///        子类可覆盖以处理多点原始流；务必调用修饰链以免 `touch()` 修饰器失效。
    /// @param e 原始多点触摸事件（就地传递，修饰链可回写其消费状态）。
    virtual auto on_pointer_event(TouchEvent &e) -> void { modifier.get().on_pointer_event(e); }

    /// @brief 由 `Application::tick` 周期性调用的公开入口：驱动手势计时（长按阈值检测）。
    /// 内部委派给受保护虚函数 `tick_gestures`；本 widget 与子树经此统一入口递归计时。
    /// 优化：若本 widget 及子树均无需手势计时（`needs_gesture_tick_ == false`），直接跳过。
    /// @param now 本次计时的时点（steady_clock），供长按阈值 / Tooltip 延迟判定。
    virtual auto tick(std::chrono::steady_clock::time_point now) -> void {
        if (!needs_gesture_tick_) {
            return;
        }
        tick_gestures(now);  // 委派给受保护虚函数：容器类覆写它即可递归子树计时
    }

    /// @brief 键盘事件入口（焦点 widget 上调用）。默认标记为已消费。
    /// @param e 键盘事件；默认实现只置 `e.is_handled = true`（吞键，不产生控件级语义）。
    virtual auto on_key_event(KeyEvent &e) -> void { e.is_handled = true; }

    /// @brief 滚轮事件入口（命中目标上调用）。默认标记为已消费。
    ///
    /// 声明了 `OverflowStrategy::Scroll` 的控件在此获得**轻量滚动**能力——
    /// 经共享 `ScrollViewport` 内核（与 Scroll 组件同一 clamp/符号约定）按滚轮增量
    /// 平移内容绘制（见 `paint_content` 的平移与裁剪），不建离屏缓冲（重内容请用 Scroll）。
    /// 派发路由见 `EventDispatcher::dispatch(ScrollEvent &)`：滚轮沿命中链自最深向根
    /// 找 `wants_scroll()` 者，可点击子控件不拦截滚轮；clamp 吃不尽的余量经
    /// `ScrollEvent::remaining_y` 上冒给更浅可滚动祖先（嵌套滚动协调）。
    /// @param e 滚轮事件：读取 `delta_y`（滚轮单位增量），回写 `is_handled`（恒置 true）与
    ///        未吸收余量 `remaining_y`（仅 `OverflowStrategy::Scroll` 时参与视口夹取）。
    virtual auto on_scroll(ScrollEvent &e) -> void {
        e.is_handled = true;
        if (overflow_ == OverflowStrategy::Scroll) {
            scroll_viewport_.content_h = scroll_content_height();
            scroll_viewport_.viewport_h = size_.height;
            const float before = scroll_viewport_.offset_y;
            if (scroll_viewport_.apply_scroll(e.delta_y)) {
                // 仅内容平移：请求重绘但不失效布局/显示列表缓存（与 Scroll 滚动帧同策略）。
                request_frame(false);
            }
            e.remaining_y =
                ScrollViewport::remaining_offset(before, scroll_viewport_.offset_y, e.delta_y, scroll_viewport_.step);
        }
    }

    /// @brief 本控件是否为「可滚动目标」：滚轮派发沿命中链自最深向根找第一个
    ///       wants_scroll 者派发。默认：声明了 `OverflowStrategy::Scroll` 的控件；
    ///       真实滚动控件（Scroll / LazyList / LazyRow / GridView）覆写为 true，
    ///       保证嵌套时**最深滚动者优先**（外层 Overflow::Scroll 不抢内层滚轮）。
    /// @return 是否为本控件的滚轮目标（默认等价于 `overflow_ == OverflowStrategy::Scroll`）。
    [[nodiscard]] virtual auto wants_scroll() const -> bool { return overflow_ == OverflowStrategy::Scroll; }

    /// @brief 本控件是否为「吸顶头部」：`StickyHeader` 覆写为 true，供滚动宿主（`Scroll`）
    ///        在 blit 合成后以**覆盖层**按 pin 位重绘——内容缓冲不因此逐帧重录
    ///        （sticky 是纯绘制层语义，不改布局盒，spec §6.5）。默认 false。
    /// @return 是否为吸顶头部：基类恒为 false，`StickyHeader` 覆写为 true。
    [[nodiscard]] virtual auto is_sticky_header() const -> bool { return false; }

    /// @brief 激活键（Enter/Space）是否优先投递给 `on_key_event`。
    ///
    /// 派发器对 Enter/Space 的默认处理是直接调用 `activate()`（按钮等「按下即激活」语义），
    /// 控件本身观察不到这两个按键。文本录入类控件需要看到 Enter 本身（提交 / 换行），
    /// 故覆写为 true：派发器先调 `on_key_event`，其消费（`is_handled`）即止；未消费再回落 `activate()`。
    /// 默认 false，保持既有激活语义（按钮 / Checkbox / Switch 等不受影响）。
    /// @return true = Enter/Space 先投递 `on_key_event`；false（默认）= 派发器直接调用 `activate()`。
    [[nodiscard]] virtual auto wants_activation_keys() const -> bool { return false; }

    /// @brief 方向键（↑/↓/←/→）是否优先投递给 `on_key_event`。
    ///
    /// 派发器对方向键的默认处理是**几何焦点导航**（`FocusManager::move_focus(Up/Down/Left/Right)`）：
    /// 有候选即移动焦点并消费，焦点控件观察不到按键。自带方向键语义的复合控件（列表内部光标与
    /// 键盘重排、树展开折叠、分页表格……）需覆写本钩子为 true：派发器先调 `on_key_event`，
    /// 其消费（`is_handled`）即止；**未消费则回落焦点导航**，故控件只需处理自己认识的按键，
    /// 其余按键行为保持不变。与 `wants_activation_keys()` 同一「控件优先、宿主兜底」约定。
    /// 默认 false，保持既有焦点导航语义（按钮 / 复选框 / 滚动容器等不受影响）；
    /// 文本录入控件（`TextInput` / `RichTextEdit`）覆写为 true，否则方向键会被焦点导航吃掉、光标无法移动。
    /// @return true = 方向键先投递 `on_key_event`；false（默认）= 派发器直接做几何焦点导航。
    [[nodiscard]] virtual auto wants_navigation_keys() const -> bool { return false; }

    /// @brief 文本输入入口（焦点 widget 上调用）。默认标记为已消费。
    /// @param e 文本输入事件；默认实现只置 `e.is_handled = true`（不落任何文本）。
    virtual auto on_text_input(TextInputEvent &e) -> void { e.is_handled = true; }

    /// @brief IME 组合输入入口（焦点 widget 上调用）。默认标记为已消费但**不落地任何文本**：
    ///       未接组合语义的控件吞掉事件，避免平台侧因「无人处理」而重复上屏。
    ///       可编辑控件（`TextInput` / `RichTextEdit`）覆写之：先落 `committed`，再更新 preedit 显示态。
    /// @param e IME 组合事件（含 committed 已确认文本与 preedit 组合串）；默认实现仅置 `is_handled = true`、不落文本。
    virtual auto on_text_composition(TextCompositionEvent &e) -> void { e.is_handled = true; }

    /// @brief IME 候选窗定位盒（最近一次绘制的**绝对窗口逻辑 dp** 盒）。
    ///
    /// 平台输入法桥（Win32 IMM32/TSF、macOS `characterRangeForBounds:` 等）需要把候选列表
    /// 摆到插入点旁，而组合期 preedit 尚未进 `value_`，故单列此钩子而非复用无障碍盒。
    /// 默认回退 `focus_bounds_`（整个控件盒）——未覆写的控件候选窗落在控件左上角，仍可用；
    /// 文本录入类控件（`TextInput` / `RichTextEdit`）覆写为 **preedit 光标处** 的零宽竖盒。
    /// 从未绘制过则返回零盒，平台侧按「无有效定位」处理（IMM32 用系统默认位置）。
    /// @return 候选窗定位盒（绝对窗口逻辑 dp）：基类回退 `focus_bounds_`（整个控件盒），从未绘制过时为零盒。
    [[nodiscard]] virtual auto composition_caret_bounds() const -> Rect { return focus_bounds_; }

    /// @brief 操作系统文件拖放落在本控件时触发；消费时置 `e.is_handled` 阻止继续。
    /// 默认不处理（交给命中目标自身）。
    /// @param e 文件拖放事件（携带落点与文件列表）；默认实现不消费、不回写 `is_handled`。
    virtual auto on_file_drop(FileDropEvent &e) -> void { (void)e; }

    /// @brief 焦点变更通知（获焦 focus=true / 失焦 focus=false）。
    /// 基类默认维护 `is_focused_` 以便 `is_focused()` 正确；子类可覆写以更新聚焦态绘制。
    /// 无论是否覆写，进入本实现即代表一次真实焦点转移，故在此统一上抛无障碍事件。
    /// @param focused true = 获得焦点；false = 失去焦点。
    virtual auto on_focus_change(bool focused) -> void {
        is_focused_ = focused;
        mark_needs_paint();  // 焦点态外观（基类统一焦点环，见 wants_focus_ring）随焦点变化
        notify_accessibility_focus_changed(this);  // 上抛焦点事件：三桥下一次投影同步 focused 位
    }

    // ---- 焦点能力（specification/05-event-navigation.md §4）----
    /// @brief 是否可参与焦点序（默认 true）。
    /// 这是**宿主侧的显式开关**：`false` 一票否决，`true` 只是「未否决」——能否成为 Tab 停点
    /// 还取决于 `wants_focus()`（该控件是否真有输入语义）。二者分工见 §4.2。
    /// @return 开关当前值（`focusable()`）；配套 `set_focusable(v)` 返回 `*this` 以续写链式属性。
    [[nodiscard]] auto focusable() const -> bool { return focusable_; }
    /// @brief 设定焦点否决开关（链式）：false 一票否决本控件参与焦点序；true 仅表示「未否决」。
    /// @param v 否决位新值——能否成为 Tab 停点还需结合 wants_focus() 判定（二者分工见 §4.2）。
    /// @return 自身引用 `*this`，支持继续链式设置焦点属性。
    auto set_focusable(bool v) -> Widget & {
        focusable_ = v;
        return *this;
    }

    /// @brief 本控件是否应被键盘焦点序纳入（Tab 停点 / 方向键导航候选的谓词）。
    ///
    /// **分级默认**（specification/05-event-navigation.md §4.2）：基类返回 `true`，即「控件默认可
    /// Tab 到」。自定义控件、第三方控件因此无需任何声明即参与焦点序，也不会因未声明交互谓词而
    /// 静默失去键盘可达性。只有**纯布局容器**（`Container` / `SingleChild` 及其派生的 `Row` /
    /// `Column` / `Stack` / `Show` / `Provider` …）与**纯展示件**（`Text` / `Divider` / `Image` /
    /// `Spacer` …）把本谓词覆写为 `has_input_semantics()`：它们自身不接收输入，只有宿主挂上点击 /
    /// 手势 / 上下文菜单 / 滚动 / 键盘认领时才重新成为停点——谓词是动态的，不按类型一刀切。
    /// 不受本谓词约束的两条路径：`FocusManager::set_focus` / `request_focus` 的**显式聚焦**，
    /// 以及指针 Press 的焦点归属（见 §4.3）——二者仍只看 `focusable()`。
    /// @return 是否应被纳入键盘焦点序（基类恒为 true = 「控件默认可 Tab 到」）。
    [[nodiscard]] virtual auto wants_focus() const -> bool { return true; }

    /// @brief 持焦时是否由基类统一绘制焦点环（默认 true）。
    ///
    /// 基类在 `Widget::paint_content` 末尾为**任何持有焦点**的控件画出主题色环，使 Tab 停点
    /// 在任意控件上都可观测（specification/05-event-navigation.md §4.4）。已自带聚焦态外观的
    /// 控件（`TextInput` 画 Fluent 式主题色加粗边框）覆写为 `false` 以免双环。
    /// 仅影响绘制，不影响焦点序归属与 `focusable()` / `wants_focus()` 判定。
    /// @return 是否在持焦时由基类绘制焦点环：基类恒为 true。
    [[nodiscard]] virtual auto wants_focus_ring() const -> bool { return true; }

    /// @brief Tab 序权重（默认 0，越小越靠前）；move_focus 按此排序。
    /// @return 当前 Tab 序权重。
    [[nodiscard]] auto tab_index() const -> int { return tab_index_; }
    /// @brief 设定 Tab 序权重（链式）：改写 `tab_index()`，焦点导航据此排序停点。
    /// @param i 权重值（可为负，表示早于默认序列；0 = 未声明权重）
    /// @return 自身引用 `*this`，支持继续链式设置焦点属性。
    auto set_tab_index(int i) -> Widget & {
        tab_index_ = i;
        return *this;
    }

    /// @brief 当前是否持有焦点。
    /// @return 焦点状态位 `is_focused_`；由焦点管理器经 `on_focus_change` 维护。
    [[nodiscard]] auto is_focused() const -> bool { return is_focused_; }

    /// @brief 经派发期间当前焦点管理器主动请求焦点（无管理器时无效）。
    /// @note 定义见 `src/aurora/widget/widget.cpp`；读取 `current_focus_manager()`，无需持有指针。
    auto request_focus() -> void;

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 类型名字符串字面量（静态存储期，如 "Button"）：结构快照 `type` 字段与角色推断表的键。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto type_name() const -> const char * = 0;

    /// @brief 运行时自描述（规格附录 B）：返回本控件完整元数据。
    /// 子类以 static describe_static() 提供编译期可访问版本，此虚函数供运行时多态调用。
    /// 默认实现返回 `{ .name = type_name() }`：无富描述控件（叶/简单容器）可省略 override，
    /// 仅当需要额外 properties/events/children_policy 时才覆写。
    /// @return 本控件的元数据描述（基类默认实现只填 `name = type_name()`）。
    [[nodiscard]] virtual auto describe() const -> WidgetDescriptor;

    /// @brief 无障碍可读名称（语义树 `AccessibilityNode::name`）：屏幕阅读器对控件的播报名。
    ///
    /// 默认返回 `explicit_label_`（未显式设置即空串）；需要自带语义的控件覆写返回可读文本
    /// （Button 取 label、Text 取显示文本…）。
    /// 覆写返回值**优先于**任何按角色推断的默认取值，但**低于**宿主经
    /// `set_accessibility_label()` 显式声明的名字（回退链的「显式声明」一级，引用式关联优先级更高；
    /// 见 `a11y_tree.h`）。
    /// @return 播报名（UTF-8）：未显式设置且控件无自带语义时为空串（由语义树按回退链继续推断）。
    /// @note Side-effects: reads state
    [[nodiscard]] virtual auto accessibility_label() const -> std::string { return explicit_label_; }

    /// @brief 宿主显式声明的读屏名（对标 ARIA `aria-label`）；未设置为空串。
    /// @return `explicit_label_` 的引用（生命周期同本控件）；未显式声明时为空串。
    /// @note Side-effects: pure
    [[nodiscard]] auto explicit_accessibility_label() const -> const std::string & { return explicit_label_; }

    /// @brief 声明读屏名：回退链的**最高优先级**来源，空串 = 撤除声明（回落既有回退链）。
    ///
    /// 用途：`Checkbox` / `Switch` / `Slider` 这类无内置文本的叶子控件，其标签在视觉上常是
    /// **兄弟**节点，语义树按几何启发式关联（`sibling_label_name`）只在同行相邻时命中；
    /// 本方法给出与布局无关的确定名字，免去了「为取个名字而子类化覆写钩子」。
    /// 名字变化上报 `NameChanged` 事件 ⇒ 三桥在下一次投影发出 Name 属性变更。
    /// @param label 读屏名（UTF-8，按值接收后搬入 `explicit_label_`）；空串 = 撤除声明、回落回退链。
    /// @return 自身引用 `*this`（同值不重复上报，可直接续写链式设置）。
    /// @note Side-effects: mutates state, notifies accessibility event channel
    auto set_accessibility_label(std::string label) -> Widget & {
        if (explicit_label_ == label) {
            return *this;  // 同值不重复上报（`present_root` 之外的每帧幂等调用亦安全）
        }
        explicit_label_ = std::move(label);
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::NameChanged, .target = this});
        return *this;
    }

    // ---- 稳定键与引用式标签关联（对标 ARIA `id` + `aria-labelledby`）----

    /// @brief 用户可设的**跨重建稳定标识**（对标 HTML `id`）；未设置为空串。
    ///
    /// 与 `runtime_id()` 的分工：后者是进程级自增、构造时分配、**不可序列化**的运行时身份（语义树
    /// 节点 id、三桥元素寻址都用它），页面重启 / `from_json` 重建后即变；本键由宿主命名，随 props
    /// 往返，是「同一棵 saved 树里稳定指认某个控件」的唯一途径。
    /// 当前消费者是 `set_labelled_by()`（引用式标签关联）；同树重名按先序取第一个并在投影路径提示一次。
    /// @return 宿主设定的稳定键（UTF-8）；未设置为空串。
    /// @note Side-effects: pure
    [[nodiscard]] auto stable_key() const -> const std::string & { return stable_key_; }

    /// @brief 设定稳定键（链式）。键内容不解析、不去空白，仅要求非空（空串 = 撤除）。
    /// @param key 稳定键（UTF-8，move 存入 `stable_key_`）；空串 = 撤除声明。
    /// @return 自身引用 `*this`，支持继续链式设置（不触发无障碍事件上报）。
    /// @note Side-effects: mutates state
    auto set_stable_key(std::string key) -> Widget & {
        stable_key_ = std::move(key);
        return *this;
    }

    /// @brief 本控件声明的「名字来源键」（对标 `aria-labelledby`）；未声明为空串。
    /// @return 引用目标控件的 `stable_key`（UTF-8）；未声明为空串。
    /// @note Side-effects: pure
    [[nodiscard]] auto labelled_by_key() const -> const std::string & { return labelled_by_; }

    /// @brief 引用式标签关联：读本控件名字的控件应改读**同树内 `stable_key() == key` 的那个控件**的名字。
    ///
    /// 对标 ARIA `aria-labelledby`——名字是**动态引用**而非一次性拷贝：被引用控件改名后，本控件的
    /// 读屏名在下一次语义树投影时自动跟随（`Name` 变化由既有 `TreeDiff::FieldChange::Name` 发出）。
    /// 求值发生在 `build_accessibility_tree` 的后置遍历里（见 `widget/a11y_tree.h`），三条桥共用同一
    /// 结果，故 UIA / AT-SPI / ARIA 读到的名字一致。
    /// 语义要点：① **优先级最高**（压制 `set_accessibility_label` 与控件自带文案，与 ARIA 一致）；
    /// ② 引用未命中（键不存在 / 子树外 / `show == false` 未入树）⇒ 保留本控件自身名字并降级申报一次，
    /// **不**念空；③ 链式引用（A→B→C）逐层解析，**环**检测后断环保自身名；④ 目标名字为空时同样保留
    /// 自身名。空串 = 撤除引用（回落 Name 回退链）。引用变化上报 `NameChanged`。
    /// @param key 目标控件的 `stable_key()`（仅在同树内查找）；空串 = 撤除引用。
    /// @return 自身引用 `*this`（同值不重复上报）。
    /// @note Side-effects: mutates state, notifies accessibility event channel
    auto set_labelled_by(std::string key) -> Widget & {
        if (labelled_by_ == key) {
            return *this;  // 同值不重复上报（同 `set_accessibility_label` 的幂等纪律）
        }
        labelled_by_ = std::move(key);
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::NameChanged, .target = this});
        return *this;
    }

    /// @brief 无障碍当前值（语义树 `AccessibilityNode::value`）：可变量控件的现值文本。
    ///
    /// 默认空串；可取值的控件覆写返回当前值（TextInput 取内容、Checkbox/Switch 取布尔、
    /// Slider/Progress 取数值…）。语义树构建时每次实时读取，故 reflected 值变化自动同步。
    /// @return 当前值文本（UTF-8）；无可变量语义时为空串。
    /// @note Side-effects: reads state
    [[nodiscard]] virtual auto accessibility_value() const -> std::string { return std::string{}; }

    /// @brief 无障碍用途提示（语义树 `AccessibilityNode::hint`）：补充 name 语义的操作说明。
    ///
    /// 默认空串；宿主可覆写给屏幕阅读器额外的用法描述（如「双击展开」）。不参与布局与绘制。
    /// @return 用途提示文本（UTF-8）：基类恒为空串。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto accessibility_hint() const -> std::string { return std::string{}; }

    // ---- 无障碍语义钩子 ----

    /// @brief 无障碍语义角色：默认走既有 `infer_accessibility_role(type_name())` 推断表（零改动兼容）。
    ///
    /// 控件可覆写声明语义（图表族覆写为 `Image`），宿主自定义控件亦可覆写而不必改推断表。
    /// @return 语义角色：默认由 `infer_accessibility_role(type_name())` 推断表给出。
    /// @note 定义见 `src/aurora/widget/widget.cpp`（需完整 `AccessibilityRole`）。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto accessibility_role() const -> AccessibilityRole;

    /// @brief 无障碍状态位集：基类默认只填 `focused`。
    ///
    /// 派生位 `visible` / `focusable` / `offscreen` 由共享语义层（`core/accessibility.h`
    /// 的 `build_accessibility_node`）统一填，覆写时无需关心。
    /// @return 状态位集：基类仅填 `focused = is_focused_`，其余位交由共享语义层补齐。
    /// @note Side-effects: reads state
    [[nodiscard]] virtual auto accessibility_state() const -> AccessibilityState {
        return AccessibilityState{.focused = is_focused_};
    }

    /// @brief 无障碍取值域：默认无（nullopt）；Slider / ProgressIndicator 覆写。
    /// @return 取值域（最小值 / 最大值 / 当前值）；无可取值语义时为 `nullopt`。
    /// @note Side-effects: reads state
    [[nodiscard]] virtual auto accessibility_range() const -> std::optional<AccessibilityRange> { return std::nullopt; }

    /// @brief 标题层级：`Header` 角色控件的 `aria-level` / UIA level；默认无。
    /// @return 标题层级值：基类非标题控件恒为 `nullopt`。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto accessibility_level() const -> std::optional<int> { return std::nullopt; }

    /// @brief 是否参与语义树（裁剪钩子）：默认 true。
    ///
    /// 纯装饰控件覆写返回 false ⇒ 读屏完全忽略（对标 Flutter `excludeSemantics` /
    ///  Chromium `IsIgnored`）；判定在共享层生效，三桥语义一致。
    /// @return 是否参与语义树：基类恒为 true；覆写 false 后语义树不再为该控件建节点。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto accessibility_is_semantic() const -> bool { return true; }

    /// @brief 无障碍滚动量：默认无（nullopt）；`Scroll` 等滚动容器覆写返回 {min,max,position}。
    /// @return 滚动量三元组（可滚范围与当前位置）；非滚动控件为 `nullopt`。
    /// @note Side-effects: reads state
    [[nodiscard]] virtual auto accessibility_scroll() const -> std::optional<AccessibilityScrollRange> {
        return std::nullopt;
    }

    /// @brief 无障碍滚动定位：把偏移直接设到 `offset`（语义同 `accessibility_scroll()`
    ///        的 position 分量）；非滚动控件默认 no-op。
    ///
    /// 供 UIA `IScrollProvider::SetScrollPercent` / AT-SPI2 `Component.ScrollTo` 这类
    /// 「绝对定位」通道使用——逐次 `ScrollUp` / `ScrollDown` 无法表达百分比跳转。
    /// @param offset 目标偏移（由桥按 min/max 夹取后传入）
    /// @note Side-effects: mutates scroll state
    virtual auto accessibility_scroll_to([[maybe_unused]] double offset) -> void {}

    // ---- 无障碍文本语义钩子（全部默认空实现，仅可编辑文本控件覆写）----

    /// @brief 纯文本全文（UTF-8；不含组合期 preedit——组合中文本属未确定态，读屏读 value 即可）。
    /// @return 控件当前纯文本的只读视图（生命周期至下一次文本变更前）；基类无可读文本语义，恒为空视图。
    [[nodiscard]] virtual auto accessibility_text() const -> std::string_view { return std::string_view{}; }
    /// @brief 当前选区（UTF-8 字节偏移半开区间）；无选区语义时返回 nullopt。
    /// @return 选区半开区间：基类无选区语义，恒为 `nullopt`。
    [[nodiscard]] virtual auto accessibility_selection() const -> std::optional<AccessibilityTextSelection> {
        return std::nullopt;
    }
    /// @brief 设置选区（UTF-8 字节偏移）；无选区语义时 no-op。
    /// @param start 选区起点（UTF-8 字节偏移；基类默认实现不消费该参数）
    /// @param end 选区终点（UTF-8 字节偏移，半开区间右端）
    virtual auto accessibility_set_selection([[maybe_unused]] std::size_t start, [[maybe_unused]] std::size_t end)
        -> void {}
    /// @brief 单字符盒（**窗口本地 DIP**）；无字体度量 / 越界时返回 nullopt（不得崩溃）。
    /// @return 目标字符的包围盒：基类无字体度量，恒为 `nullopt`。
    [[nodiscard]] virtual auto accessibility_char_bounds(std::size_t /*utf8_index*/) const -> std::optional<Rect> {
        return std::nullopt;
    }
    /// @brief 替换文本（编辑动作；UTF-8 字节偏移半开区间）；只读 / 无编辑语义时 no-op。
    /// @param start 被替换区间起点（UTF-8 字节偏移；基类默认实现不消费该参数）
    /// @param end 被替换区间终点（UTF-8 字节偏移，半开区间右端）
    /// @param utf8 替换文本（UTF-8 字节序列，可为空串表示纯删除）
    virtual auto accessibility_replace_text([[maybe_unused]] std::size_t start, [[maybe_unused]] std::size_t end,
                                            [[maybe_unused]] std::string_view utf8) -> void {}

    // ---- 无障碍动作通道 ----

    /// @brief 读屏反向操作入口。
    ///
    /// 默认实现路由到**真实事件路径**：Focus → 焦点管理器；Click/Invoke → 与
    /// `Inspector::simulate_click` 同口径的中心点 press+release 派发（不可命中则失败）。
    /// Toggle / Value / Select 语义强相关，基类不支持，须控件显式覆写。
    /// @param req 动作请求：`action` 动作位 + Value 动作携带的 `number` / `text`（由平台桥翻译而来）。
    /// @return 是否执行；false = 动作不支持（桥转平台侧「不支持」应答）。
    /// @note 定义见 `src/aurora/widget/widget.cpp`（需完整 `AccessibilityActionRequest`）。
    /// @note Side-effects: dispatches events
    virtual auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool;

    /// @brief 动态播报：请求读屏立即朗读本段文本（Live Region）。
    ///
    /// 不经语义树 diff（临时文本没有焦点或取值变化），由各桥直译平台「立即朗读」信号；
    /// 无读屏在线时为空转（事件通道无处理器）。
    /// @param text 待朗读文本（UTF-8）
    /// @note 定义见 `src/aurora/widget/widget.cpp`（需完整 `AccessibilityEvent`）。
    /// @note Side-effects: invokes accessibility event handler
    auto announce(const std::string &text) const -> void;

    /// @brief 控件级默认悬停光标（光标形状 API）。
    ///
    /// 默认空 = 无控件级声明；文本编辑控件覆写返回 `IBeam`、按钮类返回 `PointingHand`。
    /// 解析优先级：修饰链上的 `Modifier::cursor(...)` 声明**优先于**本钩子，本钩子优先于
    /// 「含 Clickable 修饰 → PointingHand」的缺省策略（解析逻辑在 `EventDispatcher` 悬停链）。
    /// @return 控件级声明的光标形状；未声明为 `nullopt`（继续走修饰链与缺省策略）。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto cursor_shape() const -> std::optional<CursorShape> { return std::nullopt; }

    /// @brief 首行文本基线相对**内容盒顶**的距离（`nullopt` = 本控件无基线概念）。
    ///
    /// 语义：内容盒顶 → 首行基线的距离，**含**控件自身内边距与垂直居中偏移（如 Button/TextInput
    /// 的 padding），**不含** `Modifier` 造成的内容盒位移——后者由容器换算到布局盒时补入
    /// （见 `widget/containers.h` 的 `container_baseline`）。
    /// 供 `CrossAxisAlignment::Baseline` 使用（见 specification/03-layout-render.md §3.8）；
    /// 默认 `nullopt`，布局器对无基线子项按 CSS 式合成基线（交叉轴底边）处理。
    /// @param ctx 构建上下文：覆写者据此解析有效字体（`Text` 走 `effective_font(font, ctx)`），
    ///             基类默认实现不使用该参数。
    /// @return 内容盒顶 → 首行基线的距离（dp）；本控件无基线概念时为 `nullopt`。
    /// @warning 返回值**不做整像素 snap**：容器按 `cross_pos = max_above - baseline` 定位后，
    ///          绘制侧 `pen_y = floor(top + ascent + 0.5)` 里的 ascent 与之相消，各子项实绘基线
    ///          像素一致（幅度不超过 1px 的取整差被 floor 吸收）；若在此提前 snap 反而会引入
    ///          0.5dp 偏置。勿"顺手"补 floor。
    /// @note Side-effects: pure
    [[nodiscard]] virtual auto baseline_distance([[maybe_unused]] const BuildContext &ctx) const
        -> std::optional<float> {
        return std::nullopt;
    }

    /// @brief 序列化自有属性到 props JSON（结构快照/工具链用）。
    /// 子类覆写时应先调用基类默认实现以保留通用属性。
    /// @param props 输出用 JSON 对象：基类写入 `width` / `height` / `show` / `overflow`，
    ///        以及非空的 `accessibility_label` / `stable_key` / `labelled_by`（未设不写键）。
    /// @note Rebuildable: yes, via from_json
    virtual auto serialize_props(Json &props) const -> void {
        props.set("width", length_to_json(width_));
        props.set("height", length_to_json(height_));
        props.set("show", Json{show.get()});
        props.set("overflow", overflow_strategy_to_json(overflow_));
        // 未声明不输出：空串即「回落 Name 回退链」，写出空值会让结构快照误读为「名字已清空」。
        if (!explicit_label_.empty()) {
            props.set("accessibility_label", explicit_label_);
        }
        // 同一条「未设不写键」纪律：两键的空串语义都是「未声明」，写出空值会被误读为撤除指令。
        if (!stable_key_.empty()) {
            props.set("stable_key", stable_key_);
        }
        if (!labelled_by_.empty()) {
            props.set("labelled_by", labelled_by_);
        }
    }

    /// @brief 从 props JSON 反序列化自有属性（to_json/from_json 闭环）。
    /// 子类覆写时应先调用基类默认实现以恢复通用属性。
    /// @param props 来源 JSON：仅读取存在的键，缺失键保持当前值不变（与 `serialize_props` 的
    ///        「未设不写键」对称，故缺键读作「未声明」而非「撤除指令」）。
    virtual auto deserialize_props(const Json &props) -> void {
        if (const auto *v = props.at("width"); v != nullptr) {
            width_ = json_to_length(*v);
        }
        if (const auto *v = props.at("height"); v != nullptr) {
            height_ = json_to_length(*v);
        }
        if (const auto *v = props.at("show"); v != nullptr) {
            show.set(v->as_or<bool>(false));
        }
        if (const auto *v = props.at("overflow"); v != nullptr) {
            overflow_strategy(json_to_overflow_strategy(*v));
        }
        if (const auto *v = props.at("accessibility_label"); v != nullptr) {
            set_accessibility_label(v->as_or<std::string>(""));
        }
        if (const auto *v = props.at("stable_key"); v != nullptr) {
            set_stable_key(v->as_or<std::string>(""));
        }
        if (const auto *v = props.at("labelled_by"); v != nullptr) {
            set_labelled_by(v->as_or<std::string>(""));
        }
    }

    /// @brief 验证当前属性值是否满足约束（debug/strict 模式下 deserialize_props 后自动调用）。
    /// @return 成功返回空 Result，失败返回含 ErrorCode 的 Error。
    /// @note Thread: main-thread only
    /// @note Side-effects: none
    [[nodiscard]] virtual auto validate_props() const -> Result<void> { return Result<void>{}; }

    /// @brief 反序列化时接纳子节点列表（Container 覆写；默认无子节点）。
    /// @param kids 待接管的子节点（右值引用；基类无子节点，故不消费也不必 move）。
    virtual auto adopt_children([[maybe_unused]] std::vector<Node> &&kids) -> void {
    }  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)

    /// @brief 遍历直接子节点（结构快照用；默认无子节点）。
    virtual auto for_each_child(const std::function<void(const Widget &)> & /*fn*/) const -> void {}

    /// @brief 返回直接子节点**视图**（引用，零拷贝；introspection/深度守卫用）。默认空。
    /// Container/Repeater/SingleChild 覆写以暴露真实子节点。
    /// @note 以引用返回（而非副本）：若按值返回 `std::vector<Node>`，临时副本析构会触发
    ///       `Node::~Node` 清空子控件的 `layout_parent_`（树所有权语义），使遍历后
    ///       `request_frame` 沿父链上溯断链、脏标记无法到达渲染根（历史 bug：grid_rows 滚动失效）。
    /// @warning 返回的引用在树重建（子节点增删）期间可能失效，仅限单帧内只读遍历。
    /// @return 直接子节点的引用视图；基类返回静态空表（无子节点的控件）。
    [[nodiscard]] virtual auto child_nodes() const -> const std::vector<Node> & {
        // 不用 static constexpr：MSVC STL 的 constexpr 容器仅 _ITERATOR_DEBUG_LEVEL==0 可用，
        // Debug（IDL=2）下报 C2131；static const（magic static）语义等价且跨编译器安全。
        static const std::vector<Node> EMPTY;  // NOLINT
        return EMPTY;
    }

    /// @brief 读取最近一次布局得到的自身尺寸（`Widget::layout` 写回 `size_` 的读数）。
    /// @return 本控件尺寸（dp）：`show == false` 时为零盒，显式宽高意图则严格等于设定值。
    [[nodiscard]] auto size() const -> Size { return size_; }

  protected:
    /// @brief 子类实现：在给定约束下返回自身尺寸（可写入子节点 bounds）。
    /// @param c 父容器下发的尺寸约束（min/max，dp）；本控件的测量必须落在其范围内。
    /// @param ctx 构建上下文：本次布局 pass 的环境读数（主题 / locale 等，见 `build_context.h`）。
    /// @return 本控件在该约束下测得的尺寸（由 `Widget::layout` 写回 `size_`）。
    /// @note Side-effects: mutates layout
    virtual auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size = 0;
    /// @brief 子类实现：在 bounds 内绘制自身内容。
    /// @param p 目标画笔（本帧的离屏 / 直绘 Painter，裁剪状态由其自带）。
    /// @param bounds 本控件内容盒（全局逻辑 dp 坐标，原点即内容左上角）。
    /// @param ctx 构建上下文：本次绘制 pass 的环境读数（主题 / locale 等）。
    /// @note Side-effects: paints
    virtual auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void = 0;
    /// @brief 子类实现：命中测试（不含 modifier 拦截，已在外层处理）。
    /// @param local 待测点（本控件本地坐标，已减去 bounds 原点）。
    /// @param bounds 本控件盒（全局逻辑 dp 坐标），供派生类换算子节点局部坐标。
    /// @param ctx 构建上下文：本次命中测试的环境读数。
    /// @return 命中的后代控件指针；基类默认无人命中 → nullptr（叶控件即「命中自身」由外层判定）。
    /// @note Side-effects: pure
    virtual auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
        (void)local;
        (void)bounds;
        (void)ctx;
        return nullptr;
    }
    /// @brief 子类实现：返回本 widget 子树内命中的后代链（不含自身）。
    /// 默认无后代（叶控件）；容器/单子控件覆写以递归下降。命中链由 `hit_test_chain` 组装。
    /// @param local 待测点（本控件本地坐标，已减去 bounds 原点）。
    /// @param bounds 本控件盒（全局逻辑 dp 坐标），供派生类换算子节点全局 origin。
    /// @param ctx 构建上下文：本次命中链组装 pass 的环境读数。
    /// @return 命中的后代节点链（root→target 顺序）；无命中则为空。
    virtual auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> {
        (void)local;
        (void)bounds;
        (void)ctx;
        return {};
    }
    /// @brief 内容自然高度（OverflowStrategy::Scroll 用）：滚轮夹取上限 = 内容高 − 视口高。
    /// 默认取自身尺寸（叶控件无溢出内容 → 不可滚）；容器覆写为子节点 bounds 的最大 bottom。
    /// @return 内容自然高度（dp）；基类返回自身 `size_.height`。
    [[nodiscard]] virtual auto scroll_content_height() const -> float { return size_.height; }
    /// @brief 子类可覆写：挂载时额外逻辑（默认递归挂载在 Container 中处理）。
    /// @param ctx 挂载上下文；基类默认实现不消费（覆写者用它的 environment / 层级信息）。
    virtual auto on_mount(const BuildContext &ctx) -> void { (void)ctx; }

    /// @brief 驱动手势计时（长按阈值检测 + Tooltip 延迟检测）。默认处理本 widget 修饰链中的 LongPress/Tooltip；
    /// 容器类覆写以递归子树。由公开入口 `tick` 委派调用。
    /// @param now 本帧的单调时钟时刻，用于长按阈值与 Tooltip 延迟的计时基准。
    virtual auto tick_gestures(std::chrono::steady_clock::time_point now) -> void {
        modifier.get().tick_long_press(now);  // 长按阈值计时：修饰链自持按下时刻，到点即触发长按回调
        modifier.get().tick_tooltip(now);  // Tooltip 计时：悬停持续到延迟到点才出提示
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    /// @brief 本 widget 修饰链是否含需每帧计时的手势（LongPress/Tooltip 等）；`tick` 据此短路。
    bool needs_gesture_tick_ = false;
    /// @brief 最近一次布局得到的自身尺寸（`size()` 的存储；`show == false` 时为零盒）。
    Size size_;
    /// @brief 布局脏标记：由 `mark_needs_layout_impl` 置位，供断点/调试指认本控件的重排请求。
    bool needs_layout_ = false;
    /// @brief 绘制脏标记：由 `mark_needs_paint_impl` 置位；帧调度本身不读此位（重绘由 `request_frame` 驱动）。
    bool needs_paint_ = false;
    // NOLINTEND(*-non-private-member-variables-in-classes)

    // ---- why_trace 热路径埋点（AURORA_ENABLE_DEBUG）----
    /// @brief 布局脏标记的实现体：公开入口 `mark_needs_layout` 委托至此。
    ///        置位后沿布局父链上溯失效祖先布局缓存，并请求一次重排帧。
    /// @param propagated 传播点显式传 true，使 `why_trace` 能区分「业务/状态直接触发的根因」
    ///        与「引擎沿父链自动冒泡的传播」；直接调用为 false。
    auto mark_needs_layout_impl([[maybe_unused]] bool propagated) -> void {
        needs_layout_ = true;
#ifdef AURORA_ENABLE_LAYOUT_CACHE
        layout_cache_valid_ = false;
        if ((layout_parent_ != nullptr) && !is_relayout_boundary()) {
            layout_parent_->mark_needs_layout_impl(true);
        }
#endif
#ifdef AURORA_ENABLE_DISPLAY_LIST
        invalidate_display_list_up();
#endif
        request_frame(true);
#ifdef AURORA_ENABLE_DEBUG
        debug::detail::record_dirty(debug::DirtyKind::Layout, type_name(), debug::current_debug_frame(), propagated);
#endif
    }
    /// @brief 绘制脏标记的实现体：公开入口 `mark_needs_paint` 委托至此。
    ///        置位后向上失效 Display List 缓存，并请求一次重绘帧。
    /// @param propagated 传播点显式传 true（沿父链自动冒泡），直接调用为 false；口径同
    ///        `mark_needs_layout_impl`。
    auto mark_needs_paint_impl([[maybe_unused]] bool propagated) -> void {
        needs_paint_ = true;
#ifdef AURORA_ENABLE_DISPLAY_LIST
        invalidate_display_list_up();
#endif
        request_frame(false);
#ifdef AURORA_ENABLE_DEBUG
        debug::detail::record_dirty(debug::DirtyKind::Paint, type_name(), debug::current_debug_frame(), propagated);
#endif
    }

    // ---- 布局缓存（AURORA_ENABLE_LAYOUT_CACHE）----
    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    /// @brief 布局缓存有效位：true 表示 `cached_size_` 对应 `cached_constraints_`，约束未变时可复用。
    bool layout_cache_valid_ = false;
    Constraints cached_constraints_{};  ///< 上一次成功布局时的输入约束（缓存键）
    Size cached_size_{};  ///< 上一次成功布局得到的尺寸
    Widget *layout_parent_ = nullptr;  ///< 布局父节点（容器在布局入口设置）
    bool is_relayout_boundary_ = false;  ///< 显式重排边界声明（见 is_relayout_boundary）
    bool mounted_ = false;  ///< 是否已挂载（mount 幂等保护，避免转场切换复用同一 widget 实例时重复订阅信号）
    bool pressed_ = false;  ///< 指针是否在本控件上按下（用于识别一次完整点击）
    bool hover_ = false;  ///< 指针是否悬停在本控件上（EventDispatcher 命中链 diff 维护）
    bool click_pending_ = false;  ///< 本次按下后待触发点击（松开且未达长按/拖拽阈值时触发）
    bool drag_moved_ = false;  ///< 本次按下后是否发生超过阈值的移动（用于抑制点击）
    Point press_pos_{.x = 0.0F, .y = 0.0F};  ///< 本次按下的绝对坐标（拖拽位移基准）
    Point last_drag_pos_{.x = 0.0F, .y = 0.0F};  ///< 上次 Move 的绝对坐标（计算拖拽增量）

    Length width_;  ///< 显式宽度意图（默认 WrapContent）
    Length height_;  ///< 显式高度意图（默认 WrapContent）
    OverflowStrategy overflow_ = OverflowStrategy::Visible;  ///< 溢出策略（默认 Visible）
    /// @brief 滚动视口内核：OverflowStrategy::Scroll 的 offset/step 状态与夹取数学，
    ///        与 Scroll 组件共享同一约定（见 scroll_viewport.h）。
    ScrollViewport scroll_viewport_{};

    /// @brief 最近一次绘制遍历写入的全局盒，充当方向键焦点导航（`FocusManager::move_focus`）的几何基准。
    ///
    /// 与 `paint_bounds_` 同源同值：二者都在 `Widget::paint` 入口按传入的绝对盒写入。之所以不放在
    /// 布局期，是因为布局只确定自身尺寸、位置由父节点写 `Node::bounds_`（见 `Widget::layout`），
    /// 布局调用链上拿不到控件自身的绝对盒。
    /// @note 离屏缓冲（如 `Scroll` 内容）内的后代处于**内容坐标系**，其盒不等于屏幕坐标——同一视口
    ///       内的相对几何仍成立，跨视口比较不精确（此限制与 `paint_bounds_` 相同）。
    Rect focus_bounds_;

    /// @brief 最近一次 paint 接收的绝对（窗口逻辑 dp）盒；脏区标记据此标记精确几何，
    ///        使 `Window::present_root` 的脏区裁剪绘制（push_clip）命中正确区域，避免整帧重绘。
    Rect paint_bounds_{};

#ifdef AURORA_ENABLE_DEBUG
    /// @brief 调试叠层（repaint_highlight）用：本控件最近一次实际重绘（render_into 入口）所在的
    ///        调试帧序号。值为 `aurora::debug::current_debug_frame()`；与当前帧相等即代表本帧重绘。
    std::uint64_t debug_paint_frame_ = 0;
#endif

    // NOLINTEND(*-non-private-member-variables-in-classes)

  public:
    /// @brief 覆盖焦点导航几何盒（测试 seam）：直接构造、未经绘制遍历的控件用它给出手工盒。
    ///        生产路径由 `Widget::paint` 每次绘制按真实绝对盒写入，调用方无需设置。
    /// @param r 手工指定的焦点盒（绝对窗口逻辑 dp 坐标），直接覆盖 `focus_bounds_`。
    auto set_focus_bounds(const Rect &r) -> void { focus_bounds_ = r; }
    /// @brief 读取焦点导航几何盒（最近一次绘制写入的绝对盒；从未绘制过则为零盒）。
    /// @return `focus_bounds_` 的值拷贝（绝对窗口逻辑 dp 盒）。
    [[nodiscard]] auto focus_bounds() const -> Rect { return focus_bounds_; }

    /// @brief 读取最近一次 paint 的绝对（窗口逻辑 dp）盒（脏区标记用）。
    /// @return `paint_bounds_` 的值拷贝；从未绘制过则为默认零盒。
    [[nodiscard]] auto paint_bounds() const -> Rect { return paint_bounds_; }

#ifdef AURORA_ENABLE_DEBUG
    /// @brief 读取最近一次实际重绘所在的调试帧序号（repaint_highlight 用）。
    ///        Release 构建不暴露此成员（见 `debug_paint_frame_`）。
    /// @return 本控件最近一次进入 `render_into`（真重绘，非 DL 回放）时的 `current_debug_frame()` 值。
    [[nodiscard]] auto debug_paint_frame() const -> std::uint64_t { return debug_paint_frame_; }
#endif

  protected:
    /// @brief 注册一个响应式信号：变化 → markNeedsLayout/Paint。
    /// @param sig 被跟踪的信号视图；Effect 首次 `run()` 时读取以登记依赖，此后其变化即标脏本控件。
    /// @param effects 接收新建 `Effect` 的容器；其生命周期决定订阅生命周期（传 `effects_` 即随本 Widget 析构而退订）。
    auto track(SignalViewBase &sig, std::vector<std::unique_ptr<Effect>> &effects) -> void {
        auto e = std::make_unique<Effect>([this, &sig]() -> void {
            sig.read();  // 在活跃 Effect 下登记依赖
            mark_needs_layout();
            mark_needs_paint();
        });
        e->run();
        effects.push_back(std::move(e));
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::unique_ptr<Effect>> effects_;
    bool focusable_ = true;  ///< 宿主侧焦点否决位（默认未否决）；实际入 Tab 序还需 wants_focus()
    int tab_index_ = 0;  ///< Tab 序权重（越小越靠前）
    bool is_focused_ = false;  ///< 当前是否持有焦点
    /// @brief 宿主显式声明的读屏名（对标 ARIA `aria-label`）：Name 回退链最高优先级，空串 = 未声明。
    std::string explicit_label_;
    /// @brief 用户可设的跨重建稳定标识（对标 HTML `id`），空串 = 未设；见 `stable_key()`。
    std::string stable_key_;
    /// @brief 引用式标签关联的目标键（对标 `aria-labelledby`），空串 = 未声明；见 `set_labelled_by()`。
    std::string labelled_by_;
    // NOLINTEND(*-non-private-member-variables-in-classes)

  private:
    /// @brief 分配下一个运行时身份（进程级原子自增，自 1 起；0 保留为无效）。
    [[nodiscard]] static auto next_runtime_id() -> std::uint64_t {
        static std::atomic<std::uint64_t> counter{0};  // NOLINT
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

  protected:
    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::uint64_t runtime_id_ = 0;  ///< 运行时身份（构造时分配；见 `runtime_id()`）
    // NOLINTEND(*-non-private-member-variables-in-classes)
    /// @brief 声明本控件为 relayout boundary（尺寸由约束决定、不依赖子节点）。
    ///        虚拟化列表/滚动容器等应在构造时调用，以截断布局脏向上冒泡、避免整树重排。
    /// @param v true 把本控件标为 relayout boundary（布局脏不再上溯）；false 取消。
    auto set_relayout_boundary(bool v) -> void { is_relayout_boundary_ = v; }

    /// @brief 「有输入语义」判据：纯布局容器与纯展示件用它实现 `wants_focus()`。
    /// 命中任一即认为该控件需要键盘可达：点击目标（含 `.clickable()` 修饰）、指针手势、
    /// 上下文菜单、滚动视口、或认领方向键 / 激活键。
    /// @return 上述任一输入语义命中时为 true；全部未命中为 false。
    [[nodiscard]] auto has_input_semantics() const -> bool {
        const Modifier &mod = modifier.get();
        return wants_click() || mod.has_gesture() || mod.has_context_menu() || wants_scroll() ||
               wants_navigation_keys() || wants_activation_keys();
    }

  private:
    /// @brief 绘制内容主体（背景 + on_paint + 边框），供直接绘制与离屏合成复用。
    /// @param p painter
    /// @param visual_box  控件视觉矩形（含 Padding/Border 的完整盒子，用于背景/裁剪/边框/阴影）。
    /// @param content_box 内容矩形（已含 Align/Offset/Padding 平移；用于子节点 on_paint）。
    /// @param ctx context
    virtual auto paint_content(Painter &p, const Rect &visual_box, const Rect &content_box, const BuildContext &ctx)
        -> void;

    /// @brief 把整棵子树渲染到目标缓冲（局部坐标），供直接绘制与离屏缓存复用。
    auto render_into(Painter &dst, const Rect &local, const BuildContext &ctx) -> void;

    /// @brief 离屏缓存（`Modifier::cache_layer`）状态：缓存位图、尺寸与失效标志。
    mutable std::unique_ptr<Painter> paint_cache_;
    mutable Size paint_cache_size_{.width = 0.0F, .height = 0.0F};
    mutable bool paint_cache_valid_ = false;
    /// @brief 离屏缓存生成时的光栅状态世代（`render::FontEngine::raster_generation`）。
    ///        世代变更即代表 AA 模式 / 默认字体已变，缓存中的光栅结果过期，必须重绘。
    mutable std::uint64_t paint_cache_raster_gen_ = 0;

    // ---- GPU 层缓存状态（录制模式专用；软件直绘不走层命令）----
    // 语义见 specification/03 §8.7：子树内容脏 / 尺寸变 / 光栅世代变 / 层代际变（消费端
    // 层存储整体丢弃）任一发生即重录 BeginLayer；干净帧仅记一条 DrawLayer（子树零重绘）。
    mutable std::uint64_t gpu_layer_key_ = 0;  ///< 进程内唯一层键（0 = 未分配）
    mutable bool gpu_layer_valid_ = false;  ///< 层纹理是否与子树内容同步
    mutable Size gpu_layer_size_{.width = 0.0F, .height = 0.0F};  ///< 层录制时的子树尺寸
    mutable std::uint64_t gpu_layer_raster_gen_ = 0;  ///< 层录制时的光栅状态世代
    mutable std::uint64_t gpu_layer_epoch_ = 0;  ///< 层录制时的层代际（消费端整体失效信号）

#ifdef AURORA_ENABLE_DISPLAY_LIST
    // ---- Display List 缓存（AURORA_ENABLE_DISPLAY_LIST）----
    DisplayList display_list_;  ///< 本控件子树（含后代）的录制命令缓冲
    bool dl_valid_ = false;  ///< 缓存是否有效（内容未变且 bounds 未变）
    Rect last_paint_bounds_{};  ///< 上次录制时的绘制全局矩形（bounds 变化须重录）
    /// @brief 录制时的光栅状态世代（同 `paint_cache_raster_gen_`）：DL 的 DrawText 命令
    ///        固化了录制时的 AA 模式，世代不匹配即须重录，否则回放旧光栅。
    std::uint64_t dl_raster_gen_ = 0;
#endif

  protected:
    /// @brief 请求下一帧重绘（不失效自身/祖先 Display List 缓存）。
    /// 子类在「仅内容平移、无需重栅整树」的场景（如滚动容器平移合成）用它替代
    /// `mark_needs_paint`，以避免 `invalidate_display_list_up` 击穿祖先缓存导致整树重录。
    ///
    /// 传播路径（**结构式**，不依赖任何接线快照）：沿 `layout_parent_` 链上溯，
    ///   ① 在每个祖先上调用 `on_descendant_dirty`（离屏缓冲宿主据此置内容脏）；
    ///   ② 到达链顶（根控件）后调用其 `on_subtree_dirty`，把脏交给渲染器。
    /// 复杂度 O(depth)（层深上限 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`），与既有
    /// `invalidate_display_list_up` / `mark_needs_layout` 的上溯同量级，不引入新数量级。
    /// @param layout 是否连带标布局脏；透传给 `on_descendant_dirty` / `on_subtree_dirty` / `on_dirty`。
    auto request_frame(bool layout = false) -> void {
        const auto *top = this;
        for (Widget *p = layout_parent_; p != nullptr; p = p->layout_parent_) {
            p->on_descendant_dirty(*this, layout);
            top = p;
        }
        if (top->on_subtree_dirty) {
            top->on_subtree_dirty(*this, layout);
        }
        if (on_dirty) {
            on_dirty(layout);  // 直接挂在本控件上的观察回调（单测/自定义驱动）
        }
        // StrictMode 不变量：已接入树（有布局父）却上溯不到任何汇聚点 ⇒ 布局父链断裂，
        // 本次脏标记被静默丢弃（表现为自驱动动画冻结 / 白屏）。属编程错误，严格模式下硬失败。
        if (strict_mode() == StrictMode::On && layout_parent_ != nullptr && !top->on_subtree_dirty && !on_dirty) {
            AURORA_ASSERT(false,
                          "Dirty flag never reached the render root: the layout parent chain is broken - a container "
                          "relaid out its children without entering Widget::layout() (or relaid out in place without "
                          "set_layout_parent), so descendant dirty marks are dropped");
        }
    }
};

/// @brief 容器基类：持有子节点，统一递归挂载/绘制/命中。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Container : public Widget {
  protected:
    std::vector<Node> children_;  // NOLINT(*-non-private-member-variables-in-classes)

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // child.bounds() 存的是“相对本容器内容区”的局部坐标；
        // 绘制前需叠加本容器内容区全局原点（bounds.origin），转为全局坐标。
#ifdef AURORA_ENABLE_OCCLUSION_CULLING
        const Rect clip = p.clip_bounds();  // 当前有效裁剪（视口/圆角容器等）逻辑 dp 全局坐标
#endif
        for (Node &child : children_) {
            const Rect cb = child.bounds();
            const auto global{Rect{.origin = bounds.origin + cb.origin, .size = cb.size}};
#ifdef AURORA_ENABLE_OCCLUSION_CULLING
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
        for (auto &child : std::views::reverse(children_)) {
            const Rect cb = child.bounds();
            // local 处于本容器局部坐标系：子节点位置为 cb（相对本容器内容区）。
            if (cb.contains(local)) {
                // 向下传递子节点“全局”盒（本容器全局原点 + 子相对原点），供更深层级本地化。
                const auto global{Rect{.origin = bounds.origin + cb.origin, .size = cb.size}};
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
        for (auto &child : std::views::reverse(children_)) {
            const Rect cb = child.bounds();
            if (cb.contains(local)) {
                // global.origin 即子节点全局 origin，随命中链带入，供派发器本地化坐标。
                const auto global{Rect{.origin = bounds.origin + cb.origin, .size = cb.size}};
                std::vector<HitNode> r = child.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        for (Node &child : children_) {
            child.widget().mount(ctx);
        }
    }

    /// @brief 内容自然高度：取子节点 bounds 的最大 bottom（子 bounds 为相对本容器
    ///        内容区的局部坐标，与 on_paint 的定位一致）。溢出滚动夹取上限据此计算。
    /// @return 各子节点 bounds 的最大 bottom（dp）；无子节点时为 0。
    [[nodiscard]] auto scroll_content_height() const -> float override {
        float h = 0.0F;
        for (const Node &child : children_) {
            h = std::max(h, child.bounds().bottom());
        }
        return h;
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);  // 本节点修饰链（LongPress 等）
        for (std::size_t i = 0; i < children_.size();) {
            Widget *cur = &children_[i].widget();
            cur->tick(now);
            // 子项 tick 可能从本容器摘除自身（如 Dismissible 飞出）：当前元素被后继顶替，
            // 不前进、复查同一位置；地址比对保护其他位置的收缩（契约：子项只摘自身）。
            if (i < children_.size() && &children_[i].widget() == cur) {
                ++i;
            }
        }
    }

  public:
    /// @brief 纯布局容器默认不是 Tab 停点：只有挂上点击/手势/菜单/滚动/键盘认领时才可聚焦
    /// （覆写基类 public virtual，保持对外可见；分级默认见 §4.2）。
    /// @return 是否纳入键盘焦点序：仅当容器自身具备输入语义（`has_input_semantics()`）时为 true。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }

    /// @brief 公开 tick 入口（覆写基类 public virtual）：递归子树计时，保持对外可见。
    /// @param now 本次计时的时点（steady_clock），透传给本容器的 `tick_gestures` 与全部子控件。
    auto tick(std::chrono::steady_clock::time_point now) -> void override {
        if (needs_gesture_tick_) {
            // 经虚函数 tick_gestures 分发：默认处理 LongPress/Tooltip；
            // 派生类（如 ToastHost 的过期、VideoPlayer 的播放时钟）可扩展自身每帧逻辑。
            tick_gestures(now);
        }
        for (std::size_t i = 0; i < children_.size();) {
            Widget *cur = &children_[i].widget();
            cur->tick(now);
            // 同 tick_gestures：子项 tick 中摘除自身时不前进，避免迭代器失效 UB。
            if (i < children_.size() && &children_[i].widget() == cur) {
                ++i;
            }
        }
    }
    /// @brief 遍历直接子节点（Container 实现：按 `children_` 顺序逐个回调，即挂载/绘制顺序）。
    /// @param fn 子控件回调（入参为子控件引用，仅在本次调用期间有效）。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const Node &child : children_) {
            fn(child.widget());
        }
    }

    /// @brief 接纳子节点列表（Container 实现：整体搬入 `children_`，替换既有子树）。
    /// @param kids 待接管的子节点；本实现以 `std::move` 消费，故抑制「右值形参未 move」告警。
    auto adopt_children(std::vector<Node> &&kids) -> void override {
        children_ = std::move(kids);
    }  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)

    /// @brief 子节点视图（Container 实现）：直接返回 `children_` 本体，零拷贝。
    /// @return 直接子节点的引用视图；树结构变更（`add` / `remove_child` / `adopt_children`）期间可能失效。
    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override { return children_; }

    /// @brief 默认收集子节点信号（遍历 `children_`）。
    /// 容器子类若有自身信号，覆写时先 push 自身信号再调用 `Container::collect_signals(out)`。
    /// @param out 输出向量：按 `children_` 顺序递归追加容器子树各控件自身信号的 `SignalViewBase *`（裸指针，非拥有）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;

    /// @brief 便捷辅助：从扁平初始化列表接管子节点（供 Column/Row 等便捷构造复用，避免重复代码）。
    /// @param kids 子节点初始化列表：整体赋值覆盖既有 `children_`（非追加）。
    auto set_children(std::initializer_list<Node> kids) -> void { children_.assign(kids.begin(), kids.end()); }

    /// @brief 运行时追加子节点（aurora::ui 工厂层与动态增子复用）。尾插并标脏，下一帧重排。
    /// @param child 待追加的子节点：拷贝入 `children_` 尾部，子控件生命周期自此由父树 `shared_ptr` 接管。
    /// @note 子控件生命周期由父树 `shared_ptr` 持有，返回/持有的裸指针仅在父树存活期间有效。
    auto add(const Node &child) -> void {
        children_.push_back(child);  // 尾插：节点的持有权随子控件留在本容器
        mark_needs_layout();  // 子树多了一个子节点：标脏布局，下一帧重排纳入测量
        notify_accessibility_structure_changed(this);  // 结构变化上报：三桥下一次投影重建语义树
    }

    /// @brief 按控件地址移除子节点（如 Dismissible 飞出后自摘；Node 随之析构释放）。
    /// @param w 待移除的子控件地址（与 `children_` 中节点的 `widget()` 地址比对；只读，不用于解引用）。
    /// @return 是否找到并移除。移除后标记重排（树结构变化对外可见，可后续 observe）。
    auto remove_child(const Widget *w) -> bool {
        const auto it = std::ranges::find_if(children_, [w](const Node &n) { return &n.widget() == w; });
        if (it == children_.end()) {
            return false;
        }
        children_.erase(it);
        mark_needs_layout();
        return true;
    }

    /// @brief 运行时访问第 `i` 个子节点（可变，用于设置 `id` / 替换内容等）。越界抛 `std::out_of_range`。
    /// @param i 子节点下标（0 ≤ i < `child_count()`）。
    /// @return 第 i 个 `Node` 的可变引用（生命周期至子树增删前）。
    [[nodiscard]] auto child(size_t i) -> Node & { return children_.at(i); }
    /// @brief 子节点数量。
    /// @return `children_` 中直接子节点的个数。
    [[nodiscard]] auto child_count() const -> size_t { return children_.size(); }

    /// @brief 布局入口（AURORA_ENABLE_LAYOUT_CACHE）：先为所有子节点登记布局父节点，
    ///        再走基类布局（命中缓存时整体跳过子树，依赖父链保证安全）。
    /// @param c 父级下发的尺寸约束（透传给 `Widget::layout`）。
    /// @param ctx 构建上下文（透传给 `Widget::layout`）。
    /// @return 本容器测得的布局尺寸（缓存命中时为上次记录的 `cached_size_`）。
    auto layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        for (Node &child : children_) {
            child.widget().set_layout_parent(this);
        }
        return Widget::layout(c, ctx);
    }
};

/// @brief 叶 widget 基类：无子节点，命中即自身。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class LeafWidget : public Widget {
  protected:
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        (void)ctx;
        // local 是相对本 widget 原点的局部坐标，需与“局部矩形”（原点 0）比较；
        // bounds 含绝对 origin，直接用 bounds.contains(local) 会使非原点控件永远命中失败。
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

// clang 无 "-Wdangling-pointer" 告警组（实测报 -Wunknown-warning-option），故压制只在 GCC 下展开。
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        (void)ctx;
        // 叶控件自身即最深命中：返回 [this]（命中链组装时由基类统一前置自身）。
        // 基类 Widget::hit_test_chain 会检测自身是否已在后代链中，避免重复入链。
        // bounds.origin 即本控件全局 origin，随命中链带入，供派发器本地化坐标。
        // weak_from_this() 返回的 weak_ptr 已被安全拷贝进 HitNode（非悬垂）；
        // 此处抑制 GCC 对该标准库惯用法的已知 -Wdangling-pointer 误报。
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)
                   ? std::vector{HitNode{this, weak_from_this(), bounds.origin}}
                   : std::vector<HitNode>{};
    }
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic pop
#endif
};

/// @brief 单子 widget 基类（Provider / 装饰器使用）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class SingleChild : public Widget {
  protected:
    SingleChild() = default;
    explicit SingleChild(Node child) : child_(std::move(child)) {}

    /// @brief 运行时替换唯一子节点（aurora::ui 工厂层复用）。标脏，下一帧重排。
    /// @param child 新的子节点；移入 `child_`，旧子节点随之释放。
    auto set_child(Node child) -> void {
        child_ = std::move(child);
        child_view_valid_ = false;
        mark_needs_layout();
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Node child_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储；set_child 时置失效）。
    mutable std::vector<Node> child_view_;
    mutable bool child_view_valid_ = false;
    // NOLINTEND(*-non-private-member-variables-in-classes)

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().paint(p, bounds, ctx);
        }
    }
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        return child_ ? child_.widget().hit_test(local, bounds, ctx) : nullptr;
    }
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        return child_ ? child_.widget().hit_test_chain(local, bounds, ctx) : std::vector<HitNode>{};
    }
    auto on_mount(const BuildContext &ctx) -> void override {
        if (child_) {
            child_.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);  // 本节点修饰链
        if (child_) {
            child_.widget().tick(now);
        }
    }

  public:
    /// @brief 单子节点包装件（`Show`/`Provider`/`Lifecycle`/`Badge` …）默认不是 Tab 停点：
    /// 它们是透传壳，只有自身挂上输入语义（如 `ExpansionPanel` 的点击展开）才可聚焦
    /// （覆写基类 public virtual，保持对外可见；分级默认见 §4.2）。
    /// @return 是否纳入键盘焦点序：仅当包装壳自身具备输入语义（`has_input_semantics()`）时为 true。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }

    /// @brief 公开 tick 入口（覆写基类 public virtual）：递归子控件计时，保持对外可见。
    /// @param now 本次计时的时点（steady_clock），透传给 `tick_gestures` 与子控件 `tick()`。
    auto tick(std::chrono::steady_clock::time_point now) -> void override {
        if (needs_gesture_tick_) {
            // 经虚函数 tick_gestures 分发：默认处理 LongPress/Tooltip；
            // 派生类（如 ToastHost 的过期）可扩展自身每帧逻辑。
            tick_gestures(now);
        }
        if (child_) {
            child_.widget().tick(now);
        }
    }

    /// @brief 遍历直接子节点（SingleChild 实现）：`child_` 非空时对其回调一次，空壳不调用。
    /// @param fn 子控件回调（入参为子控件引用，仅在本次调用期间有效）。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (child_) {
            fn(child_.widget());
        }
    }

    /// @brief 单子节点视图（惰性重建缓存：child_ 变化时经 set_child 置失效，避免每次拷贝）。
    /// @return 单子节点视图 `child_view_` 的引用（零拷贝）：`child_` 为空时为空表。
    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        if (!child_view_valid_) {
            child_view_.clear();
            if (child_) {
                child_view_.push_back(child_);
            }
            child_view_valid_ = true;
        }
        return child_view_;
    }

    /// @brief 布局入口（AURORA_ENABLE_LAYOUT_CACHE）：为子节点登记布局父节点，再走基类布局。
    /// 空子节点（默认构造的 SingleChild）为合法状态：跳过登记，基类 on_layout 由派生类自守。
    /// @param c 父级下发的尺寸约束（透传给 `Widget::layout`）。
    /// @param ctx 构建上下文（透传给 `Widget::layout`）。
    /// @return 本控件测得的布局尺寸（缓存命中时为上次记录的 `cached_size_`）。
    auto layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (child_) {
            child_.widget().set_layout_parent(this);
        }
        return Widget::layout(c, ctx);
    }
};

}  // namespace aurora
