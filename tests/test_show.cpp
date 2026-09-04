// test_show.cpp — Show 控件 1:1 测试：可见性绑定、State 驱动、布局与序列化。
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Json;
using aurora::Node;
using aurora::Show;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::State;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_show ===\n");

    constexpr BuildContext ctx;
    constexpr Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 200.0F}};

    // 1) 构造为 true 时可见。
    Show show_true{true, Node{Text{"hi"}}};
    AURORA_TEST_CHECK(show_true.is_visible());

    // 2) 构造为 false 时不可见。
    Show show_false{false, Node{Text{"hi"}}};
    AURORA_TEST_CHECK(!show_false.is_visible());

    // 3) 由 State<bool>（true）驱动可见。
    const auto vis = std::make_shared<State<bool>>(true);
    Show show_state{vis, Node{Text{"x"}}};
    AURORA_TEST_CHECK(show_state.is_visible());

    // 4) State 置 false → 不可见。
    vis->set(false);
    AURORA_TEST_CHECK(!show_state.is_visible());

    // 5) State 切回 true → 重新可见。
    vis->set(true);
    AURORA_TEST_CHECK(show_state.is_visible());

    // 6) type_name 为 "Show"。
    AURORA_TEST_CHECK(std::string(show_true.type_name()) == "Show");

    // 7) 隐藏时 layout 返回 0x0（不占空间）。
    show_false.mount(ctx);
    const Size hidden = show_false.layout(c, ctx);
    AURORA_TEST_CHECK(hidden.width == 0.0F && hidden.height == 0.0F);

    // 8) 可见时 layout 受父约束（尺寸落在约束范围内）。
    show_true.mount(ctx);
    const Size visible = show_true.layout(c, ctx);
    AURORA_TEST_CHECK(visible.width >= 0.0F && visible.width <= 200.0F);
    AURORA_TEST_CHECK(visible.height >= 0.0F && visible.height <= 200.0F);

    // 9) 用 State 构造时 collect_signals 含该信号。
    std::vector<SignalViewBase *> sigs;
    show_state.collect_signals(sigs);
    AURORA_TEST_CHECK(sigs.size() == 1);

    // 10) serialize_props 写入 "visible" 键。
    Json props = Json::object();
    show_true.serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(props.contains("visible") && props["visible"].get<bool>() == true);

    // 11) adopt_children 接管新子节点（序列化后 children 反映新子节点）。
    Show show_adopt{false, Node{Text{"old"}}};
    show_adopt.adopt_children(std::vector{Node{Text{"new"}}});
    auto aj = dump_tree_json(Node{std::move(show_adopt)});
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(aj.contains("children") && aj["children"].is_array());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(aj["children"].size() == 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(aj["children"][0]["type"].get<std::string>() == "Text");

    // 12) 嵌套：外层隐藏不影响内层结构（内层 Show 仍存在；可见性由各自 State 决定）。
    const auto inner_vis = std::make_shared<State<bool>>(true);
    Show outer{false, Node{Show{inner_vis, Node{Text{"inner"}}}}};
    AURORA_TEST_CHECK(!outer.is_visible());
    const auto sjt = get_state("children/0/type", Node{std::move(outer)});
    AURORA_TEST_CHECK(sjt.is_string() && sjt.get<std::string>() == "Show");
}