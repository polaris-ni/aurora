#include "aurora/render/rhi/software_rhi.h"

#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"

namespace aurora::rhi {

namespace {
// 哨兵对象不用 constexpr：MSVC STL 的 constexpr string/vector 仅 _ITERATOR_DEBUG_LEVEL==0
// 可用（Debug IDL=2 下构造/析构非法，C2131）；库类型 Font/Image 含 STL 成员同理。
// namespace-scope const（内部链接）语义等价，静态初始化阶段一次性构造，热路径零差异。
const std::string AURORA_EMPTY_STR;
const std::vector<Color> AURORA_EMPTY_COLORS;
const std::vector<float> AURORA_EMPTY_FLOATS;
const Font AURORA_DEFAULT_FONT{};
const Image AURORA_DEFAULT_IMAGE{};
const Matrix2D AURORA_IDENTITY_MATRIX;
const std::vector<Point> AURORA_EMPTY_POINTS;
}  // namespace

auto SoftwareRhi::submit(const DrawCmd &cmd, const CmdData &data) -> void {
    if (painter_ == nullptr) {
        return;  // 未绑定目标绘制器：no-op（测试可构造空后端）
    }
    Painter &p = *painter_;
    switch (cmd.kind) {
        case CmdKind::FillRect:
            p.fill_rect(cmd.bounds, cmd.color);
            break;
        case CmdKind::ClearRect:
            p.clear_rect(cmd.bounds);
            break;
        case CmdKind::DrawRect:
            p.draw_rect(cmd.bounds, cmd.color);
            break;
        case CmdKind::DrawLine:
            p.draw_line(cmd.pt0, cmd.pt1, cmd.f0, cmd.color);
            break;
        case CmdKind::RoundedBorder:
            p.draw_rounded_border(cmd.bounds, cmd.f0, cmd.f1, cmd.color);
            break;
        case CmdKind::DrawText: {
            const std::string &s = data.text != nullptr ? *data.text : AURORA_EMPTY_STR;
            const Font &f = data.font != nullptr ? *data.font : AURORA_DEFAULT_FONT;
            const render::TextLayoutOpts opts{
                .letter_spacing = cmd.text_ls, .word_spacing = cmd.text_ws, .italic = cmd.text_italic};
            p.draw_text(cmd.bounds, s, f, cmd.color, cmd.aa_mode, opts);
            break;
        }
        case CmdKind::DrawImage: {
            const Image &img = data.image != nullptr ? *data.image : AURORA_DEFAULT_IMAGE;
            p.draw_image(img, cmd.bounds);
            break;
        }
        case CmdKind::LinearGradient:
            p.draw_linear_gradient(cmd.bounds, cmd.pt0, cmd.pt1,
                                   data.colors != nullptr ? *data.colors : AURORA_EMPTY_COLORS,
                                   data.stops != nullptr ? *data.stops : AURORA_EMPTY_FLOATS);
            break;
        case CmdKind::RadialGradient:
            p.draw_radial_gradient(cmd.bounds, cmd.pt0, cmd.f0,
                                   data.colors != nullptr ? *data.colors : AURORA_EMPTY_COLORS,
                                   data.stops != nullptr ? *data.stops : AURORA_EMPTY_FLOATS);
            break;
        case CmdKind::Shadow:
            p.draw_shadow(cmd.bounds, cmd.f0, cmd.f1, cmd.f2, cmd.color);
            break;
        case CmdKind::BlurRegion:
            p.blur_region(cmd.bounds, cmd.f0);
            break;
        case CmdKind::BlendRegion:
            p.blend_region(cmd.bounds, cmd.blend_mode, cmd.color, cmd.f0);
            break;
        case CmdKind::MaskRegion:
            p.mask_region(cmd.bounds, cmd.mask_kind, cmd.f0);
            break;
        case CmdKind::PushClip:
            p.push_clip(cmd.bounds);
            break;
        case CmdKind::PushClipRounded:
            p.push_clip_rounded(cmd.bounds, cmd.f0, cmd.rounded_aa);
            break;
        case CmdKind::PopClip:
            p.pop_clip();
            break;
        case CmdKind::Composite: {
            const Image &img = data.image != nullptr ? *data.image : AURORA_DEFAULT_IMAGE;
            const Matrix2D &mat = data.matrix != nullptr ? *data.matrix : AURORA_IDENTITY_MATRIX;
            p.composite(img, mat, cmd.composite_scale);
            break;
        }
        case CmdKind::SetAlpha:
            p.set_alpha(cmd.alpha);
            break;
        case CmdKind::Polyline: {
            const std::vector<Point> &pts = data.points != nullptr ? *data.points : AURORA_EMPTY_POINTS;
            p.stroke_polyline(pts, cmd.f0, cmd.color);
            break;
        }
        case CmdKind::Sector:
            p.fill_sector(cmd.pt0, cmd.f0, cmd.f1, cmd.f2, cmd.f3, cmd.color);
            break;
    }
}

}  // namespace aurora::rhi
