#pragma once

#include "aurora/core/assert.h"
#include "aurora/core/types.h"

namespace aurora {

class Environment;  ///< 前向声明：env->get<T>() 在 environment.h 中定义（见下方 environment()）

// 辅助自由函数：在 environment.h 中定义（需 Environment 完整类型）。本头仅前向声明，
// 其形参为指针，允许 Environment 不完整，故 BuildContext::environment() 可内联转发而无需本头包含完整 Environment。
template <typename T>
[[nodiscard]] auto detail_env_get(const Environment *env) -> const T *; // NOLINT(*-redundant-declaration)

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
    /// @note 体内对 `Environment` 完整类型有依赖的 `get<T>()` 调用已转发至 `detail_env_get`
    /// （定义见 environment.h，需 Environment 完整）。本方法内联但仅以指针形参透传 `env`，
    /// 故本头无需 `Environment` 完整即可解析，规避「在 environment.h 之前包含本头」的
    /// "member access into incomplete type" 问题。
    template <typename T>
    [[nodiscard]] auto environment() const -> const T * {
        return detail_env_get<T>(env);
    }
};

/// @brief 便捷自由函数：从 BuildContext 读取类型 T 的环境值（引用形式）。
/// @note 若环境中不存在类型 T 的值，触发断言失败（运行时检查，因 Environment 基于 std::any）。
/// @note Thread: main-thread only
/// @note Side-effects: none
template <typename T>
[[nodiscard]] auto env_of(const BuildContext &ctx) -> const T & {
    const T *p = ctx.environment<T>();
    AURORA_ASSERT(p != nullptr, "env_of: requested type not found in Environment");
    return *p;
}

}  // namespace aurora
