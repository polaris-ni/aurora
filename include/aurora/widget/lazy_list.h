#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/diagnostics.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 虚拟滚动列表（specification/04-widget.md §3.4）：仅实例化可见区域 + 预取缓冲区的子项。
 *
 * 与 `Repeater` 区分：`Repeater` 展开全部子项（适合 <100 项），`LazyList`
 * 仅构建可见窗口内的子项（适合 1000+ 项），滚出窗口的实例被回收。
 *
 * 当前实现为固定行高模式（`item_extent`），可精确计算可见范围与总内容高度；
 * 可变行高模式作为后续增强。
 *
 * 对标 Flutter `ListView.builder`、Qt `QListView`+delegate、WPF `VirtualizingStackPanel`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class LazyList : public Widget {
  public:
    using ItemBuilder = std::function<Node(int index)>;

    LazyList() = default;
    LazyList(int count, ItemBuilder builder, float item_extent = 48.0F)
        : count_(count < 0 ? 0 : count), builder_(std::move(builder)),
          item_extent_(item_extent > 0.0F
                           ? item_extent
                           : (Diagnostics::degraded("layout", "LazyList item_extent 非正值已降级为 48"), 48.0F)) {
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "LazyList"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LazyList",
            .properties =
                {
                    {.name = "count",
                     .type = "int",
                     .default_value = "0",
                     .required = true,
                     .note = "总项数",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "item_extent",
                     .type = "float",
                     .default_value = "48.0",
                     .required = false,
                     .note = "固定行高(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "scroll_offset",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "当前滚动偏移(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "cache_extent",
                     .type = "float",
                     .default_value = "200.0",
                     .required = false,
                     .note = "可见区外预取缓冲(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "restore_key",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "滚动位置保存键（空=不参与恢复）",
                     .json_type = "string",
                     .enum_values = {},
                     .min_value = ""},
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
                },
            .events = {},
            .children_policy = "none",
            .invariants = {"count >= 0", "item_extent > 0"},
            .examples = {"au::LazyList(10000, [](int i){ return au::Text(std::to_string(i)); }, 48.0F)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 总项数。
    [[nodiscard]] auto count() const -> int { return count_; }

    /// @brief 当前滚动偏移（dp，向下为正）。
    [[nodiscard]] auto scroll_offset() const -> float { return offset_; }

    /// @brief 设置滚动偏移（钳制到内容范围；外部跳转语义，会作废进行中的收位滑动）。
    auto set_scroll_offset(float offset) -> void { apply_offset(offset, /*cancel_glide=*/true); }

    /// @brief snap/paging 吸附（默认关闭，须显式配置）：每次滚轮收位后经短滑动吸附到条目对齐点。
    ///        `ScrollSnap::page()` 以视口高为一页；reduce-motion 下直落端点。
    [[nodiscard]] auto snap() const -> const ScrollSnap & { return snap_; }
    auto set_snap(ScrollSnap snap) -> LazyList & {
        snap_ = snap;
        return *this;
    }

    /// @brief 滚动位置保存键（空 = 不参与恢复；控件重建后据 `app::ScrollStorage` 恢复偏移）。
    [[nodiscard]] auto restore_key() const -> const std::string & { return restore_key_; }
    auto set_restore_key(std::string key) -> LazyList & {
        restore_key_ = std::move(key);
        return *this;
    }

    /// @brief 滚动到指定项（使其顶端对齐可视区顶端）。
    /// @param animate true = 经收位滑动过渡（reduce-motion 下自动直落端点）；false = 立即就位。
    auto scroll_to_item(int index, bool animate = false) -> void {
        const float target = static_cast<float>(std::clamp(index, 0, std::max(0, count_ - 1))) * item_extent_;
        if (!animate || current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return;
        }
        scroll_restored_ = true;  // 同 set_scroll_offset：外部程序化设置视为已就位
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    /// @brief 程序化滚动到指定偏移（snap 对齐点由调用方决定；本接口只做夹取）。
    /// @return 目标与当前偏移不同（即发生了移动或启动滑动）时为 true。
    auto scroll_to(float offset, bool animate = true) -> bool {
        const float target = std::clamp(offset, 0.0F, max_scroll_offset());
        if (target == offset_) {
            return false;
        }
        if (!animate || current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return true;
        }
        scroll_restored_ = true;  // 同 set_scroll_offset：外部程序化设置视为已就位
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
        return true;
    }

    /// @brief 是否正在收位滑动（测试/外部控制器观测点）。
    [[nodiscard]] auto is_gliding() const -> bool { return glide_.active; }

    /// @brief 滚动偏移只读信号（滚动驱动动画原语）：宿主以纯函数派生视差/进度/淡入淡出。
    ///        懒创建；偏移每次变化（滚轮/滑动帧/程序化）写入。
    /// @note Side-effects: reads state (registers reactive dependency in Effect scope)
    [[nodiscard]] auto offset_signal() -> SignalView<float> & {
        if (!offset_state_) {
            offset_state_ = std::make_shared<State<float>>(offset_);
            published_offset_ = offset_;
        }
        return *offset_state_;
    }

    /// @brief 最大滚动偏移（内容高 - 视口高，不小于 0）。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        return std::max(0.0F, content_height() - viewport_height_);
    }

    /// @brief 总内容高度。
    [[nodiscard]] auto content_height() const -> float { return static_cast<float>(count_) * item_extent_; }

    /// @brief 当前可见范围 [first, last)（含 cache_extent 缓冲）。
    [[nodiscard]] auto visible_range() const -> std::pair<int, int> {
        if (count_ == 0 || item_extent_ <= 0.0F || viewport_height_ <= 0.0F) {
            return {0, 0};
        }
        const float lo = std::max(0.0F, offset_ - cache_extent_);
        const float hi = offset_ + viewport_height_ + cache_extent_;
        const int first = std::clamp(static_cast<int>(std::floor(lo / item_extent_)), 0, count_);
        const int last = std::clamp(static_cast<int>(std::ceil(hi / item_extent_)), 0, count_);
        return {first, last};
    }

    /// @brief 当前存活（已实例化）的子项数（测试观测点：应远小于 count）。
    [[nodiscard]] auto live_item_count() const -> std::size_t { return live_.size(); }

    /// @brief 设置预取缓冲区高度（链式）。
    auto set_cache_extent(float extent) -> LazyList & {
        cache_extent_ = extent < 0.0F ? 0.0F : extent;
        return *this;
    }

    /// @brief 滚轮滚动。
    auto on_scroll(ScrollEvent &e) -> void override {
        const float before = offset_;
        set_scroll_offset(offset_ - (e.delta_y * AURORA_SCROLL_STEP));
        e.is_handled = true;
        // 余量回传（嵌套滚动协调）：端点被夹掉的部分上冒给更浅可滚动祖先。
        e.remaining_y = ScrollViewport::remaining_offset(before, offset_, e.delta_y, AURORA_SCROLL_STEP);
        // snap/paging：收位后向条目对齐点短滑动收束（reduce-motion 下直落端点）。
        if (snap_.enabled(viewport_height_)) {
            begin_snap_glide();
        }
    }

    /// @brief 真实滚动控件：滚轮派发时本控件是可滚动目标（最深优先）。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["count"] = count_;
        props["item_extent"] = item_extent_;
        props["scroll_offset"] = offset_;
        props["cache_extent"] = cache_extent_;
        props["restore_key"] = restore_key_;
        props["snap_extent"] = snap_.extent;
        props["snap_paging"] = snap_.paging;
        props["snap_alignment"] = snap_alignment_to_json(snap_.alignment);
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &val : live_ | std::views::values) {
            fn(val.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 480.0F};
        }
        viewport_height_ = self.height;
        // 滚动位置恢复（首次可滚动布局时生效）——须在计算可见窗口之前，使恢复在本帧即生效。
        maybe_restore_scroll();

        // 计算可见窗口并同步存活实例：新进入的构建、滚出的回收
        const auto [first, last] = visible_range();

        // 回收滚出窗口的实例
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first < first || it->first >= last) {
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        // 构建新进入窗口的实例
        if (builder_) {
            for (int i = first; i < last; ++i) {
                if (!live_.contains(i)) {
                    Node item = builder_(i);
                    if (item) {
                        item.widget().mount(ctx);
                        live_.emplace(i, std::move(item));
                    }
                }
            }
        }
        // 布局存活实例（固定行高，宽度撑满视口）
        Constraints item_c;
        item_c.min = Size{.width = self.width, .height = item_extent_};
        item_c.max = Size{.width = self.width, .height = item_extent_};
        for (auto &kv : live_) {
            kv.second.widget().set_layout_parent(this);
            kv.second.widget().layout(item_c, ctx);
            const float y = (static_cast<float>(kv.first) * item_extent_) - offset_;
            kv.second.set_bounds(
                Rect{.origin = Point{.x = 0.0F, .y = y}, .size = Size{.width = self.width, .height = item_extent_}});
        }
        return c.constrain(self);
    }

    /// @brief 吸顶覆盖层：把窗口内「已滚过头顶」的 sticky 项按 pin 位压顶绘制于视口顶部，
    ///        后到的头部把先前的向上顶出（与 Scroll 的覆盖层同一语义，虚拟化下只看窗口内项）。
    /// @note 须在滚动位绘制**之后**调用，钉驻头部才能压在内容之上。
    auto paint_sticky_overlay(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        std::vector<Node *> stickies;  // index 升序（live_ 为有序 map）
        for (auto &kv : live_ | std::views::values) {
            if (kv.widget().is_sticky_header()) {
                stickies.push_back(&kv);
            }
        }
        float stack = 0.0F;
        for (std::size_t i = 0; i < stickies.size(); ++i) {
            Node &node = *stickies[i];
            const float h = std::min(node.bounds().size.height, viewport_height_);
            if (h <= 0.0F || node.bounds().origin.y >= 0.0F) {
                continue;  // 尚未滚过头顶：随窗口正常滚动绘制
            }
            // 顶出规则同 Scroll：下一条 sticky 的视口顶部逼近时，本条被它向上顶出；最后一条到底。
            const float next_top =
                (i + 1 < stickies.size()) ? stickies[i + 1]->bounds().origin.y : viewport_height_ + h;
            const float pin_y = std::min(stack, next_top - h);
            if (pin_y + h <= 0.0F || pin_y >= viewport_height_) {
                continue;  // 已整条离开视口（其位置由顶出它的后继占据）
            }
            node.widget().paint(p,
                                Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + pin_y},
                                     .size = Size{.width = bounds.size.width, .height = node.bounds().size.height}},
                                ctx);
            stack = pin_y + h;
        }
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口：软件 Painter 直接写内存缓冲，子项（含 cache 区，局部 y 可能为负或越界）
        // 若绘制坐标溢出缓冲会触发访问越界（0xC0000005）。必须裁剪到视口，与 scroll.h 的
        // ScrollingWidget 一致。
        p.push_clip(bounds);
        for (auto &kv : live_ | std::views::values) {
            const Rect cb = kv.bounds();
            // 完全在视口外的跳过绘制（缓冲区实例保留但不绘制）；用包含性边界判断，
            // 避免浮点边界处（cb 恰好贴边）的行被误绘到视口外。
            if (cb.origin.y + cb.size.height <= 0.0F || cb.origin.y >= bounds.size.height) {
                continue;
            }
            // 已滚过头顶的 sticky 项不在滚动位绘制（其可见性完全由下方覆盖层承载，
            // 跳过条件与 paint_sticky_overlay 的入口条件逐项一致，否则留空洞）。
            if (cb.origin.y < 0.0F && kv.widget().is_sticky_header()) {
                continue;
            }
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            kv.widget().paint(p, global, ctx);
        }
        // 覆盖层最后绘制：钉驻头部压在滚动内容之上（与 Scroll 的 blit-后-压顶同序）。
        paint_sticky_overlay(p, bounds, ctx);
        p.pop_clip();
    }

    // 滚轮命中：整视口优先返回自身（与 Scroll 一致）。若改回「子项优先」，
    // 光标落在子项（Text）时 hit_test 解析为叶控件，ScrollEvent 派发到叶子后 on_scroll
    // 为空操作，列表永不滚动。指针事件仍走 on_hit_test_chain（保留子项点击命中）。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (auto &kv : live_ | std::views::values) {
            const Rect cb = kv.bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                std::vector<HitNode> r = kv.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    /// @brief 收位滑动逐帧推进（自驱动 tick，不占 Animator；同 Scroll/Dismissible 模式）。
    ///        与 Scroll 的差异：本控件偏移参与子布局，每滑动帧经 apply_offset 标布局脏，
    ///        由虚拟化窗口重建保证开销仅与可见项数相关。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &kv : live_ | std::views::values) {
            kv.widget().tick(now);
        }
        if (!glide_.active) {
            needs_gesture_tick_ = false;  // 静止后摘除每帧计时，回到空闲节流
            return;
        }
        const double dt =
            glide_last_.has_value() ? std::chrono::duration<double>(now - *glide_last_).count() : (1.0 / 60.0);
        glide_last_ = now;
        float v = glide_.tick(dt);
        // 布局可能在滑动中改变内容/视口尺寸：目标随动夹取，防滑出可滚范围。
        v = std::clamp(v, 0.0F, max_scroll_offset());
        if (!glide_.active) {  // 本帧抵达终点
            glide_last_.reset();
            needs_gesture_tick_ = false;
        }
        if (v != offset_) {
            apply_offset(v, /*cancel_glide=*/false);
            request_frame(false);  // 活跃帧信号：滑动期逐帧标脏，decide_wait 不误判空闲深睡
        }
    }

  private:
    /// @brief 启动向 snap 对齐点的收位滑动；reduce-motion 下直落端点
    ///        （对齐 `AnimationController::tick` 短路语义：状态与走完一致，只是无中间帧）。
    auto begin_snap_glide() -> void {
        const float target = ScrollViewport::snap_target(offset_, content_height(), viewport_height_, snap_);
        if (target == offset_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return;
        }
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    /// @brief 偏移落位的统一实现：夹取 → 赋值 → 写回/发布 → 标脏。
    /// @param cancel_glide 收位滑动帧须传 `false`——公开入口的「外部程序化跳转作废滑动」语义
    ///        若作用于滑动自身，snap 收位只会推进一帧便冻结在中途。
    auto apply_offset(float offset, bool cancel_glide) -> void {
        const float clamped = std::clamp(offset, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        if (cancel_glide) {
            glide_.active = false;
        }
        offset_ = clamped;
        scroll_restored_ = true;  // 外部程序化设置 / 用户滚动：视为已就位，不再被键恢复覆盖
        write_back_offset();
        publish_offset();
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 偏移信号写入（仅当宿主索取过 offset_signal() 且值确实变化）。
    ///        用镜像值比较而非 `State::get()`——后者在 Effect 作用域会自订阅本控件。
    auto publish_offset() -> void {
        if (!offset_state_ || published_offset_ == offset_) {
            return;
        }
        published_offset_ = offset_;
        offset_state_->set(offset_);
    }

    /// @brief 恢复路径专用：夹取 → 赋值 → 标脏（不写回、不改归属标记）。
    auto apply_restored_offset(float raw) -> void {
        const float clamped = std::clamp(raw, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        offset_ = clamped;
        publish_offset();
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 首次可滚动布局时按 `restore_key` 恢复滚动位置；内容尚不可滚动则等待下一帧布局。
    auto maybe_restore_scroll() -> void {
        if (scroll_restored_ || restore_key_.empty() || max_scroll_offset() <= 0.0F) {
            return;
        }
        scroll_restored_ = true;
        ScrollStorage::instance().claim(restore_key_, this);
        if (const auto stored = ScrollStorage::instance().read(restore_key_); stored.has_value()) {
            apply_restored_offset(*stored);
        }
    }

    /// @brief 位置变化时写回注册表（**仅内存**：落盘由 App 经 `ScrollStorage::sync` + `Preferences::flush` 决定）。
    auto write_back_offset() -> void {
        if (restore_key_.empty()) {
            return;
        }
        ScrollStorage::instance().write(restore_key_, offset_);
    }

    static constexpr float AURORA_SCROLL_STEP = 40.0F;  ///< 每单位滚轮增量对应的 dp

    int count_ = 0;
    ItemBuilder builder_;
    float item_extent_ = 48.0F;
    float offset_ = 0.0F;
    float cache_extent_ = 200.0F;
    float viewport_height_ = 0.0F;
    std::string restore_key_;  ///< 滚动位置保存键（空 = 不参与恢复）
    bool scroll_restored_ = false;  ///< 是否已就位（恢复过一次 / 用户或外部程序化设置过）
    std::map<int, Node> live_;  ///< 存活实例：index -> Node（按序遍历便于绘制）
    ScrollSnap snap_;  ///< snap/paging 吸附配置（默认关闭）
    ScrollGlide glide_;  ///< snap 收位 / scroll_to 的短滑动时序
    std::optional<std::chrono::steady_clock::time_point> glide_last_;  ///< 上一滑动帧时刻（墙钟差 = dt）
    std::shared_ptr<State<float>> offset_state_;  ///< 懒创建的偏移信号（滚动驱动动画原语，经 offset_signal 暴露）
    float published_offset_ = 0.0F;  ///< 最近一次发布的偏移值（镜像，避免回读 get() 误订阅）
};

}  // namespace aurora
