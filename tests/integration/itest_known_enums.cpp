/// 测试类型: integration
/// 目标单元: include/aurora/core/enums.h
/// 测试说明: 守护工具链共享枚举登记表 tools/include/known_enums.h——编译期锚点引用真实枚举成员、
///           登记表结构与历史错值不复发、库自描述的每个非基础属性类型都有登记键
/// 覆盖说明: 登记表被 gen_api / aurora_mcp / aurora_cli / aurora_lsp 四端消费，漂移即补全失效

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/animation/easing.h"
#include "aurora/aurora.h"
#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/event/keycode.h"
#include "aurora/widget/alignment.h"
#include "framework/aurora_test.h"
#include "known_enums.h"

namespace aurora::test_cases::itest_known_enums {

using au::serialization::register_core_widgets;

namespace {

/// @brief 编译期锚点：引用登记表每个键对应的真实枚举成员。
/// @note 这些名字与 tools/include/known_enums.h 中的取值逐字一致；枚举演进时两处必须同步修改，
///       否则本函数编译失败（正是我们想要的失败模式）。
[[maybe_unused]] auto enum_anchors() -> int {
    (void)&aurora::colors::AURORA_WHITE;  // ColorPalette
    (void)&aurora::colors::AURORA_TRANSPARENT;  // ColorPalette
    return static_cast<int>(aurora::Alignment::BottomRight) + static_cast<int>(au::BoxFit::ScaleDown) +
           static_cast<int>(au::CrossAxisAlignment::Stretch) + static_cast<int>(au::MainAxisAlignment::SpaceEvenly) +
           static_cast<int>(au::MainAxisSize::Max) + static_cast<int>(au::StackFit::Passthrough) +
           static_cast<int>(au::LengthKind::Fraction) + static_cast<int>(au::LengthKind::Expand) +
           static_cast<int>(au::TextAlign::Justify) + static_cast<int>(au::TextOverflow::Fade) +
           static_cast<int>(au::TextDecoration::LineThrough) + static_cast<int>(au::FontWeight::ExtraBold) +
           static_cast<int>(au::FontStyle::Italic) + static_cast<int>(au::CurveKind::Custom) +
           static_cast<int>(au::KeyCode::D0) + static_cast<int>(au::KeyCode::Backquote) +
           static_cast<int>(au::KeyCode::F12) + static_cast<int>(au::DrawerSide::Right) +
           static_cast<int>(au::Orientation::Vertical) + static_cast<int>(au::SplitterOrientation::Horizontal) +
           static_cast<int>(au::ToastPosition::Top);
}

/// @brief 属性类型是否属于「不需要枚举登记」的基础 / 容器类型。
auto is_primitive_type(const std::string& t) -> bool {
    static const std::vector<std::string> BASE = {"float",      "int",   "bool",   "string",
                                                  "double",     "Color", "Length", "LocalizedString",
                                                  "EdgeInsets", "Json",  "any",    "std::string"};
    for (const auto& b : BASE) {
        if (t == b) {
            return true;
        }
    }
    // 容器与非枚举类型（vector<X>、std::function<...>、std::chrono::...）不参与枚举校验。
    return t.starts_with("vector") || t.find("std::") != std::string::npos;
}

auto has_value(const std::vector<std::string>& vals, const std::string& needle) -> bool {
    return std::ranges::any_of(vals, [&needle](const std::string& v) -> bool { return v == needle; });
}

}  // namespace

AURORA_TEST_CASE(known_enum_members_compile_anchors) {
    // 引用登记表声称存在的每个真实成员：任何改名/删除直接编译失败；运行期仅求和防优化。
    AURORA_TEST_CHECK(enum_anchors() >= 0);
}

AURORA_TEST_CASE(known_enums_registry_shape_is_valid) {
    const auto reg = aurora::tools::known_enums();
    AURORA_TEST_REQUIRE(reg.size() >= 19);
    for (const auto& [name, vals] : reg) {
        AURORA_TEST_CHECK_MSG(!name.empty(), "enum registry key must not be empty");
        AURORA_TEST_CHECK_MSG(!vals.empty(), "enum registry value list must not be empty");
        for (const auto& v : vals) {
            AURORA_TEST_CHECK_MSG(!v.empty(), "enum registry member name must not be empty");
        }
    }
}

AURORA_TEST_CASE(known_enums_historical_wrong_values_do_not_return) {
    const auto reg = aurora::tools::known_enums();

    // KeyCode：真实成员是 D0..D9，不是 Digit0/Digit9。
    AURORA_TEST_CHECK(has_value(reg.at("KeyCode"), "D0"));
    AURORA_TEST_CHECK(!has_value(reg.at("KeyCode"), "Digit0"));
    AURORA_TEST_CHECK(static_cast<int>(reg.at("KeyCode").size()) == 78);

    // LengthKind：四态是 WrapContent / Expand / Fixed / Fraction。
    AURORA_TEST_CHECK(has_value(reg.at("LengthKind"), "Expand"));
    AURORA_TEST_CHECK(has_value(reg.at("LengthKind"), "Fraction"));
    AURORA_TEST_CHECK(!has_value(reg.at("LengthKind"), "Fill"));
    AURORA_TEST_CHECK(!has_value(reg.at("LengthKind"), "Percent"));

    // Curve：取值来自 CurveKind，代码里没有 Decelerate / Spring。
    AURORA_TEST_CHECK(has_value(reg.at("Curve"), "EaseInOutCubic"));
    AURORA_TEST_CHECK(!has_value(reg.at("Curve"), "Decelerate"));
    AURORA_TEST_CHECK(!has_value(reg.at("Curve"), "Spring"));

    // ColorPalette：必须是可编译的 au::colors::AURORA_* 常量名。
    AURORA_TEST_CHECK(has_value(reg.at("ColorPalette"), "AURORA_RED"));
    AURORA_TEST_CHECK(!has_value(reg.at("ColorPalette"), "Red"));
    AURORA_TEST_CHECK(!has_value(reg.at("ColorPalette"), "Cyan"));

    // Alignment：九方位，不是 Flutter 的 Leading / Trailing 三值。
    AURORA_TEST_CHECK(static_cast<int>(reg.at("Alignment").size()) == 9);
    AURORA_TEST_CHECK(!has_value(reg.at("Alignment"), "Leading"));
}

AURORA_TEST_CASE(known_enums_cover_all_enum_typed_props) {
    register_core_widgets();
    const auto reg = aurora::tools::known_enums();
    const auto schemas = au::list_all_schemas();
    AURORA_TEST_REQUIRE(!schemas.empty());

    std::vector<std::string> uncovered;
    std::size_t checked = 0;
    for (const auto& s : schemas) {
        if (!s.contains("prop_descriptors") || !s["prop_descriptors"].is_array()) {
            continue;
        }
        const std::string wtype = s.value("type", std::string{});
        for (const auto& p : s["prop_descriptors"]) {
            if (!p.contains("type") || !p["type"].is_string()) {
                continue;
            }
            const std::string t = p["type"].get<std::string>();
            if (t.empty() || is_primitive_type(t)) {
                continue;
            }
            ++checked;
            if (!reg.contains(t)) {
                std::string item;
                item.reserve(wtype.size() + t.size() + 1U);
                item += wtype;
                item += '.';
                item += t;
                uncovered.push_back(std::move(item));
            }
        }
    }
    // 覆盖率断言仅在确实存在枚举型属性时生效，避免空注册表蒙混过关。
    AURORA_TEST_CHECK_MSG(checked > 0, "expected enum-typed props to be present in schemas");
    AURORA_TEST_CHECK_MSG(uncovered.empty(), "prop type missing from known_enums registry: " +
                                                 (uncovered.empty() ? std::string{} : uncovered.front()));
}

}  // namespace aurora::test_cases::itest_known_enums
