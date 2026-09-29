#pragma once

#include <cstdint>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/core/json.h"
#include "aurora/core/types.h"
#include "aurora/widget/scroll_viewport.h"

/// @brief Aurora 命名空间：本头承载属性值 <-> JSON 的双向序列化自由函数。
namespace aurora {

/// @brief 序列化所用的 JSON 值类型别名（aurora 自研 core/json::Value）。
using Json = json::Value;

/// @brief 强类型尺寸意图 → JSON：["px",v] / ["percent",v] / "fill" / "auto"。
/// @param len 尺寸意图；WrapContent/Expand 编为字符串，Fixed/Fraction 编为二元数组（kind + 数值）。
/// @return 编码后的 JSON 值；kind 越界等异常状态回退 "auto"。
[[nodiscard]] inline auto length_to_json(const Length &len) -> Json {
    switch (len.kind) {
        case LengthKind::WrapContent:
            return "auto";
        case LengthKind::Expand:
            return "fill";
        case LengthKind::Fixed: {
            Json a = Json::array();
            a.push_back("px");
            a.push_back(len.value);
            return a;
        }
        case LengthKind::Fraction: {
            Json a = Json::array();
            a.push_back("percent");
            a.push_back(len.value);
            return a;
        }
    }
    return "auto";
}

/// @brief JSON → 强类型尺寸意图（解析 lengthToJson 的输出）。
/// @param j 序列化产物："auto"/"fill" 或 ["px"|"percent", 数值] 二元数组。
/// @return 对应 Length；无法识别的字符串、数组形态一律回退 wrap（按内容自适应）。
[[nodiscard]] inline auto json_to_length(const Json &j) -> Length {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "auto") {
            return Length::wrap();
        }
        if (s == "fill") {
            return Length::expand();
        }
        return Length::wrap();
    }
    if (j.is_array() && j.size() == 2) {
        const auto kind = j.as_or_at<std::string>(0, "");
        const auto v = j.as_or_at<float>(1, 0.0F);
        if (kind == "px") {
            return Length::fixed(v);
        }
        if (kind == "percent") {
            return Length::ratio(v);
        }
    }
    return Length::wrap();
}

/// @brief 颜色 → JSON：[r,g,b,a]（0-255）。
/// @param c 颜色分量。
/// @return 四元素整数数组。
[[nodiscard]] inline auto color_to_json(const Color &c) -> Json {
    Json a = Json::array();
    a.push_back(c.r);
    a.push_back(c.g);
    a.push_back(c.b);
    a.push_back(c.a);
    return a;
}

/// @brief JSON → 颜色（解析 colorToJson 的输出；格式不符回退黑色）。
/// @param j 四元（及以上）整数数组 [r,g,b,a]，各分量 0-255。
/// @return 解码颜色；非数组或元素不足 4 个时返回黑色。
[[nodiscard]] inline auto json_to_color(const Json &j) -> Color {
    if (j.is_array() && j.size() >= 4) {
        return Color{static_cast<std::uint8_t>(j.as_or_at<std::int32_t>(0, 0)),
                     static_cast<std::uint8_t>(j.as_or_at<std::int32_t>(1, 0)),
                     static_cast<std::uint8_t>(j.as_or_at<std::int32_t>(2, 0)),
                     static_cast<std::uint8_t>(j.as_or_at<std::int32_t>(3, 0))};
    }
    return Color::black();
}

/// @brief EdgeInsets -> JSON 对象 {left,top,right,bottom}。
/// @param e 四边内边距。
/// @return 含 left/top/right/bottom 四键的 JSON 对象。
[[nodiscard]] inline auto edge_insets_to_json(const EdgeInsets &e) -> Json {
    Json o = Json::object();
    o.set("left", e.left);
    o.set("top", e.top);
    o.set("right", e.right);
    o.set("bottom", e.bottom);
    return o;
}

/// @brief JSON -> EdgeInsets（解析 edge_insets_to_json 输出；缺字段回退 0）。
/// @param j {left,top,right,bottom} 对象；非对象输入整体视为缺省。
/// @return 各边内边距；缺失的键取 0。
[[nodiscard]] inline auto json_to_edge_insets(const Json &j) -> EdgeInsets {
    EdgeInsets e{};
    if (j.is_object()) {
        e.left = j.as_or<float>("left", 0.0F);
        e.top = j.as_or<float>("top", 0.0F);
        e.right = j.as_or<float>("right", 0.0F);
        e.bottom = j.as_or<float>("bottom", 0.0F);
    }
    return e;
}

// ---------- 共享枚举 <-> JSON（参考 Flutter 命名） ----------

/// @brief TextAlign -> JSON 字符串。
/// @param v 对齐方式枚举值。
/// @return 与枚举同名的字符串（如 "Center"）；未知状态回退 "Left"。
[[nodiscard]] inline auto text_align_to_json(TextAlign v) -> Json {
    switch (v) {
        case TextAlign::Left:
            return "Left";
        case TextAlign::Right:
            return "Right";
        case TextAlign::Center:
            return "Center";
        case TextAlign::Start:
            return "Start";
        case TextAlign::End:
            return "End";
        case TextAlign::Justify:
            return "Justify";
    }
    return "Left";
}

/// @brief JSON -> TextAlign（未知值回退 Left）。
/// @param j 对齐方式名字字符串；非字符串输入直接回退。
/// @return 匹配的 TextAlign，未知名返回 Left。
[[nodiscard]] inline auto json_to_text_align(const Json &j) -> TextAlign {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Left") {
            return TextAlign::Left;
        }
        if (s == "Right") {
            return TextAlign::Right;
        }
        if (s == "Center") {
            return TextAlign::Center;
        }
        if (s == "Start") {
            return TextAlign::Start;
        }
        if (s == "End") {
            return TextAlign::End;
        }
        if (s == "Justify") {
            return TextAlign::Justify;
        }
    }
    return TextAlign::Left;
}

/// @brief TextDirection -> JSON 字符串。
/// @param v 文字方向枚举值。
/// @return "LTR" 或 "RTL"；未知状态回退 "LTR"。
[[nodiscard]] inline auto text_direction_to_json(TextDirection v) -> Json {
    switch (v) {
        case TextDirection::LTR:
            return "LTR";
        case TextDirection::RTL:
            return "RTL";
    }
    return "LTR";
}

/// @brief JSON -> TextDirection（未知值回退 LTR）。
/// @param j "RTL" 字符串以外的任何输入都按 LTR 处理。
/// @return 匹配的方向；未知名返回 LTR。
[[nodiscard]] inline auto json_to_text_direction(const Json &j) -> TextDirection {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "RTL") {
            return TextDirection::RTL;
        }
    }
    return TextDirection::LTR;
}

/// @brief TextOverflow -> JSON 字符串。
/// @param v 溢出策略枚举值。
/// @return "Clip"/"Ellipsis"/"Fade" 之一；未知状态回退 "Clip"。
[[nodiscard]] inline auto text_overflow_to_json(TextOverflow v) -> Json {
    switch (v) {
        case TextOverflow::Clip:
            return "Clip";
        case TextOverflow::Ellipsis:
            return "Ellipsis";
        case TextOverflow::Fade:
            return "Fade";
    }
    return "Clip";
}

/// @brief JSON -> TextOverflow（未知值回退 Clip）。
/// @param j 策略名字符串；非字符串输入直接回退。
/// @return 匹配的策略；未知名返回 Clip。
[[nodiscard]] inline auto json_to_text_overflow(const Json &j) -> TextOverflow {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Clip") {
            return TextOverflow::Clip;
        }
        if (s == "Ellipsis") {
            return TextOverflow::Ellipsis;
        }
        if (s == "Fade") {
            return TextOverflow::Fade;
        }
    }
    return TextOverflow::Clip;
}

/// @brief FontWeight -> JSON（数值字符串，如 "700"）。
/// @param v 字重枚举值（底层即 100-900 的数值）。
/// @return 该数值的字符串形式。
[[nodiscard]] inline auto font_weight_to_json(FontWeight v) -> Json { return std::to_string(static_cast<int>(v)); }

/// @brief JSON -> FontWeight（按数值匹配，未知回退 Normal）。
/// @param j 数值或数值字符串形式的字重（100-900 档位）。
/// @return 对应档位；非法数值串或非匹配值返回 Normal(400)。
/// @note 字符串经 Inspector PUT / JSON 文件加载等不可信通道进入，非法数值串
///       （如 "bold"）此前裸调 `std::stoi()` 抛 `std::invalid_argument` 可致崩溃，现回退默认。
[[nodiscard]] inline auto json_to_font_weight(const Json &j) -> FontWeight {
    int w = 400;  // 字重数值，解析失败时保持默认 Normal(400)。
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (!s.empty()) {
            try {
                w = std::stoi(s);
            } catch (...) {
                w = 400;
            }
        }
    } else if (j.is_number()) {
        w = static_cast<int>(j.as_or<double>(0.0));
    }
    switch (w) {
        case 100:
            return FontWeight::Thin;
        case 200:
            return FontWeight::ExtraLight;
        case 300:
            return FontWeight::Light;
        case 400:
            return FontWeight::Normal;
        case 500:
            return FontWeight::Medium;
        case 600:
            return FontWeight::SemiBold;
        case 700:
            return FontWeight::Bold;
        case 800:
            return FontWeight::ExtraBold;
        case 900:
            return FontWeight::Black;
        default:
            return FontWeight::Normal;
    }
}

/// @brief FontStyle -> JSON 字符串。
/// @param v 字体风格枚举值。
/// @return "Italic" 或 "Normal"。
[[nodiscard]] inline auto font_style_to_json(FontStyle v) -> Json {
    return v == FontStyle::Italic ? "Italic" : "Normal";
}

/// @brief JSON -> FontStyle（未知值回退 Normal）。
/// @param j "Italic" 字符串以外的任何输入都按 Normal 处理。
/// @return 匹配的风格；未知名返回 Normal。
[[nodiscard]] inline auto json_to_font_style(const Json &j) -> FontStyle {
    if (j.is_string() && j.as_or<std::string>("") == "Italic") {
        return FontStyle::Italic;
    }
    return FontStyle::Normal;
}

/// @brief TextDecoration -> JSON（按位组合序列化为启用的字符串数组）。
/// @param v 装饰位标志组合。
/// @return 启用的装饰名字数组；None 时输出 ["None"]。
[[nodiscard]] inline auto text_decoration_to_json(TextDecoration v) -> Json {
    Json a = Json::array();
    if (v == TextDecoration::None) {
        a.push_back("None");
        return a;
    }
    if (decoration_has(v, TextDecoration::Underline)) {
        a.push_back("Underline");
    }
    if (decoration_has(v, TextDecoration::Overline)) {
        a.push_back("Overline");
    }
    if (decoration_has(v, TextDecoration::LineThrough)) {
        a.push_back("LineThrough");
    }
    return a;
}

/// @brief JSON -> TextDecoration（接受字符串数组或单个字符串；未知项忽略）。
/// @param j 装饰名数组或单个装饰名；含 "None" 时清空其余位。
/// @return 按位合并后的装饰组合；无匹配项时为 None。
[[nodiscard]] inline auto json_to_text_decoration(const Json &j) -> TextDecoration {
    auto result = TextDecoration::None;
    auto add = [&](const std::string &s) -> void {
        if (s == "None") {
            result = TextDecoration::None;
        } else if (s == "Underline") {
            result |= TextDecoration::Underline;
        } else if (s == "Overline") {
            result |= TextDecoration::Overline;
        } else if (s == "LineThrough") {
            result |= TextDecoration::LineThrough;
        }
    };
    if (j.is_array()) {
        for (const auto &item : j) {
            if (item.is_string()) {
                add(item.as_or<std::string>(""));
            }
        }
    } else if (j.is_string()) {
        add(j.as_or<std::string>(""));
    }
    return result;
}

/// @brief MainAxisSize -> JSON 字符串。
/// @param v 主轴尺寸意图枚举值。
/// @return "Max" 或 "Min"。
[[nodiscard]] inline auto main_axis_size_to_json(MainAxisSize v) -> Json {
    return v == MainAxisSize::Max ? "Max" : "Min";
}

/// @brief ScrollSnapAlignment -> JSON 字符串（snap/paging 吸附方位，三控件共享）。
/// @param v 吸附方位枚举值。
/// @return "Start"/"Center"/"End" 之一；未知状态回退 "Start"。
[[nodiscard]] inline auto snap_alignment_to_json(ScrollSnapAlignment v) -> Json {
    switch (v) {
        case ScrollSnapAlignment::Start:
            return "Start";
        case ScrollSnapAlignment::Center:
            return "Center";
        case ScrollSnapAlignment::End:
            return "End";
    }
    return "Start";
}

/// @brief JSON -> ScrollSnapAlignment（未知值回退 Start）。
/// @param j 方位名字符串；非字符串输入直接回退。
/// @return 匹配的方位；未知名返回 Start。
[[nodiscard]] inline auto json_to_snap_alignment(const Json &j) -> ScrollSnapAlignment {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Center") {
            return ScrollSnapAlignment::Center;
        }
        if (s == "End") {
            return ScrollSnapAlignment::End;
        }
    }
    return ScrollSnapAlignment::Start;
}

/// @brief JSON -> MainAxisSize（未知值回退 Min）。
/// @param j "Max" 字符串以外的任何输入都按 Min 处理。
/// @return 匹配的尺寸意图；未知名返回 Min。
[[nodiscard]] inline auto json_to_main_axis_size(const Json &j) -> MainAxisSize {
    if (j.is_string() && j.as_or<std::string>("") == "Max") {
        return MainAxisSize::Max;
    }
    return MainAxisSize::Min;
}

/// @brief MainAxisAlignment -> JSON 字符串。
/// @param v 主轴对齐枚举值。
/// @return 与枚举同名的字符串；未知状态回退 "Start"。
[[nodiscard]] inline auto main_axis_alignment_to_json(MainAxisAlignment v) -> Json {
    switch (v) {
        case MainAxisAlignment::Start:
            return "Start";
        case MainAxisAlignment::Center:
            return "Center";
        case MainAxisAlignment::End:
            return "End";
        case MainAxisAlignment::SpaceBetween:
            return "SpaceBetween";
        case MainAxisAlignment::SpaceAround:
            return "SpaceAround";
        case MainAxisAlignment::SpaceEvenly:
            return "SpaceEvenly";
    }
    return "Start";
}

/// @brief JSON -> MainAxisAlignment（未知值回退 Start）。
/// @param j 对齐名字符串；非字符串输入直接回退。
/// @return 匹配的对齐方式；未知名返回 Start。
[[nodiscard]] inline auto json_to_main_axis_alignment(const Json &j) -> MainAxisAlignment {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Center") {
            return MainAxisAlignment::Center;
        }
        if (s == "End") {
            return MainAxisAlignment::End;
        }
        if (s == "SpaceBetween") {
            return MainAxisAlignment::SpaceBetween;
        }
        if (s == "SpaceAround") {
            return MainAxisAlignment::SpaceAround;
        }
        if (s == "SpaceEvenly") {
            return MainAxisAlignment::SpaceEvenly;
        }
    }
    return MainAxisAlignment::Start;
}

/// @brief CrossAxisAlignment -> JSON 字符串。
/// @param v 交叉轴对齐枚举值。
/// @return 与枚举同名的字符串；未知状态回退 "Start"。
[[nodiscard]] inline auto cross_axis_alignment_to_json(CrossAxisAlignment v) -> Json {
    switch (v) {
        case CrossAxisAlignment::Start:
            return "Start";
        case CrossAxisAlignment::Center:
            return "Center";
        case CrossAxisAlignment::End:
            return "End";
        case CrossAxisAlignment::Stretch:
            return "Stretch";
        case CrossAxisAlignment::Baseline:
            return "Baseline";
    }
    return "Start";
}

/// @brief JSON -> CrossAxisAlignment（未知值回退 Start）。
/// @param j 对齐名字符串；非字符串输入直接回退。
/// @return 匹配的对齐方式；未知名返回 Start。
[[nodiscard]] inline auto json_to_cross_axis_alignment(const Json &j) -> CrossAxisAlignment {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Center") {
            return CrossAxisAlignment::Center;
        }
        if (s == "End") {
            return CrossAxisAlignment::End;
        }
        if (s == "Stretch") {
            return CrossAxisAlignment::Stretch;
        }
        if (s == "Baseline") {
            return CrossAxisAlignment::Baseline;
        }
    }
    return CrossAxisAlignment::Start;
}

/// @brief StackFit -> JSON 字符串。
/// @param v Stack 子项尺寸约束枚举值。
/// @return "Loose"/"Expand"/"Passthrough" 之一；未知状态回退 "Loose"。
[[nodiscard]] inline auto stack_fit_to_json(StackFit v) -> Json {
    switch (v) {
        case StackFit::Loose:
            return "Loose";
        case StackFit::Expand:
            return "Expand";
        case StackFit::Passthrough:
            return "Passthrough";
    }
    return "Loose";
}

/// @brief JSON -> StackFit（未知值回退 Loose）。
/// @param j 拟合名字符串；非字符串输入直接回退。
/// @return 匹配的 StackFit；未知名返回 Loose。
[[nodiscard]] inline auto json_to_stack_fit(const Json &j) -> StackFit {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Loose") {
            return StackFit::Loose;
        }
        if (s == "Expand") {
            return StackFit::Expand;
        }
        if (s == "Passthrough") {
            return StackFit::Passthrough;
        }
    }
    return StackFit::Loose;
}

/// @brief BoxFit -> JSON 字符串。
/// @param v 图片缩放模式枚举值。
/// @return 与枚举同名的字符串；未知状态回退 "Fill"。
[[nodiscard]] inline auto box_fit_to_json(BoxFit v) -> Json {
    switch (v) {
        case BoxFit::Fill:
            return "Fill";
        case BoxFit::Contain:
            return "Contain";
        case BoxFit::Cover:
            return "Cover";
        case BoxFit::FitWidth:
            return "FitWidth";
        case BoxFit::FitHeight:
            return "FitHeight";
        case BoxFit::None:
            return "None";
        case BoxFit::ScaleDown:
            return "ScaleDown";
    }
    return "Fill";
}

/// @brief JSON -> BoxFit（未知值回退 Fill）。
/// @param j 模式名字符串；非字符串输入直接回退。
/// @return 匹配的 BoxFit；未知名返回 Fill。
[[nodiscard]] inline auto json_to_box_fit(const Json &j) -> BoxFit {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Fill") {
            return BoxFit::Fill;
        }
        if (s == "Contain") {
            return BoxFit::Contain;
        }
        if (s == "Cover") {
            return BoxFit::Cover;
        }
        if (s == "FitWidth") {
            return BoxFit::FitWidth;
        }
        if (s == "FitHeight") {
            return BoxFit::FitHeight;
        }
        if (s == "None") {
            return BoxFit::None;
        }
        if (s == "ScaleDown") {
            return BoxFit::ScaleDown;
        }
    }
    return BoxFit::Fill;
}

/// @brief OverflowStrategy -> JSON 字符串。
/// @param v 溢出处理策略枚举值。
/// @return 与枚举同名的字符串；未知状态回退 "Visible"。
[[nodiscard]] inline auto overflow_strategy_to_json(OverflowStrategy v) -> Json {
    switch (v) {
        case OverflowStrategy::Visible:
            return "Visible";
        case OverflowStrategy::Hidden:
            return "Hidden";
        case OverflowStrategy::Clip:
            return "Clip";
        case OverflowStrategy::Scroll:
            return "Scroll";
    }
    return "Visible";
}

/// @brief JSON -> OverflowStrategy（未知值回退 Visible）。
/// @param j 策略名字符串；非字符串输入直接回退。
/// @return 匹配的策略；未知名返回 Visible。
[[nodiscard]] inline auto json_to_overflow_strategy(const Json &j) -> OverflowStrategy {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Visible") {
            return OverflowStrategy::Visible;
        }
        if (s == "Hidden") {
            return OverflowStrategy::Hidden;
        }
        if (s == "Clip") {
            return OverflowStrategy::Clip;
        }
        if (s == "Scroll") {
            return OverflowStrategy::Scroll;
        }
    }
    return OverflowStrategy::Visible;
}

}  // namespace aurora

// ---- validate_prop<T>：属性值约束验证（specification/04-widget.md §2.2） ----
// validate_prop 模板特化定义在 descriptor.h（需要 PropDescriptor 完整定义，
// 而 descriptor.h 已 include 本头文件，避免循环依赖）。
// validate_enum_string 辅助函数同样在 descriptor.h。
