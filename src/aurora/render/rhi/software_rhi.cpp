#include "aurora/render/rhi/software_rhi.h"

#include "aurora/core/log.h"
#include "aurora/render/detail/gpu_layer.h"
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
// Font 的默认构造只做空 std::string/vector 初始化，实无抛出路径；改用函数内静态会改变
// 下列哨兵对象的作用域与初始化时机，热路径语义不等价。
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): 同上
const Font AURORA_DEFAULT_FONT{};
const Image AURORA_DEFAULT_IMAGE{};
const Matrix2D AURORA_IDENTITY_MATRIX;
const std::vector<Point> AURORA_EMPTY_POINTS;

// 进程级全局软件层存储：GPU 录制的 DL 可能回放至软件（begin_frame 失败回退 / 离屏
// render_to_png），回放每次构造临时 SoftwareRhi——层位图必须进程级持久，干净帧的
// DrawLayer 才能跨帧命中（否则逐帧未命中 → 逐帧 bump 层代际 → cache_layer 控件在
// 软件回退下永久逐帧重录）。层键进程唯一（next_gpu_layer_key），同键覆盖更新。
// 容量上限防控件销毁后的条目滞留：超限整体清空 + bump 层代际（下帧全量重录，正确性不变）。
constexpr std::size_t AURORA_LAYER_STORE_CAP = 64;

auto global_layer_store() -> std::unordered_map<std::uint64_t, Image> & {
    static std::unordered_map<std::uint64_t, Image> store;  // 单线程 UI（ARCHITECTURE §2 线程模型）
    return store;
}

/// @brief 解析本实例的层存储：显式共享指针优先（离屏子实例 / 测试隔离），否则全局存储。
auto layer_storage(std::unordered_map<std::uint64_t, Image> *override_store)
    -> std::unordered_map<std::uint64_t, Image> & {
    return override_store != nullptr ? *override_store : global_layer_store();
}
}  // namespace

auto SoftwareRhi::submit(const DrawCmd &cmd, const CmdData &data) -> void {
    if (painter_ == nullptr) {
        return;  // 未绑定目标绘制器：no-op（测试可构造空后端）
    }
    // 层捕获语义：捕获栈非空时，BeginLayer（嵌套层开新捕获帧）与 EndLayer（定稿栈顶层）
    // 由捕获逻辑处理，其余命令只入栈顶缓冲——内容像素在 EndLayer 时离屏重放定稿。
    if (!layer_captures_.empty()) {
        if (cmd.kind == CmdKind::BeginLayer) {
            layer_captures_.push_back(LayerCapture{.key = cmd.aux_key,
                                                   .width = static_cast<int>(cmd.bounds.size.width),
                                                   .height = static_cast<int>(cmd.bounds.size.height),
                                                   .cmds = {}});
            return;
        }
        if (cmd.kind == CmdKind::EndLayer) {
            finalize_layer_capture();
            return;
        }
        layer_captures_.back().cmds.emplace_back(cmd, data);
        return;
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
        case CmdKind::BeginLayer: {
            // 层捕获开始：开捕获帧（命令缓冲），EndLayer 时离屏重放为层位图。
            LayerCapture cap;
            cap.key = cmd.aux_key;
            cap.width = static_cast<int>(cmd.bounds.size.width);
            cap.height = static_cast<int>(cmd.bounds.size.height);
            layer_captures_.push_back(std::move(cap));
            break;
        }
        case CmdKind::EndLayer: {
            finalize_layer_capture();  // 防御路径：不配对的 EndLayer（捕获分支正常拦截）
            break;
        }
        case CmdKind::DrawLayer: {
            auto &store = layer_storage(layer_store_);
            const auto it = store.find(cmd.aux_key);
            if (it == store.end()) {
                // 冷存储未命中（首帧 / 消费端存储重建）：跳过本帧并整体失效层代际，
                // 下一帧全部控件重录 BeginLayer 自愈——单帧缺口，不逐帧抖动。
                if (layer_miss_warned_.insert(cmd.aux_key).second) {
                    AURORA_LOG_WARN("rhi", "DrawLayer 未命中层键 ", cmd.aux_key,
                                    "，本帧跳过（层代际已失效，下帧重录）");
                }
                render::detail::bump_gpu_layer_epoch();
                break;
            }
            const Matrix2D &mat = data.matrix != nullptr ? *data.matrix : AURORA_IDENTITY_MATRIX;
            p.composite(it->second, mat, cmd.composite_scale);
            break;
        }
    }
}

auto SoftwareRhi::finalize_layer_capture() -> void {
    if (layer_captures_.empty()) {
        return;
    }
    const LayerCapture cap = std::move(layer_captures_.back());
    layer_captures_.pop_back();
    // 离屏重放捕获命令 → 层位图（与 paint_cache_ 位图路径同语义），入共享存储。
    // 嵌套层：内层 EndLayer 先定稿入存储，外层捕获帧里的 DrawLayer(内层键) 在下方
    // 子后端重放时命中刚定稿的位图，随外层内容一并合成。
    Painter off;
    off.set_scale(painter_ != nullptr ? painter_->scale() : 1.0F);
    off.begin(cap.width, cap.height);
    SoftwareRhi sub(off, layer_store_);  // 空指针 → 子实例同样落全局存储
    for (const auto &entry : cap.cmds) {
        sub.submit(entry.first, entry.second);
    }
    auto &store = layer_storage(layer_store_);
    if (!store.contains(cap.key) && store.size() >= AURORA_LAYER_STORE_CAP) {
        // 容量上限：整体清空 + bump 层代际（控件下帧全量重录），防死控件条目无界滞留。
        store.clear();
        render::detail::bump_gpu_layer_epoch();
    }
    store.insert_or_assign(cap.key, off.to_image());
}

}  // namespace aurora::rhi
