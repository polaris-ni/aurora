/// 测试类型: unit
/// 目标单元: include/aurora/widget/placeholder.h
/// 测试说明: placeholder 单元测试
///

// placeholder_test.cpp — 覆盖占位工具（au::TODO 回调 / aurora::Placeholder 降级控件）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// todo.h（au::TODO 回调桩 + Placeholder 降级控件）；§8.4 半成品容错验收闭环
// （只填 label 的控件 / TODO 回调界面：可编译、可渲染、可运行、留可读警告）。

#include <filesystem>
#include <functional>
#include <string>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/test_helpers.h"
#include "aurora/todo.h"
#include "aurora/ui/factories.h"
#include "aurora/widget/button.h"
#include "aurora/widget/placeholder.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_placeholder {

using au::test::expect_tree_contains;
using au::test::init_headless;
using au::test::pump;
using au::test::tap;
using au::test::TestEnv;



static void test_todo() {
    // au::TODO 可转换为任意回调签名，调用时仅记录警告而不崩溃
    Diagnostics::take();  // 清空已有诊断
    const std::function<void()> fn = TODO("wire save logic");
    fn();
    AURORA_TEST_CHECK_MSG(Diagnostics::count() >= 1, "au::TODO: invocation records a warning, no crash");

    const std::function<int(int)> fn2 = TODO("compute");
    const int out = fn2(5);
    (void)out;
    AURORA_TEST_CHECK_MSG(true, "au::TODO: converts to non-void signature");
}

static void test_placeholder_widget() {
    Placeholder ph{"missing feature"};
    AURORA_TEST_CHECK_MSG(std::string(ph.type_name()) == "Placeholder", "Placeholder widget: type_name");
    const BuildContext ctx;
    const Size s =
        ph.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 200}}, ctx);
    AURORA_TEST_CHECK_MSG(s.width > 0 && s.height > 0, "Placeholder widget: layout non-zero");

    const Placeholder empty;
    AURORA_TEST_CHECK_MSG(empty.type_name() != nullptr, "Placeholder widget: default ctor");
}

static void test_placeholder_colors() {
    Placeholder p;
    p.set_message("x")
        .set_background_color(Color::red())
        .set_border_color(Color::green())
        .set_text_color(Color::blue());
    Json j;
    p.serialize_props(j);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["message"].get<std::string>() == "x", "ph message");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["background_color"][0].get<int>() == 255, "ph bg red");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["border_color"][1].get<int>() == Color::green().g, "ph border green");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["text_color"][2].get<int>() == 255, "ph text blue");

    Placeholder q;
    q.deserialize_props(j);
    Json k;
    q.serialize_props(k);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(k["background_color"][0].get<int>() == 255, "ph rt bg");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(k["border_color"][1].get<int>() == Color::green().g, "ph rt border");
}

// §8.4 验收：Placeholder 降级控件在无头环境走完整 mount/layout/paint 流程不崩溃，
// 且真实落盘 PNG（「缺少必要属性的组件渲染为占位框而非崩溃」）。
static void test_placeholder_renders() {
    TestEnv env = init_headless(240, 160);
    env.root_widget->add(Node(std::make_shared<Placeholder>("not implemented yet")));
    pump(env);
    expect_tree_contains(env.root, "Placeholder");

    const auto kids = env.root_widget->child_nodes();
    AURORA_TEST_CHECK_MSG(!kids.empty(), "placeholder: mounted into tree");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（刚断言非空）
    const auto box = kids[0].bounds();
    AURORA_TEST_CHECK_MSG(box.size.width > 0.0F && box.size.height > 0.0F,
                          "placeholder: laid out at non-zero size in tree");

    const auto png = std::filesystem::temp_directory_path() / "aurora_placeholder_render.png";
    const auto r = render_to_png(env.root, 240, 160, png.string().c_str());
    AURORA_TEST_CHECK_MSG(r.ok() && std::filesystem::exists(png),
                          "placeholder: full paint path renders to PNG without crashing");
    std::error_code ec;
    std::filesystem::remove(png, ec);
}

// §8.4 验收：只填了 label 的 Button + au::TODO 回调——可渲染、可运行（点击不崩溃）、
// 触发后留下可读警告（message=TODO、where 指明未完成处）。
static void test_half_baked_button_todo() {
    (void)Diagnostics::take();  // 清空基线诊断
    TestEnv env = init_headless(200, 100);
    // TODO 隐式转换为 std::function<void()>：半成品界面的占位事件处理。
    Button const *b = au::ui::button(*env.root_widget, "Save", {}, TODO("wire save logic"));
    pump(env);
    expect_tree_contains(env.root, "Button");

    (void)Diagnostics::take();  // 清空布局期诊断，只观察点击触发的警告
    tap(env, *b);  // 触发 TODO 回调：不得崩溃
    const auto diags = Diagnostics::take();
    bool found_todo = false;
    for (const auto &d : diags) {
        if (d.message == "TODO" && d.where.find("wire save logic") != std::string::npos) {
            found_todo = true;
        }
    }
    AURORA_TEST_CHECK_MSG(found_todo,
                          "half-baked button: TODO click leaves a readable warning naming the missing handler");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== placeholder_test ===\n");
    test_todo();
    test_placeholder_widget();
    test_placeholder_colors();
    test_placeholder_renders();
    test_half_baked_button_todo();
}

}  // namespace aurora::test_cases::utest_placeholder