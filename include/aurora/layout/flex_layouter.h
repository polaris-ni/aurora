#pragma once

#include <optional>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/layout/flex.h"

namespace aurora {

/// @brief 布局上下文基类：提供对齐存储，确保 void* 安全 reinterpret_cast 回派生类型。
/// 容器在 on_layout 中将派生结构体存入 std::vector，再把指针经 void* 传给 FlexItem::make，
/// 布局器回调时由 trampoline 函数 reinterpret_cast 回具体类型——零堆分配、类型安全。
struct LayoutCtxBase {
    // 派生结构体经 void* 存放于 std::vector，布局器回调时 reinterpret_cast 回具体类型。
    // alignas 已保证自然对齐，无需额外存储字段。
};

/// @brief 单个 flex 子项：权重 + 测量回调（给定约束返回自身尺寸）+ 可选基线回调。
///
/// 与具体 widget 解耦：容器（Row/Column）把"测某子节点"封装成 `measure` 回调传给布局器，
/// 布局器只负责按 Flutter 语义分配主轴空间并算位置，不做任何 widget 专属逻辑。
/// `baseline` 与 `measure` 同构（函数指针 + void* 上下文，零堆分配），且仅在
/// `cross_axis == CrossAxisAlignment::Baseline` 且主轴为水平时被布局器调用。
struct FlexItem {
    float flex = 0.0F;  ///< 主轴权重；0 = 不扩展（仅占内容尺寸）

    /// @brief 测量函数指针（零堆分配）：通过 void* 上下文捕获外部状态，避免 std::function 堆分配。
    using MeasureFn = auto (*)(void *ctx, const Constraints &) -> Size;
    MeasureFn measure = nullptr;  ///< 测量函数指针
    void *measure_ctx = nullptr;  ///< 测量函数上下文

    /// @brief 基线函数指针（零堆分配，与 `measure` 同构）。
    /// `measured` 为该项测量后的尺寸（换算 `Modifier::transform(measured).translation.y` 等
    /// 内容盒偏移需要它）；返回值语义 = **布局盒顶 → 首行基线** 的距离，`nullopt` 表示无基线
    /// （布局器按 CSS 式合成基线 = 交叉轴底边处理，见 specification/03-layout-render.md §3.8）。
    using BaselineFn = auto (*)(void *ctx, Size measured) -> std::optional<float>;
    BaselineFn baseline = nullptr;  ///< 基线函数指针（仅 Baseline 对齐路径被调用）
    void *baseline_ctx = nullptr;  ///< 基线函数上下文

    /// @brief 调用 `measure` 回调测量本子项尺寸。
    /// @param c 父级下发的测量约束。
    /// @return 回调返回的子项测量尺寸。
    [[nodiscard]] auto do_measure(const Constraints &c) const -> Size { return measure(measure_ctx, c); }

    /// @brief 调用 `baseline` 回调取首行基线（未提供回调时返回 `nullopt`）。
    /// @param measured 该项已测量的尺寸（回调据此换算内容盒纵向偏移）。
    /// @return 布局盒顶 → 首行基线的距离；`nullopt` = 无基线（布局器按交叉轴底边合成）。
    [[nodiscard]] auto do_baseline(const Size &measured) const -> std::optional<float> {
        return baseline != nullptr ? baseline(baseline_ctx, measured) : std::nullopt;
    }

    /// @brief 工厂：将容器布局上下文打包为 FlexItem，零堆分配。
    /// @tparam Ctx  派生上下文类型（须继承 LayoutCtxBase）
    /// @param w       flex 权重
    /// @param ctx     上下文指针（由容器存放在 vector 中，生命周期 ≥ 布局调用）
    /// @param fn      trampoline 函数指针：解包 ctx → 调用实际 widget::layout
    /// @return 仅带测量通道（基线通道为 `nullptr`）的 FlexItem。
    template <typename Ctx>
    static auto make(float w, Ctx *ctx, MeasureFn fn) -> FlexItem {
        return FlexItem{w, fn, ctx, nullptr, nullptr};
    }

    /// @brief 工厂（带基线通道）：`bfn` 仅在该容器的交叉轴对齐为 `Baseline` 时被调用。
    /// @tparam Ctx 派生上下文类型（须继承 LayoutCtxBase）
    /// @param w flex 权重
    /// @param ctx 测量上下文指针（由容器存放在 vector 中，生命周期 ≥ 布局调用）
    /// @param fn 测量 trampoline 函数指针：解包 ctx → 调用实际 widget::layout
    /// @param bfn 基线 trampoline（解包 ctx → 查子 widget 基线并计入内容盒偏移）
    /// @param bctx 基线函数上下文（通常与 `ctx` 同一个容器布局上下文）
    /// @return 同时带测量与基线通道的 FlexItem。
    template <typename Ctx>
    static auto make(float w, Ctx *ctx, MeasureFn fn, BaselineFn bfn, Ctx *bctx) -> FlexItem {
        return FlexItem{w, fn, ctx, bfn, bctx};
    }
};

/// @brief 一次 flex 布局的结果：各子项相对容器原点的 Rect + 容器自身尺寸。
struct FlexLayout {
    std::vector<Rect> children;  ///< 与输入 items 顺序一致
    Size size{};  ///< 容器自身尺寸（已夹入父约束）
};

/// @brief Flex 布局算法：按 Flutter 语义在子项间分配主轴空间并定位（两阶段：测 → 摆）。
///
/// 覆盖：
/// - 方向：`Row`/`Column`/`RowReverse`/`ColumnReverse`（反向沿主轴镜像）。
/// - 主轴对齐：`Start`/`Center`/`End`/`SpaceBetween`/`SpaceAround`/`SpaceEvenly`。
/// - 交叉轴对齐：`Start`/`Center`/`End`/`Stretch`（拉伸填满容器交叉轴）。
/// - 弹性分配：权重 > 0 的子项按权重瓜分"父约束剩余空间"；权重 0 仅占内容尺寸。
///
/// 对应 specification/03-layout-render.md §2.3 两阶段布局。与 widget 解耦，可独立单测。
///
/// @note Thread: main-thread only
/// @note Side-effects: mutates layout
/// @note Rebuildable: no
class FlexLayouter {
  public:
    /// @brief 两阶段（测 → 摆）flex 布局：按 Flutter 语义测量子项并在主轴分配空间、按对齐定位。
    /// @param config flex 布局配置（方向、主轴/交叉轴对齐、间距、主轴尺寸策略、RTL 书写方向）。
    /// @param parent 父级约束（决定主轴可用空间与容器尺寸夹取）。
    /// @param items 待布局子项（各自携带测量/基线回调）。
    /// @return 各子项相对容器原点的 Rect 序列（与 `items` 同序）与容器自身尺寸（已夹入父约束）。
    static auto layout(const Flex &config, const Constraints &parent, const std::vector<FlexItem> &items) -> FlexLayout;
};

}  // namespace aurora
