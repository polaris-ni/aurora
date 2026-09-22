// Dismissible 拖动消除 demo（对照 Flutter Dismissible）。
//
// 列表卡片沿水平方向拖出（跟手 1:1 映射 + 途中渐隐），松手按阈值裁决：低于阈值 spring
// 回位、高于阈值 spring 飞出并从列表摘除（重排）。第三张卡片注册 on_dismissed 自定义
// 回调，演示「回调接管默认摘除」的扩展点（如同步删除列表数据后重建子树）。
//
// 用 Application（而非 run_demo）：Dismissible 的 spring 阶段由每帧 gesture tick 推进，
// Application::tick 内建驱动控件树的 tick_gestures，run_demo 的静态循环不含该驱动。
#include <memory>
#include <string>

#include "aurora/app/application.h"
#include "aurora/widget/dismissible.h"
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    auto make_card = [](const std::string &label, au::Color tint, bool custom_cb) -> au::Node {
        au::Text title{label};
        title.modifier.set(au::Modifier{}.background(tint).size(280.0F, 56.0F));
        auto dis = std::make_shared<au::Dismissible>(au::Node{std::move(title)});
        if (custom_cb) {
            // 自定义回调接管默认摘除：这里只记录（真实场景多为删除数据后重建子树）。
            // 回调转入 std::function（on_dismissed），本检查对可调用对象一律判「不应抛出」；
            // 体内只走日志通道，其格式化分配即唯一抛出面。
            // NOLINTBEGIN(bugprone-exception-escape)
            dis->on_dismissed(
                [label]() -> void { AURORA_LOG_INFO("demo", "[demo_dismissible] custom dismissed: ", label); });
            // NOLINTEND(bugprone-exception-escape)
        }
        return au::Node{dis};
    };

    au::Column list = au::Column{
        au::ColumnProps{
            .children =
                {
                    make_card("swipe me ->", pal::AURORA_PRIMARY_SOFT, false),
                    make_card("swipe away ->", pal::AURORA_OK, false),
                    make_card("custom callback", pal::AURORA_ACCENT, true),
                },
        },
    };
    auto list_ptr = std::make_shared<au::Column>(std::move(list));

    au::Node root = au::Column{
        au::ColumnProps{
            .children =
                {
                    GradientTitle{"Dismissible"},
                    gap(12),
                    au::Text{"Drag a card horizontally; release past half to dismiss."},
                    gap(16),
                    Card{au::Node{list_ptr}},
                },
        },
    };

    au::Scene scene{std::move(root)};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 420.0F};
    opts.title = "Dismissible · Aurora Demo";
    auto win_res = create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    app.run();
    return 0;
}
