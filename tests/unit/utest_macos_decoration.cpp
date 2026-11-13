/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/macos_decoration.h
/// 测试说明: macOS 装饰档位映射的真值表——五档 `DecorationPolicy` 与 `frameless` 旧字段的折叠、
///           缩放能力的唯一来源是 `resizable`、`Borderless` 与 `Frameless` 的分界（前者保留
///           原生边缘缩放、后者不留）、三档原生标题栏形态（Auto / ServerSide / ClientSide）
///           在本后端不产生任何自绘布尔。逐格断言，无 AppKit 依赖。
/// 平台门控: 无——被测层刻意不 include 平台头，故映射在任何 runner 上都照常执行（非 Apple
///           构建同样能钉住这条跨后端契约；真机侧的原生效果由 `aurora_verify_macos_surface` 取证）

#include <string>

#include "aurora/window/detail/macos_decoration.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_macos_decoration {

namespace {

using aurora::DecorationPolicy;
using aurora::WindowStyleOptions;
using aurora::detail::MacOSDecoration;
using aurora::detail::map_macos_decoration;

/// @brief 构造只关心三字段的样式选项。
/// @param decoration 装饰档位。
/// @param resizable 是否可缩放。
/// @param frameless 旧字段（`true` 折叠为 `Frameless`）。
/// @return 样式选项。
[[nodiscard]] auto style_of(DecorationPolicy decoration, bool resizable, bool frameless = false) -> WindowStyleOptions {
    WindowStyleOptions style;
    style.decoration = decoration;
    style.resizable = resizable;
    style.frameless = frameless;
    return style;
}

/// @brief 把映射结论折成五个布尔的可读快照，供跨格比对时随失败打印。
/// @param deco 映射结论。
/// @return 形如 `borderless=0 full_size=0 sizable=1 hide_title=0 hide_buttons=0` 的明细串。
[[nodiscard]] auto describe(const MacOSDecoration &deco) -> std::string {
    const auto bit = [](bool v) { return v ? '1' : '0'; };
    return std::string("borderless=") + bit(deco.native_borderless) + " full_size=" + bit(deco.full_size_content) +
           " sizable=" + bit(deco.sizable) + " hide_title=" + bit(deco.hide_title) +
           " hide_buttons=" + bit(deco.hide_window_buttons);
}

/// @brief 逐字段判等：两结论是否完全一致。
/// @param a 左侧。
/// @param b 右侧。
/// @return 五个布尔逐字段相等时为 true。
[[nodiscard]] auto same(const MacOSDecoration &a, const MacOSDecoration &b) -> bool {
    return a.native_borderless == b.native_borderless && a.full_size_content == b.full_size_content &&
           a.sizable == b.sizable && a.hide_title == b.hide_title && a.hide_window_buttons == b.hide_window_buttons;
}

}  // namespace

AURORA_TEST_CASE(borderless_keeps_the_native_resize_capability) {
    // 契约：`Borderless` = 无标题栏但**保留**可拖拽缩放边框。故既不能落纯 `Borderless` 掩码
    // （那会把缩放一起丢掉，正是本缺陷的真实形态），又必须同时拿到内容铺满 + 藏栏 + 藏钮三件。
    const MacOSDecoration deco = map_macos_decoration(style_of(DecorationPolicy::Borderless, true));
    AURORA_TEST_CHECK_FALSE(deco.native_borderless);
    AURORA_TEST_CHECK_TRUE(deco.full_size_content);
    AURORA_TEST_CHECK_TRUE(deco.sizable);
    AURORA_TEST_CHECK_TRUE(deco.hide_title);
    AURORA_TEST_CHECK_TRUE(deco.hide_window_buttons);
}

AURORA_TEST_CASE(borderless_does_not_grant_resize_when_unsizable) {
    // 缩放能力的唯一来源是 `resizable`：`Borderless` 档不得绕过它。其余四件与可缩放档同形
    // ——视觉档位不变，只少 `Resizable` 一位。
    const MacOSDecoration deco = map_macos_decoration(style_of(DecorationPolicy::Borderless, false));
    AURORA_TEST_CHECK_FALSE(deco.native_borderless);
    AURORA_TEST_CHECK_TRUE(deco.full_size_content);
    AURORA_TEST_CHECK_FALSE(deco.sizable);
    AURORA_TEST_CHECK_TRUE(deco.hide_title);
    AURORA_TEST_CHECK_TRUE(deco.hide_window_buttons);
}

AURORA_TEST_CASE(frameless_drops_everything_including_resize) {
    // 契约：`Frameless` = 完全无装饰、无阴影边距、无缩放边框。它与 `Borderless` 的分界就在
    // `native_borderless` 这一位——两档塌成同一份实现是本例要防的复发形态。
    for (const bool resizable : {true, false}) {
        const MacOSDecoration deco = map_macos_decoration(style_of(DecorationPolicy::Frameless, resizable));
        AURORA_TEST_CHECK_MSG(deco.native_borderless, describe(deco));
        AURORA_TEST_CHECK_FALSE(deco.full_size_content);
        AURORA_TEST_CHECK_FALSE(deco.sizable);
        AURORA_TEST_CHECK_FALSE(deco.hide_title);
        AURORA_TEST_CHECK_FALSE(deco.hide_window_buttons);
    }
}

AURORA_TEST_CASE(frameless_field_folds_into_frameless_policy) {
    // `WindowStyleOptions::frameless == true` 的公共注释明写它等价于 `DecorationPolicy::Frameless`，
    // 折叠由 `csd::normalize_policy` 单点承担（本层不写第二份折叠式）。它是**否决位**：即便档位
    // 写了 `Borderless`，`frameless == true` 仍落完全无装饰。
    const MacOSDecoration reference = map_macos_decoration(style_of(DecorationPolicy::Frameless, true));
    for (const DecorationPolicy policy :
         {DecorationPolicy::Auto, DecorationPolicy::ServerSide, DecorationPolicy::ClientSide,
          DecorationPolicy::Borderless, DecorationPolicy::Frameless}) {
        const MacOSDecoration deco = map_macos_decoration(style_of(policy, true, true));
        AURORA_TEST_CHECK_MSG(same(deco, reference), describe(deco));
    }
}

AURORA_TEST_CASE(native_title_bar_policies_produce_no_csd_booleans) {
    // macOS 恒有服务端装饰、无协商环节：`Auto` / `ServerSide` / `ClientSide` 三档都只落原生标题栏
    // （四个自绘布尔全 false），缩放位随 `resizable`。`ClientSide` 在本后端退化为 SSD 是既成现状，
    // 不是漏写的自绘分支——若日后真接上 CSD 层，本用例即转红，逼着改动同时回写规范。
    const MacOSDecoration idle = map_macos_decoration(style_of(DecorationPolicy::Auto, false));
    for (const DecorationPolicy policy :
         {DecorationPolicy::Auto, DecorationPolicy::ServerSide, DecorationPolicy::ClientSide}) {
        const MacOSDecoration fixed = map_macos_decoration(style_of(policy, false));
        AURORA_TEST_CHECK_MSG(same(fixed, idle), describe(fixed));
        const MacOSDecoration sizable = map_macos_decoration(style_of(policy, true));
        AURORA_TEST_CHECK_MSG(
            sizable.sizable && !sizable.native_borderless && !sizable.full_size_content && !sizable.hide_title,
            describe(sizable));
    }
}

AURORA_TEST_CASE(resizable_is_the_only_resize_source_across_policies) {
    // 跨档不变式：`sizable` 恰等于「`resizable` 且本档未落纯 `Borderless` 掩码」。逐档 × 两枚
    // `resizable` 全组合核对，任何一档想私自给（或私自收）缩放能力都会转红。
    for (const DecorationPolicy policy :
         {DecorationPolicy::Auto, DecorationPolicy::ServerSide, DecorationPolicy::ClientSide,
          DecorationPolicy::Borderless, DecorationPolicy::Frameless}) {
        for (const bool resizable : {true, false}) {
            const MacOSDecoration deco = map_macos_decoration(style_of(policy, resizable));
            AURORA_TEST_CHECK_MSG(deco.sizable == (resizable && !deco.native_borderless), describe(deco));
        }
    }
}

}  // namespace aurora::test_cases::utest_macos_decoration
