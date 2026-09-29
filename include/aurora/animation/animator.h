#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "aurora/animation/timeline.h"
#include "aurora/state/state.h"

namespace aurora {

/// @brief 动画状态（对应 Flutter `AnimationStatus`）。
enum class AnimationStatus : std::uint8_t {
    Dismissed,  ///< 在起点（进度 0）
    Forward,  ///< 正向播放中
    Reverse,  ///< 反向播放中
    Completed,  ///< 在终点（进度 1）
};

/// @brief 时间驱动：在 duration 内把线性进度 0→1（或反向）推进（specification/05-event-navigation.md §6.1）。
///
/// 纯时间线，**不含曲线**——曲线在 `Tween`/`Keyframes` 上应用；控制器只输出原始
/// 归一化进度 t∈[0,1]。每帧由 `Animator::tick` 调用 `tick(dtSeconds)` 推进。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class AnimationController {
  public:
    /// @brief 构造：duration（秒，非正值夹取为 1e-6）与初始进度 value。
    /// 初值构造即推断状态：value ≥ 1.0 报 Completed；其余（含 (0,1) 中间值）报 Dismissed
    /// （中间值时进度冻结在初值、状态语义为「未开始」，后续由 forward/reverse 推进）。
    /// @param duration_seconds 播放时长（秒，非正值夹取为 1e-6）。
    /// @param value 初始归一化进度（默认 0；≥1.0 时初态即 Completed）。
    explicit AnimationController(double duration_seconds, double value = 0.0)
        : duration_(std::max(duration_seconds, 1e-6)), value_(value) {
        if (value_ >= 1.0) {
            status_ = AnimationStatus::Completed;
        }
    }

    /// @brief 当前归一化进度。
    /// @return 进度∈[0,1]（构造初值越界时原样返回，播放中由 tick 钳制到端点）。
    [[nodiscard]] auto value() const -> double { return value_; }

    /// @brief 播放状态。
    /// @return 当前 AnimationStatus（Dismissed/Forward/Reverse/Completed）。
    [[nodiscard]] auto status() const -> AnimationStatus { return status_; }

    /// @brief 播放时长。
    /// @return 构造传入的秒数（非正值已夹取为 1e-6）。
    [[nodiscard]] auto duration() const -> double { return duration_; }

    /// @brief 是否处于起点语义（静止或复位到 0）。
    /// @return 状态为 Dismissed 时为 true。
    [[nodiscard]] auto is_dismissed() const -> bool { return status_ == AnimationStatus::Dismissed; }

    /// @brief 是否到达终点语义（播放完毕或 stop 于后半程）。
    /// @return 状态为 Completed 时为 true。
    [[nodiscard]] auto is_completed() const -> bool { return status_ == AnimationStatus::Completed; }

    /// @brief 是否正在播放。
    /// @return 状态为 Forward 或 Reverse 时为 true。
    [[nodiscard]] auto is_animating() const -> bool {
        return status_ == AnimationStatus::Forward || status_ == AnimationStatus::Reverse;
    }

    /// @brief 本帧 value 是否发生变化（供 Animator 决定是否需要写目标 State）。
    /// @return tick 推进后本帧进度有变化时为 true（调用 clear_dirty 后复位）。
    [[nodiscard]] auto dirty() const -> bool { return dirty_; }

    /// @brief 清除脏标记（Animator::tick 在应用全部绑定后统一调用）。
    auto clear_dirty() -> void { dirty_ = false; }

    /// @brief 正向播放（可选从 from 起步）。
    /// @param from 起步进度，夹入 [0,1]；-1（默认）为哨兵 = 从当前进度续播。
    auto forward(double from = -1.0) -> void;

    /// @brief 反向播放。
    auto reverse() -> void;

    /// @brief 复位到 v（不播放，静止）。
    /// @param v 复位到的进度，夹入 [0,1]（默认 0）；≥1.0 时报 Completed，否则报 Dismissed。
    auto reset(double v = 0.0) -> void;

    /// @brief 停止：保持当前进度，静止于最近端点语义。
    auto stop() -> void;

    /// @brief 推进 dt 秒；到达端点时钳制并置对应终态。
    /// @param dt_seconds 本帧时间步长（秒）；reduce_motion 生效时直接落在本次播放的目标端点。
    auto tick(double dt_seconds) -> void;

  private:
    double duration_ = 0.0;
    double value_ = 0.0;
    AnimationStatus status_ = AnimationStatus::Dismissed;
    bool dirty_ = false;
};

/// @brief 帧动画管理器（specification/05-event-navigation.md §6.2 `Animator`）。
///
/// 持有所有 `AnimationController`，在每帧 `tick(dt)` 中推进它们，并把绑定到
/// `State<T>` 的补间结果写入目标（`State::set` → 触发信号定点刷新，动画不另起通道）。
/// 仅当控制器本帧进度变化时才写 State，避免空闲帧的冗余刷新。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class Animator {
  public:
    /// @brief 登记控制器（非拥有，指针）；tick 时推进。
    /// @warning 登记的是裸指针。若控制器的生命周期短于本 `Animator`（典型：作为某个
    ///          widget 的成员），**必须**在其析构前调用 `remove(c)`，否则下一帧
    ///          `tick` 会写入已释放内存。
    /// @param c 待登记的控制器（裸指针，生命周期契约见 @warning）。
    auto drive(AnimationController &c) -> void { controllers_.push_back(&c); }

    /// @brief 注销控制器及其经 `bind()` 建立的所有绑定。
    /// 与 `drive`/`bind` 配对使用：控制器或绑定目标 `State<T>` 先于 `Animator` 析构时
    /// （如 `NavigatorHost` 这类持有自身控制器、又把它注册进 `Application` 全局
    /// `Animator` 的 widget），须在析构函数中调用本函数摘除登记，避免帧循环
    /// 解引用悬垂指针（use-after-free）。
    /// @param c 待注销的控制器；未登记过时为无操作。
    auto remove(const AnimationController &c) -> void {
        std::erase(controllers_, &c);
        std::erase_if(on_tick_, [&c](const Binding &b) -> bool { return b.owner == &c; });
    }

    /// @brief 当前运行中的应用级动画管理器（由 `Application::run()` 起止设置）。
    ///        组件级动画（如图表 grow-in）在 `on_mount` 时取用；无运行中 App 时返回 nullptr
    ///        ——调用方须据此**降级到终态**，无头渲染（golden）才有确定性输出。
    /// @return 运行中实例指针；无运行中 App 时为 nullptr。
    [[nodiscard]] static auto current() -> Animator * { return current_; }

    /// @brief 设置/清除当前运行实例（由 `Application::run()` 内部调用；与 `Scheduler::set_current` 同范式）。
    /// @param a 运行实例指针；传 nullptr 表示清除。
    static auto set_current(Animator *a) -> void { current_ = a; }

    /// @brief 是否有运行中的控制器（Forward/Reverse），供帧调度决策取值，无活跃动画时 idle 帧可阻塞等待事件。
    /// @return 任一登记控制器处于播放态时为 true。
    [[nodiscard]] auto has_active() const -> bool {
        return std::ranges::any_of(
            controllers_, [](const AnimationController *c) -> bool { return c != nullptr && c->is_animating(); });
    }

    /// @brief 推进所有控制器并应用所有绑定（在帧边界调用一次）。
    /// @param dt_seconds 帧时间步长（秒）。
    auto tick(double dt_seconds) const -> void;

    /// @brief 追加一帧回调（在 tick 中于所有控制器推进后、clear_dirty 前执行）。
    /// @param fn 每帧执行一次的回调；不归属任何控制器，不随 `remove` 摘除。
    auto add_binding(std::function<void()> fn) -> void {
        on_tick_.push_back(Binding{.owner = nullptr, .fn = std::move(fn)});
    }

    /// @brief 绑定 (控制器 + 补间) → 目标 State：每帧把插值写入 State。
    /// `tw` 按值收参：调用方普遍传临时量（`Tween<double>{0, 1, curve}`），且闭包必须持有副本
    /// 才能活到后续帧。初始化捕获把形制直接搬进闭包，省掉「形参拷贝 + 捕获拷贝」中的第二次。
    /// @tparam T 目标 State 的值类型（须可被 Tween<T> 插值）。
    /// @param c 驱动控制器（登记后由本 Animator 每帧推进）。
    /// @param tw 补间（按值持有副本）。
    /// @param target 写入目标的 State（非拥有引用，须比本 Animator 存活更久）。
    template <typename T>
    auto bind(AnimationController &c, Tween<T> tw, State<T> &target) -> void {
        drive(c);
        on_tick_.push_back(Binding{&c, [&c, tw = std::move(tw), &target]() -> auto {
                                       if (c.dirty()) {
                                           target.set(tw.value(c.value()));
                                       }
                                   }});
    }

    /// @brief 绑定 (控制器 + 关键帧) → 目标 State。`kf` 的按值收参与初始化捕获同上。
    /// @tparam T 目标 State 的值类型（须可被 Keyframes<T> 插值）。
    /// @param c 驱动控制器（登记后由本 Animator 每帧推进）。
    /// @param kf 关键帧序列（按值持有副本）。
    /// @param target 写入目标的 State（非拥有引用，须比本 Animator 存活更久）。
    template <typename T>
    auto bind(AnimationController &c, Keyframes<T> kf, State<T> &target) -> void {
        drive(c);
        on_tick_.push_back(Binding{&c, [&c, kf = std::move(kf), &target]() -> auto {
                                       if (c.dirty()) {
                                           target.set(kf.value(c.value()));
                                       }
                                   }});
    }

  private:
    /// 一帧回调 + 其归属控制器（`add_binding` 的裸回调归属为空，不参与 `remove`）。
    struct Binding {
        const AnimationController *owner = nullptr;  ///< 非拥有；nullptr = add_binding 的裸回调（不参与 remove）
        std::function<void()> fn;  ///< 每帧执行一次的回调（可空）
    };

    std::vector<AnimationController *> controllers_;  ///< 非拥有
    std::vector<Binding> on_tick_;  ///< 帧回调表：tick 中在推进控制器之后、清脏之前按登记序执行

    /// @brief 当前运行实例槽位（由 `Application::run()` 起止设置；无运行时为 nullptr）。
    static Animator *current_;  // NOLINT
};

/// @brief 便捷封装：把 `State<T>` + `Tween<T>` + `AnimationController` 收拢一处，
/// 通过 `attach(animator)` 一键接入帧循环（对应 Flutter `AnimatedBuilder` 的驱动部分）。
///
/// 内部以 `shared_ptr` 持有驱动载荷（控制器 + 补间 + 目标 + completed 回调），因此本类型
/// 可自由拷贝/移动，**且 `attach` 后即使原句柄离开作用域，帧循环仍安全持有驱动载荷（不悬垂）**——
/// 这正是 `animate()` 能按值返回句柄、又能在 Animator 帧循环中长期驱动的前提。
///
/// @tparam T 被动画化的值类型，与目标 `State<T>` 和补间 `Tween<T>` 一致。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class AnimatedValue {
  public:
    /// @brief 驱动载荷：跨句柄拷贝共享的控制器/补间/目标集合。
    struct Payload {
        State<T> *target;  ///< 非拥有：目标 State 必须比本动画存活更久
        AnimationController controller;  ///< 拥有的驱动控制器
        Tween<T> tween;  ///< 拥有的补间（含曲线）
        std::function<void()> on_completed;  ///< 到达终点的一次性回调（可空）
        bool fired_completed = false;  ///< on_completed 是否已触发（保证只发一次）
    };

    /// @brief 以目标 State、补间与时长构造（控制器初值 0，未起步）。
    /// @param target 写入目标的 State（非拥有引用，须比本动画存活更久）。
    /// @param tw 补间（含曲线；按值持有副本）。
    /// @param duration_seconds 播放时长（秒，非正值夹取为 1e-6）。
    AnimatedValue(State<T> &target, Tween<T> tw, double duration_seconds)
        : m_(std::make_shared<Payload>(Payload{.target = &target,
                                               .controller = AnimationController{duration_seconds},
                                               .tween = std::move(tw),
                                               .on_completed = {},
                                               .fired_completed = false})) {}

    /// @brief 驱动控制器访问（供直接读状态/进度）。
    /// @return 内部拥有的 AnimationController 引用。
    [[nodiscard]] auto controller() -> AnimationController & { return m_->controller; }

    /// @brief 补间访问（供运行期改端点/曲线）。
    /// @return 内部拥有的 Tween<T> 引用。
    [[nodiscard]] auto tween() -> Tween<T> & { return m_->tween; }

    /// @brief 开始正向播放（可选从 from∈[0,1] 起步）。
    /// @param from 起步进度，夹入 [0,1]；-1（默认）= 从当前进度续播。
    auto forward(double from = -1.0) -> void { m_->controller.forward(from); }

    /// @brief 当前归一化进度 t∈[0,1]（控制器原始输出，未经曲线）。
    /// @return 控制器进度。
    [[nodiscard]] auto progress() const -> double { return m_->controller.value(); }

    /// @brief 目标 State 当前值（补间插值后的落点）。
    /// @return State::get() 的最新值（未播时即 State 原值）。
    [[nodiscard]] auto current() const -> T { return m_->target->get(); }

    /// @brief 播放状态。
    /// @return 控制器的 AnimationStatus。
    [[nodiscard]] auto status() const -> AnimationStatus { return m_->controller.status(); }

    /// @brief 是否已到达终点。
    /// @return 控制器状态为 Completed 时为 true。
    [[nodiscard]] auto is_completed() const -> bool { return m_->controller.is_completed(); }

    /// @brief 注册"到达终点（Completed）"的一次性回调（重复到达不会重复触发）。
    /// @param cb 回调（覆写式登记；传空 std::function 即清除）。
    auto on_completed(std::function<void()> cb) -> void { m_->on_completed = std::move(cb); }

    /// @brief 把本动画接入 Animator 的帧循环（驱动与写回委托给 Animator，句柄可离开作用域）。
    /// @param a 目标 Animator（其帧循环持载荷 shared_ptr 副本）。
    auto attach(Animator &a) -> void {
        a.drive(m_->controller);
        auto p = m_;
        a.add_binding([p]() -> void {
            if (p->controller.dirty()) {
                p->target->set(p->tween.value(p->controller.value()));
                if (p->controller.is_completed() && !p->fired_completed) {
                    p->fired_completed = true;
                    if (p->on_completed) {
                        p->on_completed();
                    }
                }
            }
        });
    }

    /// @brief 自驱动一帧（无 Animator 时手动推进）：推进控制器 → 写回目标 → 触发 completed。
    /// @param dt_seconds 帧时间步长（秒）。
    auto tick(double dt_seconds) -> void {
        m_->controller.tick(dt_seconds);
        if (m_->controller.dirty()) {
            m_->target->set(m_->tween.value(m_->controller.value()));
            if (m_->controller.is_completed() && !m_->fired_completed) {
                m_->fired_completed = true;
                if (m_->on_completed) {
                    m_->on_completed();
                }
            }
        }
        m_->controller.clear_dirty();
    }

  private:
    std::shared_ptr<Payload> m_;
};

/// @brief 统一动画入口（收敛 API 面）：创建一个已起步（forward(0)）的动画句柄。
///
/// 返回 `AnimatedValue<T>` 句柄（按值，可拷贝/移动）。动画不自动接入全局帧循环——
/// 调用方要么每帧 `handle.tick(dt)` 自驱动，要么 `handle.attach(animator)` 接入某
/// `Animator` 的帧循环（详见 `animate(target, tw, duration, animator)` 重载）。
///
/// 兼容既有 `AnimationController` / `AnimatedValue` 直接构造（不删除）。
///
/// @tparam T State 与补间的值类型。
/// @param target 写入目标的 State（非拥有引用，须比返回句柄及所接入的帧循环存活更久）。
/// @param tw 补间（含曲线；按值收走）。
/// @param duration_s 播放时长（秒，非正值夹取为 1e-6）。
/// @return 已起步（forward(0)）的动画句柄。
template <typename T>
auto animate(State<T> &target, Tween<T> tw, double duration_s) -> AnimatedValue<T> {
    AnimatedValue<T> av{target, std::move(tw), duration_s};
    av.forward(0.0);
    return av;
}

/// @brief 同上，并自动接入指定 `Animator` 的帧循环（最常用形态）。
/// @tparam T State 与补间的值类型。
/// @param target 写入目标的 State（非拥有引用，须比帧循环存活更久）。
/// @param tw 补间（含曲线；按值收走）。
/// @param duration_s 播放时长（秒，非正值夹取为 1e-6）。
/// @param anim 目标 Animator（句柄可离开作用域，帧循环持载荷副本）。
/// @return 已起步并已 attach 的动画句柄。
template <typename T>
auto animate(State<T> &target, Tween<T> tw, double duration_s, Animator &anim) -> AnimatedValue<T> {
    AnimatedValue<T> av = animate(target, std::move(tw), duration_s);
    av.attach(anim);
    return av;
}

/// @brief 自包含动画值：拥有自己的 State<T>，可独立 tick 推进。
///
/// 用法：
/// @code
/// TweenAnimation<float> anim(0.0F);
/// anim.animate_to(1.0F, 0.3, Curves::ease_in_out());
/// // 每帧：anim.tick(dt);
/// float val = anim.get();  // 0→1 过渡中的当前值
/// @endcode
///
/// @tparam T 动画值的类型，须与所用 `Tween<T>` 的端点类型一致。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class TweenAnimation {
  public:
    /// @brief 以初始值构造（静止态，animating_=false）。
    /// @param initial 初始值（同时作为 get() 与内部 State 的起点）。
    explicit TweenAnimation(T initial) : state_(initial), current_(std::move(initial)) {}

    /// @brief 启动到 target 的动画。
    /// @param target 目标值（从当前值补间过去）。
    /// @param duration_seconds 时长（秒，非正值夹取为 1e-6）。
    /// @param curve 缓动曲线（默认线性）。
    auto animate_to(T target, double duration_seconds, Curve curve = Curve{}) -> void {
        tween_ = Tween<T>(current_, std::move(target), std::move(curve));
        controller_ = AnimationController(duration_seconds);
        controller_.forward(0.0);
        animating_ = true;
    }

    /// @brief 每帧推进（由帧循环调用）。
    /// @param dt_seconds 帧时间步长（秒）；未动画时为 no-op。
    auto tick(double dt_seconds) -> void {
        if (!animating_) {
            return;
        }
        controller_.tick(dt_seconds);
        current_ = tween_.value(controller_.value());
        state_.set(current_);
        if (!controller_.is_animating()) {
            animating_ = false;
        }
    }

    /// @brief 当前值。
    /// @return 最近一次 tick 写入的补间值（未动画时即 animate_to 的目标端点或构造初值）。
    [[nodiscard]] auto get() const -> T { return current_; }

    /// @brief 是否正在动画中。
    /// @return animate_to 后、控制器到达端点前为 true。
    [[nodiscard]] auto is_animating() const -> bool { return animating_; }

    /// @brief 作为信号访问（供响应式绑定）。
    /// @return 内部拥有的 State<T> 引用（每次 set 触发定点刷新）。
    [[nodiscard]] auto as_signal() -> State<T> & { return state_; }

  private:
    State<T> state_;
    T current_;
    Tween<T> tween_;
    AnimationController controller_{1.0};
    bool animating_ = false;
};

/// @brief 时间轴播放器：单主 `AnimationController`（时长 = `TimelineResolved::duration()`）
/// 驱动全部轨道（对应 Flutter staggered 模式的「一个 controller + 多 Interval/Tween」）。
///
/// 每轨道 = (槽位区间, Tween, 目标 State)：主进度 → `interval.local(t)` → Tween 插值 →
/// `State::set`（信号定点刷新）。区间外夹取保证未开始的子段保持 Tween begin 值、
/// 已完成的保持 end 值——**反向倒放天然镜像**（主进度标量倒退，区间映射对称），
/// **中断续播** = `stop()`/`reverse()`/`forward(-1)` 都不动主进度，方向切换从当前续。
///
/// 起播瞬间（`forward(0)`）统一把全部轨道初始化到 begin 值，消除「未播轨道保持旧值」
/// 的不确定。`reduce_motion` 经主控制器短路：全轨道一步落端点，编排层零特判。
///
/// 生命周期沿用 `AnimatedValue` 模式：载荷随句柄按值拷贝移动（TimelineResolved 与轨道
/// 表皆值语义），`attach` 后帧循环持 shared_ptr 副本、句柄离开作用域不悬垂。轨道的
/// 目标 `State<T>` 为非拥有引用——目标 State 必须比播放器存活更久（同 `AnimatedValue`
/// 契约）。
///
/// @code
/// au::State<double> a{0.0}, b{0.0};
/// auto tl = au::TimelineSpec::sequence().add(0.2).add(0.3).build();
/// au::TimelinePlayer player{tl};
/// player.track<double>(0, au::Tween<double>{0.0, 1.0}, a);
/// player.track<double>(1, au::Tween<double>{0.0, 1.0}, b);
/// player.forward();
/// player.attach(app.animator());
/// @endcode
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class TimelinePlayer {
  public:
    /// @brief 构造：区间树求值结果（空 spec 合法——无轨道、时长夹取 1e-6，播放瞬时完成）。
    /// @param spec 区间树求值结果（TimelineSpec::build() 的产物）。
    explicit TimelinePlayer(TimelineResolved spec);

    /// @brief 轨道绑定（类型安全）：槽位区间 + Tween → 目标 State。
    /// 同槽位且同目标才覆盖，否则按绑定序追加；越界槽位为无操作。
    /// @warning 目标 State 为非拥有引用，必须比本播放器存活更久（同 `AnimatedValue`）。
    /// @tparam T 目标 State 的值类型。
    /// @param slot 槽位号（= build() 时叶子段的深度优先序；越界为无操作）。
    /// @param tw 补间（存于类型擦除载荷，按值收走）。
    /// @param target 写入目标的 State。
    template <typename T>
    auto track(std::size_t slot, Tween<T> tw, State<T> &target) -> void {
        Track t;
        t.interval = m_->spec.interval(slot);
        t.payload = std::make_shared<TrackBody<T>>(std::move(tw));
        t.target = &target;
        // 擦除仅限本 apply 函数内部：payload/target 窄化回 TrackBody<T>/State<T>。
        t.apply = [](const Track &base, double master_t) -> void {
            const auto &body = *static_cast<const TrackBody<T> *>(base.payload.get());
            auto *dst = static_cast<State<T> *>(base.target);
            dst->set(body.tween.value(base.interval.local(master_t)));
        };
        bind_track(slot, std::move(t));
    }

    /// @brief 正向播放（可选从 from∈[0,1] 起步；-1 哨兵 = 从当前进度续播）。
    /// @param from 起步进度；恰为 0.0 时先把全部轨道初始化到各自 begin 值（消除未播轨道的旧值）。
    auto forward(double from = -1.0) -> void;

    /// @brief 反向播放：沿时间轴镜像倒放（从当前进度倒退）。
    auto reverse() -> void;

    /// @brief 停止：保持当前进度（方向切换/手势接管用），静止于最近端点语义。
    auto stop() -> void;

    /// @brief 主进度（0..1，主控制器原始输出）。
    /// @return 主控制器当前归一化进度。
    [[nodiscard]] auto progress() const -> double;

    /// @brief 播放状态（主控制器语义）。
    /// @return 主控制器的 AnimationStatus。
    [[nodiscard]] auto status() const -> AnimationStatus;

    /// @brief 是否已到达终点。
    /// @return 主控制器状态为 Completed 时为 true。
    [[nodiscard]] auto is_completed() const -> bool;

    /// @brief 是否正在播放。
    /// @return 主控制器处于 Forward/Reverse 时为 true。
    [[nodiscard]] auto is_animating() const -> bool;

    /// @brief 到达终点（Completed）的一次性回调（同 `AnimatedValue` 语义）。
    /// @param cb 回调（覆写式登记；传空 std::function 即清除）。
    auto on_completed(std::function<void()> cb) -> void;

    /// @brief 接入 Animator 帧循环（载荷 shared_ptr 自持，句柄可离开作用域）。
    /// @param a 目标 Animator（其帧循环持载荷副本）。
    auto attach(Animator &a) -> void;

    /// @brief 无 Animator 时手动推进一帧：推进主控制器 → 应用全部轨道 → completed。
    /// @param dt_seconds 帧时间步长（秒）。
    auto tick(double dt_seconds) -> void;

  private:
    // 轨道：区间 + 类型擦除载荷（Tween<T> 存 TrackBody<T>，apply 内窄化，不泄漏公共 API）。
    struct Track {
        TimelineInterval interval;  ///< 槽位对应的归一化区间
        std::shared_ptr<void> payload;  ///< 类型擦除载荷（实为 TrackBody<T>）
        void *target = nullptr;  ///< 非拥有 State<T>*（apply 内窄化回原类型）
        void (*apply)(const Track &, double) = nullptr;  ///< 类型擦除写回函数（可空）
    };
    /// @brief Tween<T> 的类型擦除存放体（随 Track::payload 共享生命周期）。
    template <typename T>
    struct TrackBody {
        /// @brief 收存补间副本。
        /// @param tw 待存放的补间。
        explicit TrackBody(Tween<T> tw) : tween(std::move(tw)) {}

        Tween<T> tween;  ///< 拥有的补间（含端点与曲线）
    };

    /// @brief 驱动载荷：句柄按值拷贝共享同一份（同 `AnimatedValue`），attach 后帧循环持副本不悬垂。
    struct Payload {
        /// @brief 以区间树结果构造，并据此长初始化主控制器。
        /// @param s 区间树求值结果（spec）。
        explicit Payload(TimelineResolved s) : spec(std::move(s)), master(spec.duration()) {}

        TimelineResolved spec;  ///< 区间树（轨道槽位 → 区间的映射源）
        AnimationController master;  ///< 拥有的主控制器
        std::vector<Track> tracks;  ///< 轨道表（按绑定序追加，写入序确定）
        std::function<void()> on_completed;  ///< 到达终点的一次性回调（可空）
        bool fired_completed = false;  ///< on_completed 是否已触发（保证只发一次）
    };

    /// @brief 把已构造的轨道登记进载荷表（越界槽位无操作；同区间同目标覆盖旧绑定）。
    /// @param slot 槽位号。
    /// @param t 已填好区间/载荷/目标/apply 的轨道。
    auto bind_track(std::size_t slot, Track t) -> void;

    /// @brief 按绑定序把主进度映射写入全部轨道目标。
    /// @param tracks 轨道表。
    /// @param master_t 主控制器当前归一化进度。
    static auto apply_all(const std::vector<Track> &tracks, double master_t) -> void;

    std::shared_ptr<Payload> m_;  ///< 驱动载荷（跨句柄拷贝共享）
};

}  // namespace aurora
