// 动画 demo：AnimationController / Animator / Tween / Keyframes / Curve / SpringSimulation /
// SpringAnimation 的可见载体。
//
// 排版：既有两块动画（背景色呼吸 + 来回缩放）与三行静态展示值原位不动，其下按本文件既有的
// 「小标题文本 + 一块舞台」纵向形态追加五组人工可验证的场景：
//   1. repeat()：无限同向 / 无限往复 / repeat(3) 有限计数（一次性 completed 回调）；
//   2. set_time_scale()：1.00x / 0.35x / 0.00x（冻结）三档，解冻后从原进度续跑；
//   3. set_progress()：拖拽位移映射成 [0,1] 进度当跟手，松手由 forward()/reverse() 接管收尾；
//   4. SpringAnimation：animate_to 带初速度起播 + set_range 夹值域 + on_settled 收尾文本；
//   5. Keyframes<T>::Stop::curve：同一组停靠点，逐段整形 vs 全线性并排对照。
//
// 说明：本 demo 通过 au::App().on_frame(...) 驱动**同一个** Animator 每帧推进全部登记控制器，
// 并把补间 / 关键帧结果写入 State，再让 widget 每帧重读这些 State（背景色 / 缩放 / 位移 /
// 透明度），从而得到可见动画。循环一律由 AnimationController::repeat(...) 在控制器内部完成：
// 帧回调里不再有「到端点即反向」的样板（历史版本每帧判 is_completed()/is_dismissed() 再切
// 方向，正是 repeat() 要替掉的手写循环）。全部场景共用 main() 里那一个 Animator 与那一个
// steady_clock 锚点，帧循环入口也只有 animator.tick(dt) 这一处。
//
// 关于「缩放溢出」：框架刻意允许变换（scale/rotate）绘制超出自身布局盒——
// 否则旋转/放大根本看不见（utest_modifier_transform.cpp 即依赖此行为）。因此正确的
//  containment 不是去裁剪变换本身，而是：① 为变换元素预留足够空间（舞台）；
// ② 用框架自带的 .clip() 把舞台裁成边界，任何超出都只会被裁在舞台内，
// 绝不波及下方兄弟控件。下方 AURORA_STAGE 由缩放参数推导，改缩放上限也不会回归。
#include "aurora/animation/animator.h"
#include "aurora/animation/spring.h"
#include "aurora/app/application.h"
#include "demo_common.h"

// 一个可每帧重写的文本槽：控件句柄 + 上一帧文本。
struct TextSlot {
    std::shared_ptr<au::Text> widget;  ///< 挂进 UI 树的文本控件
    std::string last;  ///< 上一帧写入的文本（比较用）
};

// 建文本槽：initial 同时作为首帧文本与缓存基线。
static auto make_label(std::string initial) -> TextSlot {
    auto widget = std::make_shared<au::Text>(au::LocalizedString{initial});
    return TextSlot{.widget = std::move(widget), .last = std::move(initial)};
}

// 只在文本真的变化时改写控件内容：Text::set_content 会重建内容订阅并把控件标脏，
// 逐帧无脑重写等于把文本区变成常驻脏区（空闲帧也一直在刷新）。
static auto set_label(TextSlot &slot, const std::string &next) -> void {
    if (slot.last == next) {
        return;
    }
    slot.last = next;
    slot.widget->set_content(next);
}

// 两位小数文本：避开 std::format 的工具链差异，也避开六位小数刷屏读不清进度。
static auto two_decimals(double v) -> std::string {
    const std::string s = std::to_string(v);
    const std::size_t dot = s.find('.');
    return (dot == std::string::npos) ? s : s.substr(0, dot + 3);
}

// 轨道行首小标题：固定宽度 + 左对齐，保证同一舞台内各行的轨道左缘对齐。
static auto make_caption(const std::string &text) -> au::Node {
    au::Text caption{text};
    caption.set_modifier(au::Modifier{}.size(150.0F, 20.0F).align(au::Alignment::CenterLeft));
    return au::Node{std::move(caption)};
}

// 舞台：固定尺寸 + 底色 + 边框 + .clip()（沿用本文件既有 containment 写法：
// 预留足够空间后把舞台裁成边界，标记/条形超出只裁在舞台内，不波及兄弟控件）。
static auto make_stage(std::vector<au::Node> kids, float w, float h, au::Alignment align) -> au::Stack {
    au::Stack stage{std::move(kids), align};
    stage.set_modifier(
        au::Modifier{}.size(w, h).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER).clip());
    return stage;
}

// 把 [0,1] 进度写成舞台内标记的水平位移与透明度：位移以舞台中心为原点、左右各让出
// half_span；透明度从 min_alpha 线性升到 1（min_alpha 取 1 即不淡出，只看位移）。
static auto place_marker(const std::shared_ptr<au::Text> &marker, double progress, float side, float half_span,
                         au::Color fill, float min_alpha) -> void {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    const auto alpha = static_cast<float>(min_alpha + ((1.0 - min_alpha) * clamped));
    marker->set_modifier(au::Modifier{}
                             .size(side, side)
                             .background(fill, 4.0F)
                             .align(au::Alignment::Center)
                             .offset(static_cast<float>(((clamped * 2.0) - 1.0) * half_span), 0.0F)
                             .opacity(alpha));
}

// 把 [0,1] 进度写成条形宽度（舞台取 CenterLeft 对齐，故条形自左端生长）；保留 2dp 下限避免零宽。
static auto place_bar(const std::shared_ptr<au::Text> &bar, double progress, float full_w, float h, au::Color fill)
    -> void {
    const float scaled = static_cast<float>(std::clamp(progress, 0.0, 1.0)) * full_w;
    bar->set_modifier(au::Modifier{}.size(std::max(2.0F, scaled), h).background(fill).align(au::Alignment::CenterLeft));
}

// 倍率芯片外观：选中档用主色软底 + 加粗主色边框，未选中档浅灰底 + 细边框（文字恒为黑色，可读性优先）。
static auto chip_modifier(float w, bool active) -> au::Modifier {
    return au::Modifier{}
        .size(w, 24.0F)
        .background(active ? pal::AURORA_PRIMARY_SOFT : pal::AURORA_SURFACE)
        .border(active ? 2.0F : 1.0F, active ? pal::AURORA_PRIMARY : pal::AURORA_BORDER)
        .align(au::Alignment::Center);
}

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    // —— 背景色呼吸动画 ——
    au::AnimationController pulse{1.6};
    au::Tween tint{au::Color{59, 130, 246}, au::Color{236, 72, 153}};
    au::State tinted{au::Color{59, 130, 246}};
    // —— 来回缩放动画 ——
    au::AnimationController scale_pulse{1.2};
    au::Tween scaler{0.6F, 1.4F};
    au::State scale_v{0.6F};

    // —— 场景一：repeat() 的三条对照（无限同向 / 无限往复 / repeat(3) 有限计数）——
    // 三条轨道共用「进度 0..1 直写 State」的补间，差别只在控制器的循环形态。
    au::Tween<double> unit_ramp{0.0, 1.0};
    au::AnimationController loop_same{1.0};
    au::AnimationController loop_alt{1.0};
    au::State<double> s_same{0.0};
    au::State<double> s_alt{0.0};
    au::State<double> s_finite{0.0};
    // 有限计数那条走 AnimatedValue 句柄：它把「最后一次到达端点」的一次性 completed 回调收在一处。
    au::AnimatedValue<double> finite_av{s_finite, au::Tween<double>{0.0, 1.0}, 0.8};
    int completed_hits = 0;  // completed 回调触发次数（人工判据：恒为 1）

    // —— 场景二：Animator::set_time_scale() 慢放 / 冻结 ——
    // 该入口是调试 / 演示用途，只缩放动画时间轴：不缩放事件、布局、输入与 Scheduler 定时任务，
    // 也不改 duration / 终值 / 曲线；0.0 为冻结（帧调度同时转空闲，不出现「停着却满帧」）。
    au::AnimationController ramp{4.0};
    au::State<double> s_ramp{0.0};
    int scale_step = 0;  // 0 = 1.00x，1 = 0.35x，2 = 0.00x（冻结）

    // —— 场景三：AnimationController::set_progress() 手势定位 ——
    // 与 reset(v) 的分别：set_progress 不改状态语义、不清循环模式，故「跟手 → 松手接管」
    // 无需借道 reset；静止态下写入只定位，不会让 Animator 误报「有动画在跑」。
    au::AnimationController scrub{1.0};
    au::State<double> s_scrub{0.0};
    double scrub_px = 0.0;  // 拖拽累计位移（Draggable 回调上报的是逐次增量，这里自行累加）
    constexpr double scrub_travel = 260.0;  // 拖满这段位移 = 进度 1.0（set_progress 内部夹入 [0,1]）

    // —— 场景四：SpringAnimation 驱动一个标量 State<double> ——
    au::State<double> s_spring{0.0};
    au::SpringAnimation spring_anim{s_spring};
    bool spring_goal = false;  // 每点一次换一个收敛目标（0.0 / 1.0）
    std::string spring_phase = "idle";  // idle / running / settled（on_settled 改写为 settled）

    // —— 场景五：Keyframes<T>::Stop::curve 每段整形 ——
    au::AnimationController kf_shaped_ctrl{2.4};
    au::AnimationController kf_plain_ctrl{2.4};
    au::State<double> s_kf_shaped{0.0};
    au::State<double> s_kf_plain{0.0};
    // 两条关键帧的停靠时刻与取值完全相同，只在「每段的整形曲线」上分家：上面那条逐段换曲线
    // （段曲线取自该段**起点**那个停靠点），下面那条全部缺省线性——肉眼即可看出各段快慢不同。
    const au::Keyframes<double> kf_shaped{std::vector<au::Keyframes<double>::Stop>{
        {.time = 0.00, .value = 0.00, .curve = au::Curves::ease_in_cubic()},
        {.time = 0.20, .value = 1.00, .curve = au::Curves::ease_out_cubic()},
        {.time = 0.45, .value = 0.10, .curve = au::Curves::ease_in_out_sine()},
        {.time = 0.65, .value = 0.95, .curve = au::Curves::bounce_out()},
        {.time = 0.85, .value = 0.20},
        {.time = 1.00, .value = 0.00},
    }};
    const au::Keyframes<double> kf_plain{std::vector<au::Keyframes<double>::Stop>{
        {.time = 0.00, .value = 0.00},
        {.time = 0.20, .value = 1.00},
        {.time = 0.45, .value = 0.10},
        {.time = 0.65, .value = 0.95},
        {.time = 0.85, .value = 0.20},
        {.time = 1.00, .value = 0.00},
    }};

    au::Animator animator;
    animator.bind(pulse, tint, tinted);
    animator.bind(scale_pulse, scaler, scale_v);
    // 场景一：两条无限循环经 bind 写回；有限计数那条经 AnimatedValue::attach（自带一次性 completed）。
    animator.bind(loop_same, unit_ramp, s_same);
    animator.bind(loop_alt, unit_ramp, s_alt);
    finite_av.on_completed([&completed_hits]() -> void { ++completed_hits; });
    finite_av.attach(animator);
    // 场景二 / 三：同一个 Animator、同一套帧循环。
    animator.bind(ramp, unit_ramp, s_ramp);
    animator.bind(scrub, unit_ramp, s_scrub);
    // 场景四：弹簧接进同一 Animator 的帧循环（attach 内部把它的时钟控制器登记给本 Animator，
    // 故摘除用 animator.remove(spring_anim.clock())——连同其帧回调一并注销，不留幽灵帧）。
    spring_anim.set_range(0.0, 1.0);  // 欠阻尼会越过目标，值域受限（0..1 进度）故夹取
    spring_anim.on_settled([&spring_phase]() -> void { spring_phase = "settled"; });
    spring_anim.attach(animator);
    // 场景五：两条关键帧各用一个控制器——同一控制器被 bind 两次会重复登记、每帧推进两次，
    // 故这里分登记；两者同时起播、步长同源，进度逐帧相等，差别只来自各自的段曲线。
    animator.bind(kf_shaped_ctrl, kf_shaped, s_kf_shaped);
    animator.bind(kf_plain_ctrl, kf_plain, s_kf_plain);

    // 一次性计算的演示值（静态展示，不参与动画循环）。
    au::Curve curve{au::CurveKind::EaseInOut};
    au::SpringSimulation spring{au::SpringDescription{.stiffness = 180.0, .damping = 20.0}, 0.0, 1.0};
    au::Keyframes<au::Color> kf{std::vector<au::Keyframes<au::Color>::Stop>{
        {.time = 0.0, .value = au::Color{59, 130, 246}},
        {.time = 0.5, .value = au::Color{236, 72, 153}},
        {.time = 1.0, .value = au::Color{34, 197, 94}},
    }};
    const double curved = curve.transform(0.5);
    const double sprung = spring.value(1.0);
    const au::Color kf_color = kf.value(0.5);

    // 缩放演示的几何参数：须与 scaler 上限一致，舞台尺寸由之推导，
    // 使最大缩放（AURORA_BASE_BOX * AURORA_MAX_SCALE）仍完整落在带 .clip() 的舞台内。
    constexpr float base_box = 80.0F;
    constexpr float max_scale = 1.4F;  // 与 scaler 上限一致
    constexpr float stage_size = (base_box * max_scale) + 8.0F;  // 留余量，最大缩放仍可见、不贴边
    // 各轨道的公共形制：窄轨（场景一的三行）与宽轨（场景二/三/四/五的舞台）。
    constexpr float narrow_w = 150.0F;
    constexpr float narrow_h = 22.0F;
    constexpr float wide_w = 320.0F;
    constexpr float marker_side = 20.0F;
    constexpr float narrow_span = (narrow_w - marker_side) * 0.5F;  // 窄轨内标记的左右让位量
    constexpr float wide_span = (wide_w - marker_side) * 0.5F;  // 宽轨内标记的左右让位量

    // 两个 widget 以 shared_ptr 持有，便于每帧更新其修饰（动画色 / 缩放）。
    auto box = std::make_shared<au::Text>(au::LocalizedString{"color pulse"});
    auto scale_inner = std::make_shared<au::Text>(au::LocalizedString{"scale"});
    // 五组舞台里每帧改修饰的标记，以及会改文本的说明行。
    auto same_marker = std::make_shared<au::Text>();
    auto alt_marker = std::make_shared<au::Text>();
    auto finite_marker = std::make_shared<au::Text>();
    auto ramp_bar = std::make_shared<au::Text>();
    auto scrub_marker = std::make_shared<au::Text>();
    auto spring_marker = std::make_shared<au::Text>();
    auto shaped_marker = std::make_shared<au::Text>();
    auto plain_marker = std::make_shared<au::Text>();
    auto chip_normal = std::make_shared<au::Text>(au::LocalizedString{"1.00x"});
    auto chip_slow = std::make_shared<au::Text>(au::LocalizedString{"0.35x"});
    auto chip_frozen = std::make_shared<au::Text>(au::LocalizedString{"0.00x freeze"});
    TextSlot loop_note = make_label("repeat(3) completed fired: 0 (must stay 1)");
    TextSlot scale_note = make_label("time scale = 1.00x, progress = 0.00");
    TextSlot scrub_note = make_label("scrub progress = 0.00, dragged = 0.00 px");
    TextSlot spring_note = make_label("spring: idle, value = 0.00");
    TextSlot kf_note = make_label("per-stop shaped = 0.00, all-linear = 0.00");

    // 芯片外观只在档位变化时重写（apply_chips）：逐帧重建 Clickable 节点等于每帧抖一次输入链，
    // 且会把空闲帧也标脏。三枚芯片各挂一个 set_time_scale 调用。
    auto apply_chips = [&]() -> void {
        chip_normal->set_modifier(chip_modifier(60.0F, scale_step == 0).clickable([&animator, &scale_step]() -> void {
            scale_step = 0;
            animator.set_time_scale(1.0);
        }));
        chip_slow->set_modifier(chip_modifier(60.0F, scale_step == 1).clickable([&animator, &scale_step]() -> void {
            scale_step = 1;
            animator.set_time_scale(0.35);
        }));
        chip_frozen->set_modifier(chip_modifier(104.0F, scale_step == 2).clickable([&animator, &scale_step]() -> void {
            scale_step = 2;
            animator.set_time_scale(0.0);
        }));
    };

    // 舞台：固定尺寸 + .clip()，把缩放方块的绘制严格裁在其边界内（治本：
    // 用框架自带裁剪原语保证不越界，而非依赖巧合的空间余量）。
    au::Stack stage{std::vector{au::Node{scale_inner}}, au::Alignment::Center};
    stage.set_modifier(au::Modifier{}.size(stage_size, stage_size).clip());

    // 场景一舞台：三行「小标题 + 轨道」，末行是 completed 回调计数。
    au::Stack track_same = make_stage(std::vector{au::Node{same_marker}}, narrow_w, narrow_h, au::Alignment::Center);
    au::Stack track_alt = make_stage(std::vector{au::Node{alt_marker}}, narrow_w, narrow_h, au::Alignment::Center);
    au::Stack track_finite =
        make_stage(std::vector{au::Node{finite_marker}}, narrow_w, narrow_h, au::Alignment::Center);
    au::Row row_same{au::RowProps{
        .children = {make_caption("infinite, same direction"), au::Node{std::move(track_same)}}, .gap = 8.0F}};
    au::Row row_alt{
        au::RowProps{.children = {make_caption("infinite, alternate"), au::Node{std::move(track_alt)}}, .gap = 8.0F}};
    au::Row row_finite{
        au::RowProps{.children = {make_caption("repeat(3), finite"), au::Node{std::move(track_finite)}}, .gap = 8.0F}};
    au::Column repeat_rows{au::ColumnProps{.children = {au::Node{std::move(row_same)}, au::Node{std::move(row_alt)},
                                                        au::Node{std::move(row_finite)}, au::Node{loop_note.widget}},
                                           .gap = 4.0F}};

    // 场景二舞台：三枚倍率芯片 + 一条 4 秒循环的进度条 + 当前倍率/进度读数。
    au::Row chips_row{
        au::RowProps{.children = {au::Node{chip_normal}, au::Node{chip_slow}, au::Node{chip_frozen}}, .gap = 8.0F}};
    au::Stack ramp_track = make_stage(std::vector{au::Node{ramp_bar}}, wide_w, 20.0F, au::Alignment::CenterLeft);
    au::Column scale_rows{au::ColumnProps{
        .children = {au::Node{std::move(chips_row)}, au::Node{std::move(ramp_track)}, au::Node{scale_note.widget}},
        .gap = 6.0F}};

    // 场景三舞台：整块舞台即拖拽面（拖拽来源是库里的 Draggable 修饰，与 demo_multitouch 同接法：
    // 按下绑指针、Move 上报增量、Release 结束），标记随进度位移并淡入淡出。
    au::Stack scrub_stage{std::vector{au::Node{scrub_marker}}, au::Alignment::Center};
    scrub_stage.set_modifier(au::Modifier{}
                                 .size(wide_w, 44.0F)
                                 .background(pal::AURORA_SURFACE)
                                 .border(1.0F, pal::AURORA_BORDER)
                                 .clip()
                                 .draggable(
                                     [&scrub, &scrub_px, scrub_travel](au::Point delta, au::Point) -> void {
                                         // 位移 → [0,1] 进度：写入的外部脏位活到帧末，同一帧的绑定
                                         // 阶段即插值写回 State，故跟手无「下一帧才动」的滞后。
                                         scrub_px += static_cast<double>(delta.x);
                                         scrub.set_progress(scrub_px / scrub_travel);
                                     },
                                     [&scrub, &scrub_px]() -> void {
                                         scrub.stop();  // 拖拽期间由手势掌轴：停住时间推进
                                         scrub_px = 0.0;
                                     },
                                     [&scrub]() -> void {
                                         // 松手接管：过半 forward() 收尾，否则 reverse() 回到起点。
                                         if (scrub.value() >= 0.5) {
                                             scrub.forward();
                                         } else {
                                             scrub.reverse();
                                         }
                                     }));
    au::Column scrub_rows{
        au::ColumnProps{.children = {au::Node{std::move(scrub_stage)}, au::Node{scrub_note.widget}}, .gap = 4.0F}};

    // 场景四舞台：弹簧轨道 + 再踢一脚的点击条 + 收敛状态读数。
    au::Stack spring_track = make_stage(std::vector{au::Node{spring_marker}}, wide_w, 26.0F, au::Alignment::Center);
    au::Text spring_kick{au::LocalizedString{"kick again (click)"}};
    spring_kick.set_modifier(au::Modifier{}
                                 .size(wide_w, 24.0F)
                                 .background(pal::AURORA_PRIMARY_SOFT)
                                 .border(1.0F, pal::AURORA_BORDER)
                                 .align(au::Alignment::Center)
                                 .clickable([&spring_anim, &spring_goal, &spring_phase]() -> void {
                                     spring_goal = !spring_goal;
                                     spring_phase = "running";
                                     // 带初速度起播：交接拖拽速度那一类用法，方向与目标一致。
                                     spring_anim.animate_to(spring_goal ? 1.0 : 0.0,
                                                            au::SpringDescription{.stiffness = 180.0, .damping = 12.0},
                                                            /*initial_velocity=*/spring_goal ? 2.5 : -2.5,
                                                            /*tolerance=*/0.005);
                                 }));
    au::Column spring_rows{au::ColumnProps{
        .children = {au::Node{std::move(spring_track)}, au::Node{std::move(spring_kick)}, au::Node{spring_note.widget}},
        .gap = 4.0F}};

    // 场景五舞台：两条同形对照轨道（上：逐段整形，下：全线性）。
    au::Stack track_shaped = make_stage(std::vector{au::Node{shaped_marker}}, wide_w, narrow_h, au::Alignment::Center);
    au::Stack track_plain = make_stage(std::vector{au::Node{plain_marker}}, wide_w, narrow_h, au::Alignment::Center);
    au::Column kf_rows{au::ColumnProps{
        .children = {make_caption("per-stop curves"), au::Node{std::move(track_shaped)}, make_caption("all linear"),
                     au::Node{std::move(track_plain)}, au::Node{kf_note.widget}},
        .gap = 4.0F}};

    au::Node root = au::Column{
        GradientTitle{"Animation"},
        gap(12),
        au::Text{au::LocalizedString{"Background color breathing"}},
        au::Node{box},
        au::Text{au::LocalizedString{"Ping-pong scaling"}},
        au::Node{std::move(stage)},
        au::Text{au::LocalizedString{"curve@0.5 = " + std::to_string(curved)}},
        au::Text{au::LocalizedString{"spring value = " + std::to_string(sprung)}},
        au::Text{au::LocalizedString{"keyframe@0.5 = rgb(" + std::to_string(kf_color.r) + "," +
                                     std::to_string(kf_color.g) + "," + std::to_string(kf_color.b) + ")"}},
        gap(12),
        au::Text{au::LocalizedString{"repeat(): no manual bounce - the controller loops itself"}},
        au::Node{std::move(repeat_rows)},
        gap(12),
        au::Text{au::LocalizedString{"set_time_scale(): 1.00x / 0.35x / 0.00x via the chips"}},
        au::Node{std::move(scale_rows)},
        gap(12),
        au::Text{au::LocalizedString{"set_progress(): drag the strip, release hands over"}},
        au::Node{std::move(scrub_rows)},
        gap(12),
        au::Text{au::LocalizedString{"SpringAnimation: animate_to + set_range + on_settled"}},
        au::Node{std::move(spring_rows)},
        gap(12),
        au::Text{au::LocalizedString{"Keyframes Stop::curve: shaped segments vs all-linear"}},
        au::Node{std::move(kf_rows)},
    };

    // 启动动画并进入帧循环。
    pulse.forward();  // 进入 Forward 状态，tick 才会推进进度
    pulse.repeat(-1, /*alternate=*/true);  // 往复：到端点翻转方向（替掉帧回调里的端点反向样板）
    scale_pulse.forward();
    scale_pulse.repeat(-1, /*alternate=*/true);
    loop_same.forward(0.0);
    loop_same.repeat(-1, /*alternate=*/false);  // 同向重启：到 1.0 后回到 0.0 续跑（有跳变，故重启帧置脏）
    loop_alt.forward(0.0);
    loop_alt.repeat(-1, /*alternate=*/true);
    finite_av.controller().repeat(3, /*alternate=*/false);  // 第 3 次到达端点才清循环模式并置 Completed
    finite_av.forward(0.0);
    ramp.forward(0.0);
    ramp.repeat(-1, /*alternate=*/false);
    kf_shaped_ctrl.forward(0.0);
    kf_shaped_ctrl.repeat(-1, /*alternate=*/false);
    kf_plain_ctrl.forward(0.0);
    kf_plain_ctrl.repeat(-1, /*alternate=*/false);
    // 场景三的控制器刻意不起播：跟手期间只经 set_progress 定位，松手才交给 forward()/reverse()。
    // 场景四先踢一脚，使不点按也能看到「收敛 → settled」，点按再换目标复验。
    spring_goal = true;
    spring_phase = "running";
    spring_anim.animate_to(1.0, au::SpringDescription{.stiffness = 180.0, .damping = 12.0},
                           /*initial_velocity=*/2.5, /*tolerance=*/0.005);
    apply_chips();

    auto last = std::chrono::steady_clock::now();
    int painted_step = scale_step;  // 已绘到芯片上的档位（与 scale_step 不同才重写）
    au::App()
        .title("Animation · Aurora Demo")
        .size(520, 1000)
        .view(std::move(root))
        .on_frame([&]() -> void {
            // 按真实帧间隔推进动画控制器（全 demo 唯一的 tick 入口；循环形态由控制器自己处理）。
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            animator.tick(dt);
            // 每帧把当前动画值写回修饰（present_root 会重绘）。
            box->set_modifier(au::Modifier{}
                                  .padding(16.0F)
                                  .size(180.0F, 60.0F)
                                  .background(tinted.get())
                                  .align(au::Alignment::Center));
            // 缩放作用在内层方块上；舞台的 .clip() 保证任何超出只裁在舞台内。
            scale_inner->set_modifier(au::Modifier{}
                                          .size(base_box, base_box)
                                          .background(au::Color{34, 197, 94})
                                          .align(au::Alignment::Center)
                                          .scale(scale_v.get()));
            // 场景一：三条轨道的标记位置即各自控制器进度。
            place_marker(same_marker, s_same.get(), marker_side, narrow_span, pal::AURORA_PRIMARY, 1.0F);
            place_marker(alt_marker, s_alt.get(), marker_side, narrow_span, pal::AURORA_ACCENT, 1.0F);
            place_marker(finite_marker, s_finite.get(), marker_side, narrow_span, pal::AURORA_OK, 1.0F);
            set_label(loop_note, "repeat(3) completed fired: " + std::to_string(completed_hits) + " (must stay 1)");
            // 场景二：倍率读数 + 进度条；冻结时进度停在原地，解冻从该进度续跑（不丢进度）。
            if (painted_step != scale_step) {
                painted_step = scale_step;
                apply_chips();
            }
            place_bar(ramp_bar, s_ramp.get(), wide_w - 2.0F, 16.0F, pal::AURORA_WARN);
            set_label(scale_note, "time scale = " + two_decimals(animator.time_scale()) + "x, progress = " +
                                      two_decimals(s_ramp.get()) + " (frozen keeps it, unfrozen resumes)");
            // 场景三：标记随 set_progress 写回的进度位移并淡入。
            place_marker(scrub_marker, s_scrub.get(), marker_side, wide_span, pal::AURORA_PRIMARY, 0.25F);
            set_label(scrub_note, "scrub progress = " + two_decimals(s_scrub.get()) + ", dragged = " +
                                      two_decimals(scrub_px) + " px (release: past half goes forward)");
            // 场景四：弹簧写出的是夹在 [0,1] 内的标量（越界回弹被 set_range 夹住）。
            place_marker(spring_marker, s_spring.get(), marker_side, wide_span, pal::AURORA_WARN, 1.0F);
            set_label(spring_note, "spring: " + spring_phase + ", value = " + two_decimals(s_spring.get()));
            // 场景五：同一时刻两条轨道的取值差就是各段曲线的整形差。
            place_marker(shaped_marker, s_kf_shaped.get(), marker_side, wide_span, pal::AURORA_ACCENT, 1.0F);
            place_marker(plain_marker, s_kf_plain.get(), marker_side, wide_span, pal::AURORA_MUTED, 1.0F);
            set_label(kf_note, "per-stop shaped = " + two_decimals(s_kf_shaped.get()) +
                                   ", all-linear = " + two_decimals(s_kf_plain.get()));
        })
        .run();
    return 0;
}
