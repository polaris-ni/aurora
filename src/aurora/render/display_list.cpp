#include "aurora/render/display_list.h"

#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"

namespace aurora {

namespace {
constexpr std::string AURORA_EMPTY_STR;
constexpr std::vector<Color> AURORA_EMPTY_COLORS;
constexpr std::vector<float> AURORA_EMPTY_FLOATS;
constexpr Font AURORA_DEFAULT_FONT{};
constexpr Image AURORA_DEFAULT_IMAGE{};
constexpr Matrix2D AURORA_IDENTITY_MATRIX;
}  // namespace

auto DisplayList::replay(Painter &p) const -> void {
    // 所有 pool 下标均由 DisplayList 自身录制时生成，范围已保证合法；replay 为热路径，使用 operator[] 避免 at() 开销。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto &cmd : cmds_) {
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
                const std::string &s =
                    cmd.str_idx >= 0 ? str_pool_[static_cast<size_t>(cmd.str_idx)] : AURORA_EMPTY_STR;
                const Font &f =
                    cmd.font_idx >= 0 ? font_pool_[static_cast<size_t>(cmd.font_idx)] : AURORA_DEFAULT_FONT;
                render::TextLayoutOpts opts{
                    .letter_spacing = cmd.text_ls, .word_spacing = cmd.text_ws, .italic = cmd.text_italic};
                p.draw_text(cmd.bounds, s, f, cmd.color, cmd.aa_mode, opts);
                break;
            }
            case CmdKind::DrawImage: {
                const Image &img =
                    cmd.image_idx >= 0 ? image_pool_[static_cast<size_t>(cmd.image_idx)] : AURORA_DEFAULT_IMAGE;
                p.draw_image(img, cmd.bounds);
                break;
            }
            case CmdKind::LinearGradient:
                p.draw_linear_gradient(
                    cmd.bounds, cmd.pt0, cmd.pt1,
                    cmd.col_idx >= 0 ? color_pool_[static_cast<size_t>(cmd.col_idx)] : AURORA_EMPTY_COLORS,
                    cmd.flt_idx >= 0 ? float_pool_[static_cast<size_t>(cmd.flt_idx)] : AURORA_EMPTY_FLOATS);
                break;
            case CmdKind::RadialGradient:
                p.draw_radial_gradient(
                    cmd.bounds, cmd.pt0, cmd.f0,
                    cmd.col_idx >= 0 ? color_pool_[static_cast<size_t>(cmd.col_idx)] : AURORA_EMPTY_COLORS,
                    cmd.flt_idx >= 0 ? float_pool_[static_cast<size_t>(cmd.flt_idx)] : AURORA_EMPTY_FLOATS);
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
                const Image &img =
                    cmd.image_idx >= 0 ? image_pool_[static_cast<size_t>(cmd.image_idx)] : AURORA_DEFAULT_IMAGE;
                const Matrix2D &mat =
                    cmd.matrix_idx >= 0 ? matrix_pool_[static_cast<size_t>(cmd.matrix_idx)] : AURORA_IDENTITY_MATRIX;
                p.composite(img, mat, cmd.composite_scale);
                break;
            }
            case CmdKind::SetAlpha:
                p.set_alpha(cmd.alpha);
                break;
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

}  // namespace aurora
