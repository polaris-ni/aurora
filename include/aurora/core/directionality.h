#pragma once

#include <optional>

#include "aurora/core/enums.h"

namespace aurora {

/// @brief 方向性：`TextDirection` 的树级注入与解析，对标 accessibility.h 的双来源模式。
/// 三级优先：**控件显式属性**（如 `Text::direction`）> `Environment` 注入（树级作用域，
/// 宿主经 `env.with<Directionality>()/set<Directionality>()`）> 进程级默认（无上下文子系统兜底）。
/// 模板化以避开 `core → environment/*` 的表级依赖：任何提供成员模板 `environment<T>() const`
/// 的上下文（现为 `BuildContext`）均可传入；实例化点补全类型（同 `resolved_accessibility_settings`）。
///
/// `explicit_text_direction` 与 `resolved_text_direction` 的分工：
///  - 后者返回**确定值**（缺省回落 LTR），供对齐解析（`TextAlign::Start/End`）等决策使用；
///  - 前者返回 `nullopt` 表示「无显式来源」——FontEngine 据此保持 hb 按内容自动 guess
///    （默认行为与接入前完全一致，golden 逐位不变；进程级默认 LTR 不强制覆盖 guess）。
/// @note Thread: main-thread only
/// @note Side-effects: none
struct Directionality {
    TextDirection direction = TextDirection::LTR;  ///< 书写方向；默认 LTR，RTL 影响 shaping/对齐解析。
    bool host_set = false;  ///< 宿主显式设置标记：仅显式来源参与 shaping 的强制覆盖
};

/// @brief 进程级方向性（默认来源）：无 `Environment` 可用的路径取此值。
/// @return 进程级 `Directionality` 的可变引用（有意返回引用，供宿主就地更新同一实例）。
/// @note 有意返回可变引用：宿主在启动/语言切换时自然更新同一实例。
/// @note Thread: main-thread only
[[nodiscard]] inline auto current_directionality() -> Directionality & {
    static Directionality d;  // NOLINT
    return d;
}

/// @brief 设置进程级方向性（标记 host_set=true）。
/// @param d 新的方向性；写入后其 `host_set` 被置 true 以参与 shaping 强制覆盖。
inline auto set_directionality(Directionality d) -> void {
    d.host_set = true;
    current_directionality() = d;
}

/// @brief 显式方向来源（nullopt = 无显式来源，shaping 保持按内容 guess）。
/// @tparam Ctx 提供成员模板 `environment<T>() const` 的上下文类型（现为 `BuildContext`）。
/// @param ctx 读取 `Environment` 注入、缺省回落进程级方向的构建上下文。
/// @return 显式来源方向；环境未注入且进程未显式设置时返回 `nullopt`。
template <typename Ctx>
[[nodiscard]] auto explicit_text_direction(const Ctx &ctx) -> std::optional<TextDirection> {
    // ① 控件显式属性由调用方先行合并（此处处理环境/进程两级）。
    if (const auto *injected = ctx.template environment<Directionality>()) {
        return injected->direction;
    }
    const Directionality &proc = current_directionality();
    if (proc.host_set) {
        return proc.direction;
    }
    return std::nullopt;
}

/// @brief 读取**生效**书写方向（确定值；缺省 LTR）。供 `TextAlign::Start/End` 解析等使用。
/// @tparam Ctx 提供成员模板 `environment<T>() const` 的上下文类型（现为 `BuildContext`）。
/// @param ctx 用于解析环境/进程级方向的构建上下文。
/// @return 确定书写方向；无任何显式来源时回落 `TextDirection::LTR`。
template <typename Ctx>
[[nodiscard]] auto resolved_text_direction(const Ctx &ctx) -> TextDirection {
    if (const auto d = explicit_text_direction(ctx); d.has_value()) {
        return *d;
    }
    return TextDirection::LTR;
}

}  // namespace aurora
