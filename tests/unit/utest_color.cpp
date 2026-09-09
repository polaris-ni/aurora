/// 测试类型: unit
/// 目标单元: include/aurora/core/color.h
/// 测试说明: 覆盖 Color 构造与默认值、具名色工厂与 colors 调色板、shaded/with_alpha 派生、逐通道相等比较与 _rgb/_rgba 字面量

#include <cstdint>
#include <type_traits>

#include "aurora/core/color.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_color {

namespace au = aurora;

/// @brief 默认构造：黑色、不透明（alpha 默认 255），且为 constexpr 可用。
AURORA_TEST_CASE(default_ctor_is_opaque_black) {
    constexpr aurora::Color c{};
    static_assert(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255);
    AURORA_TEST_CHECK_EQ(c.r, 0);
    AURORA_TEST_CHECK_EQ(c.g, 0);
    AURORA_TEST_CHECK_EQ(c.b, 0);
    AURORA_TEST_CHECK_EQ(c.a, 255);
}

/// @brief 四参构造的 alpha 缺省为 255；from_rgba 与构造等价。
AURORA_TEST_CASE(rgba_ctor_defaults_alpha_to_opaque) {
    constexpr aurora::Color c{10, 20, 30};
    static_assert(c.a == 255);
    AURORA_TEST_CHECK_EQ(c.r, 10);
    AURORA_TEST_CHECK_EQ(c.g, 20);
    AURORA_TEST_CHECK_EQ(c.b, 30);
    AURORA_TEST_CHECK_EQ(c.a, 255);

    constexpr auto explicit_alpha = aurora::Color::from_rgba(1, 2, 3, 4);
    static_assert(explicit_alpha.a == 4);
    AURORA_TEST_CHECK((explicit_alpha == aurora::Color{1, 2, 3, 4}));  // 外加括号：花括号列表含逗号会被宏切参
}

/// @brief 具名色工厂的通道值契约（green 为 0x00A000 而非纯绿）。
AURORA_TEST_CASE(named_palette_factories) {
    AURORA_TEST_CHECK((aurora::Color::white() == aurora::Color{255, 255, 255, 255}));
    AURORA_TEST_CHECK((aurora::Color::black() == aurora::Color{0, 0, 0, 255}));
    AURORA_TEST_CHECK((aurora::Color::red() == aurora::Color{255, 0, 0, 255}));
    AURORA_TEST_CHECK((aurora::Color::green() == aurora::Color{0, 160, 0, 255}));
    AURORA_TEST_CHECK((aurora::Color::blue() == aurora::Color{0, 0, 255, 255}));
    AURORA_TEST_CHECK((aurora::Color::gray() == aurora::Color{128, 128, 128, 255}));
    AURORA_TEST_CHECK((aurora::Color::yellow() == aurora::Color{255, 255, 0, 255}));
    AURORA_TEST_CHECK((aurora::Color::transparent() == aurora::Color{0, 0, 0, 0}));
}

/// @brief colors 调色板命名空间与 Color 静态工厂一一对应。
AURORA_TEST_CASE(colors_palette_mirrors_static_factories) {
    AURORA_TEST_CHECK(aurora::colors::AURORA_WHITE == aurora::Color::white());
    AURORA_TEST_CHECK(aurora::colors::AURORA_BLACK == aurora::Color::black());
    AURORA_TEST_CHECK(aurora::colors::AURORA_RED == aurora::Color::red());
    AURORA_TEST_CHECK(aurora::colors::AURORA_GREEN == aurora::Color::green());
    AURORA_TEST_CHECK(aurora::colors::AURORA_BLUE == aurora::Color::blue());
    AURORA_TEST_CHECK(aurora::colors::AURORA_GRAY == aurora::Color::gray());
    AURORA_TEST_CHECK(aurora::colors::AURORA_YELLOW == aurora::Color::yellow());
    AURORA_TEST_CHECK(aurora::colors::AURORA_TRANSPARENT == aurora::Color::transparent());
}

/// @brief shaded 只缩放 RGB、保留 alpha；k>1 饱和到 255、k<=0 夹到 0。
AURORA_TEST_CASE(shaded_scales_rgb_and_keeps_alpha) {
    constexpr aurora::Color base{200, 100, 40, 77};
    static_assert(base.shaded(1.0F) == aurora::Color{200, 100, 40, 77});
    static_assert(base.shaded(0.0F) == aurora::Color{0, 0, 0, 77});
    static_assert(base.shaded(2.0F) == aurora::Color{255, 200, 80, 77});  // 400 饱和到 255
    static_assert(base.shaded(0.5F) == aurora::Color{100, 50, 20, 77});
    static_assert(base.shaded(-1.0F) == aurora::Color{0, 0, 0, 77});
    AURORA_TEST_CHECK((base.shaded(10.0F) == aurora::Color{255, 255, 255, 77}));
}

/// @brief with_alpha 只替换 alpha，RGB 不变；原对象不被修改（返回新值）。
AURORA_TEST_CASE(with_alpha_replaces_only_alpha) {
    constexpr aurora::Color base{10, 20, 30, 255};
    static_assert(base.with_alpha(64) == aurora::Color{10, 20, 30, 64});
    AURORA_TEST_CHECK((base.with_alpha(64) == aurora::Color{10, 20, 30, 64}));
    AURORA_TEST_CHECK((base == aurora::Color{10, 20, 30, 255}));  // 派生不改源
    AURORA_TEST_CHECK((aurora::Color::transparent().with_alpha(128) == aurora::Color{0, 0, 0, 128}));
}

/// @brief 相等比较为逐通道：任一通道不同即不等。
AURORA_TEST_CASE(equality_is_channel_wise) {
    constexpr aurora::Color base{10, 20, 30, 40};
    static_assert(base == aurora::Color{10, 20, 30, 40});
    AURORA_TEST_CHECK_FALSE((base == aurora::Color{11, 20, 30, 40}));  // r 不同
    AURORA_TEST_CHECK_FALSE((base == aurora::Color{10, 21, 30, 40}));  // g 不同
    AURORA_TEST_CHECK_FALSE((base == aurora::Color{10, 20, 31, 40}));  // b 不同
    AURORA_TEST_CHECK_FALSE((base == aurora::Color{10, 20, 30, 41}));  // a 不同
    AURORA_TEST_CHECK((base != aurora::Color{10, 20, 30, 41}));
    AURORA_TEST_CHECK_FALSE((base != aurora::Color{10, 20, 30, 40}));
}

/// @brief _rgb/_rgba 字面量按 0xRRGGBB / 0xRRGGBBAA 高字节在前的通道序解释。
AURORA_TEST_CASE(rgb_rgba_literals_match_channel_layout) {
    using aurora::literals::operator""_rgb;  // using-declaration：仅引入具名字面量（库约定：TU 内显式引入）
    using aurora::literals::operator""_rgba;

    static_assert(0xFF0000_rgb == aurora::Color::red());
    AURORA_TEST_CHECK((0xFF0000_rgb) == aurora::Color::red());
    AURORA_TEST_CHECK(((0x00FF00_rgb) == aurora::Color{0, 255, 0}));
    AURORA_TEST_CHECK((0x0000FF_rgb) == aurora::Color::blue());
    AURORA_TEST_CHECK(((0x11223344_rgba) == aurora::Color{0x11, 0x22, 0x33, 0x44}));
    AURORA_TEST_CHECK((0x0000FFFF_rgba) == aurora::Color::blue());
    AURORA_TEST_CHECK_EQ((0x123456_rgb).a, 255);  // _rgb 无 alpha 段 → 不透明
}

/// @brief 编译期契约：Color 不可从裸标量隐式构造（防止整数误当颜色）。
AURORA_TEST_CASE(color_rejects_implicit_scalar_conversion) {
    static_assert(!std::is_convertible_v<int, aurora::Color>);
    static_assert(!std::is_convertible_v<unsigned int, aurora::Color>);
    static_assert(!std::is_convertible_v<double, aurora::Color>);
    static_assert(std::is_constructible_v<aurora::Color, std::uint8_t, std::uint8_t, std::uint8_t>);
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_color
