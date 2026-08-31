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
 * @brief 二维虚拟网格（§6 新增）：纵向滚动 + 固定列数 + 固定单元格高度，
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
    GridView(int count, int columns, ItemBuilder builder, float cell_extent = 96.0f)
        : m_count(count < 0 ? 0 : count),
          m_columns(columns > 0 ? columns : (Diagnostics::degraded("layout", "GridView columns 非正已降级为 1"), 1)),
          m_builder(std::move(builder)),
          m_cell_extent(cell_extent > 0.0f
                            ? cell_extent
                            : (Diagnostics::degraded("layout", "GridView cell_extent 非正已降级为 96"), 96.0f)) {
        set_relayout_boundary(true); // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "GridView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "GridView",
            .properties = {
                { .name="count", .type="int", .default_value="0", .required=true, .note="总项数", .json_type="integer", .enum_values={}, .min_value="0" },
                { .name = "columns", .type = "int", .default_value = "2", .required = true, .note = "列数", .json_type = "integer", .enum_values = {}, .min_value = "1" },
                { .name = "cell_extent", .type = "float", .default_value = "96.0", .required = false, .note = "单元格高(dp)（宽=视口宽/列数）", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "scroll_offset", .type = "float", .default_value = "0.0", .required = false, .note = "纵向滚动偏移(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "cache_extent", .type = "float", .default_value = "200.0", .required = false, .note = "可见区外预取缓冲(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            },
            .events = {},
            .children_policy = "none",
            .invariants = { "count >= 0", "columns >= 1", "cell_extent > 0" },
            .examples = { "au::GridView(1000, 3, [](int i){ return au::Text(std::to_string(i)); }, 96.0f)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    [[nodiscard]] auto count() const -> int { return m_count; }
    [[nodiscard]] auto columns() const -> int { return m_columns; }
    [[nodiscard]] auto scroll_offset() const -> float { return m_offset; }
    [[nodiscard]] auto cell_extent() const -> float { return m_cell_extent; }
    [[nodiscard]] auto live_item_count() const -> std::size_t { return m_live.size(); }

    /// @brief 总行数。
    [[nodiscard]] auto row_count() const -> int { return (m_count + m_columns - 1) / m_columns; }
    /// @brief 总内容高度。
    [[nodiscard]] auto content_height() const -> float { return static_cast<float>(row_count()) * m_cell_extent; }
    /// @brief 最大滚动偏移。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        return std::max(0.0f, content_height() - m_viewport_height);
    }

    auto set_scroll_offset(float offset) -> void {
        const float clamped = std::clamp(offset, 0.0f, max_scroll_offset());
        if (clamped != m_offset) {
            m_offset = clamped;
            mark_needs_layout();
            mark_needs_paint();
        }
    }

    auto set_cache_extent(float extent) -> GridView & {
        m_cache_extent = extent < 0.0f ? 0.0f : extent;
        return *this;
    }

    /// @brief 当前可见行范围 [first_row, last_row)（含 cache_extent 缓冲）。
    [[nodiscard]] auto visible_row_range() const -> std::pair<int, int> {
        if (m_count == 0 || m_cell_extent <= 0.0f || m_viewport_height <= 0.0f) {
            return { 0, 0 };
        }
        const float lo = std::max(0.0f, m_offset - m_cache_extent);
        const float hi = m_offset + m_viewport_height + m_cache_extent;
        const int first = std::clamp(static_cast<int>(std::floor(lo / m_cell_extent)), 0, row_count());
        const int last = std::clamp(static_cast<int>(std::ceil(hi / m_cell_extent)), 0, row_count());
        return { first, last };
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        set_scroll_offset(m_offset - (e.delta_y * m_aurora_scroll_step));
        e.handled = true;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["count"] = m_count;
        props["columns"] = m_columns;
        props["cell_extent"] = m_cell_extent;
        props["scroll_offset"] = m_offset;
        props["cache_extent"] = m_cache_extent;
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &val : m_live | std::views::values) {
            fn(val.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx)
        -> Size override { // NOLINT(readability-function-cognitive-complexity) 虚拟网格布局核心算法
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 320.0f, .height = 480.0f };
        }
        m_viewport_height = self.height;
        m_cell_width = self.width / static_cast<float>(m_columns);

        const auto [first, last] = visible_row_range();

        // 回收滚出窗口的实例
        for (auto it = m_live.begin(); it != m_live.end();) {
            const int row = it->first / m_columns;
            if (row < first || row >= last) {
                it = m_live.erase(it);
            } else {
                ++it;
            }
        }
        // 构建新进入窗口的实例
        if (m_builder) {
            for (int r = first; r < last; ++r) {
                for (int col = 0; col < m_columns; ++col) {
                    const int idx = (r * m_columns) + col;
                    if (idx >= m_count) {
                        break;
                    }
                    if (!m_live.contains(idx)) {
                        Node item = m_builder(idx);
                        if (item) {
                            item.widget().mount(ctx);
                            m_live.emplace(idx, std::move(item));
                        }
                    }
                }
            }
        }
        // 布局存活实例
        Constraints cell_c;
        cell_c.min = Size{ .width = m_cell_width, .height = m_cell_extent };
        cell_c.max = Size{ .width = m_cell_width, .height = m_cell_extent };
        for (auto &kv : m_live) {
            kv.second.widget().set_layout_parent(this);
            kv.second.widget().layout(cell_c, ctx);
            const int row = kv.first / m_columns;
            const int col = kv.first % m_columns;
            const float x = static_cast<float>(col) * m_cell_width;
            const float y = (static_cast<float>(row) * m_cell_extent) - m_offset;
            kv.second.set_bounds(Rect{ .origin = Point{ .x = x, .y = y },
                                       .size = Size{ .width = m_cell_width, .height = m_cell_extent } });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口矩形：避免被圆角/裁剪父容器包裹时 Painter 慢路径越界访问
        // （与 LazyList 同类崩溃修复，见架构 §已知崩溃根因）。
        p.push_clip(bounds);
        for (auto &val : m_live | std::views::values) {
            const Rect cb = val.bounds();
            if (cb.origin.y + cb.size.height < 0.0f || cb.origin.y > bounds.size.height) {
                continue;
            }
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            val.widget().paint(p, global, ctx);
        }
        p.pop_clip();
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        (void)ctx;
        // 整视口优先返回自身（与 Scroll / LazyList 一致）：滚轮事件须命中被滚动容器本身，
        // 否则落到子单元格后 on_scroll 为空操作，网格永不滚动。点击 / 指针命中经
        // on_hit_test_chain 递归子节点，不受此处影响。
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (auto &val : m_live | std::views::values) {
            const Rect cb = val.bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
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
        for (auto &val : m_live | std::views::values) {
            val.widget().tick(now);
        }
    }

  private:
    static constexpr float m_aurora_scroll_step = 40.0f;

    int m_count = 0;
    int m_columns = 1;
    ItemBuilder m_builder;
    float m_cell_extent = 96.0f;
    float m_cell_width = 0.0f;
    float m_offset = 0.0f;
    float m_cache_extent = 200.0f;
    float m_viewport_height = 0.0f;
    std::map<int, Node> m_live;
};

} // namespace aurora
