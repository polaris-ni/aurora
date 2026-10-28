#pragma once

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/environment/media_query.h"
#include "aurora/i18n/locale.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 按注入值类型 `T` 决定序列化 `type` 名，使不同 Provider 可被分别反序列化。
/// 对已知类型特化；默认返回通用 `"Provider"`（兜底）。
/// @tparam T 注入值类型。
/// @return 序列化用的 `type` 名字符串。
template <typename T>
auto provider_type_name() -> const char * {
    return "Provider";
}
/// @brief `Theme` 特化：返回主题 Provider 的类型名。
/// @return "ThemeProvider"。
template <>
inline auto provider_type_name<Theme>() -> const char * {
    return "ThemeProvider";
}
/// @brief `Locale` 特化：返回区域设置 Provider 的类型名。
/// @return "LocaleProvider"。
template <>
inline auto provider_type_name<Locale>() -> const char * {
    return "LocaleProvider";
}
/// @brief `MediaQuery` 特化：返回设备度量 Provider 的类型名。
/// @return "MediaQueryProvider"。
template <>
inline auto provider_type_name<MediaQuery>() -> const char * {
    return "MediaQueryProvider";
}

/// @brief 环境注入器（参考 Flutter InheritedWidget / SwiftUI Environment）。
/// @tparam T 要向下注入的值类型（如 Theme / Locale）。
/// 把值 `T` 注入环境，子树经 `BuildContext::environment<T>()` 取到最近祖先的 Provider。
/// 注意：实作放在 widget 模块以避免 environment→widget 的循环依赖；环境机制
/// （Environment/BuildContext）本身在 environment 模块。
/// 本行隐式生成的拷贝/移动构造复制响应式持有 State<T>（其拷贝即分配值存储与订阅），被异常逃逸检查判
/// 「不应抛出」；该隐式特成员按 [except.spec] 本就是 potentially-throwing，抛出（bad_alloc）沿栈交给
/// 构造方。本类刻意依赖隐式拷贝/移动（CODING_STANDARDS.md §5.1），故不补 = delete 而逐点豁免。
/// 豁免取**行尾**形态而非「下一行」形态：本类是模板，告警报在**实例化点**所属的声明行
/// （clang-tidy 把实例化归到模板声明行），下一行形态只压住紧邻的那一行，压不住模板的各次实例化。
/// @note LAYOUT_EXEMPT: 下行的行尾豁免是本段理由的落点（`check_nolint_layout` 的合法例外）。
template <typename T>
class Provider : public SingleChild {  // NOLINT(bugprone-exception-escape)
  public:
    /// @brief 注入只读值句柄：常量值、`State<T>` 与 `ReadOnlyState<T>` 都经单向转换进来。
    ///
    /// 本控件只读注入值、不提供写入口：需要后代修改时显式用 `Provider<State<T>>`
    /// （注入的是句柄本身，后代拿到后可写原来源），或注入带领域命令的应用模型。
    /// @param value 注入值句柄；可指向调用方持有的来源。
    /// @param child 子节点。
    Provider(ReadOnlyState<T> value, Node child) : SingleChild(std::move(child)), value_(std::move(value)) {}

    /// @brief 注入只读值句柄并接受具体 widget 引用（隐式转 Node 挂接）。
    /// @tparam W 子 widget 的具体类型。
    /// @param value 注入值句柄。
    /// @param child 转发给 SingleChild 的子 widget 引用。
    template <typename W>
        requires std::derived_from<W, Widget>
    Provider(ReadOnlyState<T> value, W &&child) : SingleChild(Node{std::forward<W>(child)}), value_(std::move(value)) {}

    /// @brief 收集本控件的可订阅信号。
    /// @param out 输出容器；追加响应式持有 value_ 的视图指针。
    auto collect_signals(StateSink &out) -> void override { out.push(value_); }

    /// @brief 控件类型名：按注入值类型 `T` 映射（见 provider_type_name）。
    /// @return "ThemeProvider"/"LocaleProvider"/"MediaQueryProvider"，其余类型返回通用 "Provider"。
    [[nodiscard]] auto type_name() const -> const char * override { return provider_type_name<T>(); }

    /// @brief 运行时自描述（规格附录 B）：名称取 provider_type_name<T>()，属性键含 width/height/show。
    /// @return 描述符：single 子策略与 Provider 构造示例。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = std::string(provider_type_name<T>()),
            .properties =
                {
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false, .note = ""},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false, .note = ""},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false, .note = ""},
                },
            .events = {},
            .children_policy = "single",
            .examples = {"au::Provider<Theme>(theme, child)"},
        };
    }
    /// @brief 实例侧描述入口：转发静态描述。
    /// @return 与 describe_static() 相同的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 当前注入值（读取只读句柄，自动登记依赖）。
    /// @return 注入值 `T` 的当前值引用。
    [[nodiscard]] auto value() const -> const T & { return value_.get(); }

  protected:
    /// @brief 换值标脏口径：在默认「标脏本控件及其祖先缓存」之外，追加**子树**缓存清零。
    ///
    /// 本控件的脏来自一份向下传播的输入（注入值 `value_`）：子树后代自身并未变脏，默认口径失效不到
    /// 它们，于是文本一类「布局期按环境解析」的后代会沿用旧值那帧的解析结果（换区域/换肤内容冻结）。
    /// 故换值那一帧把整子树的布局与 Display List 缓存清零，令其按新环境重排重录。
    /// @note 只在信号真变化时执行一次；稳态帧不经过此处，缓存收益不受影响。
    auto on_signal_dirty() -> void override {
        mark_needs_layout();
        mark_needs_paint();
        if (child_) {
            child_.widget().invalidate_subtree_caches();
        }
    }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        BuildContext child_ctx = ctx;
        child_ctx.env = &child_env_;  // 把注入环境沿布局阶段向下传播
        rebuild_env(ctx);  // 每次布局按当前值重建（运行时换肤/换区域）
        child_.widget().set_layout_parent(this);
        return child_.widget().layout(c, child_ctx);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        BuildContext child_ctx = ctx;
        child_ctx.env = &child_env_;  // 把注入环境沿绘制阶段向下传播（主题/区域读取依赖）
        rebuild_env(ctx);  // 每次绘制按当前值重建（运行时换肤/换区域）
        child_.widget().paint(p, bounds, child_ctx);
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        rebuild_env(ctx);
        BuildContext child_ctx = ctx;
        child_ctx.env = &child_env_;
        child_.widget().mount(child_ctx);
    }

    /// @brief 递归卸载子节点（与 `on_mount` 逐字对称）。
    ///
    /// 子节点各自回传**它自己**挂载时记录的 ctx（即带本控件 `child_env_` 的那份），故这里无需也不能
    /// 替它重建环境——`on_unmount` 的契约就是「读当初挂载的那份」。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        if (child_) {
            child_.widget().unmount();
        }
    }

  private:
    /// @brief 按当前 `value_` 重建子环境（共享父环境与 State 变化时均生效）。
    auto rebuild_env(const BuildContext &ctx) -> void {
        if (ctx.env != nullptr) {
            child_env_ = ctx.env->with(value_.get());  // 父环境存活于树内，指针安全
        } else {
            child_env_.set_local(value_.get());  // 根 Provider：无父，避免悬空
        }
    }

    ReadOnlyState<T> value_;  ///< 注入值的只读句柄（常量、外部 State 或外部只读句柄同源）
    Environment child_env_;
};

/// @brief 便捷别名：注入主题（specification/07-environment-modifier.md §5）。
using ThemeProvider = Provider<Theme>;
/// @brief 便捷别名：注入区域设置（specification/07-environment-modifier.md §6）。
using LocaleProvider = Provider<Locale>;
/// @brief 便捷别名：注入设备度量（specification/07-environment-modifier.md §3.1 / 安卓 MediaQuery）。
using MediaQueryProvider = Provider<MediaQuery>;

}  // namespace aurora
