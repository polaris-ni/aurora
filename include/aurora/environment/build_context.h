#pragma once

#include "aurora/core/assert.h"
#include "aurora/core/types.h"
#include "aurora/environment/environment.h"

namespace aurora {

/**
 * @brief 树中位置句柄：随 mount/layout/paint 向下传递，供 widget 读取环境与尺寸。
 *
 * 对应 specification/07-environment-modifier.md §2.1 `BuildContext`。注意：本类型只持有「指向环境/尺寸」的只读视图，
 * 不拥有任何资源；生命周期由 widget 树保证。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class BuildContext {
  public:
    const Environment *env = nullptr;  ///< 当前环境（由 Provider 注入）；可为 nullptr
    float scale_factor = 1.0F;  ///< 设备像素密度（dpi / 160 等），快速访问器
    Size size{};  ///< 本节点布局后的尺寸（布局阶段填充）

    /// @brief 向上查找类型 T 的环境值；不存在返回 nullptr。
    /// @note 环境值是开放类型集（任意 T 均可经 Provider 注入）
    template <typename T>
    [[nodiscard]] auto environment() const -> const T * {
        return env != nullptr ? env->get<T>() : nullptr;
    }
};

/// @brief 便捷自由函数：从 BuildContext 读取类型 T 的环境值（引用形式）。
/// @note 若环境中不存在类型 T 的值，触发断言失败（运行时检查，因 Environment 基于 std::any）。
/// @note Thread: main-thread only
/// @note Side-effects: none
template <typename T>
[[nodiscard]] auto env_of(const BuildContext &ctx) -> const T & {
    const T *p = ctx.environment<T>();
    AURORA_CHECK(p != nullptr, "env_of: requested type not found in Environment");  // 解引用 nullptr = UB，常开拦截
    return *p;
}

}  // namespace aurora
