#pragma once

#include "aurora/environment/environment.h"
#include "aurora/environment/media_query.h"
#include "aurora/i18n/locale.h"
#include "aurora/state/reactive.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 按注入值类型 `T` 决定序列化 `type` 名，使不同 Provider 可被分别反序列化。
 * 对已知类型特化；默认返回通用 `"Provider"`（兜底）。
 */
template<typename T> auto provider_type_name() -> const char * { return "Provider"; }
template<> inline auto provider_type_name<Theme>() -> const char * { return "ThemeProvider"; }
template<> inline auto provider_type_name<Locale>() -> const char * { return "LocaleProvider"; }
template<> inline auto provider_type_name<MediaQuery>() -> const char * { return "MediaQueryProvider"; }

/**
 * @brief 环境注入器（参考 Flutter InheritedWidget / SwiftUI Environment）。
 *
 * 把值 `T` 注入环境，子树经 `BuildContext::environment<T>()` 取到最近祖先的 Provider。
 * 注意：实作放在 widget 模块以避免 environment→widget 的循环依赖；环境机制
 * （Environment/BuildContext）本身在 environment 模块。
 *
 * @tparam T 要向下注入的值类型（如 Theme / Locale）。
 */
template<typename T> class Provider : public SingleChild {
  public:
    /// @brief 用静态值注入（按值构造响应式持有）。
    Provider(T value, Node child) : SingleChild(std::move(child)), m_value(std::move(value)) {}

    template<typename W>
        requires std::derived_from<W, Widget>
    Provider(T value, W &&child) : SingleChild(Node{ std::forward<W>(child) }), m_value(std::move(value)) {}

    /// @brief 用共享 `State<T>` 注入：外部 State 变化时子树自动重渲染（运行时换肤/换区域）。
    explicit Provider(std::shared_ptr<State<T>> state, Node child)
        : SingleChild(std::move(child)), m_value(std::move(state)) {}

    template<typename W>
        requires std::derived_from<W, Widget>
    explicit Provider(std::shared_ptr<State<T>> state, W &&child)
        : SingleChild(Node{ std::forward<W>(child) }), m_value(std::move(state)) {}

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_value); }

    [[nodiscard]] auto type_name() const -> const char * override { return provider_type_name<T>(); }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = std::string(provider_type_name<T>()),
            .properties = {
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "" },
            },
            .events = {},
            .children_policy = "single",
            .examples = { "au::Provider<Theme>(theme, child)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前注入值（读取响应式持有，自动登记依赖）。
    [[nodiscard]] auto value() const -> const T & { return m_value.get(); }

    /// @brief 运行时改写注入值（触发子树刷新）。
    auto set_value(T v) -> void { m_value = std::move(v); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        BuildContext child_ctx = ctx;
        child_ctx.env = &m_child_env; // 把注入环境沿布局阶段向下传播
        rebuild_env(ctx);             // 每次布局按当前值重建（运行时换肤/换区域）
        m_child.widget().set_layout_parent(this);
        return m_child.widget().layout(c, child_ctx);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        BuildContext child_ctx = ctx;
        child_ctx.env = &m_child_env; // 把注入环境沿绘制阶段向下传播（主题/区域读取依赖）
        rebuild_env(ctx);             // 每次绘制按当前值重建（运行时换肤/换区域）
        m_child.widget().paint(p, bounds, child_ctx);
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        rebuild_env(ctx);
        BuildContext child_ctx = ctx;
        child_ctx.env = &m_child_env;
        m_child.widget().mount(child_ctx);
    }

  private:
    /// @brief 按当前 `m_value` 重建子环境（共享父环境与 State 变化时均生效）。
    auto rebuild_env(const BuildContext &ctx) -> void {
        if (ctx.env != nullptr) {
            m_child_env = ctx.env->with(m_value.get()); // 父环境存活于树内，指针安全
        } else {
            m_child_env.set_local(m_value.get()); // 根 Provider：无父，避免悬空
        }
    }

    Reactive<T> m_value; ///< 响应式持有（支持按值或共享 State<T> 注入）
    Environment m_child_env;
};

/// @brief 便捷别名：注入主题（specification/07-environment-modifier.md §5）。
using ThemeProvider = Provider<Theme>;
/// @brief 便捷别名：注入区域设置（specification/07-environment-modifier.md §6）。
using LocaleProvider = Provider<Locale>;
/// @brief 便捷别名：注入设备度量（specification/07-environment-modifier.md §3.1 / 安卓 MediaQuery）。
using MediaQueryProvider = Provider<MediaQuery>;

} // namespace aurora
