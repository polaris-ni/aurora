#pragma once

#include <algorithm>
#include <cmath>
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

/// @brief 播放模式：端点到达后是停住还是续播（specification/05-event-navigation.md §6.1）。
enum class PlaybackMode : std::uint8_t {
    Once,  ///< 到端点即钳制并置对应终态（缺省，既有语义）
    Repeat,  ///< 到端点按 `AnimationController::repeat` 的次数与方向规则续播
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
    /// @return 推进脏或外部脏任一为真：`tick` 推进后进度有变化，或 `set_progress` 外部写入待消费。
    [[nodiscard]] auto dirty() const -> bool { return dirty_ || external_dirty_; }

    /// @brief 清除全部脏标记（Animator::tick 在应用全部绑定后统一调用）。
    /// 推进脏与外部脏**一并清**：二者都只保证「自上次帧末以来已写回一次」，逐帧由 Animator 收口。
    auto clear_dirty() -> void {
        dirty_ = false;
        external_dirty_ = false;
    }

    /// @brief 当前播放模式。
    /// @return Once（到端点停住，缺省）或 Repeat（到端点续播）。
    [[nodiscard]] auto mode() const -> PlaybackMode { return mode_; }

    /// @brief 是否处于循环模式（等价 `mode() == PlaybackMode::Repeat`）。
    /// @return 循环模式为真；与是否正在播放（`is_animating()`）无关。
    [[nodiscard]] auto is_repeating() const -> bool { return mode_ == PlaybackMode::Repeat; }

    /// @brief 进入循环模式（只登记模式，不起播、不改方向）。
    ///
    /// 与 `forward`/`reverse` 正交：先 `forward` 再 `repeat` 不覆盖方向，先 `repeat` 再 `forward`
    /// 不清模式。起播仍需 `forward()`/`reverse()`——与非循环模式一样，静止的控制器不会自己动。
    ///
    /// **循环在途期间 `Animator::has_active()` 恒真**（无限循环尤甚：应用永不进入空闲深睡，
    /// 这是能力本身的代价）；退出方式是 `stop()` 或 `reset()`，二者都把模式清回 `Once`。
    ///
    /// @param count 到达端点的次数上限；`<= 0`（含缺省 `-1` 与 `0`）为**无限循环**——`0` 与 `-1`
    ///              同义，不表示「播 0 次」；`> 0` 为有限次数，每次到达端点递减，归零后在最后
    ///              到达的端点置终态并把模式清回 `Once`。
    /// @param alternate 为真时每次到达端点**翻转方向**（往复：到 1 转 Reverse、到 0 转 Forward，
    ///                  进度停在被到达的端点、下一帧反向续跑）；为假时**同向重启**（Forward 到 1
    ///                  后回 0 续跑、Reverse 到 0 后回 1）。两种形态在重启帧都置脏。
    auto repeat(int count = -1, bool alternate = false) -> void {
        mode_ = PlaybackMode::Repeat;
        alternate_ = alternate;
        remaining_ = (count > 0) ? count : AURORA_INFINITE_REMAINING;
    }

    /// @brief 外部写入归一化进度（手势拖拽跟手用），**保留状态语义不变**。
    ///
    /// 与 `reset(v)` 的区别是本函数不改写 `status_`、也不清 `PlaybackMode`：播放中调用 =
    /// 重定位后从新进度续跑；静止态调用 = 只定位，不会使 `Animator::has_active()` 变真。
    /// 因此「拖拽跟手 → 松手由 `forward`/`reverse` 接管收尾」无需借道 `reset` 的状态副作用。
    ///
    /// 写回经**外部脏位**送达（不碰推进脏）：`tick()` 入口只清推进脏，本位活到 Animator 帧末
    /// 统一 `clear_dirty()`，故手势在帧边界前写入、同一帧绑定阶段即可插值写回目标 `State`。
    /// 时间缩放为 0（冻结）时本函数**仍然生效**——缩放只冻结推进步长，不改定位语义。
    ///
    /// 与构造初值同为「夹取」口径（构造不夹取、越界原样返回，本函数夹入 `[0,1]`）。
    /// @param t 目标归一化进度；越界夹入 [0,1]。
    auto set_progress(double t) -> void {
        value_ = std::clamp(t, 0.0, 1.0);
        external_dirty_ = true;
    }

    /// @brief 正向播放（可选从 from 起步）。
    /// @param from 起步进度，夹入 [0,1]；-1（默认）为哨兵 = 从当前进度续播。
    auto forward(double from = -1.0) -> void;

    /// @brief 反向播放。
    auto reverse() -> void;

    /// @brief 复位到 v（不播放，静止）；同时把循环模式清回 `Once`。
    /// @param v 复位到的进度，夹入 [0,1]（默认 0）；≥1.0 时报 Completed，否则报 Dismissed。
    auto reset(double v = 0.0) -> void;

    /// @brief 停止：保持当前进度，静止于最近端点语义；同时把循环模式清回 `Once`。
    auto stop() -> void;

    /// @brief 推进 dt 秒；端点处按 `PlaybackMode` 决定停住还是续播。
    /// @param dt_seconds 本帧时间步长（秒）；reduce_motion 生效时直接落在本次播放的目标端点，
    ///                  且**优先于循环**——无障碍档下一步到位即止，不进入循环。
    auto tick(double dt_seconds) -> void;

  private:
    /// @brief 无限循环哨兵（`remaining_` 取值；`repeat` 把 count<=0 归一到这里）。
    static constexpr int AURORA_INFINITE_REMAINING = -1;

    /// @brief 进度到达端点时的行为分派：Once 置终态停住，Repeat 按剩余次数 / alternate 续播。
    /// @param terminal 本帧到达的端点终态（Completed = 到达 1.0，Dismissed = 到达 0.0）。
    auto on_endpoint(AnimationStatus terminal) -> void;

    double duration_ = 0.0;
    double value_ = 0.0;
    AnimationStatus status_ = AnimationStatus::Dismissed;
    bool dirty_ = false;  ///< 推进脏：tick 入口清、推进到变化时置
    bool external_dirty_ = false;  ///< 外部脏：set_progress 置，仅帧末 clear_dirty 清（与推进脏分离）
    PlaybackMode mode_ = PlaybackMode::Once;  ///< 端点行为：停住 / 续播
    bool alternate_ = false;  ///< 循环到端点是否翻转方向（false = 同向重启）
    int remaining_ = AURORA_INFINITE_REMAINING;  ///< 有限循环剩余端点到达次数；AURORA_INFINITE_REMAINING = 无限
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
    ///        缩放为 `0.0`（冻结）时**恒假**：本帧无人推进，就不该再把帧循环钉在满帧——冻结的
    ///        空闲纪律在 `Animator` 侧靠这一位落地，解冻由 `take_rearm_request` 送回首帧。
    /// @return 倍率非零且任一登记控制器处于播放态时为 true。
    [[nodiscard]] auto has_active() const -> bool {
        if (time_scale_ == 0.0) {
            return false;
        }
        return std::ranges::any_of(
            controllers_, [](const AnimationController *c) -> bool { return c != nullptr && c->is_animating(); });
    }

    /// @brief 设置应用内全局时间缩放（调试 / 演示 / 降速用途，**非业务动画契约**）。
    ///
    /// 倍率语义：`1.0` 正常、`2.0` 两倍速、`0.5` 慢放、`0.0` 冻结。与 Flutter
    /// `timeDilation` 互为倒数（`timeDilation = 2` ⇔ `time_scale = 0.5`）。
    ///
    /// 只缩放**动画时间轴**：不改 `duration`、不改目标 `State` 终值、不改曲线，也不影响
    /// 事件、布局、输入与 `Scheduler` 定时任务。明确排除在外的两类：近边缘自动滚边
    /// （`DragAutoScroll` / `ReorderableList` 滚边）是速度型手感而非一段有始有终的时序；
    /// 媒体播放钟（`VideoPlayer`）是内容时钟，缩放它等于改播放速度，不属本口径。
    /// `reduce_motion` 优先于缩放——短路落端点的路径不乘倍率，否则「一步到位」的无障碍语义
    /// 会变成「慢速播放」。
    ///
    /// 冻结（`0.0`）不产生常驻帧：登记控制器本帧不推进即不置脏、绑定不写回，`has_active()` 同时
    /// 转假（本 Animator 不再有帧需求）；控件自驱路径拿到 0 步长后同样不推进、不标脏、不请求帧，
    /// 帧调度因此回到空闲分支（不出现「停着不动却满帧唤醒」）。解冻不能指望控件自己醒——每帧计时
    /// 是逐控件私有标志，全局入口触达不到，故 `0 → >0` 置一位重挂请求，由 `Application::wait_once`
    /// 消费出「下一帧不深睡」，那一帧里各控件照常 `tick_gestures` 并自行续帧。
    ///
    /// 就地取舍（漏斗不夹取，见 `scale_frame_dt`）：冻结期间若帧循环已深睡，控件侧的墙钟锚点
    /// 就停在那一帧，解冻首帧的 `now - last` 含整段停摆时长——在途滑动会一次跳进相应进度。
    /// 本入口是调试 / 演示用途，为此引入夹取会改掉「调用方声明步长」与累积相位两类既有语义，
    /// 代价大于收益。冻结帧照常刷新各控件的墙钟锚点，故只要帧循环还在产帧（另有脏源），解冻即
    /// 从原处续；只有帧循环已进入深睡，才出现上述一次性折算。
    ///
    /// @param scale 速度倍率；负值与非有限值（NaN / inf）一律夹取为 `0.0`（冻结）。
    auto set_time_scale(double scale) -> void {
        const double next = (scale > 0.0 && std::isfinite(scale)) ? scale : 0.0;
        // 解冻（0 → >0）须显式重挂一帧：冻结帧按纪律不标脏、不请求帧，帧循环可能已深睡，
        // 少了这一位就再没有人产出帧，在途的控件自驱滑动 / 弹簧永远停在冻结前的进度上。
        // 只置一位、由 `Application::wait_once` 消费——不做每帧全树扫描（取舍见 ARCHITECTURE §5.2）。
        if (time_scale_ == 0.0 && next > 0.0) {
            rearm_pending_ = true;
        }
        time_scale_ = next;
    }

    /// @brief 取出并清除「解冻重挂」请求（应用帧循环的接线点，非业务动画 API）。
    ///
    /// 控件自驱路径的每帧计时是逐控件私有标志，`set_time_scale` 拿不到控件指针，因此解冻不
    /// 逐个重挂，而是请求「下一帧不许深睡」：那一帧里各控件照常 `tick_gestures`，步长已 > 0，
    /// 在途滑动便自行恢复逐帧请求帧。无在途动画时这一帧是空转帧，代价与「解冻」这一动作同频。
    /// @return 本次调用前存在重挂请求时为 true，并清除该请求（每轮唤醒决策至多消费一次）。
    [[nodiscard]] auto take_rearm_request() -> bool {
        const bool pending = rearm_pending_;
        rearm_pending_ = false;
        return pending;
    }

    /// @brief 当前时间缩放倍率。
    /// @return `set_time_scale` 存入的倍率（缺省 `1.0`，取值域 `0.0` 或正有限值）。
    [[nodiscard]] auto time_scale() const -> double { return time_scale_; }

    /// @brief 把帧步长折算为动画时间轴步长（缩放漏斗的实例侧实现）。
    ///
    /// **只做乘法，不夹取**：入参可能是调用方声明的步长（`Animator::tick(dt)` 传 1.0 做「一步到位」
    /// 是既有契约），也可能是控件自驱路径的墙钟差（既有 `Skeleton` 用例以 3 秒差推进相位）。在这里
    /// 夹取会同时改掉两者的既有语义，故倍率为 `1.0` 时逐比特恒等（判据 12 的确定性据此成立）。
    /// 停摆时长（后台、断点、冻结后深睡）确实会一次性折算进本帧——见 `set_time_scale` 的就地取舍。
    /// @param raw_seconds 帧步长（秒，裸值；负值原样乘，由各消费点的 `<= 0` 守卫视为不推进）。
    /// @return `raw_seconds * time_scale_`；冻结时为 0。
    [[nodiscard]] auto scale_frame_dt(double raw_seconds) const -> double { return raw_seconds * time_scale_; }

    /// @brief 推进所有控制器并应用所有绑定（在帧边界调用一次）。
    /// @param dt_seconds 帧时间步长（秒，裸值；本函数按 `time_scale_` 自行折算，故保持 `const`）。
    auto tick(double dt_seconds) const -> void;

    /// @brief 追加一帧回调（在 tick 中于所有控制器推进后、clear_dirty 前执行）。
    /// @param fn 每帧执行一次的回调；不归属任何控制器，不随 `remove` 摘除。
    auto add_binding(std::function<void()> fn) -> void {
        on_tick_.push_back(Binding{.owner = nullptr, .fn = std::move(fn)});
    }

    /// @brief 追加一条**带帧步长且按控制器归属可注销**的帧回调（弹簧这类自持时间轴的驱动件用）。
    ///
    /// 与 `add_binding` 的三点差别，逐条对应 `SpringAnimation` 的接入需求：
    /// ① 回调收到本帧**已缩放**的动画时间轴步长（裸 dt 在此不可得，避免调用方绕过漏斗）；
    /// ② 归属 `owner`，故 `remove(owner)` 会连同本回调一并摘除（`add_binding` 的裸回调摘不掉）；
    /// ③ `owner` 须已 `drive` 登记，这样驱动件在途期间 `has_active()` 为真、
    ///    `Application::wait_once` 不会深睡把动画半途冻住。
    ///
    /// @param fn 每帧执行一次的回调，入参为折算后的帧步长（秒）。
    /// @param owner 归属控制器（非拥有；须同时经 `drive` 登记，生命周期契约同 `drive`）。
    auto add_frame_binding(std::function<void(double)> fn, AnimationController &owner) -> void {
        on_tick_.push_back(Binding{.owner = &owner, .fn = {}, .dt_fn = std::move(fn)});
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
        std::function<void(double)> dt_fn;  ///< 带帧步长的回调（`add_frame_binding` 登记；可空）
    };

    std::vector<AnimationController *> controllers_;  ///< 非拥有
    std::vector<Binding> on_tick_;  ///< 帧回调表：tick 中在推进控制器之后、清脏之前按登记序执行
    double time_scale_ = 1.0;  ///< 应用内全局时间缩放倍率（1.0 正常，0.0 冻结）
    bool rearm_pending_ = false;  ///< 解冻重挂请求：置位后下一轮唤醒决策不得深睡（`take_rearm_request` 消费）

    /// @brief 当前运行实例槽位（由 `Application::run()` 起止设置；无运行时为 nullptr）。
    static Animator *current_;  // NOLINT
};

/// @brief 动画时间轴的唯一帧步长入口：把裸 dt 折算为受应用内全局缩放约束的 dt。
///
/// 一切「推进动画」的 dt 都必须经此漏斗（`Animator::tick` 与控件自驱的 `ScrollGlide` /
/// `SpringAnimation` / `AnimatedValue::tick` / `TimelinePlayer::tick` / `TweenAnimation::tick`），
/// 缩放才是**应用内全局**而非只覆盖 Animator。倍率取自 `Animator::current()`：
/// 无运行中应用（无头渲染 / golden / 手工驱动单测）时**原样返回入参**，以保持既有 golden 的
/// 逐字节确定性——这条恒等回退同时是文档里「不覆盖的例外」。
///
/// @param raw_seconds 裸帧步长（秒），通常来自单调钟相邻两帧之差。
/// @return 缩放后的动画时间轴步长；冻结时为 0；无运行中 Animator 时为 `raw_seconds`。
inline auto scaled_frame_dt(double raw_seconds) -> double {
    Animator *a = Animator::current();
    return a != nullptr ? a->scale_frame_dt(raw_seconds) : raw_seconds;
}

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
    /// @param dt_seconds 帧时间步长（秒，裸值；内部经 `scaled_frame_dt` 折算，故受应用内缩放约束）。
    auto tick(double dt_seconds) -> void {
        m_->controller.tick(scaled_frame_dt(dt_seconds));
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
    /// @param dt_seconds 帧时间步长（秒，裸值；内部经 `scaled_frame_dt` 折算，故受应用内缩放约束）；
    ///                   未动画时为 no-op。
    auto tick(double dt_seconds) -> void {
        if (!animating_) {
            return;
        }
        controller_.tick(scaled_frame_dt(dt_seconds));
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
    auto forward(double from = -1.0) const -> void;

    /// @brief 反向播放：沿时间轴镜像倒放（从当前进度倒退）。
    auto reverse() const -> void;

    /// @brief 停止：保持当前进度（方向切换/手势接管用），静止于最近端点语义。
    auto stop() const -> void;

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
    auto on_completed(std::function<void()> cb) const -> void;

    /// @brief 接入 Animator 帧循环（载荷 shared_ptr 自持，句柄可离开作用域）。
    /// @param a 目标 Animator（其帧循环持载荷副本）。
    auto attach(Animator &a) const -> void;

    /// @brief 无 Animator 时手动推进一帧：推进主控制器 → 应用全部轨道 → completed。
    /// @param dt_seconds 帧时间步长（秒）。
    auto tick(double dt_seconds) const -> void;

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
    auto bind_track(std::size_t slot, Track t) const -> void;

    /// @brief 按绑定序把主进度映射写入全部轨道目标。
    /// @param tracks 轨道表。
    /// @param master_t 主控制器当前归一化进度。
    static auto apply_all(const std::vector<Track> &tracks, double master_t) -> void;

    std::shared_ptr<Payload> m_;  ///< 驱动载荷（跨句柄拷贝共享）
};

}  // namespace aurora
