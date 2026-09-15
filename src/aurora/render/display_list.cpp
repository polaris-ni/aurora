#include "aurora/render/display_list.h"

#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/software_rhi.h"

namespace aurora {

namespace {
/// @brief 把一条命令引用的池下标解析为只读指针，供 RHI 后端使用。
///
/// 下标合法性由录制方（`Painter::record*`）保证；负数表示该命令不引用对应池，解析为
/// `nullptr` 由后端回退到空值（与 D 轨抽取前的 `replay` 语义逐条对应）。
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 见函数内说明
auto resolve_cmd_data(const DrawCmd &cmd, const DisplayList &dl) -> rhi::CmdData {
    rhi::CmdData data;
    if (cmd.str_idx >= 0) {
        data.text = &dl.string_at(cmd.str_idx);
    }
    if (cmd.font_idx >= 0) {
        data.font = &dl.font_at(cmd.font_idx);
    }
    if (cmd.col_idx >= 0) {
        data.colors = &dl.colors_at(cmd.col_idx);
    }
    if (cmd.flt_idx >= 0) {
        data.stops = &dl.floats_at(cmd.flt_idx);
    }
    if (cmd.image_idx >= 0) {
        data.image = &dl.image_at(cmd.image_idx);
    }
    if (cmd.matrix_idx >= 0) {
        data.matrix = &dl.matrix_at(cmd.matrix_idx);
    }
    if (cmd.pt_idx >= 0) {
        data.points = &dl.points_at(cmd.pt_idx);
    }
    return data;
}
}  // namespace

auto DisplayList::replay(rhi::RhiBackend &backend) const -> void {
    for (const auto &cmd : cmds_) {
        backend.submit(cmd, resolve_cmd_data(cmd, *this));
    }
}

auto DisplayList::replay(Painter &p) const -> void {
    // D 轨（RHI 抽象）之后：软件路径同样经 RHI 消费者，保证「同一命令流、同一解释点」。
    rhi::SoftwareRhi software{p};
    replay(software);
}

}  // namespace aurora
