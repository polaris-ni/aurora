#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/transform.h"
#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/perf/counters.h"
#include "aurora/render/detail/paint_timing.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"
#include "aurora/core/accessibility.h"

namespace aurora {

/// @brief Scroll 属性（聚合）：单子滚动容器。
struct ScrollProps {
    Node child;
    float step = 16.0F;  ///< 每单位滚轮增量的滚动像素
    float overscan = 1.0F;  ///< 缓冲上下各留 overscan 屏：离屏缓冲 = 视口高 ×(1+2×overscan)（滑动窗口）
    /// @brief 滚动位置保存键（空 = 不参与）：控件重建后据 `app::ScrollStorage` 恢复滚动偏移。
    ///        恢复延迟到**首次可滚动布局**；显式反序列化的 `offset` 优先于本键的恢复。
    std::string restore_key;
    /// @brief snap/paging 吸附（默认关闭）：每次滚轮收位后经短滑动吸附到条目对齐点；
    ///        `paging = true` 时以视口高为一页。reduce-motion 下直落端点（不产生中间帧）。
    ScrollSnap snap;
};

/**
 * @brief 单子滚动容器：在固定视口内裁切内容，按滚轮增量垂直滚动。
 *
 * 内容在宽松约束下测量自然尺寸；容器自身取父约束给出的视口尺寸。
 *
 * 性能模型（滚动流畅、跟手、不卡顿的关键，滑动窗口缓冲）：
 * - 内容在宽松约束下测量自然尺寸；容器自身取父约束给出的视口尺寸。
 * - 离屏缓冲 `content_` 是**滑动窗口**而非整页：尺寸 = 视口宽 × 视口高 ×(1 + 2×overscan)，
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
            children_.push_back(std::move(props.child));
        }
        step = props.step;
        overscan = props.overscan;
        restore_key = std::move(props.restore_key);
        snap = props.snap;
    }
    /// @brief 便捷构造：扁平罗列子项，取首项为唯一子节点（Scroll{ Column{...} }）。
    Scroll(std::initializer_list<Node> kids) {
        if (kids.size() > 0) {
            children_.push_back(*kids.begin());
        }
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "Scroll"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Scroll",
            .properties =
                {
                    {.name = "step",
                     .type = "float",
                     .default_value = "16.0",
                     .required = false,
                     .note = "滚轮增量(px)"},
                    {.name = "restore_key",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "滚动位置保存键（空=不参与恢复）"},
                    {.name = "snap_extent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "吸附周期dp（<=0=关闭；snap_paging=true 时忽略）",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "snap_paging",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "分页模式：以视口高为一页吸附",
                     .json_type = "boolean",
                     .enum_values = {},
                     .min_value = ""},
                    {.name = "snap_alignment",
                     .type = "ScrollSnapAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "吸附对齐方位（Start/Center/End）",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End"},
                     .min_value = ""},
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::Scroll{ au::Column{ au::Text(\"long content\") } }"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["step"] = step;
        props["offset"] = offset_y_;  // 运行时滚动位置（AI-first 可观测；与 LazyList/GridView 同口径）
        props["restore_key"] = restore_key;
        props["snap_extent"] = snap.extent;
        props["snap_paging"] = snap.paging;
        props["snap_alignment"] = snap_alignment_to_json(snap.alignment);
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("step")) {
            step = props["step"].get<float>();
        }
        if (props.contains("restore_key")) {
            restore_key = props["restore_key"].get<std::string>();
        }
        if (props.contains("snap_extent")) {
            snap.extent = props["snap_extent"].get<float>();
        }
        if (props.contains("snap_paging")) {
            snap.paging = props["snap_paging"].get<bool>();
        }
        if (props.contains("snap_alignment")) {
            snap.alignment = json_to_snap_alignment(props["snap_alignment"]);
        }
        if (props.contains("offset")) {
            // 显式偏移优先于 restore_key 恢复（见 maybe_restore_scroll）：记入 pending 待布局后应用。
            pending_offset_ = props["offset"].get<float>();
            scroll_restored_ = false;
        }
    }

    /// @brief Scroll 自行管理离屏内容缓冲，禁用框架对 Scroll 自身的 DL 缓存，
    /// 避免缓存录制依赖会变化的 `content_` 缓冲。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    /// @brief Scroll 的布局结果可缓存，**仅当其直接内容子控件也可缓存时**。
    ///        原因：Scroll::on_layout 直接调用 `children_[0].widget().layout()`；
    ///        若子控件覆写了 can_cache_layout()=false（on_layout 含时间/状态依赖副作用，
    ///        如骨架→真实内容切换），Scroll 缓存自身布局会跳过 on_layout → 不调用子控件 layout()
    ///        → 子控件的延期逻辑永不触发 → 内容冻结/白屏（Path B 类 bug）。
    ///
    ///        此检查仅针对直接子控件，不向上递归——更远的祖先不受影响（避免 grid_rows 等测试
    ///        因 AppShell 额外 on_layout 调用导致动画状态提前推进）。
    [[nodiscard]] auto can_cache_layout() const -> bool override {
        if (children_.empty()) {
            return true;
        }
        return children_[0].widget().can_cache_layout();
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
            content_dirty_ = true;  // 布局变化：尺寸/结构可能变，整块重录
        } else {
            // 仅绘制变化（动画后代）：合并其绘制区域（缓冲局部坐标）为脏带，下一帧只重录该带，
            // 避免把整块 3 屏离屏缓冲每帧全量重录（动画标脏拖垮帧率的症结）。
            // origin.paint_bounds() 处于内容坐标系；本 Scroll 以内容坐标固定录制缓冲
            // （children_[0].paint 传入 bounds.origin=(0,-buffer_origin_y_)），故 paint_bounds
            // 即缓冲局部坐标（x∈[0,content_w], y∈[0,buffer_h]），可直接夹到缓冲窗口使用，无需屏幕坐标换算。
            const Rect &ob = origin.paint_bounds();
            if (has_dirty_band_) {
                const float x0 = std::min(dirty_band_.origin.x, ob.origin.x);
                const float y0 = std::min(dirty_band_.origin.y, ob.origin.y);
                const float x1 = std::max(dirty_band_.right(), ob.right());
                const float y1 = std::max(dirty_band_.bottom(), ob.bottom());
                dirty_band_ =
                    Rect{.origin = Point{.x = x0, .y = y0}, .size = Size{.width = x1 - x0, .height = y1 - y0}};
            } else {
                dirty_band_ = ob;
                has_dirty_band_ = true;
            }
        }
        // 把 Scroll 自身视口标脏：确保其 on-screen 区域被重绘（见上方类注释）。
        // 不递归到自身：本调用发生在后代的 request_frame 遍历中，mark_needs_paint 触发的是
        // 从 Scroll 向上的新一次遍历，不会再次进入 Scroll::on_descendant_dirty。
        mark_needs_paint();
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        // clamp/符号约定走共享 ScrollViewport 内核（与 Widget 基类 Overflow::Scroll 一致）。
        const float before = offset_y_;
        const float target = ScrollViewport::clamp_offset(offset_y_, e.delta_y, step, content_h_, viewport_h_);
        e.is_handled = true;
        // 余量回传（嵌套滚动协调）：端点被夹掉的部分上冒给更浅可滚动祖先。
        e.remaining_y = ScrollViewport::remaining_offset(before, target, e.delta_y, step);
        if (target != offset_y_) {
            offset_y_ = target;
            // 仅请求重绘本视口、不触发子树缓存失效：滚动不改内容，只改下方 composite 平移量，
            // 复用已录制的离屏内容缓冲，整页仅一次 blit → 跟手、不卡顿。
            scrolling_ = true;
            scroll_restored_ = true;  // 用户主动滚动：放弃尚未生效的键恢复
            write_back_offset();
            publish_offset();
            request_frame(false);
        }
        // snap/paging：收位后向条目对齐点短滑动收束（reduce-motion 下直落端点）。
        if (snap.enabled(viewport_h_)) {
            begin_snap_glide();
        }
    }

    /// @brief 真实滚动控件：滚轮派发时本控件是可滚动目标（最深优先）。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    /// 程序化滚动（供测试 / 无障碍 / 外部控制器驱动），delta_y 正方向为向上滚动。
    /// 覆写 `Widget::scroll_by`（基类为虚，避免同名隐藏非虚函数）；返回 offset 是否实际变化。
    auto scroll_by(float delta_y) -> bool override {
        ScrollEvent e;
        e.delta_y = delta_y;
        const float before = offset_y_;
        on_scroll(e);
        return offset_y_ != before;
    }
    [[nodiscard]] auto offset_y() const -> float { return offset_y_; }

    /// @brief 无障碍滚动量（G32）：{0, 内容量−视口量, 当前偏移}。
    ///
    /// 供读屏驱动滚动（UIA `IScrollProvider` / AT-SPI2 `Component.ScrollTo` /
    /// macOS `accessibilityPerformScrollToVisible`）；不可滚时 max = 0，桥据此不暴露滚动 pattern。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_scroll() const -> std::optional<AccessibilityScrollRange> override {
        const float max_offset = std::max(0.0F, content_h_ - viewport_h_);
        return AccessibilityScrollRange{.min = 0.0,
                                        .max = static_cast<double>(max_offset),
                                        .position = static_cast<double>(offset_y_)};
    }

    /// @brief 无障碍滚动定位（G32）：走 `set_offset` 既有夹取路径（不标布局脏）。
    /// @note Side-effects: mutates scroll state
    auto accessibility_scroll_to(double offset) -> void override {
        (void)set_offset(static_cast<float>(offset));
        mark_needs_paint();
    }

    /// @brief 读屏滚动动作：按自身 step 换算「一屏」增量（比基类的通用估算精确）。
    /// @note Side-effects: mutates scroll state
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        const bool down = req.action == AccessibilityAction::ScrollDown;
        const bool up = req.action == AccessibilityAction::ScrollUp;
        if (!down && !up) {
            return Container::perform_accessibility_action(req);
        }
        const float unit = step > 0.0F ? step : 16.0F;
        const float delta = (down ? -1.0F : 1.0F) * (viewport_h_ / unit);
        return scroll_by(delta);
    }

    /// @brief 程序化设置滚动偏移（返回是否实际变化）：夹取到 `[0, 内容高 - 视口高]`。
    ///
    /// 与 `scroll_by` 同一符号约定（offset 增大 = 内容上移、露出下方内容）。契约（与
    /// `LazyList::set_scroll_offset` 的差异见 specification/03-layout-render.md §8.1）：
    /// - **不标布局脏**：偏移不参与子布局（内容以稳定内容坐标录制，子控件 bounds 不随偏移变化）；
    /// - **不置 `content_valid_ = false`**：大跳越出已录制窗口时，由 `on_paint` 的
    ///   `!in_buffer → reanchor` 分支整块/条带重录兜底，无需在此强制整块重录；
    /// - 内容/视口尚未确定（未布局过）时夹到 0 并返回 `false`——调用方（如 `restore_key` 恢复）
    ///   须等到「首次可滚动布局」再调用。
    auto set_offset(float offset) -> bool {
        const float target = ScrollViewport::clamp_offset(offset, 0.0F, step, content_h_, viewport_h_);
        if (target == offset_y_) {
            return false;
        }
        glide_.active = false;  // 外部程序化跳转：作废进行中的收位滑动
        offset_y_ = target;
        scroll_restored_ = true;  // 外部程序化设置：视为已就位，不再被键恢复覆盖
        write_back_offset();
        publish_offset();
        // 只请求重绘本视口：滚动不改内容，越窗跳转由重锚点路径重录（见上）。
        request_frame(false);
        return true;
    }

    /// @brief 程序化滚动到指定偏移（snap 关闭时即普通夹取目标）。
    /// @param animate true = 经收位滑动过渡（reduce-motion 下自动直落端点）；false = 立即就位。
    /// @return 目标与当前偏移不同（即发生了移动或启动滑动）时为 true。
    /// @note Side-effects: mutates scroll state
    auto scroll_to(float offset, bool animate = true) -> bool {
        const float target = std::clamp(offset, 0.0F, std::max(0.0F, content_h_ - viewport_h_));
        if (target == offset_y_) {
            return false;
        }
        if (!animate || current_accessibility_settings().reduce_motion) {
            return set_offset(target);
        }
        scroll_restored_ = true;  // 同 set_offset：外部程序化设置视为已就位
        glide_.start(offset_y_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
        return true;
    }

    /// @brief 滚动偏移只读信号（滚动驱动动画原语）：宿主以纯函数派生视差/进度/淡入淡出。
    ///        懒创建；偏移每次变化（滚轮/拖拽/滑动帧/程序化）写入。
    /// @note Side-effects: reads state (registers reactive dependency in Effect scope)
    [[nodiscard]] auto offset_signal() -> SignalView<float> & {
        if (!offset_state_) {
            offset_state_ = std::make_shared<State<float>>(offset_y_);
            published_offset_ = offset_y_;
        }
        return *offset_state_;
    }

    /// @brief 是否正处于收位/程序化滑动中（测试与帧调度观测点）。
    [[nodiscard]] auto is_gliding() const -> bool { return glide_.active; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 垂直滚动容器的内容宽度应受视口宽度约束，避免子节点返回 inf 宽度，
        // 导致离屏内容缓冲尺寸计算出现 lround(inf) 等未定义行为而大面积黑块。
        const float viewport_w = [this, &c]() -> float {
            if (c.max.width != Size::infinity().width) {
                return c.max.width;
            }
            return (viewport_w_ > 0.0F) ? viewport_w_ : 1.0F;
        }();

        Size content{.width = viewport_w, .height = 0.0F};
        if (!children_.empty()) {
            Constraints cc;
            cc.min = Size{.width = viewport_w, .height = 0.0F};
            cc.max = Size{.width = viewport_w, .height = Size::infinity().height};
            content = children_[0].widget().layout(cc, ctx);
        }
        // 内容尺寸变化 → 离屏缓冲失效，下帧整体重建；视口尺寸变化不影响内容缓冲。
        if (content.width != content_w_ || content.height != content_h_) {
            content_valid_ = false;
        }
        content_h_ = content.height;
        content_w_ = content.width;
        // 吸顶节点发现（布局帧一次，绘制帧 O(1) 短路）：树形变化必经布局，缓存不会漏新头部。
        sticky_nodes_.clear();
        collect_stickies(children_, 0.0F, sticky_nodes_);

        const float vh = (c.max.height != Size::infinity().height) ? c.max.height : content.height;
        viewport_h_ = vh;
        viewport_w_ = viewport_w;
        // 内容脏由 on_descendant_dirty 结构式接收（后代 request_frame 沿布局父链上溯），
        // 此处无需接线——旧的 wire_content_dirty 快照式接线漏掉 on_layout 中动态新建的子控件。
        // 恢复滚动位置（延迟到内容/视口确定之后；内容尚不可滚动时继续等待下一帧布局）。
        maybe_restore_scroll();
        return c.constrain(Size{.width = viewport_w, .height = vh});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        if (children_.empty()) {
            p.pop_clip();
            return;
        }
        ensure_content_buffer(ctx);
        if (content_ == nullptr || content_->width() <= 0) {
            p.pop_clip();
            return;
        }
        // 计量：滑动窗口离屏缓冲的常驻字节数（RGBA8888）。缓冲尺寸 = 视口宽 × 视口高×(1+2×overscan)，
        // 与内容总量无关（内容 ×10 时缓冲不增长），这是「整页缓冲 → 滑动窗口」优化的直接验收锚点，
        // 不埋点就没有优化前的对照读数。
        AURORA_PROFILE_COUNT(scroll_buffer_bytes, static_cast<std::uint64_t>(content_->width()) *
                                                      static_cast<std::uint64_t>(content_->height()) * 4U);

        // 滑动窗口逻辑高（与 ensure_content_buffer 一致）：视口高 ×(1 + 2×overscan)，
        // 与内容总量解耦。必须用逻辑 dp（viewport_h_ 系列），不得取 content_->height()
        // （那是设备像素，scale≠1 时会把物理高误当逻辑高算入 max_origin/reanchor/clear/裁剪）。
        const float buffer_h = viewport_h_ * (1.0F + (2.0F * overscan));
        const float overscan_h = viewport_h_ * overscan;
        const float max_origin = std::max(0.0F, content_h_ - buffer_h);
        // 视口是否仍完全落在已录制的缓冲窗口 [origin, origin+buffer_h] 内（纯 blit 的前提）。
        const bool in_buffer =
            offset_y_ >= buffer_origin_y_ && (offset_y_ + viewport_h_) <= (buffer_origin_y_ + buffer_h);
        // reanchor（增量条带重录）触发条件：
        //  (1) 视口已脱离缓冲（安全网，正常不应发生）；
        //  (2) 长内容（max_origin>0）且正在滚动 —— 每个滚动帧都重锚点并向后偏移缓冲，delta 仅为一帧
        //      滚动量（≈12dp），暴露条带极小 → 单帧成本稳定且低。若只在临近缓冲边缘才重锚点，delta 会
        //      累积到 ~0.75 屏，条带重绘反而更贵（最坏帧 37ms 的根源）。短内容（max_origin==0）缓冲
        //      已覆盖全部可滚内容，永不重锚点，走 pure_scroll_blit 只做平移合成（最廉价）。
        //  content_valid_ 是增量路径的前提：缓冲刚重建/内容尺寸变化时里面没有可复用像素，
        //  此时若走增量只绘条带会漏画其余部分，必须回落整块重录。
        // 重录策略（按优先级）：
        //  ① 整块重录：缓冲无效（首建/尺寸变化/刚 resize）或后代布局标脏（content_dirty_）。
        //  ② 局部重录：后代仅绘制标脏（动画）——只重录合并脏带，其余缓冲像素经下方 blit 复用，
        //     避免整块 3 屏离屏缓冲每帧全量重录（动画后代标脏拖垮帧率的症结，见 on_descendant_dirty）。
        //  ③ 增量重锚：长内容滚动（reanchor，见下方注释）。
        //  ④ 其余（缓冲有效、内容未变、非重锚、无脏带）仅平移合成（blit），不重栅。
        // 全局光栅状态世代（AA 模式 / 默认字体，见 FontEngine::raster_generation）：content_ 把整棵
        // 子树的光栅结果固化在录制那一刻，而 mark_needs_paint 只沿父链向上失效、不触及后代缓存，故世代
        // 是「缓冲内旧光栅是否过期」的唯一 O(1) 判据。世代不匹配一律整块重录，避免回放切换 AA 前的旧
        // 光栅（表现为「F6 切了没变化、直到某子控件标脏时局部才零星刷新」）。
        const std::uint64_t raster_gen = render::FontEngine::raster_generation();
        bool whole_redraw = !content_valid_ || content_dirty_ || content_raster_gen_ != raster_gen;
        // 计算脏带（缓冲局部坐标，夹到缓冲窗口）；无效则降级整块重录。
        Rect band;
        bool band_redraw = false;
        if (has_dirty_band_ && !whole_redraw) {
            Rect b = dirty_band_;
            b.origin.x = std::max(0.0F, b.origin.x - 1.0F);  // 外扩 1dp 吸收圆角 AA / dp→物理取整
            b.origin.y = std::max(0.0F, b.origin.y - 1.0F);
            const float bx1 = std::min(content_w_, b.right() + 1.0F);
            const float by1 = std::min(buffer_h, b.bottom() + 1.0F);
            b = Rect{
                .origin = Point{.x = b.origin.x, .y = b.origin.y},
                .size = Size{.width = std::max(0.0F, bx1 - b.origin.x), .height = std::max(0.0F, by1 - b.origin.y)}};
            if (b.size.width > 0.0F && b.size.height > 0.0F) {
                band = b;
                band_redraw = true;
            } else {
                content_dirty_ = true;  // 脏带无效/越界：保守降级整块重录
                whole_redraw = true;
            }
        }
        const bool reanchor = content_valid_ && !whole_redraw && (!in_buffer || (max_origin > 0.0F && scrolling_));
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
        // on_descendant_dirty 置回 content_dirty_ / 合并脏带。若在重录之后才清零（旧逻辑），本次录制
        // 期间产生的新脏会被一并擦掉 → 下一帧判定「内容未变」仅平移合成 → 子树 on_paint 永不再执行 →
        // 自驱动动画冻结在首帧（白屏）。
        content_dirty_ = false;
        has_dirty_band_ = false;
        // need_redraw 为假时（缓冲有效、内容未变、非重锚、无脏带）无论是否滚动都只做下方平移合成（blit），不重栅。
        if (need_redraw) {
            if (whole_redraw) {
                // 整块重录（首建 / 内容尺寸变化 / 子控件布局标脏）：锚点重对齐到视口，全窗口重录。
                // 锚点必须重新对齐到当前视口：缓冲失效时旧 origin 可能与 offset_y_ 相距甚远
                // （如已滚到中段后内容尺寸变化触发重建），沿用旧 origin 会把视口落到缓冲窗口
                // 之外而整片空白。重算后视口必然落在 [origin, origin+buffer_h] 内。
                buffer_origin_y_ = std::clamp(offset_y_ - overscan_h, 0.0F, max_origin);
                // 先清零（子控件常以半透明内容自绘，若不先清，新帧半透明像素会与上帧残留 source-over
                // 叠加，阴影/黑边逐帧累积致黑）；再按 -buffer_origin_y_ 偏移把子控件绘制进缓冲，
                // 仅缓冲窗口 [buffer_origin_y_, +buffer_h] 内的内容被录制。
                content_->clear_rect(
                    Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = content_w_, .height = buffer_h}});
                content_->push_clip(
                    Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = content_w_, .height = buffer_h}});
                children_[0].widget().paint(*content_,
                                            Rect{.origin = Point{.x = 0.0F, .y = -buffer_origin_y_},
                                                 .size = Size{.width = content_w_, .height = content_h_}},
                                            ctx);
                content_->pop_clip();
                content_valid_ = true;
                content_raster_gen_ = raster_gen;  // 整块重录后全缓冲统一为当前光栅世代
            } else if (band_redraw) {
                // 局部重录：仅重绘脏带（动画后代区域），其余缓冲像素（静态内容）经下方 blit 复用，
                // 不再把整块 3 屏离屏缓冲每帧全量重录。脏带已夹到缓冲窗口（见上方计算）。
                content_->clear_rect(band);
                content_->push_clip(band);
                children_[0].widget().paint(*content_,
                                            Rect{.origin = Point{.x = 0.0F, .y = -buffer_origin_y_},
                                                 .size = Size{.width = content_w_, .height = content_h_}},
                                            ctx);
                content_->pop_clip();
                // content_valid_ 保持 true（仅更新带内像素，带外像素仍有效）
            } else {
                // 增量条带重录：缓冲里已栅格化的像素按新旧锚点差**原地按行 memmove**
                // 搬移（Painter::shift_pixels），只重绘新进入窗口的条带。
                // 旧做法是「分配 scratch 缓冲 + 整块 composite 位移 + swap」，composite 的逐像素
                // 矩阵求逆让单次位移在 3 屏缓冲上稳定 ~30ms —— 与整块重绘同量级，正是最坏帧
                // 击穿 33.3ms 的直接原因；memmove 是连续块搬移，同尺寸下降到 ~1ms。
                const float old_origin = buffer_origin_y_;
                const float new_origin = std::clamp(offset_y_ - overscan_h, 0.0F, max_origin);
                const float delta = old_origin - new_origin;  // 逻辑 dp：旧像素相对新锚点的位移（>0=下移，露出顶条带）
                if (delta != 0.0F) {
                    content_->shift_pixels(delta);
                    buffer_origin_y_ = new_origin;
                    // 让出的条带：delta>0 露顶、delta<0 露底；|delta| ≥ buffer_h 时条带覆盖整块，
                    // 自然退化为全量重录。两端各外扩 1dp 吸收 dp→物理像素的取整差，
                    // 避免接缝处残留半行陈旧像素。
                    const float exposed_top_content = (delta > 0.0F) ? new_origin : (old_origin + buffer_h);
                    const float raw_top = exposed_top_content - new_origin;
                    const float top = std::max(0.0F, raw_top - 1.0F);
                    const float bottom = std::min(buffer_h, raw_top + std::fabs(delta) + 1.0F);
                    const Rect re_band{.origin = Point{.x = 0.0F, .y = top},
                                       .size = Size{.width = content_w_, .height = bottom - top}};
                    // 外扩的 1dp 落在仍有效的旧像素上，须先归零基底再重绘，
                    // 否则半透明内容会与旧像素 source-over 二次叠加（阴影逐帧变黑）。
                    content_->clear_rect(re_band);
                    content_->push_clip(re_band);
                    children_[0].widget().paint(*content_,
                                                Rect{.origin = Point{.x = 0.0F, .y = -buffer_origin_y_},
                                                     .size = Size{.width = content_w_, .height = content_h_}},
                                                ctx);
                    content_->pop_clip();
                }
                content_valid_ = true;
            }
        }
        // 注意：content_dirty_ 已在 need_redraw 判定后、重录之前清零（见上方注释），
        // 此处不得再清——否则会擦掉内容子树在本次录制期间产生的新脏，冻结自驱动动画。

        // 仅一次平移合成：把有界缓冲按滚动偏移贴到视口（与旧整页缓冲的可见像素逐位一致）。
        const float dx = bounds.origin.x;
        const float dy = bounds.origin.y + buffer_origin_y_ - offset_y_;
        p.composite(*content_, Matrix2D::from_translate(dx, dy));

        paint_sticky_overlay(p, bounds, ctx);  // 吸顶覆盖层：blit 后重绘，不触发内容重录

        p.pop_clip();
        scrolling_ = false;  // 消费本帧滚动标记
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        if (bounds.contains(local)) {
            return this;  // 整个视口可滚动（容器优先）
        }
        return nullptr;
    }

    /// @brief 收位滑动逐帧推进（自驱动 tick，不占 Animator；同 Dismissible/ReorderableList 模式）。
    ///        滑动帧与滚轮帧同策略：仅平移 blit，不重录内容缓冲。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Container::tick_gestures(now);  // 子树手势照常计时
        if (!glide_.active) {
            needs_gesture_tick_ = false;  // 静止后摘除每帧计时，回到空闲节流
            return;
        }
        const double dt = glide_last_.has_value()
                              ? std::chrono::duration<double>(now - *glide_last_).count()
                              : (1.0 / 60.0);
        glide_last_ = now;
        float v = glide_.tick(dt);
        // 布局可能在滑动中改变内容/视口尺寸：目标随动夹取，防滑出可滚范围。
        v = std::clamp(v, 0.0F, std::max(0.0F, content_h_ - viewport_h_));
        if (!glide_.active) {  // 本帧抵达终点
            glide_last_.reset();
            needs_gesture_tick_ = false;
        }
        if (v != offset_y_) {
            offset_y_ = v;
            scrolling_ = true;
            write_back_offset();
            publish_offset();
            request_frame(false);  // 活跃帧信号：滑动期逐帧标脏，decide_wait 不误判空闲深睡
        }
    }

  private:
    /// @brief 启动向 snap 对齐点的收位滑动；reduce-motion 下直落端点
    ///        （对齐 `AnimationController::tick` 短路语义：状态与走完一致，只是无中间帧）。
    auto begin_snap_glide() -> void {
        const float target = ScrollViewport::snap_target(offset_y_, content_h_, viewport_h_, snap);
        if (target == offset_y_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            glide_.active = false;
            set_offset(target);
            return;
        }
        glide_.start(offset_y_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    /// @brief 偏移信号写入（仅当宿主索取过 offset_signal() 且值确实变化）。
    ///        用镜像值比较而非 `State::get()`——后者在 Effect 作用域会自订阅本控件。
    auto publish_offset() -> void {
        if (!offset_state_ || published_offset_ == offset_y_) {
            return;
        }
        published_offset_ = offset_y_;
        offset_state_->set(offset_y_);
    }

    ScrollGlide glide_;  ///< snap 收位 / scroll_to 的短滑动时序
    std::optional<std::chrono::steady_clock::time_point> glide_last_;  ///< 上一滑动帧时刻（墙钟差 = dt）
    std::shared_ptr<State<float>> offset_state_;  ///< 懒创建的偏移信号（滚动驱动动画原语，经 offset_signal 暴露）
    float published_offset_ = 0.0F;  ///< 最近一次发布的偏移值（镜像，避免回读 get() 误订阅）

    /// @brief 内容子树中 `StickyHeader` 节点的发现缓存（on_layout 重建树时刷新；空 = 覆盖层零开销）。
    ///        natural_y 在收集时沿遍历路径累加（Node 不指回父，bounds 只存于父的 children 视图）。
    struct StickyEntry {
        const Node *node;
        float natural_y;  ///< 内容坐标下的顶部（Scroll 内容空间）
    };
    std::vector<StickyEntry> sticky_nodes_;

    /// @brief 递归发现内容子树中的吸顶节点（就地短路，不再深入其子树）。
    auto collect_stickies(const std::vector<Node> &kids, float base, std::vector<StickyEntry> &out) -> void {
        for (const Node &n : kids) {
            if (n.widget().is_sticky_header()) {
                out.push_back(StickyEntry{.node = &n, .natural_y = base + n.bounds().origin.y});
            } else {
                collect_stickies(n.widget().child_nodes(), base + n.bounds().origin.y, out);
            }
        }
    }

    /// @brief 吸顶覆盖层：把「已滚过头顶」的 StickyHeader 按 pin 位重绘于视口顶部，
    ///        后来的头部把先前的向上顶出（内容序即堆叠序）。纯绘制路径——内容缓冲照常
    ///        blit/重录，sticky 不进缓冲（否则每滚动帧都要整块重录，缓冲模型即告破产）。
    auto paint_sticky_overlay(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        if (sticky_nodes_.empty()) {
            return;
        }
        std::sort(sticky_nodes_.begin(), sticky_nodes_.end(),
                  [](const StickyEntry &a, const StickyEntry &b) { return a.natural_y < b.natural_y; });
        float stack_bottom = 0.0F;  ///< 已钉驻头部占用的顶部区带高度
        for (std::size_t i = 0; i < sticky_nodes_.size(); ++i) {
            const StickyEntry &s = sticky_nodes_[i];
            const float h = s.node->bounds().size.height;
            if (h <= 0.0F || s.natural_y >= offset_y_) {
                continue;  // 尚未滚过头顶（或零高）：随内容缓冲正常滚动
            }
            // 顶出规则（CSS position:sticky / RecyclerView 同语义）：下一个头部的视口顶部
            // (next_natural − offset) 一旦逼近，本头部就被它向上顶出；最后一个头部的分组延伸到
            // 内容末尾，故无顶出对手。
            const float next_natural = (i + 1 < sticky_nodes_.size()) ? sticky_nodes_[i + 1].natural_y : content_h_;
            const float pin_y = std::min(stack_bottom, (next_natural - offset_y_) - h);
            if (pin_y + h <= 0.0F || pin_y >= viewport_h_) {
                continue;  // 已整条离开视口（其位置由顶出它的后继占据）
            }
            // paint 本身是非虚 public 入口（含修饰链/缓存），绘制期可安全可变更节点。
            const_cast<Node *>(s.node)->widget().paint(
                p,
                Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + pin_y},
                     .size = Size{.width = bounds.size.width, .height = h}},
                ctx);
            stack_bottom = pin_y + h;
        }
    }

    /// @brief 恢复路径专用：夹取 → 赋值 → 强制整块重录（不写回、不改归属标记）。
    auto apply_restored_offset(float raw) -> void {
        const float target = ScrollViewport::clamp_offset(raw, 0.0F, step, content_h_, viewport_h_);
        if (target == offset_y_) {
            return;
        }
        offset_y_ = target;
        publish_offset();
        // 恢复是一次性成帧：显式置缓冲无效（首帧本就无效，此处只为语义明确、防未来改动回放旧像素）。
        content_valid_ = false;
        mark_needs_paint();
    }

    /// @brief 首次可滚动布局时恢复滚动位置：
    ///        ① 显式反序列化的 `offset` 优先；② 否则查 `ScrollStorage` 的 `restore_key`；
    ///        ③ 内容尚不可滚动（`content_h_ <= viewport_h_`）时保持等待，避免先夹到 0 再被内容吞掉。
    auto maybe_restore_scroll() -> void {
        if (scroll_restored_) {
            return;
        }
        if (pending_offset_.has_value()) {
            scroll_restored_ = true;
            const float explicit_offset = *pending_offset_;
            pending_offset_.reset();
            apply_restored_offset(explicit_offset);
            return;
        }
        if (restore_key.empty() || content_h_ <= viewport_h_) {
            return;
        }
        scroll_restored_ = true;
        ScrollStorage::instance().claim(restore_key, this);
        if (const auto stored = ScrollStorage::instance().read(restore_key); stored.has_value()) {
            apply_restored_offset(*stored);
        }
    }

    /// @brief 位置变化时写回注册表（**仅内存**：落盘由 App 经 `ScrollStorage::sync` + `Preferences::flush` 决定）。
    auto write_back_offset() -> void {
        if (restore_key.empty()) {
            return;
        }
        ScrollStorage::instance().write(restore_key, offset_y_);
    }

    auto ensure_content_buffer(const BuildContext &ctx) -> void {
        if (content_w_ <= 0.0F || viewport_h_ <= 0.0F) {
            return;
        }
        // 滑动窗口缓冲：高度 = 视口高 ×(1 + 2×overscan)，与内容总量解耦。
        const float buffer_h = viewport_h_ * (1.0F + (2.0F * overscan));
        const float scale = ctx.scale_factor > 0.0F ? ctx.scale_factor : 1.0F;
        const int w = static_cast<int>(std::lround(content_w_));
        const int h = static_cast<int>(std::lround(buffer_h));
        if (!content_) {
            content_ = std::make_unique<Painter>();
        }
        const int pw = static_cast<int>(std::lround(static_cast<float>(w) * scale));
        const int ph = static_cast<int>(std::lround(static_cast<float>(h) * scale));
        if (content_->width() != pw || content_->height() != ph) {
            content_->set_scale(scale);
            content_->begin(w, h);
            content_valid_ = false;  // 尺寸变化：下帧重建缓冲
        }
    }

    float offset_y_ = 0.0F;
    std::optional<float> pending_offset_;  ///< 显式反序列化的偏移（优先于 restore_key 恢复，布局后应用）
    bool scroll_restored_ = false;  ///< 是否已就位（恢复过一次 / 用户或外部程序化设置过）
    float content_h_ = 0.0F;
    float content_w_ = 0.0F;
    float viewport_h_ = 0.0F;
    float viewport_w_ = 0.0F;
    float buffer_origin_y_ = 0.0F;  ///< 滑动窗口锚点：缓冲顶对应的内容坐标 Y（重锚点时更新）
    std::unique_ptr<Painter> content_;  ///< 滑动窗口离屏缓冲（尺寸 = 视口宽 × 视口高×(1+2×overscan)，与滚动偏移无关）
    bool content_valid_ = false;  ///< 离屏缓冲是否需要整体重建（首建 / 内容尺寸变化 / 重锚点）
    std::uint64_t content_raster_gen_ = 0;  ///< 离屏缓冲栅格化时的光栅状态世代（AA 模式 / 默认字体变更须整块重录）
    bool scrolling_ = false;  ///< 本帧是否由滚动驱动（=true 时仅 blit，不重录内容）
    bool content_dirty_ = true;  ///< 内容子树自上次栅格化后是否变化（由 on_descendant_dirty 置位；首帧必重录）
    Rect dirty_band_{.origin = Point{.x = 0.0F, .y = 0.0F},
                     .size = Size{.width = 0.0F, .height = 0.0F}};  ///< 后代绘制标脏合并的脏带（缓冲局部坐标）
    bool has_dirty_band_ = false;  ///< 是否存在绘制标脏带（布局标脏走整块重录）
};

}  // namespace aurora
