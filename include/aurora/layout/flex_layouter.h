#pragma once

#include <vector>

#include "aurora/core/types.h"
#include "aurora/layout/flex.h"

namespace aurora {

/// @brief 布局上下文基类：提供对齐存储，确保 void* 安全 reinterpret_cast 回派生类型。
///
/// 容器在 on_layout 中将派生结构体存入 std::vector，再把指针经 void* 传给 FlexItem::make，
/// 布局器回调时由 trampoline 函数 reinterpret_cast 回具体类型——零堆分配、类型安全。
struct LayoutCtxBase {
    /// @brief 派生结构体经 void* 存放于 std::vector，布局器回调时 reinterpret_cast 回具体类型。
    /// alignas 已保证自然对齐，无需额外存储字段。
};

/**
 * @brief 单个 flex 子项：权重 + 测量回调（给定约束返回自身尺寸）。
 *
 * 与具体 widget 解耦：容器（Row/Column）把"测某子节点"封装成 `measure` 回调传给布局器，
 * 布局器只负责按 Flutter 语义分配主轴空间并算位置，不做任何 widget 专属逻辑。
 */
struct FlexItem {
    float flex = 0.0f; ///< 主轴权重；0 = 不扩展（仅占内容尺寸）

    /// @brief 测量函数指针（零堆分配）：通过 void* 上下文捕获外部状态，避免 std::function 堆分配。
    using MeasureFn = auto (*)(void *ctx, const Constraints &) -> Size;
    MeasureFn measure = nullptr; ///< 测量函数指针
    void *measure_ctx = nullptr; ///< 测量函数上下文

    /// @brief 调用测量函数。
    [[nodiscard]] auto do_measure(const Constraints &c) const -> Size { return measure(measure_ctx, c); }

    /// @brief 工厂：将容器布局上下文打包为 FlexItem，零堆分配。
    ///
    /// @tparam Ctx  派生上下文类型（须继承 LayoutCtxBase）
    /// @param w       flex 权重
    /// @param ctx     上下文指针（由容器存放在 vector 中，生命周期 ≥ 布局调用）
    /// @param fn      trampoline 函数指针：解包 ctx → 调用实际 widget::layout
    template<typename Ctx> static auto make(float w, Ctx *ctx, MeasureFn fn) -> FlexItem {
        return FlexItem{ w, fn, ctx };
    }
};

/**
 * @brief 一次 flex 布局的结果：各子项相对容器原点的 Rect + 容器自身尺寸。
 */
struct FlexLayout {
    std::vector<Rect> children; ///< 与输入 items 顺序一致
    Size size{};                ///< 容器自身尺寸（已夹入父约束）
};

/**
 * @brief Flex 布局算法：按 Flutter 语义在子项间分配主轴空间并定位（两阶段：测 → 摆）。
 *
 * 覆盖：
 * - 方向：`Row`/`Column`/`RowReverse`/`ColumnReverse`（反向沿主轴镜像）。
 * - 主轴对齐：`Start`/`Center`/`End`/`SpaceBetween`/`SpaceAround`/`SpaceEvenly`。
 * - 交叉轴对齐：`Start`/`Center`/`End`/`Stretch`（拉伸填满容器交叉轴）。
 * - 弹性分配：权重 > 0 的子项按权重瓜分"父约束剩余空间"；权重 0 仅占内容尺寸。
 *
 * 对应 specification/03-layout-render.md §2.3 两阶段布局。与 widget 解耦，可独立单测。
 *
 * @note Thread: main-thread only
 * @note Side-effects: mutates layout
 * @note Rebuildable: no
 */
class FlexLayouter {
  public:
    static auto layout(const Flex &config, const Constraints &parent, const std::vector<FlexItem> &items) -> FlexLayout;
};

} // namespace aurora
