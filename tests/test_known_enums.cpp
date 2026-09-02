// test_known_enums.cpp — 工具链共享枚举登记表 tools/include/known_enums.h 的守护测试。
//
// 存在的理由：登记表此前在 gen_api / aurora_mcp / aurora_cli / aurora_lsp 四份文件里各抄一份，
// 已静默漂移（LSP 的 Alignment 写成三值、四份都缺 DrawerSide 等真实属性类型、LengthKind 与
// ColorPalette 含代码中不存在的取值）。本测试从两端夹住它：
//   1) 编译期锚点：逐个引用登记表声称存在的真实枚举成员，任何改名 / 删除直接编译失败；
//   2) 运行期不变量：登记表键值非空，且库自描述里出现的每个「非基础属性类型」都有对应键
//      ——缺失即意味着 LSP / MCP 无法为该属性做补全与校验。
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/widget/serialization.h"

#include "../tools/include/known_enums.h"
#include "test_harness.h"

using aurora::list_all_schemas;
using aurora::serialization::register_core_widgets;

namespace {

/// @brief 编译期锚点：引用登记表每个键对应的真实枚举成员。
/// @note 这些名字与 tools/include/known_enums.h 中的取值逐字一致；枚举演进时两处必须同步修改，
///       否则本函数编译失败（正是我们想要的失败模式）。
[[maybe_unused]] auto enum_anchors() -> int {
    (void)&aurora::colors::AURORA_WHITE;       // ColorPalette
    (void)&aurora::colors::AURORA_TRANSPARENT; // ColorPalette
    return static_cast<int>(aurora::Alignment::BottomRight) + static_cast<int>(aurora::BoxFit::ScaleDown) +
           static_cast<int>(aurora::CrossAxisAlignment::Stretch) +
           static_cast<int>(aurora::MainAxisAlignment::SpaceEvenly) + static_cast<int>(aurora::MainAxisSize::Max) +
           static_cast<int>(aurora::StackFit::Passthrough) + static_cast<int>(aurora::LengthKind::Fraction) +
           static_cast<int>(aurora::LengthKind::Expand) + static_cast<int>(aurora::TextAlign::Justify) +
           static_cast<int>(aurora::TextOverflow::Fade) + static_cast<int>(aurora::TextDecoration::LineThrough) +
           static_cast<int>(aurora::FontWeight::ExtraBold) + static_cast<int>(aurora::FontStyle::Italic) +
           static_cast<int>(aurora::CurveKind::Custom) + static_cast<int>(aurora::KeyCode::D0) +
           static_cast<int>(aurora::KeyCode::Backquote) + static_cast<int>(aurora::KeyCode::F12) +
           static_cast<int>(aurora::DrawerSide::Right) + static_cast<int>(aurora::Orientation::Vertical) +
           static_cast<int>(aurora::SplitterOrientation::Horizontal) + static_cast<int>(aurora::ToastPosition::Top);
}

/// @brief 属性类型是否属于「不需要枚举登记」的基础 / 容器类型。
auto is_primitive_type(const std::string &t) -> bool {
    static const std::vector<std::string> base = { "float",      "int",   "bool",   "string",
                                                   "double",     "Color", "Length", "LocalizedString",
                                                   "EdgeInsets", "Json",  "any",    "std::string" };
    for (const auto &b : base) {
        if (t == b) {
            return true;
        }
    }
    // 容器与非枚举类型（vector<X>、std::function<...>、std::chrono::...）不参与枚举校验。
    return t.starts_with("vector") || t.find("std::") != std::string::npos;
}

auto has_value(const std::vector<std::string> &vals, const std::string &needle) -> bool {
    return std::ranges::any_of(vals, [&needle](const std::string &v) -> bool { return v == needle; });
}

// ---------- 1) 结构不变量 ----------

void test_registry_shape() {
    const auto reg = aurora::tools::known_enums();
    AURORA_TEST_REQUIRE(reg.size() >= 19);
    for (const auto &[name, vals] : reg) {
        AURORA_TEST_CHECK_MSG(!name.empty(), "enum registry key must not be empty");
        AURORA_TEST_CHECK_MSG(!vals.empty(), "enum registry value list must not be empty");
        for (const auto &v : vals) {
            AURORA_TEST_CHECK_MSG(!v.empty(), "enum registry member name must not be empty");
        }
    }
}

// ---------- 2) 历史错值不得复发 ----------

void test_registry_corrected_values() {
    const auto reg = aurora::tools::known_enums();

    // KeyCode：真实成员是 D0..D9，不是 Digit0/Digit9。
    AURORA_TEST_CHECK(has_value(reg.at("KeyCode"), "D0"));
    AURORA_TEST_CHECK(!has_value(reg.at("KeyCode"), "Digit0"));
    AURORA_TEST_CHECK(reg.at("KeyCode").size() == 78);

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
    AURORA_TEST_CHECK(reg.at("Alignment").size() == 9);
    AURORA_TEST_CHECK(!has_value(reg.at("Alignment"), "Leading"));
}

// ---------- 3) 属性类型覆盖率（静默失守的那一条）----------

void test_registry_covers_prop_types() {
    register_core_widgets();
    const auto reg = aurora::tools::known_enums();
    const auto schemas = list_all_schemas();
    AURORA_TEST_REQUIRE(!schemas.empty());

    std::vector<std::string> uncovered;
    std::size_t checked = 0;
    for (const auto &s : schemas) {
        if (!s.contains("prop_descriptors") || !s["prop_descriptors"].is_array()) {
            continue;
        }
        const std::string wtype = s.value("type", std::string{});
        for (const auto &p : s["prop_descriptors"]) {
            if (!p.contains("type") || !p["type"].is_string()) {
                continue;
            }
            const std::string t = p["type"].get<std::string>();
            if (t.empty() || is_primitive_type(t)) {
                continue;
            }
            ++checked;
            if (!reg.contains(t)) {
                auto key = wtype + '.';
                key += t;
                uncovered.push_back(std::move(key));
            }
        }
    }
    // 覆盖率断言仅在确实存在枚举型属性时生效，避免空注册表蒙混过关。
    AURORA_TEST_CHECK_MSG(checked > 0, "expected enum-typed props to be present in schemas");
    AURORA_TEST_CHECK_MSG(uncovered.empty(), "prop type missing from known_enums registry: " +
                                                 (uncovered.empty() ? std::string{} : uncovered.front()));
}

} // namespace

AURORA_TEST() {
    (void)enum_anchors();
    test_registry_shape();
    test_registry_corrected_values();
    test_registry_covers_prop_types();
}
