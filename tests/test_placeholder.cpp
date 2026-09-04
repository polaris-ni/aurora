// placeholder_test.cpp — 覆盖占位工具（au::TODO 回调 / aurora::Placeholder 降级控件）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// todo.h（au::TODO 回调桩 + Placeholder 降级控件）。

#include <string>

#include "aurora/aurora.h"
#include "aurora/todo.h"
#include "aurora/widget/placeholder.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Diagnostics;
using aurora::Json;
using aurora::Placeholder;
using aurora::Size;
using aurora::TODO;

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
    AURORA_TEST_CHECK_MSG(j["border_color"][1].get<int>() == Color::green().m_g, "ph border green");
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
    AURORA_TEST_CHECK_MSG(k["border_color"][1].get<int>() == Color::green().m_g, "ph rt border");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== placeholder_test ===\n");
    test_todo();
    test_placeholder_widget();
    test_placeholder_colors();
}