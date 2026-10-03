#pragma once

// ============================================================
// glyph_emit.h — 字形发射桥（仓库私有内部头，不入 include/、不进 aurora_api.json）
// ------------------------------------------------------------
// font_engine.cpp 提供实现；GPU 栅格后端（gpu_gl_rhi.cpp）消费。同一条 shaping / 软件
// 图集代码路径既驱动软件 blit 也驱动 GPU 图集上传，杜绝两套文本实现漂移（度量、行切分、
// 基线 snap、间距推进与 FontEngine::draw_text 逐位同源）。
// ============================================================

#include <cstdint>
#include <functional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/display_list.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/glyph_atlas.h"

namespace aurora::render {

/// @brief 字形发射回调：软件图集条目 + 物理像素位图坐标 + 图集键。
///
/// `entry` 仅在本次回调调用期内有效（软件图集 LRU 可能随后淘汰）——消费方须在回调内
/// 即时拷贝/上传位图，不得缓存指针。`dx0/dy0` 为字形位图左上角的物理像素坐标（行盒顶
/// 语义，与 `Painter::draw_text` 的原点换算一致）；`mode` 指示位图布局（Gray：width×rows
/// 的 A8；Lcd：3×width×rows 的 RGB 子像素，GPU 路径不应接受）。
using GlyphEmitSink = std::function<void(const GlyphAtlas::Entry &entry, GlyphAtlas::Mode mode, int dx0, int dy0,
                                         std::uint64_t atlas_key)>;

/// @brief 逐字形发射一段文本（与 `FontEngine::draw_text` 同源实现）。
///
/// @param text       UTF-8 文本（可含 '\n'，逐行发射）
/// @param f          字体
/// @param opts       排版属性（间距/斜体/方向/回退链/固定格；与度量接口同语义）
/// @param scale      设备像素 / 逻辑 dp（决定光栅 px 尺寸与间距换算，同 `Painter::scale`）
/// @param aa         抗锯齿策略（GPU 路径传 `Supersample` 强制灰度，规避 LCD 子像素着色）
/// @param c          文本色（仅参与 ClearType 模式判定；着色由消费方自理）
/// @param origin_x/y 绘制原点（物理像素；行盒顶语义）
/// @param sink       逐字形回调（含零位图字形——空格等，消费方自行跳过）
/// @return false = 无可用字体面（调用方自行兜底；软件路径回退 BitmapFont）
auto emit_text_glyphs(const std::string &text, const Font &f, const TextLayoutOpts &opts, float scale, TextAAMode aa,
                      Color c, float origin_x, float origin_y, const GlyphEmitSink &sink) -> bool;

// ---- DrawText 录制/回放的排版选项编解码 ----
//
// 存在的理由：`TextLayoutOpts` 有六个字段，而 `DrawCmd` 逐字段拆槽存储（降低头耦合、避免命令
// 内嵌大对象）。手工在「录制端搬字段」与「回放端重建 opts」两侧各写一遍，是本仓最容易产生
// 「加了字段忘了同步 ⇒ 静默丢设置」的地方——`direction` 此前就是这样在录制中丢失的。
// 故把两侧编解码收敛成下面这一对函数，三个回放后端共用同一份实现。

/// @brief 把 `TextLayoutOpts` 的**排版标量**字段写入 `DrawCmd`（录制侧）。
/// @param cmd 待填充的命令（调用方负责其余字段，**含** `text_chain_idx`：链的序列化与入池
///        需持有 DisplayList，本函数拿不到，故该字段由调用方填）。
/// @param opts 待序列化的排版选项。
void encode_text_layout(DrawCmd &cmd, const TextLayoutOpts &opts);

/// @brief 由 `DrawCmd` 的排版字段重建 `TextLayoutOpts`（回放侧）。
/// @param cmd 已录制的命令。
/// @param chain_str `text_chain_idx` 指向的链字符串（U+001F 分隔的族名序列）；索引为 -1 时可为空串。
/// @return 与录制端逐字段等价的排版选项（含回退链重建）。
[[nodiscard]] auto decode_text_layout(const DrawCmd &cmd, const std::string &chain_str) -> TextLayoutOpts;

}  // namespace aurora::render
