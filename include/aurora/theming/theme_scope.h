#pragma once

#include <concepts>
#include <utility>

#include "aurora/environment/build_context.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 主题作用域：为子树覆盖（或首次注入）主题（specification/07-environment-modifier.md §5.1 显式主题传递）。
///
/// 语义等价于 `Provider<Theme>`，但提供领域化的名称与构造。子树内任意 widget
/// 经 `inherit_theme(ctx)` 读取最近祖先的 `ThemeScope` 注入值（最近祖先优先，
/// 天然实现主题继承与局部覆盖）。
///
/// @code
/// ThemeScope{ Theme::dark(), Column{ .children = { Text{"Hello"} } } };
/// @endcode
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: yes, via from_json
/// @note bugprone-exception-escape 豁免：本控件继承 Provider<Theme>，其构造/拷贝链持有 std::function 回调，
///       触发 .clang-tidy 已记录的系统性假告警面——「任何转入 std::function 的可调用对象一律判『不应抛出』」
///       （operator() 无 noexcept 规格）。抛出仅可能为 bad_alloc，回调实抛由上层帧循环 try/catch 兜底。
///       NOLINTNEXTLINE(bugprone-exception-escape)
class ThemeScope : public Provider<Theme> {
  public:
    /// @brief 继承 `Provider<Theme>(ReadOnlyState<Theme>, Node/Widget)` 的构造重载。
    ///
    /// 主题值、`State<Theme>` 与 `ReadOnlyState<Theme>` 都经单向转换进来；`State<Theme>` 变化时
    /// 子树自动重渲染。
    using Provider::Provider;

    /// @brief 控件类型名。
    /// @return 恒为 "ThemeScope"。
    [[nodiscard]] auto type_name() const -> const char * override { return "ThemeScope"; }
};

/// @brief 读取最近祖先 `ThemeScope` 注入的主题（specification/07-environment-modifier.md §5.1 显式主题传递）。
///
/// 在 widget 的 `on_paint`/`on_layout` 中调用以获取当前主题令牌（颜色、形状、间距、度量、字体），
/// 从而让绘制随主题变化。无注入主题时返回默认浅色主题，保证永不崩溃。
///
/// **返回引用而非值**：`Theme` 现在携带 `TypeScale`（15 枚 `Font`，各持一个族名字符串），按值返回
/// 会让「读一枚令牌字段」变成整份主题拷贝——绘制路径每帧多次命中，拷贝即新增堆分配。改为引用后
/// 取令牌是直接字段访问；需要跨越 `ctx` 生命周期使用主题值（存进成员、闭包捕获）的调用方显式
/// 拷贝（`const Theme t = inherit_theme(ctx);` 不变）。
///
/// 越界档的实测稳态帧成本为按引用档的 2.25–2.36 倍（`tools/bench/bench_theme.cpp`，Headless 探针方阵；
/// 四次复跑，分配计数逐位相同）；默认族名落在小串优化容量内，故整值拷贝在缺省配置下零分配。
///
/// @param ctx 当前构建上下文（由 layout/paint 传入）。
/// @return 最近祖先注入的 Theme 的引用；未注入则指向 Material 基线档（与 `Theme::light()` 逐值相同，
///         进程内一份，无兜底失败态）。
/// @note 兜底档恒 ≡ Material 基线：风格探测得到的平台档只经 `Window` 根注入或显式 `set_theme` 进入，
///       不经本兜底进入（判据同源：无宿主 / 无窗口的语境与「无窗口后端即回落 Material」同解）。
/// @note 有效期：命中注入时返回值指向环境存储本身，**只在本次布局 / 绘制遍历内有效**——`Provider` 每次
///       进入本层都按当前值重建子环境（旧存储随之析构），故不得跨帧或跨遍历持有本引用；需要留存就显式拷贝。
/// @note 兜底合一：`theming/theme_query.h` 的 `resolve_theme` 无注入兜底与此处同一枚 `Theme::light()`
///       （旧偏差：前者曾兜 `Theme::with_defaults()`，两枚只差 `scheme` 位；本主题的 `roles` / `csd` 已
///       确定为明色列，再标 `Unspecified`（＝不表态、交给优先链下层）是自我矛盾的）。
/// @note Thread: 兜底那一份由函数内静态量承担，首次初始化由语言保证线程安全，读取路径无锁。
[[nodiscard]] inline auto inherit_theme(const BuildContext &ctx) -> const Theme & {
    // 兜底档本就该懒初始化：进程内单份、首次调用时线程安全地构造；提到命名空间作用域反而引入
    // 跨 TU 初始化顺序依赖。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static const Theme LIGHT_FALLBACK = Theme::light();  // 无注入时的 Material 基线档（一份，进程内复用）
    const auto *t = ctx.environment<Theme>();
    return t != nullptr ? *t : LIGHT_FALLBACK;
}

}  // namespace aurora
