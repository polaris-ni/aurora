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
    int item_count = 0;  ///< 子项总数
    float item_extent = 96.0F;  ///< 每个子项的固定宽度（主轴尺寸）
    EdgeInsets padding;  ///< 内边距
    float cache_extent = 0.0F;  ///< 视口外预构建缓冲（主轴像素）
    std::function<Node(int)> item_builder;  ///< 子项构造器（按需惰性调用）
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
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }
    LazyRow(int count, ItemBuilder builder, float item_extent = 96.0F)
        : item_count_(count), item_builder_(std::move(builder)), item_extent_(item_extent) {
        item_count = count;
        item_builder = item_builder_;
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "LazyRow"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LazyRow",
            .properties =
                {
                    {.name = "item_count", .type = "int", .default_value = "0", .required = false, .note = "子项总数"},
                    {.name = "item_extent",
                     .type = "float",
                     .default_value = "96.0",
                     .required = false,
                     .note = "子项固定宽度(px)"},
                    {.name = "cache_extent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "视口外预构建缓冲(px)"},
                },
            .events = {{"on_item_click", "void(int)", "子项被点击时回调（参数为索引）"}},
            .children_policy = "virtual",
            .examples = {"au::LazyRow{ 10, [](int i){ return au::Text(std::to_string(i)); } }"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["item_count"] = item_count;
        props["item_extent"] = item_extent_;
        props["cache_extent"] = cache_extent;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("item_count")) {
            item_count = props["item_count"].get<int>();
            item_count_ = item_count;
        }
        if (props.contains("item_extent")) {
            item_extent_ = props["item_extent"].get<float>();
            item_extent = item_extent_;
        }
        if (props.contains("cache_extent")) {
            cache_extent = props["cache_extent"].get<float>();
        }
    }

    // ---- 双模 setter ----
    auto set_item_count(int c) -> LazyRow & {
        item_count = c;
        item_count_ = c;
        built_.clear();
        mark_needs_layout();
        return *this;
    }
    auto set_item_builder(ItemBuilder b) -> LazyRow & {
        item_builder_ = std::move(b);
        item_builder = item_builder_;
        built_.clear();
        mark_needs_layout();
        return *this;
    }
    auto set_item_extent(float e) -> LazyRow & {
        item_extent_ = e;
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
        on_item_click_ = std::move(cb);
        return *this;
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        const float vw = std::max(1.0F, size().width - padding.left - padding.right);
        const float max_off = std::max(0.0F, full_content_ - vw);
        offset_ = std::max(0.0F, std::min(max_off, offset_ + (e.delta_y * item_extent_ * 0.5F)));
        e.is_handled = true;
        mark_needs_paint();
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            pressed_ = true;
            dragging_ = false;
            press_local_ = e.local_position;
            press_index_ = index_at(e.local_position.x);
            last_drag_x_ = e.local_position.x;
        } else if (e.action == MouseAction::Move) {
            if (pressed_) {
                const float dx = e.local_position.x - last_drag_x_;
                if (std::fabs(e.local_position.x - press_local_.x) > 4.0F) {
                    dragging_ = true;
                }
                if (dragging_) {
                    const float vw = std::max(1.0F, size().width - padding.left - padding.right);
                    const float max_off = std::max(0.0F, full_content_ - vw);
                    offset_ = std::max(0.0F, std::min(max_off, offset_ - dx));
                    last_drag_x_ = e.local_position.x;
                    e.is_handled = true;
                    mark_needs_paint();
                }
            }
        } else if (e.action == MouseAction::Release) {
            if (pressed_ && !dragging_ && press_index_ >= 0 && press_index_ < item_count_) {
                if (on_item_click_) {
                    on_item_click_(press_index_);
                }
                e.is_handled = true;
            }
            pressed_ = false;
            dragging_ = false;
        }
    }

  protected:
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float h_pad = padding.left + padding.right;
        const float v_pad = padding.top + padding.bottom;
        const float full = (static_cast<float>(item_count_) * item_extent_) + h_pad;
        const float h = item_extent_ + v_pad;
        full_content_ = full;
        return c.constrain(Size{.width = full, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        const float v_pad = padding.top + padding.bottom;
        const float ch = std::max(1.0F, bounds.size.height - v_pad);
        const int first = std::max(0, static_cast<int>(std::floor((offset_ - cache_extent) / item_extent_)));
        const int last = std::min(
            item_count_ - 1,
            static_cast<int>(std::floor((offset_ + bounds.size.width - padding.left + cache_extent) / item_extent_)));
        // 标注：本帧不需要但仍在缓存中的子项回收
        for (size_t i = 0; i < built_.size(); ++i) {
            if (built_[i] && (std::cmp_less(i, first) || std::cmp_greater(i, last))) {
                built_[i] = Node{};
            }
        }
        if (built_.size() <= static_cast<size_t>(last)) {
            built_.resize(static_cast<size_t>(last) + 1);
        }

        for (int i = first; i <= last; ++i) {
            if (std::cmp_greater_equal(i, built_.size())) {
                built_.resize(static_cast<size_t>(i) + 1);
            }
            if (!built_[i]) {
                if (item_builder_) {
                    built_[i] = Node{item_builder_(i)};
                } else {
                    continue;
                }
            }
            const float x = padding.left + (static_cast<float>(i) * item_extent_) - offset_;
            const Rect cb{.origin = Point{.x = x, .y = padding.top}, .size = Size{.width = item_extent_, .height = ch}};
            built_[i].set_bounds(cb);
            built_[i].widget().layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = cb.size}, ctx);
            built_[i].widget().paint(p, Rect{.origin = bounds.origin + cb.origin, .size = cb.size}, ctx);
        }
        p.pop_clip();
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)) {
            return {};
        }
        (void)ctx;
        // 虚拟化子项不以稳定控件形态参与命中链：横向列表自身作为点击/滚动叶。
        return std::vector{HitNode{this, weak_from_this(), bounds.origin}};
    }
#pragma GCC diagnostic pop

  private:
    [[nodiscard]] auto index_at(float local_x) const -> int {
        const float idx = std::floor((local_x - padding.left + offset_) / item_extent_);
        const int i = static_cast<int>(idx);
        if (i < 0 || i >= item_count_) {
            return -1;
        }
        return i;
    }

    int item_count_ = 0;
    ItemBuilder item_builder_;
    float item_extent_ = 96.0F;
    float full_content_ = 0.0F;
    float offset_ = 0.0F;
    std::vector<Node> built_;

    std::function<void(int)> on_item_click_;
    bool pressed_ = false;
    bool dragging_ = false;
    Point press_local_{.x = 0.0F, .y = 0.0F};
    float last_drag_x_ = 0.0F;
    int press_index_ = -1;
};

}  // namespace aurora
