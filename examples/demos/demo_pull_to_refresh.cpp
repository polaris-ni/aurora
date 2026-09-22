// PullToRefresh 下拉刷新 demo：滚动子树到顶后继续下拉，越阈值松手触发刷新，指示器回弹收拢。
//
// 两条输入通道都可用：鼠标/触摸**向下拖拽**（橡皮筋跟手，远端渐硬），以及光标停在列表顶部时的
// **滚轮上滚余量**（内层 Scroll 到顶后未消费的量上冒给本容器）。刷新回调里模拟 900ms 异步取数，
// 完成后 `finish_refresh()` 收拢；系统开启「减弱动态效果」时直落端点、不产生回弹中间帧。
//
// 用 Application（而非 run_demo）：指示器旋转与回弹由每帧 gesture tick 推进。
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scheduler.h"
#include "aurora/widget/pull_to_refresh.h"
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    auto status = std::make_shared<au::State<au::LocalizedString>>(
        au::LocalizedString{"拖拽或滚轮上滚到列表顶部继续下拉 → 松手刷新"});

    std::vector<au::Node> rows;
    for (int i = 0; i < 30; ++i) {
        au::Text line{au::LocalizedString{"item " + std::to_string(i)}};
        line.modifier.set(
            au::Modifier{}.padding(6.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
        rows.emplace_back(std::move(line));
    }
    au::Scroll scroll{au::ScrollProps{.child = au::Node{au::Column{au::ColumnProps{.children = std::move(rows)}}}}};
    scroll.modifier.set(au::Modifier{}.size(340.0F, 240.0F));

    auto pull = std::make_shared<au::PullToRefresh>(au::Node{std::move(scroll)});
    pull->modifier.set(au::Modifier{}.border(1.0F, pal::AURORA_BORDER));

    std::weak_ptr<au::PullToRefresh> weak_pull = pull;
    pull->on_refresh([status, weak_pull]() -> void {
        status->set(au::LocalizedString{"刷新中…（异步取数）"});
        auto finish = [status, weak_pull]() -> void {
            if (auto p = weak_pull.lock()) {
                p->finish_refresh();  // 指示器收拢回弹
            }
            status->set(au::LocalizedString{"刷新完成，可再次下拉"});
        };
        if (auto *sch = au::Scheduler::current()) {
            (void)sch->set_timeout(std::chrono::milliseconds(900), finish);
        } else {
            finish();  // 无运行中 App（如无头渲染）：立即收拢，不留常驻刷新态
        }
    });

    au::Text status_line{au::TextProps{.content = au::Reactive<au::LocalizedString>{status}}};

    au::Node root = au::Column{
        GradientTitle{"PullToRefresh"}, gap(12), std::move(status_line), gap(8), Card{au::Node{pull}},
    };

    au::Scene scene{std::move(root)};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 420.0F};
    opts.title = "PullToRefresh · Aurora Demo";
    auto win_res = create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    app.run();
    return 0;
}
