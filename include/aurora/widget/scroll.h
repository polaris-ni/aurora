#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

#include "aurora/core/transform.h"
#include "aurora/event/event.h"
#include "aurora/perf/counters.h"
#include "aurora/render/detail/paint_timing.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Scroll 属性（聚合）：单子滚动容器。
struct ScrollProps {
    Node child;
    float step = 16.0f;    ///< 每单位滚轮增量的滚动像素
    float overscan = 1.0f; ///< 缓冲上下各留 overscan 屏：离屏缓冲 = 视口高 ×(1+2×overscan)（滑动窗口）
};

/**
 * @brief 单子滚动容器：在固定视口内裁切内容，按滚轮增量垂直滚动。
 *
 * 内容在宽松约束下测量自然尺寸；容器自身取父约束给出的视口尺寸。
 *
 * 性能模型（滚动流畅、跟手、不卡顿的关键，滑动窗口缓冲）：
 * - 内容在宽松约束下测量自然尺寸；容器自身取父约束给出的视口尺寸。
 * - 离屏缓冲 `m_content` 是**滑动窗口**而非整页：尺寸 = 视口宽 × 视口高 ×(1 + 2×overscan)，
 *   与内容总量解耦（缓冲内存随内容 ×10 不增长）。缓冲以「稳定的内容坐标」录制
 *   （偏移不烘焙进子控件 bounds，子控件的 Display List 缓存不被偏移击穿）。
 * - 滚动只改变下方 `composite` 的平移量，纯滚动帧整页仅一次 blit（平移合成），**不重新栅格化**。
 * - 视口滚出缓冲安全区（上下各 overscan 屏）时才**重锚点并整块重录有界缓冲**；重录频率正比于
 *   滚动距离（每滚约 1 屏触发一次），而非内容总量 —— 这才是正确的复杂度。
 * - 非滚动帧（如自动轮播 banner 标脏）重录同一块有界缓冲（已从上百 MB 降到约 3 屏量级）。
 * 这避免了旧实现把偏移烤进 bounds + 绘制时压裁剪，导致每帧重栅整页内容而卡顿的问题。
 *
 * 采用**继承式双模 API**（specification/04-widget.md §2.5）：`ScrollProps` 字段即本控件公有字段，
 * `step` 可直接赋值（`scroll.step = 16`）或以配置块构造
 * `Scroll{ ScrollProps{.child = ..., .step = 16} }`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Scroll : public Container, public ScrollProps {
  public:
    Scroll() = default;
    explicit Scroll(ScrollProps props) {
        if (props.child) {
            m_children.push_back(std::move(props.child));
        }
        step = props.step;
        overscan = props.overscan;
    }
    /// @brief 便捷构造：扁平罗列子项，取首项为唯一子节点（Scroll{ Column{...} }）。
    Scroll(std::initializer_list<Node> kids) {
        if (kids.size() > 0) {
            m_children.push_back(*kids.begin());
        }
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "Scroll"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Scroll",
            .properties = {
                { .name = "step", .type = "float", .default_value = "16.0", .required = false, .note = "滚轮增量(px)" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false },
                { .name = "show", .type = "bool", .default_value = "true", .required = false },
            },
            .events = {},
            .children_policy = "single",
            .examples = { "au::Scroll{ au::Column{ au::Text(\"long content\") } }" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["step"] = step;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("step")) {
            step = props["step"].get<float>();
        }
    }

    /// @brief Scroll 自行管理离屏内容缓冲，禁用框架对 Scroll 自身的 DL 缓存，
    /// 避免缓存录制依赖会变化的 `m_content` 缓冲。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    /// @brief Scroll 的布局结果可缓存，**仅当其直接内容子控件也可缓存时**。
    ///        原因：Scroll::on_layout 直接调用 `m_children[0].widget().layout()`；
    ///        若子控件覆写了 can_cache_layout()=false（on_layout 含时间/状态依赖副作用，
    ///        如骨架→真实内容切换），Scroll 缓存自身布局会跳过 on_layout → 不调用子控件 layout()
    ///        → 子控件的延期逻辑永不触发 → 内容冻结/白屏（Path B 类 bug）。
    ///
    ///        此检查仅针对直接子控件，不向上递归——更远的祖先不受影响（避免 grid_rows 等测试
    ///        因 AppShell 额外 on_layout 调用导致动画状态提前推进）。
    [[nodiscard]] auto can_cache_layout() const -> bool override {
        if (m_children.empty()) {
            return true;
        }
        return m_children[0].widget().can_cache_layout();
    }

    /// @brief 后代标脏 ⇒ 离屏内容缓冲失效，下帧重录（区分「内容真变化」与「仅需重新合成」）。
    ///
    /// 取代旧的递归接线（`wire_content_dirty`）：接线是布局时的树**快照**，而 `on_layout` 中
    /// 动态新建的子控件（骨架切换后的 banner / 卡片等自驱动动画）永不在快照内 → 其
    /// `mark_needs_paint` 不会置内容脏 → Scroll 判定「内容未变」只平移合成 → 子树 `on_paint`
    /// 再不执行 → 动画冻结在首帧、骨架永不切成真实内容（白屏类 bug）。`Widget::request_frame`
    /// 的上溯式传播没有快照，动态新建的子树天然被覆盖。
    ///
    /// 后代的 `request_frame` 上溯到 `Window` 汇聚点（`on_subtree_dirty`）时，标记的是**起源控件**
    /// 自身的 `paint_bounds()`——而离屏缓冲内的后代处于**内容坐标系**，其 `paint_bounds()` 不等于
    /// Scroll **视口的屏幕坐标**，不会覆盖视口区域 → `present_root` 的增量裁剪把视口排除在外 →
    /// 视口永不重绘（白屏 / 内容冻结）。故此处显式 `mark_needs_paint()`：它经布局父链上溯到 sink 时
    /// 标记的是 **Scroll 自身的视口（窗口坐标）**，保证离屏缓冲重录后合成到屏幕的视口被真正重绘。
    ///
    /// @note 只响应**后代**：Scroll 自身 `on_scroll` 的 `request_frame(false)` 不经此路径，
    ///       故纯滚动帧仍只平移合成、不重栅 3 屏缓冲（滚动跟手的关键路径不受影响）。
    auto on_descendant_dirty(Widget &origin, bool layout) -> void override {
        if (layout) {
            m_content_dirty = true; // 布局变化：尺寸/结构可能变，整块重录
        } else {
            // 仅绘制变化（动画后代）：合并其绘制区域（缓冲局部坐标）为脏带，下一帧只重录该带，
            // 避免把整块 3 屏离屏缓冲每帧全量重录（动画标脏拖垮帧率的症结）。
            // origin.paint_bounds() 处于内容坐标系；本 Scroll 以内容坐标固定录制缓冲
            // （m_children[0].paint 传入 bounds.origin=(0,-m_buffer_origin_y)），故 paint_bounds
            // 即缓冲局部坐标（x∈[0,content_w], y∈[0,buffer_h]），可直接夹到缓冲窗口使用，无需屏幕坐标换算。
            const Rect &ob = origin.paint_bounds();
            if (m_has_dirty_band) {
                const float x0 = std::min(m_dirty_band.origin.x, ob.origin.x);
                const float y0 = std::min(m_dirty_band.origin.y, ob.origin.y);
                const float x1 = std::max(m_dirty_band.right(), ob.right());
                const float y1 = std::max(m_dirty_band.bottom(), ob.bottom());
                m_dirty_band =
                    Rect{ .origin = Point{ .x = x0, .y = y0 }, .size = Size{ .width = x1 - x0, .height = y1 - y0 } };
            } else {
                m_dirty_band = ob;
                m_has_dirty_band = true;
            }
        }
        // 把 Scroll 自身视口标脏：确保其 on-screen 区域被重绘（见上方类注释）。
        // 不递归到自身：本调用发生在后代的 request_frame 遍历中，mark_needs_paint 触发的是
        // 从 Scroll 向上的新一次遍历，不会再次进入 Scroll::on_descendant_dirty。
        mark_needs_paint();
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        const float max_off = std::max(0.0f, m_content_h - m_viewport_h);
        // 与全库滚动约定一致（见 lazy_list/grid_view/lazy_row）：delta_y 正方向为「向上滚动」，
        // 此时 m_offset_y 应减小；故用减号。m_offset_y 增大表示内容上移露出下方内容。
        const float target = std::max(0.0f, std::min(max_off, m_offset_y - (e.delta_y * step)));
        e.handled = true;
        if (target != m_offset_y) {
            m_offset_y = target;
            // 仅请求重绘本视口、不触发子树缓存失效：滚动不改内容，只改下方 composite 平移量，
            // 复用已录制的离屏内容缓冲，整页仅一次 blit → 跟手、不卡顿。
            m_scrolling = true;
            request_frame(false);
        }
    }

    /// 程序化滚动（供测试 / 无障碍 / 外部控制器驱动），delta_y 正方向为向上滚动。
    auto scroll_by(float delta_y) -> void {
        ScrollEvent e;
        e.delta_y = delta_y;
        on_scroll(e);
    }
    [[nodiscard]] auto offset_y() const -> float { return m_offset_y; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 垂直滚动容器的内容宽度应受视口宽度约束，避免子节点返回 inf 宽度，
        // 导致离屏内容缓冲尺寸计算出现 lround(inf) 等未定义行为而大面积黑块。
        const float viewport_w = [this, &c]() -> float {
            if (c.max.width != Size::infinity().width) {
                return c.max.width;
            }
            return (m_viewport_w > 0.0f) ? m_viewport_w : 1.0f;
        }();

        Size content{ .width = viewport_w, .height = 0.0f };
        if (!m_children.empty()) {
            Constraints cc;
            cc.min = Size{ .width = viewport_w, .height = 0.0f };
            cc.max = Size{ .width = viewport_w, .height = Size::infinity().height };
            content = m_children[0].widget().layout(cc, ctx);
        }
        // 内容尺寸变化 → 离屏缓冲失效，下帧整体重建；视口尺寸变化不影响内容缓冲。
        if (content.width != m_content_w || content.height != m_content_h) {
            m_content_valid = false;
        }
        m_content_h = content.height;
        m_content_w = content.width;

        const float vh = (c.max.height != Size::infinity().height) ? c.max.height : content.height;
        m_viewport_h = vh;
        m_viewport_w = viewport_w;
        // 内容脏由 on_descendant_dirty 结构式接收（后代 request_frame 沿布局父链上溯），
        // 此处无需接线——旧的 wire_content_dirty 快照式接线漏掉 on_layout 中动态新建的子控件。
        return c.constrain(Size{ .width = viewport_w, .height = vh });
    }

    // NOLINTNEXTLINE(*-function-cognitive-complexity)
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        if (m_children.empty()) {
            p.pop_clip();
            return;
        }
        ensure_content_buffer(ctx);
        if (m_content == nullptr || m_content->width() <= 0) {
            p.pop_clip();
            return;
        }
        // 计量：滑动窗口离屏缓冲的常驻字节数（RGBA8888）。缓冲尺寸 = 视口宽 × 视口高×(1+2×overscan)，
        // 与内容总量无关（内容 ×10 时缓冲不增长），这是「整页缓冲 → 滑动窗口」优化的直接验收锚点，
        // 不埋点就没有优化前的对照读数。
        AURORA_PROFILE_COUNT(scroll_buffer_bytes, static_cast<std::uint64_t>(m_content->width()) *
                                                      static_cast<std::uint64_t>(m_content->height()) * 4u);

        // 滑动窗口逻辑高（与 ensure_content_buffer 一致）：视口高 ×(1 + 2×overscan)，
        // 与内容总量解耦。必须用逻辑 dp（m_viewport_h 系列），不得取 m_content->height()
        // （那是设备像素，scale≠1 时会把物理高误当逻辑高算入 max_origin/reanchor/clear/裁剪）。
        const float buffer_h = m_viewport_h * (1.0f + (2.0f * overscan));
        const float overscan_h = m_viewport_h * overscan;
        const float max_origin = std::max(0.0f, m_content_h - buffer_h);
        // 视口是否仍完全落在已录制的缓冲窗口 [origin, origin+buffer_h] 内（纯 blit 的前提）。
        const bool in_buffer =
            m_offset_y >= m_buffer_origin_y && (m_offset_y + m_viewport_h) <= (m_buffer_origin_y + buffer_h);
        // reanchor（增量条带重录）触发条件：
        //  (1) 视口已脱离缓冲（安全网，正常不应发生）；
        //  (2) 长内容（max_origin>0）且正在滚动 —— 每个滚动帧都重锚点并向后偏移缓冲，delta 仅为一帧
        //      滚动量（≈12dp），暴露条带极小 → 单帧成本稳定且低。若只在临近缓冲边缘才重锚点，delta 会
        //      累积到 ~0.75 屏，条带重绘反而更贵（最坏帧 37ms 的根源）。短内容（max_origin==0）缓冲
        //      已覆盖全部可滚内容，永不重锚点，走 pure_scroll_blit 只做平移合成（最廉价）。
        //  m_content_valid 是增量路径的前提：缓冲刚重建/内容尺寸变化时里面没有可复用像素，
        //  此时若走增量只绘条带会漏画其余部分，必须回落整块重录。
        // 重录策略（按优先级）：
        //  ① 整块重录：缓冲无效（首建/尺寸变化/刚 resize）或后代布局标脏（m_content_dirty）。
        //  ② 局部重录：后代仅绘制标脏（动画）——只重录合并脏带，其余缓冲像素经下方 blit 复用，
        //     避免整块 3 屏离屏缓冲每帧全量重录（动画后代标脏拖垮帧率的症结，见 on_descendant_dirty）。
        //  ③ 增量重锚：长内容滚动（reanchor，见下方注释）。
        //  ④ 其余（缓冲有效、内容未变、非重锚、无脏带）仅平移合成（blit），不重栅。
        bool whole_redraw = !m_content_valid || m_content_dirty;
        // 计算脏带（缓冲局部坐标，夹到缓冲窗口）；无效则降级整块重录。
        Rect band{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 0.0f, .height = 0.0f } };
        bool band_redraw = false;
        if (m_has_dirty_band && !whole_redraw) {
            Rect b = m_dirty_band;
            b.origin.x = std::max(0.0f, b.origin.x - 1.0f); // 外扩 1dp 吸收圆角 AA / dp→物理取整
            b.origin.y = std::max(0.0f, b.origin.y - 1.0f);
            const float bx1 = std::min(m_content_w, b.right() + 1.0f);
            const float by1 = std::min(buffer_h, b.bottom() + 1.0f);
            b = Rect{ .origin = Point{ .x = b.origin.x, .y = b.origin.y },
                      .size = Size{ .width = std::max(0.0f, bx1 - b.origin.x),
                                    .height = std::max(0.0f, by1 - b.origin.y) } };
            if (b.size.width > 0.0f && b.size.height > 0.0f) {
                band = b;
                band_redraw = true;
            } else {
                m_content_dirty = true; // 脏带无效/越界：保守降级整块重录
                whole_redraw = true;
            }
        }
        const bool reanchor = m_content_valid && !whole_redraw && (!in_buffer || (max_origin > 0.0f && m_scrolling));
        const bool need_redraw = whole_redraw || band_redraw || reanchor;
        // [性能排查] 累积计数：本帧「整块/脏带/重锚重录」vs「仅平移合成」，归因滚动缓冲开销。
        auto &pt = detail::paint_timing();
        if (!need_redraw) {
            pt.scroll_r_blit++;
        } else if (whole_redraw) {
            pt.scroll_r_whole++;
        } else if (band_redraw) {
            pt.scroll_r_band++;
        } else {
            pt.scroll_r_reanchor++;
        }
        // 渲染前清脏（时序与 present_root 一致）：下方重录会调用内容子树的 paint，自驱动动画
        // （骨架微光、banner 入场/轮播）在其 on_paint 内 mark_needs_paint 以驱动下一帧，该标记经
        // on_descendant_dirty 置回 m_content_dirty / 合并脏带。若在重录之后才清零（旧逻辑），本次录制
        // 期间产生的新脏会被一并擦掉 → 下一帧判定「内容未变」仅平移合成 → 子树 on_paint 永不再执行 →
        // 自驱动动画冻结在首帧（白屏）。
        m_content_dirty = false;
        m_has_dirty_band = false;
        // need_redraw 为假时（缓冲有效、内容未变、非重锚、无脏带）无论是否滚动都只做下方平移合成（blit），不重栅。
        if (need_redraw) {
            if (whole_redraw) {
                // 整块重录（首建 / 内容尺寸变化 / 子控件布局标脏）：锚点重对齐到视口，全窗口重录。
                // 锚点必须重新对齐到当前视口：缓冲失效时旧 origin 可能与 m_offset_y 相距甚远
                // （如已滚到中段后内容尺寸变化触发重建），沿用旧 origin 会把视口落到缓冲窗口
                // 之外而整片空白。重算后视口必然落在 [origin, origin+buffer_h] 内。
                m_buffer_origin_y = std::clamp(m_offset_y - overscan_h, 0.0f, max_origin);
                // 先清零（子控件常以半透明内容自绘，若不先清，新帧半透明像素会与上帧残留 source-over
                // 叠加，阴影/黑边逐帧累积致黑）；再按 -m_buffer_origin_y 偏移把子控件绘制进缓冲，
                // 仅缓冲窗口 [m_buffer_origin_y, +buffer_h] 内的内容被录制。
                m_content->clear_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                            .size = Size{ .width = m_content_w, .height = buffer_h } });
                m_content->push_clip(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                           .size = Size{ .width = m_content_w, .height = buffer_h } });
                m_children[0].widget().paint(*m_content,
                                             Rect{ .origin = Point{ .x = 0.0f, .y = -m_buffer_origin_y },
                                                   .size = Size{ .width = m_content_w, .height = m_content_h } },
                                             ctx);
                m_content->pop_clip();
                m_content_valid = true;
            } else if (band_redraw) {
                // 局部重录：仅重绘脏带（动画后代区域），其余缓冲像素（静态内容）经下方 blit 复用，
                // 不再把整块 3 屏离屏缓冲每帧全量重录。脏带已夹到缓冲窗口（见上方计算）。
                m_content->clear_rect(band);
                m_content->push_clip(band);
                m_children[0].widget().paint(*m_content,
                                             Rect{ .origin = Point{ .x = 0.0f, .y = -m_buffer_origin_y },
                                                   .size = Size{ .width = m_content_w, .height = m_content_h } },
                                             ctx);
                m_content->pop_clip();
                // m_content_valid 保持 true（仅更新带内像素，带外像素仍有效）
            } else {
                // 增量条带重录：缓冲里已栅格化的像素按新旧锚点差**原地按行 memmove**
                // 搬移（Painter::shift_pixels），只重绘新进入窗口的条带。
                // 旧做法是「分配 scratch 缓冲 + 整块 composite 位移 + swap」，composite 的逐像素
                // 矩阵求逆让单次位移在 3 屏缓冲上稳定 ~30ms —— 与整块重绘同量级，正是最坏帧
                // 击穿 33.3ms 的直接原因；memmove 是连续块搬移，同尺寸下降到 ~1ms。
                const float old_origin = m_buffer_origin_y;
                const float new_origin = std::clamp(m_offset_y - overscan_h, 0.0f, max_origin);
                const float delta = old_origin - new_origin; // 逻辑 dp：旧像素相对新锚点的位移（>0=下移，露出顶条带）
                if (delta != 0.0f) {
                    m_content->shift_pixels(delta);
                    m_buffer_origin_y = new_origin;
                    // 让出的条带：delta>0 露顶、delta<0 露底；|delta| ≥ buffer_h 时条带覆盖整块，
                    // 自然退化为全量重录。两端各外扩 1dp 吸收 dp→物理像素的取整差，
                    // 避免接缝处残留半行陈旧像素。
                    const float exposed_top_content = (delta > 0.0f) ? new_origin : (old_origin + buffer_h);
                    const float raw_top = exposed_top_content - new_origin;
                    const float top = std::max(0.0f, raw_top - 1.0f);
                    const float bottom = std::min(buffer_h, raw_top + std::fabs(delta) + 1.0f);
                    const Rect re_band{ .origin = Point{ .x = 0.0f, .y = top },
                                        .size = Size{ .width = m_content_w, .height = bottom - top } };
                    // 外扩的 1dp 落在仍有效的旧像素上，须先归零基底再重绘，
                    // 否则半透明内容会与旧像素 source-over 二次叠加（阴影逐帧变黑）。
                    m_content->clear_rect(re_band);
                    m_content->push_clip(re_band);
                    m_children[0].widget().paint(*m_content,
                                                 Rect{ .origin = Point{ .x = 0.0f, .y = -m_buffer_origin_y },
                                                       .size = Size{ .width = m_content_w, .height = m_content_h } },
                                                 ctx);
                    m_content->pop_clip();
                }
                m_content_valid = true;
            }
        }
        // 注意：m_content_dirty 已在 need_redraw 判定后、重录之前清零（见上方注释），
        // 此处不得再清——否则会擦掉内容子树在本次录制期间产生的新脏，冻结自驱动动画。

        // 仅一次平移合成：把有界缓冲按滚动偏移贴到视口（与旧整页缓冲的可见像素逐位一致）。
        const float dx = bounds.origin.x;
        const float dy = bounds.origin.y + m_buffer_origin_y - m_offset_y;
        p.composite(*m_content, Matrix2D::from_translate(dx, dy));

        p.pop_clip();
        m_scrolling = false; // 消费本帧滚动标记
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        if (bounds.contains(local)) {
            return this; // 整个视口可滚动（容器优先）
        }
        return nullptr;
    }

  private:
    auto ensure_content_buffer(const BuildContext &ctx) -> void {
        if (m_content_w <= 0.0f || m_viewport_h <= 0.0f) {
            return;
        }
        // 滑动窗口缓冲：高度 = 视口高 ×(1 + 2×overscan)，与内容总量解耦。
        const float buffer_h = m_viewport_h * (1.0f + (2.0f * overscan));
        const float scale = ctx.scale_factor > 0.0f ? ctx.scale_factor : 1.0f;
        const int w = static_cast<int>(std::lround(m_content_w));
        const int h = static_cast<int>(std::lround(buffer_h));
        if (!m_content) {
            m_content = std::make_unique<Painter>();
        }
        const int pw = static_cast<int>(std::lround(static_cast<float>(w) * scale));
        const int ph = static_cast<int>(std::lround(static_cast<float>(h) * scale));
        if (m_content->width() != pw || m_content->height() != ph) {
            m_content->set_scale(scale);
            m_content->begin(w, h);
            m_content_valid = false; // 尺寸变化：下帧重建缓冲
        }
    }

    float m_offset_y = 0.0f;
    float m_content_h = 0.0f;
    float m_content_w = 0.0f;
    float m_viewport_h = 0.0f;
    float m_viewport_w = 0.0f;
    float m_buffer_origin_y = 0.0f;     ///< 滑动窗口锚点：缓冲顶对应的内容坐标 Y（重锚点时更新）
    std::unique_ptr<Painter> m_content; ///< 滑动窗口离屏缓冲（尺寸 = 视口宽 × 视口高×(1+2×overscan)，与滚动偏移无关）
    bool m_content_valid = false;       ///< 离屏缓冲是否需要整体重建（首建 / 内容尺寸变化 / 重锚点）
    bool m_scrolling = false;           ///< 本帧是否由滚动驱动（=true 时仅 blit，不重录内容）
    bool m_content_dirty = true;        ///< 内容子树自上次栅格化后是否变化（由 on_descendant_dirty 置位；首帧必重录）
    Rect m_dirty_band{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                       .size = Size{ .width = 0.0f, .height = 0.0f } }; ///< 后代绘制标脏合并的脏带（缓冲局部坐标）
    bool m_has_dirty_band = false;                                      ///< 是否存在绘制标脏带（布局标脏走整块重录）
};

} // namespace aurora
