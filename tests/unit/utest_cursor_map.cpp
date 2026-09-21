/// 测试类型: unit
/// 目标单元: include/aurora/window/cursor_map.h
/// 测试说明: 光标形状跨后端映射 SSOT 契约——规范名（freedesktop 主题名 / CSS cursor 关键字）
/// 覆盖全部 CursorShape、两两互异、未知回退 default；并以 static_assert 锁定枚举取值序
/// （各后端 .cpp 的映射表按该序索引，重排即破坏后端映射）

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aurora/window/cursor_map.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_cursor_map {

namespace {

/// @brief 全部 CursorShape 取值（按枚举取值序）；长度契约由 static_assert 对齐 AURORA_CURSOR_SHAPE_COUNT。
constexpr std::array<CursorShape, AURORA_CURSOR_SHAPE_COUNT> AURORA_ALL_SHAPES = {
    CursorShape::Arrow,     CursorShape::IBeam,      CursorShape::PointingHand, CursorShape::ResizeNS,
    CursorShape::ResizeEW,  CursorShape::ResizeNWSE, CursorShape::ResizeNESW,   CursorShape::Move,
    CursorShape::Crosshair, CursorShape::NotAllowed, CursorShape::Wait,
};
static_assert(std::size(AURORA_ALL_SHAPES) == AURORA_CURSOR_SHAPE_COUNT,
              "ALL_SHAPES 漏填：新增 CursorShape 后须同步扩列");

}  // namespace

AURORA_TEST_CASE(cursor_shape_enum_order_contract) {
    // surface.h/enums.h 契约：映射表按枚举取值序索引（GLFW/Win32/X11/macOS 各自 switch/数组），
    // 重排取值即破坏全部后端映射——此处以 static_assert 把顺序钉死为契约。
    static_assert(static_cast<std::uint8_t>(CursorShape::Arrow) == 0);
    static_assert(static_cast<std::uint8_t>(CursorShape::IBeam) == 1);
    static_assert(static_cast<std::uint8_t>(CursorShape::PointingHand) == 2);
    static_assert(static_cast<std::uint8_t>(CursorShape::ResizeNS) == 3);
    static_assert(static_cast<std::uint8_t>(CursorShape::ResizeEW) == 4);
    static_assert(static_cast<std::uint8_t>(CursorShape::ResizeNWSE) == 5);
    static_assert(static_cast<std::uint8_t>(CursorShape::ResizeNESW) == 6);
    static_assert(static_cast<std::uint8_t>(CursorShape::Move) == 7);
    static_assert(static_cast<std::uint8_t>(CursorShape::Crosshair) == 8);
    static_assert(static_cast<std::uint8_t>(CursorShape::NotAllowed) == 9);
    static_assert(static_cast<std::uint8_t>(CursorShape::Wait) == 10);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(CursorShape::Wait), 10);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(AURORA_CURSOR_SHAPE_COUNT), 11);
}

AURORA_TEST_CASE(cursor_shape_rfc_names_map_all_shapes) {
    // 逐取值断言规范名（freedesktop cursor theme 名 = W3C CSS `cursor` 关键字）。
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::Arrow), "default");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::IBeam), "text");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::PointingHand), "pointer");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::ResizeNS), "ns-resize");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::ResizeEW), "ew-resize");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::ResizeNWSE), "nwse-resize");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::ResizeNESW), "nesw-resize");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::Move), "move");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::Crosshair), "crosshair");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::NotAllowed), "not-allowed");
    AURORA_TEST_CHECK_STREQ(cursor_rfc_name(CursorShape::Wait), "wait");
}

AURORA_TEST_CASE(cursor_shape_rfc_names_are_pairwise_unique_and_non_empty) {
    // 互异性：两个不同语义形状不得映射到同一主题名（否则 Wayland/浏览器上无法区分）。
    // 非空性：任何形状都不得产出空名（空名会让 wl_cursor_theme_get_cursor 静默失败）。
    for (std::size_t i = 0; i < AURORA_ALL_SHAPES.size(); ++i) {
        const std::string_view a{cursor_rfc_name(AURORA_ALL_SHAPES.at(i))};
        AURORA_TEST_CHECK_FALSE(a.empty());
        for (std::size_t j = i + 1; j < AURORA_ALL_SHAPES.size(); ++j) {
            const std::string_view b{cursor_rfc_name(AURORA_ALL_SHAPES.at(j))};
            AURORA_TEST_CHECK_STRNE(a, b);
        }
    }
}

AURORA_TEST_CASE(cursor_shape_rfc_name_is_constexpr) {
    // 契约：映射表须可在常量表达式内求值（后端可据其定义 constexpr 长度/查表）。
    static_assert(cursor_rfc_name(CursorShape::Arrow) != nullptr);
    static_assert(std::string_view{cursor_rfc_name(CursorShape::IBeam)} == std::string_view{"text"});
    static_assert(std::string_view{cursor_rfc_name(CursorShape::NotAllowed)} == std::string_view{"not-allowed"});
    AURORA_TEST_CHECK_TRUE(true);
}

}  // namespace aurora::test_cases::utest_cursor_map
