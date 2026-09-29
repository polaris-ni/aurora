#pragma once

#include <functional>
#include <utility>

#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 声明式挂载/卸载副作用钩子（控制流 Widget）。
///
/// 包裹一棵子树：子树挂载完成后恰好触发一次 `on_mount(const BuildContext&)`（用户可注册外部源、
/// 启动定时器、加载数据、读环境注入）；控件被销毁（`Repeater` 缩容 / `Navigator` pop / 持有 `Node`
/// 释放，RAII 析构）时触发 `on_unmount()` 做清理（取消定时器、退订、释放资源）。
///
/// 设计定位（跨框架对照）：
/// - 对齐 React `useEffect(…, [])`（挂载跑一次 + 返回 cleanup）+ Flutter `initState` + `dispose`；
/// - 对应 Android `View.onAttachedToWindow` / `onDetachedFromWindow`（控件级，**非** `Activity` 生命周期）；
/// - `Show` 隐藏子树时**保留**子节点存活（不析构、不卸载），故 `on_unmount` 不被触发，
/// 与 Flutter `Visibility` 的「隐藏保留状态」语义一致。
///
/// 用法：
/// @code
/// au::Lifecycle(
/// au::Text("hello"),                       // 被包裹的子树
/// [](const au::BuildContext &ctx) {        // on_mount：挂载后恰好一次
/// subscribe_external_source();
/// },
/// []() { unsubscribe_external_source(); }  // on_unmount：销毁时清理
/// );
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Lifecycle : public SingleChild {
  public:
    using MountCb =
        std::function<void(const BuildContext &)>;  ///< 挂载回调签名：接收挂载时的 BuildContext（读环境注入等）。
    using UnmountCb = std::function<void()>;  ///< 卸载回调签名：无参清理函数（取消定时器、退订、释放资源）。

    Lifecycle() = default;

    /// @brief 构造挂载/卸载副作用钩子控件。
    /// @param child      被包裹的子树（任意 `Node`）。
    /// @param on_mount   挂载后恰好触发一次的用户回调（可访问 `BuildContext`，如读环境注入）。
    /// @param on_unmount 控件销毁时触发的清理回调；可空（留空表示无需清理）。
    Lifecycle(Node child, MountCb on_mount, UnmountCb on_unmount = {})
        : SingleChild(std::move(child)), on_mount_(std::move(on_mount)), on_unmount_(std::move(on_unmount)) {}

    /// @brief 析构：控件销毁时触发 `on_unmount_` 清理回调（未设置则跳过）。
    ///        豁免 bugprone-exception-escape：析构调用用户的 `on_unmount_`（std::function），
    ///        其 operator() 无 noexcept 规格——即 .clang-tidy 记录在案的系统性假告警面。控件
    ///        销毁期无调用方可回报，本库回调路径刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
    /// NOLINTNEXTLINE(bugprone-exception-escape)
    ~Lifecycle() override {
        if (on_unmount_) {
            on_unmount_();
        }
    }

    Lifecycle(const Lifecycle &) = delete;
    auto operator=(const Lifecycle &) -> Lifecycle & = delete;
    Lifecycle(Lifecycle &&) = default;
    auto operator=(Lifecycle &&) -> Lifecycle & = default;

    /// @brief 运行时类型名。
    /// @return 固定字符串 "Lifecycle"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Lifecycle"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 静态描述符：属性表（on_mount 必填 / on_unmount 可空）、事件名、单孩子策略与用法示例。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Lifecycle",
            .properties =
                {
                    {.name = "on_mount",
                     .type = "std::function<void(const BuildContext&)>",
                     .default_value = "—",
                     .required = true,
                     .note = "User callback fired exactly once after mount "
                             "(accesses BuildContext: environment injection, size, etc.)"},
                    {.name = "on_unmount",
                     .type = "std::function<void()>",
                     .default_value = "nullptr",
                     .required = false,
                     .note = "Cleanup callback on widget destruction (nullable)"},
                },
            .events = {"on_mount", "on_unmount"},
            .children_policy = "single",
            .examples =
                {
                    "au::Lifecycle(au::Text(\"hi\"), [](const au::BuildContext&){ start(); }, [](){ stop(); });",
                },
        };
    }
    /// @brief 实例自描述：转发 `describe_static()`。
    /// @return 与静态描述符一致的内容。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号收集：空操作——Lifecycle 无自有响应式信号，子节点信号在其 mount 时独立收集。
    /// @param out 输出向量（本控件不追加任何信号）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        (void)out;  // Lifecycle 无自有响应式信号；子节点信号在其 mount 时独立收集
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return child_ ? child_.widget().layout(c, ctx) : Size{};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        SingleChild::on_mount(ctx);  // 先递归挂载子节点（注册其响应式依赖）
        if (on_mount_) {
            on_mount_(ctx);
        }
    }

  private:
    MountCb on_mount_;
    UnmountCb on_unmount_;
};

}  // namespace aurora
