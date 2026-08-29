#include "aurora/layout/flex_layouter.h"

#include <algorithm>

namespace aurora {

// 布局工作态：把一个 FlexLayouter::layout 调用的中间量集中起来，
// 使各趟 pass（测量 / 主轴分配 / 交叉轴对齐 / 定位后处理）成为独立、可读、可单测的成员函数。
// 全部算术与原单体函数逐位一致（无重排、无近似），golden 像素基线不变。
namespace {

struct FlexLayoutContext {
    Flex m_config;
    Constraints m_parent;
    std::vector<FlexItem> m_items;
    bool m_horizontal;
    bool m_reverse;
    int m_main_axis;
    float m_inf;
    bool m_main_finite;

    float m_parent_max_main;
    float m_parent_min_main;
    float m_parent_max_cross;
    float m_parent_min_cross;
    float m_gap;

    // ---- 工作态（逐趟累积）----
    std::vector<Size> sizes;
    float used_main = 0.0f;
    float max_cross = 0.0f;
    float total_flex = 0.0f;

    float container_main = 0.0f;
    float container_cross = 0.0f;
    float free_space = 0.0f;

    float leading = 0.0f;
    float between = 0.0f;

    FlexLayoutContext(const Flex &cfg, const Constraints &p, const std::vector<FlexItem> &it)
        : m_config(cfg), m_parent(p), m_items(it),
          m_horizontal(cfg.direction == FlexDirection::Row || cfg.direction == FlexDirection::RowReverse),
          m_reverse(cfg.direction == FlexDirection::RowReverse || cfg.direction == FlexDirection::ColumnReverse),
          m_main_axis(m_horizontal ? 0 : 1), m_inf(Size::infinity().width), m_main_finite(p_max_main() != m_inf),
          m_parent_max_main(p_max_main()), m_parent_min_main(p_min_main()), m_parent_max_cross(p_max_cross()),
          m_parent_min_cross(p_min_cross()), m_gap(cfg.gap > 0.0f ? cfg.gap : 0.0f), sizes(it.size(), Size{}) {}

    // ---- 轴访问器（与主轴/交叉轴选择绑定）----
    [[nodiscard]] auto get_main(const Size &s) const -> float { return m_main_axis == 0 ? s.width : s.height; }
    [[nodiscard]] auto get_cross(const Size &s) const -> float { return m_main_axis == 1 ? s.width : s.height; }
    auto set_main(Size &s, float v) const -> void {
        if (m_main_axis == 0) {
            s.width = v;
        } else {
            s.height = v;
        }
    }
    auto set_cross(Size &s, float v) const -> void {
        if (m_main_axis == 1) {
            s.width = v;
        } else {
            s.height = v;
        }
    }

  private:
    [[nodiscard]] auto p_max_main() const -> float {
        return m_main_axis == 0 ? m_parent.max.width : m_parent.max.height;
    }
    [[nodiscard]] auto p_min_main() const -> float {
        return m_main_axis == 0 ? m_parent.min.width : m_parent.min.height;
    }
    [[nodiscard]] auto p_max_cross() const -> float {
        return m_main_axis == 1 ? m_parent.max.width : m_parent.max.height;
    }
    [[nodiscard]] auto p_min_cross() const -> float {
        return m_main_axis == 1 ? m_parent.min.width : m_parent.min.height;
    }

  public:
    // 阶段一(A)+ (B) + 固定间距计入：先测量非 flex 子项，再按权重瓜分剩余主轴，最后把
    // 相邻子项间的固定间距计入容器主轴占用。
    auto measure_pass() -> void {
        const size_t n = m_items.size();
        for (size_t i = 0; i < n; ++i) {
            if (m_items[i].flex > 0.0f && m_main_finite) {
                total_flex += m_items[i].flex; // 延后到阶段一(B)
                continue;
            }
            Constraints cc;
            set_main(cc.min, 0.0f);
            set_cross(cc.min, m_parent_min_cross);
            const float remaining = m_main_finite ? std::max(0.0f, m_parent_max_main - used_main) : m_inf;
            set_main(cc.max, remaining);
            set_cross(cc.max, m_parent_max_cross);
            const Size s = m_items[i].do_measure(cc);
            sizes[i] = s;
            used_main += get_main(s);
            max_cross = std::max(max_cross, get_cross(s));
        }

        // flex 子项按权重瓜分剩余主轴空间（仅主轴有限时才有意义）。
        if (m_main_finite && total_flex > 0.0f) {
            // 固定间距（相邻子项间）须先从可用主轴空间中扣除，否则 flex 子项会溢出容器。
            const float total_gap = (n > 1) ? static_cast<float>(n - 1) * m_gap : 0.0f;
            const float free = std::max(0.0f, m_parent_max_main - used_main - total_gap);
            for (size_t i = 0; i < n; ++i) {
                if (m_items[i].flex <= 0.0f) {
                    continue;
                }
                const float alloc = free * m_items[i].flex / total_flex;
                Constraints cc;
                set_main(cc.min, 0.0f);
                set_cross(cc.min, m_parent_min_cross);
                set_main(cc.max, std::max(0.0f, alloc));
                set_cross(cc.max, m_parent_max_cross);
                const Size s = m_items[i].do_measure(cc);
                sizes[i] = s;
                used_main += get_main(s);
                max_cross = std::max(max_cross, get_cross(s));
            }
        }

        // 固定间距占用（相邻子项间）：计入容器主轴尺寸，使 gap 成为布局的内在部分。
        if (n > 1) {
            used_main += static_cast<float>(n - 1) * m_gap;
        }
    }

    // 主轴分配 pass：容器主轴尺寸取内容所需（或撑满父级），剩余即对齐时的自由空间；
    // 容器交叉轴尺寸取子项最大交叉尺寸夹入父约束。
    auto main_axis_alloc_pass() -> void {
        // MainAxisSize::Max：把容器主轴撑满父级可用主轴空间（仅当主轴约束有限时），
        // 从而产生可见自由空间，使 main_axis 对齐（Center/End/Space*）真正生效。
        // MainAxisSize::Min（默认）：容器主轴取内容所需尺寸，与历史行为保持一致。
        if (m_config.main_axis_size == MainAxisSize::Max && m_main_finite) {
            container_main = m_parent_max_main;
        } else {
            container_main = std::clamp(used_main, m_parent_min_main, m_parent_max_main);
        }
        free_space = container_main - used_main;
        free_space = std::max(free_space, 0.0f);

        container_cross = std::clamp(max_cross, m_parent_min_cross, m_parent_max_cross);
    }

    // 交叉轴对齐 pass（容器交叉轴尺寸已定，此处仅为语义分组占位，保持 pass 边界清晰）。
    static auto cross_axis_align_pass() -> void {
        // 容器交叉轴尺寸由 main_axis_alloc_pass 确定；子项交叉轴定位在 place_children_pass 完成。
    }

    // 主轴对齐 pass：由 main_axis 对齐方式推出首项前导间距 leading 与子项间间距 between。
    auto main_axis_align_pass() -> void {
        const size_t n = m_items.size();
        if (n == 0) {
            return;
        }
        switch (m_config.main_axis) {
        case MainAxisAlignment::Start:
            leading = 0.0f;
            between = 0.0f;
            break;
        case MainAxisAlignment::Center:
            leading = free_space / 2.0f;
            between = 0.0f;
            break;
        case MainAxisAlignment::End:
            leading = free_space;
            between = 0.0f;
            break;
        case MainAxisAlignment::SpaceBetween:
            leading = 0.0f;
            between = (n > 1) ? (free_space / static_cast<float>(n - 1)) : 0.0f;
            break;
        case MainAxisAlignment::SpaceAround:
            leading = free_space / static_cast<float>(n) / 2.0f;
            between = free_space / static_cast<float>(n);
            break;
        case MainAxisAlignment::SpaceEvenly:
            leading = free_space / static_cast<float>(n + 1);
            between = free_space / static_cast<float>(n + 1);
            break;
        }
    }

    // 定位后处理 pass：逐子项定位（主轴：前导 + 累加；交叉轴：按对齐方式），
    // 反向布局时沿主轴镜像每个子项位置（对齐语义随之反向，与 Flutter 一致）。
    auto place_children_pass(std::vector<Rect> &rects) const -> void {
        const size_t n = m_items.size();
        rects.resize(n);
        float pos = leading;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                pos += m_gap; // 相邻子项间的固定间距（首个子项前不加）
            }
            Size s = sizes[i];
            float cross_size = get_cross(s);
            float cross_pos = 0.0f;
            switch (m_config.cross_axis) {
            case CrossAxisAlignment::Start: cross_pos = 0.0f; break;
            case CrossAxisAlignment::Center: cross_pos = (container_cross - cross_size) / 2.0f; break;
            case CrossAxisAlignment::End: cross_pos = container_cross - cross_size; break;
            case CrossAxisAlignment::Stretch:
                cross_size = container_cross;
                cross_pos = 0.0f;
                break;
            }
            cross_pos = std::max(cross_pos, 0.0f);

            Point origin;
            Size rsize;
            if (m_horizontal) {
                origin = Point{ .x = pos, .y = cross_pos };
                rsize = Size{ .width = s.width, .height = cross_size };
            } else {
                origin = Point{ .x = cross_pos, .y = pos };
                rsize = Size{ .width = cross_size, .height = s.height };
            }
            rects[i] = Rect{ .origin = origin, .size = rsize };
            pos += get_main(s) + between;
        }

        if (m_reverse) {
            for (Rect &r : rects) {
                const float m = get_main(r.size);
                const float start_m = m_horizontal ? r.origin.x : r.origin.y;
                if (m_horizontal) {
                    r.origin.x = container_main - (start_m + m);
                } else {
                    r.origin.y = container_main - (start_m + m);
                }
            }
        }
    }
};

} // namespace

auto FlexLayouter::layout(const Flex &config, const Constraints &parent, const std::vector<FlexItem> &items)
    -> FlexLayout {
    FlexLayoutContext ctx(config, parent, items);

    // 测量 pass：非 flex + flex 瓜分 + 固定间距计入容器主轴占用。
    ctx.measure_pass();
    // 主轴分配 pass：容器尺寸 + 自由空间 + 交叉轴尺寸。
    ctx.main_axis_alloc_pass();
    // 交叉轴对齐 pass（容器交叉轴尺寸已定）。
    FlexLayoutContext::cross_axis_align_pass();
    // 主轴对齐 pass：leading / between。
    ctx.main_axis_align_pass();
    // 定位后处理 pass：逐子项定位 + 反向镜像。
    std::vector<Rect> rects;
    ctx.place_children_pass(rects);

    FlexLayout result;
    result.children = std::move(rects);
    if (ctx.m_horizontal) {
        result.size = Size{ .width = ctx.container_main, .height = ctx.container_cross };
    } else {
        result.size = Size{ .width = ctx.container_cross, .height = ctx.container_main };
    }
    return result;
}

} // namespace aurora
