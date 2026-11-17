#include "aurora/render/display_list.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

#include "aurora/render/font_engine.h"
#include "aurora/render/glyph_emit.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/software_rhi.h"

namespace aurora {

namespace {
// 回放合并文本命令的固定批量：栈上小缓冲，满即分段提交（不累积到动态容器、不逐帧分配）。
constexpr std::size_t AURORA_TEXT_REPLAY_BATCH = 16;

// 回退链下标为 -1（未启用）时的替身：与 `decode_text_layout` 收到的空串同义。
const std::string AURORA_EMPTY_CHAIN;

/// @brief 把一条命令引用的池下标解析为只读指针，供 RHI 后端使用。
///
/// 下标合法性由录制方（`Painter::record*`）保证；负数表示该命令不引用对应池，解析为
/// `nullptr` 由后端回退到空值。
/// @param cmd 待解析的命令。
/// @param dl 提供各数据池的显示列表。
/// @return 后端可直接消费的只读视图。
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
    // DrawText 的按族回退链与文本同走字符串池，故一并在此解析（后端不接触池下标语义）。
    if (cmd.text_chain_idx >= 0) {
        data.text_chain = &dl.string_at(cmd.text_chain_idx);
    }
    return data;
}

/// @brief 判断两条 DrawText 能否并入同一批量调用。
///
/// 判等口径 = 决定字形发射上下文的全部字段：字体（按值比较，池里同值字体常各占一位）、颜色、
/// 抗锯齿策略、排版标量（字距/词距/斜体/方向/固定格）与按族回退链（按内容比较，见
/// `record_text_cmd` 每次录制各入池一位）。文本与 box 不参与判等：合并只省固定开销，逐片段
/// 仍以自己的原文与原点落笔，故不影响像素。
/// @param a 批次首条命令（其字段即整批共用属性）。
/// @param b 待并入的相邻命令。
/// @param dl 提供池解析的显示列表。
/// @return 各属性全同时为 true。
[[nodiscard]] auto same_text_style(const DrawCmd &a, const DrawCmd &b, const DisplayList &dl) -> bool {
    if (a.aa_mode != b.aa_mode || a.color != b.color || a.text_ls != b.text_ls || a.text_ws != b.text_ws ||
        a.text_italic != b.text_italic || a.text_dir != b.text_dir || a.text_cell_px != b.text_cell_px) {
        return false;
    }
    if (a.font_idx != b.font_idx) {
        // 一侧未引用字体（回放取默认字体）而另一侧引用真实字体：即便值相近也保守判不等。
        if (a.font_idx < 0 || b.font_idx < 0) {
            return false;
        }
        if (dl.font_at(a.font_idx) != dl.font_at(b.font_idx)) {
            return false;
        }
    }
    if (a.text_chain_idx != b.text_chain_idx) {
        const std::string &chain_a = a.text_chain_idx >= 0 ? dl.string_at(a.text_chain_idx) : AURORA_EMPTY_CHAIN;
        const std::string &chain_b = b.text_chain_idx >= 0 ? dl.string_at(b.text_chain_idx) : AURORA_EMPTY_CHAIN;
        if (chain_a != chain_b) {
            return false;
        }
    }
    return true;
}

/// @brief 由一条 DrawText 命令构造批量片段：文本与 box 原样透传，不改坐标。
/// @param cmd 源命令。
/// @param dl 提供字符串池 / 字体池解析的显示列表。
/// @return 供 `Painter::draw_text_runs` 使用的一个片段。
[[nodiscard]] auto make_text_run(const DrawCmd &cmd, const DisplayList &dl) -> render::TextRun {
    render::TextRun run{};
    if (cmd.str_idx >= 0) {
        run.text = dl.string_at(cmd.str_idx);
    }
    run.box = cmd.bounds;
    if (cmd.font_idx >= 0) {
        run.font = dl.font_at(cmd.font_idx);
    }
    run.color = cmd.color;
    return run;
}
}  // namespace

auto DisplayList::replay(rhi::RhiBackend &backend) const -> void {
    for (const auto &cmd : cmds_) {
        backend.submit(cmd, resolve_cmd_data(cmd, *this));
    }
}

auto DisplayList::replay(Painter &p) const -> void {
    // 软件路径同样经 RHI 消费者，保证「同一命令流、同一解释点」。
    // 差异仅在于文本：相邻且属性相同的 DrawText 在此合并为一次 `Painter::draw_text_runs`，
    // 省掉每条命令重复的字体面解析与行高度量；其余命令仍逐条交 `SoftwareRhi`，故覆盖顺序、
    // 裁剪栈作用范围与现有行为一致。单条文本不批量（走原逐条路径，避免多余的片段构造与字符串副本）。
    rhi::SoftwareRhi software{p};

    std::array<render::TextRun, AURORA_TEXT_REPLAY_BATCH> runs{};
    std::size_t run_count = 0;
    const DrawCmd *batch_head = nullptr;
    // 层捕获栈深度：`SoftwareRhi` 捕获期间的命令只入栈顶缓冲、不即时落笔，
    // 此时文本不得绕过 `submit` 直绘，因此深度非零时一律不合并。
    int layer_depth = 0;

    auto flush_batch = [&]() -> void {
        if (run_count == 0) {
            return;
        }
        if (run_count == 1) {
            software.submit(*batch_head, resolve_cmd_data(*batch_head, *this));
        } else {
            const std::string &chain =
                batch_head->text_chain_idx >= 0 ? string_at(batch_head->text_chain_idx) : AURORA_EMPTY_CHAIN;
            const render::TextLayoutOpts opts = render::decode_text_layout(*batch_head, chain);
            p.draw_text_runs(std::span<const render::TextRun>{runs.data(), run_count}, batch_head->aa_mode, opts);
        }
        run_count = 0;
        batch_head = nullptr;
    };

    for (const auto &cmd : cmds_) {
        if (layer_depth == 0 && cmd.kind == CmdKind::DrawText) {
            // 属性不同或批量已满：先提交当前批次，再以本条为新批次首条继续（批次之间保持原顺序）。
            if (batch_head != nullptr && (!same_text_style(*batch_head, cmd, *this) || run_count == runs.size())) {
                flush_batch();
            }
            if (batch_head == nullptr) {
                batch_head = &cmd;
            }
            runs.at(run_count) = make_text_run(cmd, *this);
            ++run_count;
            continue;
        }
        // 任何非 DrawText 命令（含裁剪 / 层命令）都断开合并：不跨命令重排，也不跨裁剪区合并。
        flush_batch();
        if (cmd.kind == CmdKind::BeginLayer) {
            ++layer_depth;
        } else if (cmd.kind == CmdKind::EndLayer && layer_depth > 0) {
            --layer_depth;
        }
        software.submit(cmd, resolve_cmd_data(cmd, *this));
    }
    flush_batch();
}

}  // namespace aurora
