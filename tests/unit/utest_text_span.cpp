/// 测试类型: unit
/// 目标单元: include/aurora/widget/text_span.h
/// 测试说明: 覆盖 TextSpan 片段——默认与样式构造不变量、序列组装与子片段拷贝、Font/Color
/// 逐值相等语义（同样式合并的前提）与 i18n tr 元数据保留

#include <memory>
#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/text_span.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_text_span {

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用
/// （调用方持有 holder 即保证环境存活；BuildContext 本就只供当帧读取）。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

AURORA_TEST_CASE(default_span_invariants) {
    // 默认构造：空文本、默认字体（sans-serif / 14pt / 400）、黑色。
    const TextSpan span;
    AURORA_TEST_CHECK_EQ(span.text.text, std::string{});
    AURORA_TEST_CHECK_FALSE(span.text.localize);
    AURORA_TEST_CHECK_EQ(span.text.key, std::string{});
    AURORA_TEST_CHECK_TRUE(span.font == Font{});
    AURORA_TEST_CHECK_NEAR(span.font.size_pt, 14.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(span.font.weight, 400);
    // CHG-011 迁移批：默认色未显式设置（空 optional）→ 跟随主题 on_surface（ThemeScope 换肤即生效），
    // 不再是固定的黑 {0,0,0,255}。
    AURORA_TEST_CHECK_FALSE(span.color.has_value());
}

AURORA_TEST_CASE(styled_span_construction_keeps_fields) {
    // 聚合指定初始化：文本 + 字体 + 颜色逐字段保留；未指定字段取默认值。
    const TextSpan styled{
        .text = LocalizedString{"Hi"}, .font = Font{.size_pt = 18.0F, .weight = 700}, .color = Color::red()};
    AURORA_TEST_CHECK_EQ(styled.text.text, std::string{"Hi"});
    AURORA_TEST_CHECK_NEAR(styled.font.size_pt, 18.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(styled.font.weight, 700);
    AURORA_TEST_CHECK_TRUE(styled.font.family == std::string{"sans-serif"});  // 未指定 → 默认字族
    AURORA_TEST_CHECK_TRUE(styled.color == Color::red());

    const TextSpan text_only{.text = LocalizedString{"plain"}};
    AURORA_TEST_CHECK_TRUE(text_only.font == Font{});
    AURORA_TEST_CHECK_FALSE(text_only.color.has_value());  // 未设色 → 跟随主题 on_surface

    // LocalizedString 可由字符串字面量隐式构造。
    const TextSpan implicit_text{.text = "implicit"};
    AURORA_TEST_CHECK_EQ(implicit_text.text.text, std::string{"implicit"});
}

AURORA_TEST_CASE(spans_assemble_and_subrange_preserves_styles) {
    // 多 span 顺序拼接构成富文本；取子区间得到的新序列逐字段保留原样式。
    const TextSpan first{.text = LocalizedString{"big"}, .font = Font{.size_pt = 20.0F}, .color = Color::red()};
    const TextSpan second{.text = LocalizedString{"small"}, .font = Font{.size_pt = 10.0F}, .color = Color::blue()};
    const TextSpan third{.text = LocalizedString{"!"}, .color = Color::green()};

    const std::vector<TextSpan> spans{first, second, third};
    AURORA_TEST_REQUIRE_EQ(spans.size(), 3U);
    AURORA_TEST_CHECK_EQ(spans[0].text.text, std::string{"big"});
    AURORA_TEST_CHECK_EQ(spans[1].text.text, std::string{"small"});
    AURORA_TEST_CHECK_EQ(spans[2].text.text, std::string{"!"});

    // 子 span（拷贝切片）：样式随值语义一并复制。
    const std::vector<TextSpan> sub(spans.begin() + 1, spans.end());
    AURORA_TEST_REQUIRE_EQ(sub.size(), 2U);
    AURORA_TEST_CHECK_EQ(sub[0].text.text, std::string{"small"});
    AURORA_TEST_CHECK_TRUE(sub[0].font == Font{.size_pt = 10.0F});
    AURORA_TEST_CHECK_TRUE(sub[0].color == Color::blue());
    AURORA_TEST_CHECK_TRUE(sub[1].color == Color::green());
}

AURORA_TEST_CASE(span_copy_is_independent) {
    // 值语义深拷贝：修改副本不影响原件。
    TextSpan original{.text = LocalizedString{"origin"}, .font = Font{.size_pt = 12.0F}, .color = Color::gray()};
    TextSpan copy = original;
    copy.text.text = "changed";
    copy.font.size_pt = 99.0F;
    copy.color = Color::yellow();

    AURORA_TEST_CHECK_EQ(original.text.text, std::string{"origin"});
    AURORA_TEST_CHECK_NEAR(original.font.size_pt, 12.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(original.color == Color::gray());
    AURORA_TEST_CHECK_EQ(copy.text.text, std::string{"changed"});
    AURORA_TEST_CHECK_TRUE(copy.color == Color::yellow());
}

AURORA_TEST_CASE(font_color_equality_supports_style_merging) {
    // Font 按 family/size/weight 三元组相等；Color 按四通道相等——
    // 这是「连续同样式字符合并为一个 span」类合并逻辑的比较前提。
    const Font a{.size_pt = 14.0F};
    const Font b{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    AURORA_TEST_CHECK_TRUE(a == b);
    AURORA_TEST_CHECK_TRUE(a != Font{.size_pt = 15.0F});
    AURORA_TEST_CHECK_TRUE(a != Font{.weight = 700});
    AURORA_TEST_CHECK_TRUE(a != Font{.family = "serif"});

    AURORA_TEST_CHECK_TRUE(Color{1, 2, 3, 4} == Color{1, 2, 3, 4});
    AURORA_TEST_CHECK_TRUE(Color{1, 2, 3, 4} != Color{1, 2, 3, 5});
    AURORA_TEST_CHECK_TRUE(Color::black() != Color::white());
}

AURORA_TEST_CASE(localized_span_keeps_tr_metadata) {
    // LocalizedString::tr 的查表元数据在 span 内原样保留（文本字段回退值为空）。
    const TextSpan span{.text = LocalizedString::tr("greeting"), .color = Color::blue()};
    AURORA_TEST_CHECK_TRUE(span.text.text.empty());  // 回退文本为空
    AURORA_TEST_CHECK_EQ(span.text.key, std::string{"greeting"});
    AURORA_TEST_CHECK_TRUE(span.text.localize);
    AURORA_TEST_CHECK_TRUE(span.text.args.empty());
    AURORA_TEST_CHECK_TRUE(span.color == Color::blue());
}

// ---- 主题跟随（CHG-011 迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

AURORA_TEST_CASE(text_span_color_follows_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题 on_surface；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    TextSpan span;
    AURORA_TEST_CHECK_FALSE(span.color.has_value());  // 默认未设色
    AURORA_TEST_CHECK(span.resolve_color(bare) == Color{0, 0, 0});  // light on_surface = 黑

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(span.resolve_color(dark) == Color{0xFF, 0xFF, 0xFF});  // dark on_surface = 白
}

AURORA_TEST_CASE(text_span_explicit_color_overrides_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    TextSpan span;
    span.color = Color{0xE0, 0x1B, 0x24};  // error 红，刻意区别于两档 on_surface
    AURORA_TEST_CHECK(span.resolve_color(dark) == Color{0xE0, 0x1B, 0x24});
}

}  // namespace aurora::test_cases::utest_text_span
