#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 二维虚拟网格：纵向滚动 + 固定列数 + 固定单元格高度，
 * 仅实例化可见行内的单元格。
 *
 * 对标 Flutter `GridView`（`SliverGridDelegateWithFixedCrossAxisCount`）、
 * Qt `QListView`+`setIconSize`、WPF `UniformGrid`+虚拟化。
 *
 * 与 `LazyList` 区分：`LazyList` 一维单列；`GridView` 一维索引映射到 (row,col) 二维布局，
 * 适合图库 / 数据卡片网格。当前为固定行高 + 等宽列模式；可变尺寸作为后续增强。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class GridView : public Widget {
  public:
    using ItemBuilder = std::function<Node(int index)>;

    GridView() = default;
    GridView(int count, int columns, ItemBuilder builder, float cell_extent = 96.0F)
        : count_(count < 0 ? 0 : count),
          columns_(columns > 0 ? columns : (Diagnostics::degraded("layout", "GridView columns 非正已降级为 1"), 1)),
          builder_(std::move(builder)),
          cell_extent_(cell_extent > 0.0F
                           ? cell_extent
                           : (Diagnostics::degraded("layout", "GridView cell_extent 非正已降级为 96"), 96.0F)) {
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "GridView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "GridView",
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
                    {.name = "columns",
                     .type = "int",
                     .default_value = "2",
                     .required = true,
                     .note = "列数",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "1"},
                    {.name = "cell_extent",
                     .type = "float",
                     .default_value = "96.0",
                     .required = false,
                     .note = "单元格高(dp)（宽=视口宽/列数）",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "scroll_offset",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "纵向滚动偏移(dp)",
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
                },
            .events = {},
            .children_policy = "none",
            .invariants = {"count >= 0", "columns >= 1", "cell_extent > 0"},
            .examples = {"au::GridView(1000, 3, [](int i){ return au::Text(std::to_string(i)); }, 96.0F)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    [[nodiscard]] auto count() const -> int { return count_; }
    [[nodiscard]] auto columns() const -> int { return columns_; }
    [[nodiscard]] auto scroll_offset() const -> float { return offset_; }
    [[nodiscard]] auto cell_extent() const -> float { return cell_extent_; }
    [[nodiscard]] auto live_item_count() const -> std::size_t { return live_.size(); }

    /// @brief 总行数。
    [[nodiscard]] auto row_count() const -> int { return (count_ + columns_ - 1) / columns_; }
    /// @brief 总内容高度。
    [[nodiscard]] auto content_height() const -> float { return static_cast<float>(row_count()) * cell_extent_; }
    /// @brief 最大滚动偏移。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        return std::max(0.0F, content_height() - viewport_height_);
    }

    auto set_scroll_offset(float offset) -> void {
        const float clamped = std::clamp(offset, 0.0F, max_scroll_offset());
        if (clamped != offset_) {
            offset_ = clamped;
            mark_needs_layout();
            mark_needs_paint();
        }
    }

    auto set_cache_extent(float extent) -> GridView & {
        cache_extent_ = extent < 0.0F ? 0.0F : extent;
        return *this;
    }

    /// @brief 当前可见行范围 [first_row, last_row)（含 cache_extent 缓冲）。
    [[nodiscard]] auto visible_row_range() const -> std::pair<int, int> {
        if (count_ == 0 || cell_extent_ <= 0.0F || viewport_height_ <= 0.0F) {
            return {0, 0};
        }
        const float lo = std::max(0.0F, offset_ - cache_extent_);
        const float hi = offset_ + viewport_height_ + cache_extent_;
        const int first = std::clamp(static_cast<int>(std::floor(lo / cell_extent_)), 0, row_count());
        const int last = std::clamp(static_cast<int>(std::ceil(hi / cell_extent_)), 0, row_count());
        return {first, last};
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        set_scroll_offset(offset_ - (e.delta_y * AURORA_SCROLL_STEP));
        e.is_handled = true;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["count"] = count_;
        props["columns"] = columns_;
        props["cell_extent"] = cell_extent_;
        props["scroll_offset"] = offset_;
        props["cache_extent"] = cache_extent_;
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
        cell_width_ = self.width / static_cast<float>(columns_);

        const auto [first, last] = visible_row_range();

        // 回收滚出窗口的实例
        for (auto it = live_.begin(); it != live_.end();) {
            const int row = it->first / columns_;
            if (row < first || row >= last) {
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        // 构建新进入窗口的实例
        if (builder_) {
            for (int r = first; r < last; ++r) {
                for (int col = 0; col < columns_; ++col) {
                    const int idx = (r * columns_) + col;
                    if (idx >= count_) {
                        break;
                    }
                    if (!live_.contains(idx)) {
                        Node item = builder_(idx);
                        if (item) {
                            item.widget().mount(ctx);
                            live_.emplace(idx, std::move(item));
                        }
                    }
                }
            }
        }
        // 布局存活实例
        Constraints cell_c;
        cell_c.min = Size{.width = cell_width_, .height = cell_extent_};
        cell_c.max = Size{.width = cell_width_, .height = cell_extent_};
        for (auto &kv : live_) {
            kv.second.widget().set_layout_parent(this);
            kv.second.widget().layout(cell_c, ctx);
            const int row = kv.first / columns_;
            const int col = kv.first % columns_;
            const float x = static_cast<float>(col) * cell_width_;
            const float y = (static_cast<float>(row) * cell_extent_) - offset_;
            kv.second.set_bounds(
                Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = cell_width_, .height = cell_extent_}});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口矩形：避免被圆角/裁剪父容器包裹时 Painter 慢路径越界访问
        // （与 LazyList 同类崩溃修复）。
        p.push_clip(bounds);
        for (auto &val : live_ | std::views::values) {
            const Rect cb = val.bounds();
            if (cb.origin.y + cb.size.height < 0.0F || cb.origin.y > bounds.size.height) {
                continue;
            }
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            val.widget().paint(p, global, ctx);
        }
        p.pop_clip();
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        (void)ctx;
        // 整视口优先返回自身（与 Scroll / LazyList 一致）：滚轮事件须命中被滚动容器本身，
        // 否则落到子单元格后 on_scroll 为空操作，网格永不滚动。点击 / 指针命中经
        // on_hit_test_chain 递归子节点，不受此处影响。
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (auto &val : live_ | std::views::values) {
            const Rect cb = val.bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                std::vector<HitNode> r = val.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &val : live_ | std::views::values) {
            val.widget().tick(now);
        }
    }

  private:
    static constexpr float AURORA_SCROLL_STEP = 40.0F;

    int count_ = 0;
    int columns_ = 1;
    ItemBuilder builder_;
    float cell_extent_ = 96.0F;
    float cell_width_ = 0.0F;
    float offset_ = 0.0F;
    float cache_extent_ = 200.0F;
    float viewport_height_ = 0.0F;
    std::map<int, Node> live_;
};

}  // namespace aurora
