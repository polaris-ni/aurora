#include "aurora/icon/icon.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/core/utf8.h"
#include "aurora/icon/icon_svg.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"

namespace aurora::icon {
namespace {

/// 未命中诊断里最多回带的候选名条数：够 AI 纠偏，又不让 message 无限膨胀。
constexpr std::size_t AURORA_ICON_MAX_CANDIDATES = 12U;

/// 进程级语义名表：惰性构造，跨 TU 初始化顺序安全。
auto registry_table() -> std::unordered_map<std::string, IconData> & {
    static std::unordered_map<std::string, IconData> table;
    return table;
}

/// @brief 把已注册名渲染成候选提示片段。
/// @param exclude 触发未命中的名字（候选里不必重复它，它本来就不在表中）。
/// @return 供 `{detail}` 占位使用的描述文本。
auto candidate_detail(std::string_view exclude) -> std::string {
    auto &table = registry_table();
    (void)exclude;
    if (table.empty()) {
        return "no icon names are registered";
    }
    std::vector<std::string> all;
    all.reserve(table.size());
    for (const auto &entry : table) {
        all.push_back(entry.first);
    }
    std::sort(all.begin(), all.end());
    std::string out = "registered names:";
    const std::size_t n = std::min(all.size(), AURORA_ICON_MAX_CANDIDATES);
    for (std::size_t i = 0; i < n; ++i) {
        out += " ";
        out += all[i];
    }
    if (all.size() > n) {
        out += " (+" + std::to_string(all.size() - n) + " more)";
    }
    return out;
}

}  // namespace

auto IconData::glyph(std::string font_family, char32_t codepoint) -> IconData {
    IconData d;
    d.data_ = GlyphIcon{.font_family = std::move(font_family), .codepoint = codepoint};
    return d;
}

auto IconData::svg(std::vector<std::uint8_t> source, std::string source_id) -> IconData {
    IconData d;
    d.data_ = SvgIcon{.source = std::move(source), .source_id = std::move(source_id)};
    return d;
}

auto IconData::kind() const -> Kind { return std::holds_alternative<SvgIcon>(data_) ? Kind::Svg : Kind::Glyph; }

auto IconData::as_glyph() const -> const GlyphIcon * { return std::get_if<GlyphIcon>(&data_); }

auto IconData::as_svg() const -> const SvgIcon * { return std::get_if<SvgIcon>(&data_); }

auto IconData::as_glyph() -> GlyphIcon * { return std::get_if<GlyphIcon>(&data_); }

auto IconData::as_svg() -> SvgIcon * { return std::get_if<SvgIcon>(&data_); }

auto IconRegistry::register_icon(std::string name, IconData data) -> void {
    registry_table().insert_or_assign(std::move(name), std::move(data));
}

auto IconRegistry::unregister(std::string_view name) -> bool { return registry_table().erase(std::string{name}) > 0U; }

auto IconRegistry::lookup(std::string_view name) -> Result<IconData> {
    auto &table = registry_table();
    const auto it = table.find(std::string{name});
    if (it != table.end()) {
        return it->second;
    }
    return make_error(ErrorCode::IconNameNotRegistered,
                      ErrorParams{{"name", std::string{name}}, {"detail", candidate_detail(name)}});
}

auto IconRegistry::names() -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(registry_table().size());
    for (const auto &entry : registry_table()) {
        out.push_back(entry.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

auto IconRegistry::clear() -> void { registry_table().clear(); }

auto paint_icon_data(Painter &p, const Rect &box, const IconData &data, const Color &color, float optical_bias)
    -> void {
    if (const auto *glyph = data.as_glyph()) {
        Font f{.family = glyph->font_family, .size_pt = box.size.height, .weight = 400};
        const std::string text = utf8_encode(static_cast<std::uint32_t>(glyph->codepoint));
        const float h = render::FontEngine::measure_height(f);
        // 字号框垂直居中；跨大跨度再叠光学中心补偿（基线视觉中心偏差）。
        const float origin_y = box.origin.y + ((box.size.height - h) / 2.0F) + optical_bias;
        p.draw_text(
            Rect{.origin = Point{.x = box.origin.x, .y = origin_y}, .size = Size{.width = box.size.width, .height = h}},
            text, f, color);
        return;
    }
    if (const auto *svg = data.as_svg()) {
        const int dim = static_cast<int>(box.size.width);
        const auto img = SvgIconCache::instance().get_or_rasterize(
            SvgIconSource{.source = svg->source, .target_w = dim, .target_h = dim, .tint = color});
        if (img.ok()) {
            p.draw_image(img.value(), box);
        }
        // 光栅失败（白名单 / 光栅器）：不绘制、不崩溃；诊断由缓存与 `validate_ui` 承担。
    }
}

}  // namespace aurora::icon
