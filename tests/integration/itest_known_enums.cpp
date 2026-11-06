/// 测试类型: integration
/// 目标单元: include/aurora/core/enums.h
/// 测试说明: 守护工具链共享枚举登记表 tools/include/known_enums.h——编译期锚点引用真实枚举成员、
///           登记表结构与历史错值不复发、库自描述的每个可枚举属性类型都有登记键
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
#include "framework/json_access.h"
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
           static_cast<int>(au::CrossAxisAlignment::Stretch) + static_cast<int>(au::CrossAxisAlignment::Baseline) +
           static_cast<int>(au::MainAxisAlignment::SpaceEvenly) + static_cast<int>(au::MainAxisSize::Max) +
           static_cast<int>(au::StackFit::Passthrough) + static_cast<int>(au::LengthKind::Fraction) +
           static_cast<int>(au::LengthKind::Expand) + static_cast<int>(au::TextAlign::Justify) +
           static_cast<int>(au::TextDirection::RTL) + static_cast<int>(au::TextOverflow::Fade) +
           static_cast<int>(au::TextDecoration::LineThrough) + static_cast<int>(au::FontWeight::ExtraBold) +
           static_cast<int>(au::FontStyle::Italic) + static_cast<int>(au::CurveKind::Custom) +
           static_cast<int>(au::KeyCode::D0) + static_cast<int>(au::KeyCode::Backquote) +
           static_cast<int>(au::KeyCode::F12) + static_cast<int>(au::DrawerSide::Right) +
           static_cast<int>(au::Orientation::Vertical) + static_cast<int>(au::SplitterOrientation::Horizontal) +
           static_cast<int>(au::ToastPosition::Top) + static_cast<int>(au::LegendPosition::Right) +
           static_cast<int>(aurora::net::AddressFamily::LocalPath) +
           static_cast<int>(aurora::net::ConnectivityChangeCause::StalenessMarked) +
           static_cast<int>(aurora::net::ConnectivitySource::LinkFiles) +
           static_cast<int>(aurora::net::NetworkState::Offline) +
           static_cast<int>(aurora::net::LengthByteOrder::Little) + static_cast<int>(aurora::net::ProxyKind::Socks5) +
           static_cast<int>(aurora::net::ShutdownMode::Both) + static_cast<int>(aurora::net::TlsVersion::Tls13) +
           static_cast<int>(aurora::net::WsMessageKind::Binary) + static_cast<int>(aurora::net::http::Method::Delete) +
           static_cast<int>(aurora::net::http::ProgressPhase::Download) +
           static_cast<int>(aurora::net::http::SinkFlow::Stop) +
           static_cast<int>(aurora::net::http::ExchangeLeg::WebSocket) +
           static_cast<int>(aurora::net::http::HttpErrorClass::Cancelled) +
           static_cast<int>(aurora::net::http::AuthScheme::Unsupported) +
           static_cast<int>(aurora::net::ExportSurface::InspectorJson) +
           static_cast<int>(aurora::net::FieldClass::Secret) +
           static_cast<int>(aurora::net::http::HttpStage::BodyDone) + static_cast<int>(aurora::ResourcePhase::Failed) +
           static_cast<int>(aurora::StreamPolicy::Replace) + static_cast<int>(au::webview::WebViewMode::Snapshot) +
           static_cast<int>(au::ThemeStyle::Kde) + static_cast<int>(au::ThemeStyle::MacOs) +
           static_cast<int>(au::DesktopEnvironment::Other) + static_cast<int>(au::DesktopEnvironment::Gnome) +
           static_cast<int>(au::ColorScheme::Unspecified) + static_cast<int>(au::ColorScheme::Dark);
}

/// @brief 属性类型是否属于「不参与枚举登记」的一类：标量 / 容器 / 值对象结构体。
/// @note 登记表 `tools/include/known_enums.h` 的键必须能列出成员名（工具链据此做补全与校验），
///       值对象只有字段而非成员名，进表即让 LSP / MCP 建议出编译不过的取值，故按类别豁免。
///       新增裸结构体属性会让覆盖率用例转红，由作者显式把类型名补进 VALUE 一档 ——
///       这是刻意的 fail-safe（漏登记会响），不是豁免通道。
auto is_non_enumerable_type(const std::string &t) -> bool {
    // 标量与以标量 / 字符串承载的类型；`Json` 是「值原样透传」属性的历史写法。
    static const std::vector<std::string> SCALAR = {"float",  "int",  "bool", "string",
                                                    "double", "Json", "any",  "std::string"};
    // 值对象结构体：有字段、无成员名，序列化面自成一体。
    static const std::vector<std::string> VALUE = {"Color",      "Length",        "LocalizedString",
                                                   "EdgeInsets", "ChartAxisSpec", "ChartLegendSpec"};
    for (const auto &b : SCALAR) {
        if (t == b) {
            return true;
        }
    }
    for (const auto &v : VALUE) {
        if (t == v) {
            return true;
        }
    }
    // 容器与非枚举类型（vector<X>、std::function<...>、std::chrono::...）不参与枚举校验。
    return t.starts_with("vector") || t.find("std::") != std::string::npos;
}

auto has_value(const std::vector<std::string> &vals, const std::string &needle) -> bool {
    return std::ranges::any_of(vals, [&needle](const std::string &v) -> bool { return v == needle; });
}

}  // namespace

AURORA_TEST_CASE(known_enum_members_compile_anchors) {
    // 引用登记表声称存在的每个真实成员：任何改名/删除直接编译失败；运行期仅求和防优化。
    AURORA_TEST_CHECK(enum_anchors() >= 0);
}

AURORA_TEST_CASE(known_enums_registry_shape_is_valid) {
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

    // CrossAxisAlignment：五态；末位 Baseline 是「Row 内按文本首行基线对齐」，不是 Flutter 的
    // textBaseline 分档（alphabetic / ideographic 在 Aurora 未区分）。
    AURORA_TEST_CHECK(static_cast<int>(reg.at("CrossAxisAlignment").size()) == 5);
    AURORA_TEST_CHECK(has_value(reg.at("CrossAxisAlignment"), "Baseline"));
}

AURORA_TEST_CASE(theme_axes_are_both_enumerable) {
    // 两轴正交模型在代码生成面上的钉子：风格轴与明暗轴必须同时可枚举，且风格轴里不含明暗位。
    const auto reg = aurora::tools::known_enums();
    AURORA_TEST_REQUIRE(reg.count("ThemeStyle") == 1);
    AURORA_TEST_REQUIRE(reg.count("DesktopEnvironment") == 1);
    AURORA_TEST_REQUIRE(reg.count("ColorScheme") == 1);

    AURORA_TEST_CHECK(static_cast<int>(reg.at("ThemeStyle").size()) == 5);
    AURORA_TEST_CHECK(has_value(reg.at("ThemeStyle"), "MacOs"));  // 拼写随 PlatformKind::MacOs
    AURORA_TEST_CHECK(!has_value(reg.at("ThemeStyle"), "MacOS"));
    AURORA_TEST_CHECK(!has_value(reg.at("ThemeStyle"), "Unknown"));  // 无证据回落 Material，不设不可用档
    for (const auto &v : reg.at("ThemeStyle")) {
        AURORA_TEST_CHECK_MSG(v.find("Dark") == std::string::npos, "style axis must not carry a brightness bit");
        AURORA_TEST_CHECK_MSG(v.find("Light") == std::string::npos, "style axis must not carry a brightness bit");
    }
    // 明暗三态不塌：Unspecified 必须在登记值里，否则 AI 侧无法表达「本层不表态」。
    AURORA_TEST_CHECK(static_cast<int>(reg.at("ColorScheme").size()) == 3);
    AURORA_TEST_CHECK(has_value(reg.at("ColorScheme"), "Unspecified"));
}

AURORA_TEST_CASE(known_enums_cover_all_enum_typed_props) {
    register_core_widgets();
    const auto reg = aurora::tools::known_enums();
    const auto schemas = au::list_all_schemas();
    AURORA_TEST_REQUIRE(!schemas.empty());

    std::vector<std::string> uncovered;
    std::size_t checked = 0;
    for (const auto &s : schemas) {
        const auto *descriptors = s.find("props");
        if (descriptors == nullptr || !descriptors->is_array()) {
            continue;
        }
        const auto wtype = s.as_or<std::string>("type", "");
        for (const auto &descriptor : *descriptors) {
            if (!descriptor.contains("type") || !descriptor.at("type")->is_string()) {
                continue;
            }
            const auto t = descriptor.at("type")->as_or<std::string>("");
            if (t.empty() || is_non_enumerable_type(t)) {
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
