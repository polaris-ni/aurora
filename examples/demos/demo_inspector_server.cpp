// InspectorServer 远程检视载体：经 localhost HTTP 暴露运行时 UI 树，供外部工具 / AI Agent 读写。
//
// 与 demo_google_play 的分工：后者是应用级演示（根为 NavigatorHost，树随路由变化）；
// 本载体的根树刻意做成**扁平表单**，索引路径（"0".."4"）稳定可预期，
// 便于逐端点核对 /api/tree、/api/widget、/api/input、/api/patch 的读写闭环。
//
// 构建：需 -DAURORA_BUILD_INSPECTOR_SERVER=ON（见 codespec/BUILD_OPTIONS.md §2.3）；
// 未开启时该选项的宏未定义，本文件的分支整体编译剔除，程序仅提示后退出。
#include <cstdint>
#include <memory>
#include <string>

#include "demo_common.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER

// InspectorServer 不在 aurora.h 的聚合入口内（其实现编在独立静态库 aurora_inspector_server），
// 故需显式包含；未开该选项时此头不参与编译。
#include "aurora/inspector/inspector_server.h"

namespace {

constexpr std::uint16_t AURORA_INSPECTOR_PORT = 6280;

/// @brief 被检视的表单：根为 Column，五个子节点的索引 0..4 恒定不变。
///
/// 布局（索引 → 控件）：
///   0 Text「User info form」 · 1 TextInput(空) · 2 TextInput(alice@example.com)
///   3 Button「Submit」        · 4 Text（提交计数）
/// 索引 3 → 4 构成可远程观测的因果：`POST /api/input/click {"path":"3"}` 应使索引 4 的文本递增。
auto build_form(const std::shared_ptr<au::State<int>> &count,
                const std::shared_ptr<au::State<au::LocalizedString>> &label) -> au::Node {
    au::Button submit{au::ButtonProps{.label = "Submit"}};
    submit.on_click = [count, label]() -> void {
        const int next = count->get() + 1;
        count->set(next);
        label->set(au::LocalizedString{"submit count = " + std::to_string(next)});
    };

    return au::Node{au::Column{au::ColumnProps{
        .children = {
            au::Node{au::Text{"User info form"}},
            au::Node{au::TextInput{au::TextInputProps{.value = "", .placeholder = "Enter name"}}},
            au::Node{au::TextInput{au::TextInputProps{.value = "alice@example.com", .placeholder = "Enter email"}}},
            au::Node{std::move(submit)},
            au::Node{au::Text{au::TextProps{.content = au::Reactive{label}}}},
        }}}};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::enable_dpi_awareness();  // 必须在建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();

    auto count = std::make_shared<au::State<int>>(0);
    auto label = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"submit count = 0"});

    au::WindowOptions opts;
    opts.size = au::Size{.width = 620.0F, .height = 420.0F};
    opts.title = "Inspector server · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[inspector] window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{build_form(count, label)}, std::move(win_res.value()), opts};

    // 注入 live 根树与运行时 Surface：前者供树查询 / 属性读写 / 交互模拟，后者供截图与状态端点。
    au::InspectorServer server{[&app]() -> au::Node { return app.scene().root_node(); }};
    server.set_surface_getter([&app]() -> au::Surface * {
        auto *w = app.window();
        return w != nullptr ? &w->surface() : nullptr;
    });

    if (!server.start(AURORA_INSPECTOR_PORT)) {
        AURORA_LOG_ERROR("demo", "[inspector] failed to start on port ", static_cast<int>(AURORA_INSPECTOR_PORT));
        return -1;
    }
    AURORA_LOG_INFO("demo", "[inspector] listening on http://127.0.0.1:", static_cast<int>(AURORA_INSPECTOR_PORT));

    app.run();
    server.stop();
    return 0;
}

#else

// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    AURORA_LOG_INFO("demo", "[inspector] AURORA_BUILD_INSPECTOR_SERVER 未开启，跳过");
    return 0;
}

#endif
