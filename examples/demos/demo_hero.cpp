// Hero 共享元素转场演示（对照 Flutter Hero）。
//
// 两个页面都放置同 tag 的 Hero("logo")，但位置/尺寸不同。启动时由 home 转场到 detail，
// 共享元素在 TransitionLayer 覆盖层上做矩形 lerp + 交叉淡变「形变飞入」；点 Back 触发反向转场。
//
// 用 Application（而非 run_demo）以驱动 Animator：每帧 set_on_frame 推进 host 绑定的动画，
// 使 NavigatorHost 的转场进度自动演进。
#include <memory>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"

#include "demo_common.h"

auto main() -> int {
    aurora::Animator anim;
    auto host = std::make_shared<aurora::NavigatorHost>(anim);

    // 详情页：Hero("logo") 更大（绿），返回按钮 pop。
    auto make_detail = [host]() -> aurora::Node {
        aurora::Text lbl{ "Aurora" };
        lbl.modifier.set(aurora::Modifier{}.background(pal::AURORA_OK).size(160.0f, 120.0f));
        aurora::Hero logo{ "logo", aurora::Node{ std::move(lbl) } };
        auto btn = aurora::Button{ "Back" };
        btn.set_on_click([host]() -> void { (void)host->pop(); });
        return aurora::Column{ aurora::ColumnProps{ .children = { std::move(logo), gap(12.0f), std::move(btn) } } };
    };

    // 首页：Hero("logo") 较小（品红），按钮 push 到详情页。
    auto make_home = [host, make_detail]() -> aurora::Node {
        aurora::Text lbl{ "Aurora" };
        lbl.modifier.set(aurora::Modifier{}.background(pal::AURORA_ACCENT).size(64.0f, 48.0f));
        aurora::Hero logo{ "logo", aurora::Node{ std::move(lbl) } };
        auto btn = aurora::Button{ "Go to detail" };
        btn.set_on_click([host, make_detail]() -> void { host->push(aurora::Route{ make_detail(), "detail" }); });
        return aurora::Column{ aurora::ColumnProps{ .children = { std::move(logo), gap(12.0f), std::move(btn) } } };
    };

    host->push(aurora::Route{ make_home(), "home" });

    aurora::RouteTransition fade;
    fade.animated = true;
    fade.kind = aurora::TransitionKind::Fade;
    fade.duration_seconds = 0.6;
    host->push(aurora::Route{ make_detail(), "detail", fade }); // 启动即播放共享元素转场

    aurora::Scene scene{ aurora::Node{ host } };
    aurora::WindowOptions wopts;
    wopts.size = aurora::Size{ .width = 420.0f, .height = 320.0f };
    wopts.title = "Hero 共享元素转场";
    auto win_res = create_native_window(wopts);
    aurora::Application app{ std::move(scene), win_res ? std::move(win_res.value()) : nullptr, wopts };
    // 驱动 host 绑定的 Animator，使转场进度自动演进（Application 内部 animator 与此独立）。
    app.set_on_frame([&anim]() -> void { anim.tick(1.0 / 60.0); });
    app.run();
    return 0;
}
