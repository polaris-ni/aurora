#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 横向虚拟列表属性（聚合）：主轴为水平的按需虚拟化列表。
struct LazyRowProps {
    int item_count = 0;                    ///< 子项总数
    float item_extent = 96.0f;             ///< 每个子项的固定宽度（主轴尺寸）
    EdgeInsets padding;                    ///< 内边距
    float cache_extent = 0.0f;             ///< 视口外预构建缓冲（主轴像素）
    std::function<Node(int)> item_builder; ///< 子项构造器（按需惰性调用）
};

/**
 * @brief 横向虚拟列表（镜像 `LazyList`，主轴改为水平）。
 *
 * 仅构建可见窗口（含 `cache_extent` 缓冲）内的子项，复杂度为 O(可见单元数)。
 * 横向滚轮（或拖拽）调整 `m_offset`；`on_paint` 内 `push_clip` 防止父级圆角裁剪
 * 下的慢路径越界。命中测试返回自身（作为横向滚动叶），其内部子项点击通过
 * `on_item_click` 回调（按按下位置计算索引）上报，避免虚拟化子项不可作为稳定控件。
 *
 * 采用继承式双模 API：`LazyRowProps` 字段即本控件公有字段，可直接赋值或以配置块构造。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class LazyRow : public Widget, public LazyRowProps {
  public:
    using ItemBuilder = std::function<Node(int)>;

    LazyRow() = default;
    explicit LazyRow(LazyRowProps props) {
        item_count = props.item_count;
        item_extent = props.item_extent;
        padding = props.padding;
        cache_extent = props.cache_extent;
        item_builder = std::move(props.item_builder);
        set_relayout_boundary(true); // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }
    LazyRow(int count, ItemBuilder builder, float item_extent = 96.0f)
        : m_item_count(count), m_item_builder(std::move(builder)), m_item_extent(item_extent) {
        item_count = count;
        item_builder = m_item_builder;
        set_relayout_boundary(true); // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "LazyRow"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LazyRow",
            .properties = {
                { .name = "item_count", .type = "int", .default_value = "0", .required = false, .note = "子项总数" },
                { .name = "item_extent", .type = "float", .default_value = "96.0", .required = false, .note = "子项固定宽度(px)" },
                { .name = "cache_extent", .type = "float", .default_value = "0.0", .required = false, .note = "视口外预构建缓冲(px)" },
            },
            .events = { { "on_item_click", "void(int)", "子项被点击时回调（参数为索引）" } },
            .children_policy = "virtual",
            .examples = { "au::LazyRow{ 10, [](int i){ return au::Text(std::to_string(i)); } }" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["item_count"] = item_count;
        props["item_extent"] = m_item_extent;
        props["cache_extent"] = cache_extent;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("item_count")) {
            item_count = props["item_count"].get<int>();
            m_item_count = item_count;
        }
        if (props.contains("item_extent")) {
            m_item_extent = props["item_extent"].get<float>();
            item_extent = m_item_extent;
        }
        if (props.contains("cache_extent")) {
            cache_extent = props["cache_extent"].get<float>();
        }
    }

    // ---- 双模 setter ----
    auto set_item_count(int c) -> LazyRow & {
        item_count = c;
        m_item_count = c;
        m_built.clear();
        mark_needs_layout();
        return *this;
    }
    auto set_item_builder(ItemBuilder b) -> LazyRow & {
        m_item_builder = std::move(b);
        item_builder = m_item_builder;
        m_built.clear();
        mark_needs_layout();
        return *this;
    }
    auto set_item_extent(float e) -> LazyRow & {
        m_item_extent = e;
        item_extent = e;
        mark_needs_layout();
        return *this;
    }
    auto set_padding(EdgeInsets e) -> LazyRow & {
        padding = e;
        mark_needs_layout();
        return *this;
    }
    auto set_cache_extent(float e) -> LazyRow & {
        cache_extent = e;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置子项点击回调（参数为子项索引）。
    auto set_on_item_click(std::function<void(int)> cb) -> LazyRow & {
        m_on_item_click = std::move(cb);
        return *this;
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        const float vw = std::max(1.0f, size().width - padding.left - padding.right);
        const float max_off = std::max(0.0f, m_full_content - vw);
        m_offset = std::max(0.0f, std::min(max_off, m_offset + (e.delta_y * m_item_extent * 0.5f)));
        e.handled = true;
        mark_needs_paint();
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            m_pressed = true;
            m_dragging = false;
            m_press_local = e.local_position;
            m_press_index = index_at(e.local_position.x);
            m_last_drag_x = e.local_position.x;
        } else if (e.action == MouseAction::Move) {
            if (m_pressed) {
                const float dx = e.local_position.x - m_last_drag_x;
                if (std::fabs(e.local_position.x - m_press_local.x) > 4.0f) {
                    m_dragging = true;
                }
                if (m_dragging) {
                    const float vw = std::max(1.0f, size().width - padding.left - padding.right);
                    const float max_off = std::max(0.0f, m_full_content - vw);
                    m_offset = std::max(0.0f, std::min(max_off, m_offset - dx));
                    m_last_drag_x = e.local_position.x;
                    e.handled = true;
                    mark_needs_paint();
                }
            }
        } else if (e.action == MouseAction::Release) {
            if (m_pressed && !m_dragging && m_press_index >= 0 && m_press_index < m_item_count) {
                if (m_on_item_click) {
                    m_on_item_click(m_press_index);
                }
                e.handled = true;
            }
            m_pressed = false;
            m_dragging = false;
        }
    }

  protected:
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float h_pad = padding.left + padding.right;
        const float v_pad = padding.top + padding.bottom;
        const float full = (static_cast<float>(m_item_count) * m_item_extent) + h_pad;
        const float h = m_item_extent + v_pad;
        m_full_content = full;
        return c.constrain(Size{ .width = full, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        const float v_pad = padding.top + padding.bottom;
        const float ch = std::max(1.0f, bounds.size.height - v_pad);
        const int first = std::max(0, static_cast<int>(std::floor((m_offset - cache_extent) / m_item_extent)));
        const int last = std::min(
            m_item_count - 1,
            static_cast<int>(std::floor((m_offset + bounds.size.width - padding.left + cache_extent) / m_item_extent)));
        // 标注：本帧不需要但仍在缓存中的子项回收
        for (size_t i = 0; i < m_built.size(); ++i) {
            if (m_built[i] && (std::cmp_less(i, first) || std::cmp_greater(i, last))) {
                m_built[i] = Node{};
            }
        }
        if (m_built.size() <= static_cast<size_t>(last)) {
            m_built.resize(static_cast<size_t>(last) + 1);
        }

        for (int i = first; i <= last; ++i) {
            if (std::cmp_greater_equal(i, m_built.size())) {
                m_built.resize(static_cast<size_t>(i) + 1);
            }
            if (!m_built[i]) {
                if (m_item_builder) {
                    m_built[i] = Node{ m_item_builder(i) };
                } else {
                    continue;
                }
            }
            const float x = padding.left + (static_cast<float>(i) * m_item_extent) - m_offset;
            const Rect cb{ .origin = Point{ .x = x, .y = padding.top },
                           .size = Size{ .width = m_item_extent, .height = ch } };
            m_built[i].set_bounds(cb);
            m_built[i].widget().layout(Constraints{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = cb.size },
                                       ctx);
            m_built[i].widget().paint(p, Rect{ .origin = bounds.origin + cb.origin, .size = cb.size }, ctx);
        }
        p.pop_clip();
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local)) {
            return {};
        }
        (void)ctx;
        // 虚拟化子项不以稳定控件形态参与命中链：横向列表自身作为点击/滚动叶。
        return std::vector{ HitNode{ this, weak_from_this(), bounds.origin } };
    }
#pragma GCC diagnostic pop

  private:
    [[nodiscard]] auto index_at(float local_x) const -> int {
        const float idx = std::floor((local_x - padding.left + m_offset) / m_item_extent);
        const int i = static_cast<int>(idx);
        if (i < 0 || i >= m_item_count) {
            return -1;
        }
        return i;
    }

    int m_item_count = 0;
    ItemBuilder m_item_builder;
    float m_item_extent = 96.0f;
    float m_full_content = 0.0f;
    float m_offset = 0.0f;
    std::vector<Node> m_built;

    std::function<void(int)> m_on_item_click;
    bool m_pressed = false;
    bool m_dragging = false;
    Point m_press_local{ .x = 0.0f, .y = 0.0f };
    float m_last_drag_x = 0.0f;
    int m_press_index = -1;
};

} // namespace aurora
