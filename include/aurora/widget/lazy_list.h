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
 * @brief 虚拟滚动列表（规格 §2.2）：仅实例化可见区域 + 预取缓冲区的子项。
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
    LazyList(int count, ItemBuilder builder, float item_extent = 48.0f)
        : m_count(count < 0 ? 0 : count), m_builder(std::move(builder)),
          m_item_extent(item_extent > 0.0f
                            ? item_extent
                            : (Diagnostics::degraded("layout", "LazyList item_extent 非正值已降级为 48"), 48.0f)) {
        set_relayout_boundary(true); // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "LazyList"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LazyList",
            .properties = {
                { .name="count", .type="int", .default_value="0", .required=true, .note="总项数", .json_type="integer", .enum_values={}, .min_value="0" },
                { .name = "item_extent", .type = "float", .default_value = "48.0", .required = false, .note = "固定行高(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "scroll_offset", .type = "float", .default_value = "0.0", .required = false, .note = "当前滚动偏移(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "cache_extent", .type = "float", .default_value = "200.0", .required = false, .note = "可见区外预取缓冲(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            },
            .events = {},
            .children_policy = "none",
            .invariants = { "count >= 0", "item_extent > 0" },
            .examples = { "au::LazyList(10000, [](int i){ return au::Text(std::to_string(i)); }, 48.0f)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 总项数。
    [[nodiscard]] auto count() const -> int { return m_count; }

    /// @brief 当前滚动偏移（dp，向下为正）。
    [[nodiscard]] auto scroll_offset() const -> float { return m_offset; }

    /// @brief 设置滚动偏移（钳制到内容范围）。
    auto set_scroll_offset(float offset) -> void {
        const float max_off = max_scroll_offset();
        const float clamped = std::clamp(offset, 0.0f, max_off);
        if (clamped != m_offset) {
            m_offset = clamped;
            mark_needs_layout();
            mark_needs_paint();
        }
    }

    /// @brief 滚动到指定项（使其顶端对齐可视区顶端）。
    auto scroll_to_item(int index) -> void {
        set_scroll_offset(static_cast<float>(std::clamp(index, 0, std::max(0, m_count - 1))) * m_item_extent);
    }

    /// @brief 最大滚动偏移（内容高 - 视口高，不小于 0）。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        return std::max(0.0f, content_height() - m_viewport_height);
    }

    /// @brief 总内容高度。
    [[nodiscard]] auto content_height() const -> float { return static_cast<float>(m_count) * m_item_extent; }

    /// @brief 当前可见范围 [first, last)（含 cache_extent 缓冲）。
    [[nodiscard]] auto visible_range() const -> std::pair<int, int> {
        if (m_count == 0 || m_item_extent <= 0.0f || m_viewport_height <= 0.0f) {
            return { 0, 0 };
        }
        const float lo = std::max(0.0f, m_offset - m_cache_extent);
        const float hi = m_offset + m_viewport_height + m_cache_extent;
        const int first = std::clamp(static_cast<int>(std::floor(lo / m_item_extent)), 0, m_count);
        const int last = std::clamp(static_cast<int>(std::ceil(hi / m_item_extent)), 0, m_count);
        return { first, last };
    }

    /// @brief 当前存活（已实例化）的子项数（测试观测点：应远小于 count）。
    [[nodiscard]] auto live_item_count() const -> std::size_t { return m_live.size(); }

    /// @brief 设置预取缓冲区高度（链式）。
    auto set_cache_extent(float extent) -> LazyList & {
        m_cache_extent = extent < 0.0f ? 0.0f : extent;
        return *this;
    }

    /// @brief 滚轮滚动。
    auto on_scroll(ScrollEvent &e) -> void override {
        set_scroll_offset(m_offset - (e.delta_y * m_aurora_scroll_step));
        e.handled = true;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["count"] = m_count;
        props["item_extent"] = m_item_extent;
        props["scroll_offset"] = m_offset;
        props["cache_extent"] = m_cache_extent;
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &val : m_live | std::views::values) {
            fn(val.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 320.0f, .height = 480.0f };
        }
        m_viewport_height = self.height;

        // 计算可见窗口并同步存活实例：新进入的构建、滚出的回收
        const auto [first, last] = visible_range();

        // 回收滚出窗口的实例
        for (auto it = m_live.begin(); it != m_live.end();) {
            if (it->first < first || it->first >= last) {
                it = m_live.erase(it);
            } else {
                ++it;
            }
        }
        // 构建新进入窗口的实例
        if (m_builder) {
            for (int i = first; i < last; ++i) {
                if (!m_live.contains(i)) {
                    Node item = m_builder(i);
                    if (item) {
                        item.widget().mount(ctx);
                        m_live.emplace(i, std::move(item));
                    }
                }
            }
        }
        // 布局存活实例（固定行高，宽度撑满视口）
        Constraints item_c;
        item_c.min = Size{ .width = self.width, .height = m_item_extent };
        item_c.max = Size{ .width = self.width, .height = m_item_extent };
        for (auto &kv : m_live) {
            kv.second.widget().set_layout_parent(this);
            kv.second.widget().layout(item_c, ctx);
            const float y = (static_cast<float>(kv.first) * m_item_extent) - m_offset;
            kv.second.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = y },
                                       .size = Size{ .width = self.width, .height = m_item_extent } });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口：软件 Painter 直接写内存缓冲，子项（含 cache 区，局部 y 可能为负或越界）
        // 若绘制坐标溢出缓冲会触发访问越界（0xC0000005）。必须裁剪到视口，与 scroll.h 的
        // ScrollingWidget 一致。
        p.push_clip(bounds);
        for (auto &kv : m_live | std::views::values) {
            const Rect cb = kv.bounds();
            // 完全在视口外的跳过绘制（缓冲区实例保留但不绘制）；用包含性边界判断，
            // 避免浮点边界处（cb 恰好贴边）的行被误绘到视口外。
            if (cb.origin.y + cb.size.height <= 0.0f || cb.origin.y >= bounds.size.height) {
                continue;
            }
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            kv.widget().paint(p, global, ctx);
        }
        p.pop_clip();
    }

    // 滚轮命中：整视口优先返回自身（与 Scroll 一致）。若改回「子项优先」，
    // 光标落在子项（Text）时 hit_test 解析为叶控件，ScrollEvent 派发到叶子后 on_scroll
    // 为空操作，列表永不滚动。指针事件仍走 on_hit_test_chain（保留子项点击命中）。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (auto &kv : m_live | std::views::values) {
            const Rect cb = kv.bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                std::vector<HitNode> r = kv.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &kv : m_live | std::views::values) {
            kv.widget().tick(now);
        }
    }

  private:
    static constexpr float m_aurora_scroll_step = 40.0f; ///< 每单位滚轮增量对应的 dp

    int m_count = 0;
    ItemBuilder m_builder;
    float m_item_extent = 48.0f;
    float m_offset = 0.0f;
    float m_cache_extent = 200.0f;
    float m_viewport_height = 0.0f;
    std::map<int, Node> m_live; ///< 存活实例：index -> Node（按序遍历便于绘制）
};

} // namespace aurora
