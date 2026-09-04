// 动画 demo：AnimationController / Animator / Tween / Keyframes / Curve / SpringSimulation，
// 以及两个可见动画：背景色呼吸 + 来回缩放。
//
// 说明：本 demo 通过 au::App().on_frame(...) 驱动 Animator 每帧推进控制器，
// 并把补间结果写入 State，再让 widget 每帧重读这些 State（背景色 / 缩放），
// 从而得到可见动画。两个控制器均在端点自动反向，形成循环。
//
// 关于「缩放溢出」：框架刻意允许变换（scale/rotate）绘制超出自身布局盒——
// 否则旋转/放大根本看不见（test_modifier_transform.cpp 即依赖此行为）。因此正确的
//  containment 不是去裁剪变换本身，而是：① 为变换元素预留足够空间（舞台）；
// ② 用框架自带的 .clip() 把舞台裁成边界，任何超出都只会被裁在舞台内，
// 绝不波及下方兄弟控件。下方 kStage 由缩放参数推导，改缩放上限也不会回归。
#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "demo_common.h"

// 让一个 AnimationController 在端点自动反向，形成来回循环。
static auto bounce(au::AnimationController &c) -> void {
    if (c.is_completed()) {
        c.reverse();
    } else if (c.is_dismissed()) {
        c.forward();
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // —— 背景色呼吸动画 ——
    au::AnimationController pulse{1.6};
    au::Tween tint{au::Color{59, 130, 246}, au::Color{236, 72, 153}};
    au::State tinted{au::Color{59, 130, 246}};
    // —— 来回缩放动画 ——
    au::AnimationController scale_pulse{1.2};
    au::Tween scaler{0.6F, 1.4F};
    au::State scale_v{0.6F};

    au::Animator animator;
    animator.bind(pulse, tint, tinted);
    animator.bind(scale_pulse, scaler, scale_v);

    au::Curve curve{au::CurveKind::EaseInOut};
    au::SpringSimulation spring{au::SpringDescription{.stiffness = 180.0, .damping = 20.0}, 0.0, 1.0};
    au::Keyframes<au::Color> kf{std::vector<au::Keyframes<au::Color>::Stop>{
        {.time = 0.0, .value = au::Color{59, 130, 246}},
        {.time = 0.5, .value = au::Color{236, 72, 153}},
        {.time = 1.0, .value = au::Color{34, 197, 94}},
    }};

    // 一次性计算的演示值（静态展示，不参与动画循环）。
    const double curved = curve.transform(0.5);
    const double sprung = spring.value(1.0);
    const au::Color kf_color = kf.value(0.5);

    // 缩放演示的几何参数：须与 scaler 上限一致，舞台尺寸由之推导，
    // 使最大缩放（kBaseBox * kMaxScale）仍完整落在带 .clip() 的舞台内。
    constexpr float base_box = 80.0F;
    constexpr float max_scale = 1.4F;  // 与 scaler 上限一致
    constexpr float stage_size = (base_box * max_scale) + 8.0F;  // 留余量，最大缩放仍可见、不贴边
    // 两个 widget 以 shared_ptr 持有，便于每帧更新其修饰（动画色 / 缩放）。
    auto box = std::make_shared<au::Text>(au::LocalizedString{"color pulse"});
    auto scale_inner = std::make_shared<au::Text>(au::LocalizedString{"scale"});

    // 舞台：固定尺寸 + .clip()，把缩放方块的绘制严格裁在其边界内（治本：
    // 用框架自带裁剪原语保证不越界，而非依赖巧合的空间余量）。
    au::Stack stage{std::vector{au::Node{scale_inner}}, au::Alignment::Center};
    stage.modifier.set(au::Modifier{}.size(stage_size, stage_size).clip());

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
    };

    // 启动两个动画并进入帧循环。
    pulse.forward();  // 进入 Forward 状态，tick 才会推进进度
    scale_pulse.forward();

    auto last = std::chrono::steady_clock::now();
    au::App()
        .title("Animation · Aurora Demo")
        .size(520, 520)
        .view(std::move(root))
        .on_frame([&]() -> void {
            // 端点自动反向，形成循环。
            bounce(pulse);
            bounce(scale_pulse);
            // 按真实帧间隔推进动画控制器。
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            animator.tick(dt);
            // 每帧把当前动画值写回修饰（present_root 会重绘）。
            box->modifier.set(au::Modifier{}
                                  .padding(16.0F)
                                  .size(180.0F, 60.0F)
                                  .background(tinted.get())
                                  .align(au::Alignment::Center));
            // 缩放作用在内层方块上；舞台的 .clip() 保证任何超出只裁在舞台内。
            scale_inner->modifier.set(au::Modifier{}
                                          .size(base_box, base_box)
                                          .background(au::Color{34, 197, 94})
                                          .align(au::Alignment::Center)
                                          .scale(scale_v.get()));
        })
        .run();
    return 0;
}