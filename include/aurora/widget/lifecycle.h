#pragma once

#include <functional>
#include <utility>

#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 声明式挂载/卸载副作用钩子（控制流 Widget）。
 *
 * 包裹一棵子树：子树挂载完成后恰好触发一次 `on_mount(const BuildContext&)`（用户可注册外部源、
 * 启动定时器、加载数据、读环境注入）；控件被销毁（`Repeater` 缩容 / `Navigator` pop / 持有 `Node`
 * 释放，RAII 析构）时触发 `on_unmount()` 做清理（取消定时器、退订、释放资源）。
 *
 * 设计定位（跨框架对照）：
 * - 对齐 React `useEffect(…, [])`（挂载跑一次 + 返回 cleanup）+ Flutter `initState` + `dispose`；
 * - 对应 Android `View.onAttachedToWindow` / `onDetachedFromWindow`（控件级，**非** `Activity` 生命周期）；
 * - `Show` 隐藏子树时**保留**子节点存活（不析构、不卸载），故 `on_unmount` 不被触发，
 *   与 Flutter `Visibility` 的「隐藏保留状态」语义一致。
 *
 * 用法：
 * @code
 * au::Lifecycle(
 *     au::Text("hello"),                       // 被包裹的子树
 *     [](const au::BuildContext &ctx) {        // on_mount：挂载后恰好一次
 *         subscribe_external_source();
 *     },
 *     []() { unsubscribe_external_source(); }  // on_unmount：销毁时清理
 * );
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Lifecycle : public SingleChild {
  public:
    using MountCb = std::function<void(const BuildContext &)>;
    using UnmountCb = std::function<void()>;

    Lifecycle() = default;

    /// @brief 构造挂载/卸载副作用钩子控件。
    /// @param child      被包裹的子树（任意 `Node`）。
    /// @param on_mount   挂载后恰好触发一次的用户回调（可访问 `BuildContext`，如读环境注入）。
    /// @param on_unmount 控件销毁时触发的清理回调；可空（留空表示无需清理）。
    Lifecycle(Node child, MountCb on_mount, UnmountCb on_unmount = {})
        : SingleChild(std::move(child)), m_on_mount(std::move(on_mount)), m_on_unmount(std::move(on_unmount)) {}

    ~Lifecycle() override {
        if (m_on_unmount) {
            m_on_unmount();
        }
    }

    Lifecycle(const Lifecycle &) = delete;
    auto operator=(const Lifecycle &) -> Lifecycle & = delete;
    Lifecycle(Lifecycle &&) = default;
    auto operator=(Lifecycle &&) -> Lifecycle & = default;

    [[nodiscard]] auto type_name() const -> const char * override { return "Lifecycle"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Lifecycle",
            .properties = {
                { .name = "on_mount",
                  .type = "std::function<void(const BuildContext&)>",
                  .default_value = "—",
                  .required = true,
                  .note = "挂载后恰好触发一次的用户回调（访问 BuildContext：环境注入、尺寸等）" },
                { .name = "on_unmount", .type = "std::function<void()>", .default_value = "nullptr", .required = false, .note = "控件销毁时触发的清理回调（可空）" },
            },
            .events = { "on_mount", "on_unmount" },
            .children_policy = "single",
            .examples = {
                "au::Lifecycle(au::Text(\"hi\"), [](const au::BuildContext&){ start(); }, [](){ stop(); });",
            },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        (void)out; // Lifecycle 无自有响应式信号；子节点信号在其 mount 时独立收集
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return m_child ? m_child.widget().layout(c, ctx) : Size{};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        SingleChild::on_mount(ctx); // 先递归挂载子节点（注册其响应式依赖）
        if (m_on_mount) {
            m_on_mount(ctx);
        }
    }

  private:
    MountCb m_on_mount;
    UnmountCb m_on_unmount;
};

} // namespace aurora
