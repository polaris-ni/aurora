/// 测试类型: unit
/// 目标单元: include/aurora/window/native_surfaces.h
/// 测试说明: 聚合包含入口的类型可见性与 Surface 抽象契约，窗口样式默认值、
/// DecorationPolicy/WindowResizeEdge 枚举值序的跨后端映射契约

#include <cstdint>
#include <type_traits>

#include "aurora/window/native_surfaces.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_native_surfaces {

AURORA_TEST_CASE(window_style_options_defaults) {
    const WindowStyleOptions opts;
    AURORA_TEST_CHECK_FALSE(opts.always_on_top);
    AURORA_TEST_CHECK_FALSE(opts.frameless);
    AURORA_TEST_CHECK(opts.decoration == DecorationPolicy::Auto);
    AURORA_TEST_CHECK_FALSE(opts.transparent);
    AURORA_TEST_CHECK_TRUE(opts.resizable);
    // 0 = 不限（最小/最大尺寸默认不限）。
    AURORA_TEST_CHECK_NEAR(opts.min_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.min_size.height, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.max_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.max_size.height, 0.0F, 1e-4F);
    // CSD 标题栏样式默认 36dp 高。
    AURORA_TEST_CHECK_NEAR(opts.title_bar.height, 36.0F, 1e-4F);
}

AURORA_TEST_CASE(decoration_policy_enum_order_contract) {
    // surface.h 契约：枚举值序是各后端映射表的公共契约，不得重排。
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Auto) == 0);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::ServerSide) == 1);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::ClientSide) == 2);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Borderless) == 3);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Frameless) == 4);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(DecorationPolicy::Frameless), 4);
}

AURORA_TEST_CASE(window_resize_edge_enum_order_contract) {
    // 契约：None 为下标 0 哨兵（后端据此拒绝），其余按 上/下/左/右/四角 排列。
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::None) == 0);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Top) == 1);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Bottom) == 2);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Left) == 3);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Right) == 4);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::TopLeft) == 5);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::TopRight) == 6);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::BottomLeft) == 7);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::BottomRight) == 8);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(WindowResizeEdge::BottomRight), 8);
}

AURORA_TEST_CASE(aggregate_header_exposes_surface_contract) {
    // 聚合头至少暴露抽象层：Surface 抽象、不可拷贝/移动。
    static_assert(std::is_abstract_v<Surface>);
    static_assert(!std::is_copy_constructible_v<Surface>);
    static_assert(!std::is_move_constructible_v<Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_abstract_v<Surface>);

#ifdef AURORA_BACKEND_HEADLESS
    // 聚合头注释契约：Headless 零三方依赖、默认可用。
    HeadlessSurface surface;
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 0);
    AURORA_TEST_CHECK_NEAR(surface.size().width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NULL(surface.data());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，聚合头不暴露 HeadlessSurface");
#endif
}

}  // namespace aurora::test_cases::utest_native_surfaces
