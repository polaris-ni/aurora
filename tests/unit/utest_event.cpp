/// 测试类型: unit
/// 目标单元: include/aurora/event/event.h
/// 测试说明: 各事件结构默认值与 is_handled 标志、ModifierKey 位组合算子、同类型拷贝与多态析构、TouchEvent
/// 活跃触点统计/按 id 查找/双指几何
///           另覆盖「平台主修饰键」折算：`fold_primary_modifier` 的逐值表（macOS 把 Command 位并入
///           Ctrl 位、其余平台逐位恒等、锁定态位不受染），与 `has_primary_modifier` 的判定档

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_event {

namespace {

/// @brief 修饰位集 → 诊断文本（折算不符时一眼看出差在哪一枚位）。
/// @param bits 修饰位集的原始值。
/// @return 以 `|` 连接的位名串；无任何位时为 "None"。
[[nodiscard]] auto modifier_bits_text(std::uint8_t bits) -> std::string {
    constexpr std::array<std::pair<std::uint8_t, std::string_view>, 5> names{{
        {static_cast<std::uint8_t>(ModifierKey::Shift), "Shift"},
        {static_cast<std::uint8_t>(ModifierKey::Control), "Control"},
        {static_cast<std::uint8_t>(ModifierKey::Alt), "Alt"},
        {static_cast<std::uint8_t>(ModifierKey::Meta), "Meta"},
        {static_cast<std::uint8_t>(ModifierKey::NumLock), "NumLock"},
    }};
    std::string out;
    for (const auto &[bit, name] : names) {
        if ((bits & bit) != 0U) {
            if (!out.empty()) {
                out += '|';
            }
            out += name;
        }
    }
    return out.empty() ? std::string{"None"} : out;
}

/// @brief 本平台期望的取法：两档期望都在表里，选哪档由编译期平台分支决定。
/// @tparam T 期望值类型（位集或判定结果皆可）。
/// @param macos 期望（macOS 融合腿）。
/// @param elsewhere 期望（其余平台恒等腿）。
/// @return 当前构建应满足的那一档。
template <typename T>
[[nodiscard]] constexpr auto platform_expected(T macos, T elsewhere) -> T {
#ifdef AURORA_PLATFORM_MACOS
    return macos;
#else
    return elsewhere;
#endif
}

}  // namespace

AURORA_TEST_CASE(event_defaults_and_handled_flag) {
    const Event base{};
    AURORA_TEST_CHECK_FALSE(base.is_handled);

    const MouseEvent mouse{};
    AURORA_TEST_CHECK_FALSE(mouse.is_handled);
    AURORA_TEST_CHECK_NEAR(mouse.position.x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.position.y, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.local_position.x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.local_position.y, 0.0F, 1e-6F);
    AURORA_TEST_CHECK(mouse.button == MouseButton::Left);
    AURORA_TEST_CHECK(mouse.action == MouseAction::Press);
    AURORA_TEST_CHECK_FALSE(mouse.pointer_id.has_value());

    const KeyEvent key{};
    AURORA_TEST_CHECK_EQ(key.key, 0);
    AURORA_TEST_CHECK(key.action == KeyAction::Down);
    AURORA_TEST_CHECK(key.modifiers == ModifierKey::None);

    const ScrollEvent scroll{};
    AURORA_TEST_CHECK_EQ(scroll.delta_x, 0.0F);
    AURORA_TEST_CHECK_EQ(scroll.delta_y, 0.0F);

    const TextInputEvent text{};
    AURORA_TEST_CHECK(text.text.empty());

    const SystemDropEvent drop{};
    AURORA_TEST_CHECK(drop.formats.empty());
    AURORA_TEST_CHECK_EQ(drop.phase, SystemDropPhase::Drop);  // 默认态 = drop（程序化构造的常态）
    AURORA_TEST_CHECK_EQ(drop.proposed, DragAction::None);
    AURORA_TEST_CHECK_EQ(drop.allowed, DragAction::None);
    AURORA_TEST_CHECK(drop.payload == nullptr);  // 未携带载荷：读取按空口径返回

    // 无载荷时的便利读取一律为空口径（空内容不是失败），且不得崩溃。
    AURORA_TEST_REQUIRE(drop.files().ok());
    AURORA_TEST_CHECK(drop.files().value().empty());
    AURORA_TEST_REQUIRE(drop.text().ok());
    AURORA_TEST_CHECK(drop.text().value().empty());
    AURORA_TEST_REQUIRE(drop.image().ok());
    AURORA_TEST_CHECK_EQ(drop.image().value().width, 0);
    AURORA_TEST_REQUIRE(drop.bytes("text/plain").ok());
    AURORA_TEST_CHECK(drop.bytes("text/plain").value().empty());

    const TouchPoint point{};
    AURORA_TEST_CHECK_EQ(point.id, 0);
    AURORA_TEST_CHECK_TRUE(point.is_active);

    const TouchEvent touch{};
    AURORA_TEST_CHECK_EQ(touch.active_count(), 0);
    AURORA_TEST_CHECK_EQ(touch.pinch_distance(), 0.0F);  // 少于 2 活跃触点按约定返回 0
    AURORA_TEST_CHECK_EQ(touch.pinch_angle(), 0.0F);
    AURORA_TEST_CHECK_FALSE(touch.point_by_id(0).has_value());
}

AURORA_TEST_CASE(modifier_key_bitwise_or_and_and) {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::None), std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Shift), std::uint8_t{1});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Control), std::uint8_t{2});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Alt), std::uint8_t{4});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Meta), std::uint8_t{8});

    // operator| 组合位掩码；operator& 返回 uint8_t 便于判位
    const auto combo = ModifierKey::Shift | ModifierKey::Control;
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(combo), std::uint8_t{3});
    AURORA_TEST_CHECK_NE(combo & ModifierKey::Shift, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(combo & ModifierKey::Control, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(combo & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(combo | ModifierKey::Shift), std::uint8_t{3});  // 幂等
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::None | ModifierKey::Meta), std::uint8_t{8});

    // 与 KeyEvent.modifiers 字段的往返用法
    KeyEvent e;
    e.modifiers = ModifierKey::Alt | ModifierKey::Meta;
    AURORA_TEST_CHECK_NE(e.modifiers & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(e.modifiers & ModifierKey::Shift, std::uint8_t{0});
}

// NumLock 是**锁定**态而非按住态，故单列一条：位值必须与既有四位互不重叠，且能与其它位共存。
// 位值写死断言的理由与 KeyCode 相同：消费方按 `modifiers & ModifierKey::NumLock` 判小键盘语义，
// 改位值会让所有这类判读静默失效。
AURORA_TEST_CASE(numlock_is_a_separate_modifier_bit) {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::NumLock), std::uint8_t{16});
    // 与既有四位两两不重叠：按位或之后仍可逐位反解。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::NumLock | ModifierKey::Shift), std::uint8_t{17});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::NumLock | ModifierKey::Meta), std::uint8_t{24});
    const auto all =
        ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta | ModifierKey::NumLock;
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(all), std::uint8_t{31});
    AURORA_TEST_CHECK_NE(all & ModifierKey::NumLock, std::uint8_t{0});
    // NumLock 关闭时该位为 0：消费方正是靠这个「零」把 KP_Prior 当 PageUp 用。
    const auto off = ModifierKey::Shift;
    AURORA_TEST_CHECK_EQ(off & ModifierKey::NumLock, std::uint8_t{0});
}

// 指针与滚轮事件复用同一 ModifierKey 枚举、同一判读方式（specification/05-event-navigation.md
// §2.2.2）。本条守三件事：① 两个字段缺省都是 None（250 处既有构造点逐位不变）；② 写入后
// 逐位往返与 KeyEvent 完全一致；③ 缺省语义两义（「没按修饰键」与「不由真实指针产生」不可
// 从字段区分，需靠 pointer_id），此处只把「缺省确为 None」钉住，不去编码那层推断。
AURORA_TEST_CASE(pointer_and_scroll_carry_the_same_modifier_bits) {
    const MouseEvent fresh_mouse{};
    AURORA_TEST_CHECK(fresh_mouse.modifiers == ModifierKey::None);
    const ScrollEvent fresh_scroll{};
    AURORA_TEST_CHECK(fresh_scroll.modifiers == ModifierKey::None);

    // 逐位往返：Alt|Shift 在两个结构体上判读一致，且与 KeyEvent 同值。
    const auto want = ModifierKey::Shift | ModifierKey::Alt;
    MouseEvent m;
    m.modifiers = want;
    ScrollEvent s;
    s.modifiers = want;
    KeyEvent k;
    k.modifiers = want;
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(m.modifiers), std::uint8_t{5});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(s.modifiers), static_cast<std::uint8_t>(k.modifiers));
    AURORA_TEST_CHECK_NE(m.modifiers & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(s.modifiers & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(s.modifiers & ModifierKey::Shift, std::uint8_t{0});
    // 未置位的位判读必须为 0，否则消费方的 `& Alt` 判定会误命中。
    AURORA_TEST_CHECK_EQ(m.modifiers & ModifierKey::Meta, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(s.modifiers & ModifierKey::Control, std::uint8_t{0});
    // NumLock 位与指针事件的四位共存：X11 / Wayland / Win32 侧的真值源会把它一并带上，
    // 消费方只看四个可按住位时自行掩码。既有四位的判读不受其影响。
    const auto with_numlock = want | ModifierKey::NumLock;
    AURORA_TEST_CHECK_NE(with_numlock & ModifierKey::NumLock, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(with_numlock & ModifierKey::Shift, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(with_numlock & ModifierKey::Meta, std::uint8_t{0});
}

AURORA_TEST_CASE(same_type_copy_preserves_event_payload) {
    MouseEvent press;
    press.position = Point{.x = 12.0F, .y = 34.0F};
    press.button = MouseButton::Right;
    press.action = MouseAction::Release;
    press.pointer_id = 7;
    press.is_handled = true;

    MouseEvent copied = press;
    AURORA_TEST_CHECK_NEAR(copied.position.x, 12.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(copied.position.y, 34.0F, 1e-6F);
    AURORA_TEST_CHECK(copied.button == MouseButton::Right);
    AURORA_TEST_CHECK(copied.action == MouseAction::Release);
    AURORA_TEST_REQUIRE(copied.pointer_id.has_value());
    AURORA_TEST_CHECK_EQ(copied.pointer_id.value(), 7);
    AURORA_TEST_CHECK_TRUE(copied.is_handled);

    // 改副本不影响原件
    copied.is_handled = false;
    copied.pointer_id.reset();
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_TRUE(press.pointer_id.has_value());

    // 触摸事件：points 向量随拷贝独立
    TouchEvent with_points;
    with_points.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 1.0F, .y = 1.0F}});
    TouchEvent touch_copy = with_points;
    AURORA_TEST_CHECK_EQ(touch_copy.points.size(), std::size_t{1});
    touch_copy.points.clear();
    AURORA_TEST_CHECK_EQ(with_points.points.size(), std::size_t{1});

    // 经基类指针多态销毁（虚析构）
    std::unique_ptr<Event> polymorphic = std::make_unique<ScrollEvent>();
    AURORA_TEST_CHECK_NO_THROW(polymorphic.reset());
}

AURORA_TEST_CASE(touch_active_count_and_point_lookup) {
    TouchEvent e;
    e.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}});
    e.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 5.0F, .y = 0.0F}, .is_active = false});
    e.points.push_back(TouchPoint{.id = 3, .position = Point{.x = 9.0F, .y = 9.0F}});

    AURORA_TEST_CHECK_EQ(e.active_count(), 2);

    const auto hit = e.point_by_id(2);
    AURORA_TEST_REQUIRE(hit.has_value());
    // value() 替代 operator->：tidy 无法识别宏内 has_value 断言，value() 空时抛出、失败信息更清晰。
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(hit.value().id, 2);
    AURORA_TEST_CHECK_FALSE(hit.value().is_active);
    AURORA_TEST_CHECK_NEAR(hit.value().position.x, 5.0F, 1e-6F);
    // NOLINTEND(bugprone-unchecked-optional-access)

    AURORA_TEST_CHECK_FALSE(e.point_by_id(99).has_value());
}

AURORA_TEST_CASE(touch_pinch_distance_and_angle) {
    TouchEvent e;
    e.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}});
    e.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 3.0F, .y = 4.0F}});
    AURORA_TEST_CHECK_NEAR(e.pinch_distance(), 5.0F, 1e-4F);  // 3-4-5 直角三角形
    AURORA_TEST_CHECK_NEAR(e.pinch_angle(), 0.9272952F, 1e-4F);  // atan2(4, 3)

    // 仅剩 1 个活跃触点：几何查询按约定返回 0
    e.points[1].is_active = false;
    AURORA_TEST_CHECK_EQ(e.active_count(), 1);
    AURORA_TEST_CHECK_EQ(e.pinch_distance(), 0.0F);
    AURORA_TEST_CHECK_EQ(e.pinch_angle(), 0.0F);
}

AURORA_TEST_CASE(base_reference_shares_handled_flag) {
    // 派发器经由 Event& 写 is_handled 停止冒泡：基类引用与派生对象共享同一标志
    KeyEvent key;
    Event &base = key;
    AURORA_TEST_CHECK_FALSE(base.is_handled);
    base.is_handled = true;
    AURORA_TEST_CHECK_TRUE(key.is_handled);

    MouseEvent mouse;
    Event &mouse_base = mouse;
    mouse_base.is_handled = true;
    AURORA_TEST_CHECK_TRUE(mouse.is_handled);
}

AURORA_TEST_CASE(click_window_default_follows_system) {
    // 系统双击速度查询返回值落在合理区间（≥1，且不过大）。
    const std::uint32_t sys = os_double_click_ms();
    AURORA_TEST_CHECK(sys >= 1U);
    AURORA_TEST_CHECK(sys <= 10000U);

    // 默认构造的派发器以系统值为初值。
    EventDispatcher ed;
    AURORA_TEST_CHECK_EQ(ed.click_window_ms, sys);

    TouchDispatcher td;
    AURORA_TEST_CHECK_EQ(td.click_window_ms, sys);

    // 显式覆盖系统值后保持；再回填系统值即恢复「跟随系统」。
    ed.click_window_ms = 123U;
    AURORA_TEST_CHECK_EQ(ed.click_window_ms, 123U);
    ed.click_window_ms = os_double_click_ms();
    AURORA_TEST_CHECK_EQ(ed.click_window_ms, sys);

    // 特殊阈值语义不受默认来源影响：置 0 关闭连击判定、极大值恒开仍可用。
    ed.click_window_ms = 0U;
    AURORA_TEST_CHECK_EQ(ed.click_window_ms, 0U);
    ed.click_window_ms = 60000U;
    AURORA_TEST_CHECK_EQ(ed.click_window_ms, 60000U);
}

AURORA_TEST_CASE(click_window_linux_reads_gsettings_or_fallback) {
#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    AURORA_TEST_SKIP("GSettings double-click read is Linux-only");
#endif
    // Linux 分支：要么经 GLib GSettings 读到真实系统值，要么回退库级兜底常量；
    // 两者均须为正且落在合理区间（避免极端负值或 0 穿透到派发器初值）。
    const std::uint32_t ms = os_double_click_ms();
    AURORA_TEST_CHECK_TRUE(ms >= 1U);
    AURORA_TEST_CHECK_TRUE(ms <= 10000U);
}

// 主修饰键折算的逐值表：macOS 把 Command 位（本框架的 `Meta`）并入 Ctrl 位，其余平台逐位恒等。
// 两档期望都写进同一张表、由编译期分支选档断言，故 Linux / Windows 作业钉的是恒等腿、macOS 作业钉的
// 是融合腿，两条腿各有真 runner 跑过——不存在「另一档靠 SKIP 蒙绿」的窗口。锁定态位（`NumLock`）
// 单列两行：它不属主修饰键，折算不得染指。
AURORA_TEST_CASE(fold_primary_modifier_follows_the_platform_table) {
    struct Sample {
        ModifierKey input;  ///< 折算前位集
        ModifierKey macos;  ///< macOS 期望
        ModifierKey elsewhere;  ///< 其余平台期望
    };
    const std::array<Sample, 10> samples{{
        Sample{.input = ModifierKey::None, .macos = ModifierKey::None, .elsewhere = ModifierKey::None},
        Sample{.input = ModifierKey::Control, .macos = ModifierKey::Control, .elsewhere = ModifierKey::Control},
        Sample{.input = ModifierKey::Meta, .macos = ModifierKey::Control, .elsewhere = ModifierKey::Meta},
        Sample{.input = ModifierKey::Shift, .macos = ModifierKey::Shift, .elsewhere = ModifierKey::Shift},
        Sample{.input = ModifierKey::Alt, .macos = ModifierKey::Alt, .elsewhere = ModifierKey::Alt},
        Sample{.input = ModifierKey::Control | ModifierKey::Meta,
               .macos = ModifierKey::Control,
               .elsewhere = ModifierKey::Control | ModifierKey::Meta},
        Sample{.input = ModifierKey::Shift | ModifierKey::Meta,
               .macos = ModifierKey::Shift | ModifierKey::Control,
               .elsewhere = ModifierKey::Shift | ModifierKey::Meta},
        Sample{.input = ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta,
               .macos = ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt,
               .elsewhere = ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta},
        Sample{.input = ModifierKey::Meta | ModifierKey::NumLock,
               .macos = ModifierKey::Control | ModifierKey::NumLock,
               .elsewhere = ModifierKey::Meta | ModifierKey::NumLock},
        Sample{.input = ModifierKey::Shift | ModifierKey::NumLock,
               .macos = ModifierKey::Shift | ModifierKey::NumLock,
               .elsewhere = ModifierKey::Shift | ModifierKey::NumLock},
    }};

    for (const auto &s : samples) {
        const ModifierKey got = fold_primary_modifier(s.input);
        const ModifierKey want = platform_expected(s.macos, s.elsewhere);
        AURORA_TEST_CHECK_MSG(got == want,
                              modifier_bits_text(static_cast<std::uint8_t>(s.input)) + " -> " +
                                  modifier_bits_text(static_cast<std::uint8_t>(got)) + ", want " +
                                  modifier_bits_text(static_cast<std::uint8_t>(want)));
        // 幂等：折两次与折一次同值——`matches` 比较前两侧各折一次，正靠这条不会二次改位。
        AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(fold_primary_modifier(got)), static_cast<std::uint8_t>(got));
    }

#ifdef AURORA_PLATFORM_MACOS
    // 融合腿的专属不变量：折算后 ⌘ 位不再单独存在（Control 与 Meta 不同时置位）——同一枚主修饰键
    // 只有一种表示，比较结果才与位序无关。
    for (const auto &s : samples) {
        const auto folded = static_cast<std::uint8_t>(fold_primary_modifier(s.input));
        AURORA_TEST_CHECK_EQ(folded & static_cast<std::uint8_t>(ModifierKey::Meta), std::uint8_t{0});
    }
#endif
}

// 主修饰键判定（控件编辑判定的唯一入口）：按着的到底算不算本平台那枚键。判定与折算必须同解，
// 否则控件与注册表会对同一次按键各说各话——故最后一节直接把两式对表逐位比对。
AURORA_TEST_CASE(has_primary_modifier_reads_the_platform_primary_key) {
    struct Sample {
        ModifierKey mods;  ///< 待判定位集
        bool macos;  ///< macOS 期望
        bool elsewhere;  ///< 其余平台期望
    };
    const std::array<Sample, 8> samples{{
        Sample{.mods = ModifierKey::None, .macos = false, .elsewhere = false},
        Sample{.mods = ModifierKey::Control, .macos = true, .elsewhere = true},
        Sample{.mods = ModifierKey::Meta, .macos = true, .elsewhere = false},
        Sample{.mods = ModifierKey::Shift, .macos = false, .elsewhere = false},
        Sample{.mods = ModifierKey::Alt, .macos = false, .elsewhere = false},
        Sample{.mods = ModifierKey::Shift | ModifierKey::Meta, .macos = true, .elsewhere = false},
        Sample{.mods = ModifierKey::NumLock, .macos = false, .elsewhere = false},
        Sample{.mods = ModifierKey::Meta | ModifierKey::NumLock, .macos = true, .elsewhere = false},
    }};
    for (const auto &s : samples) {
        const bool want = platform_expected(s.macos, s.elsewhere);
        const bool got = has_primary_modifier(s.mods);
        AURORA_TEST_CHECK_MSG(got == want,
                              modifier_bits_text(static_cast<std::uint8_t>(s.mods)) + (got ? " is primary" :
                                                                                        " is not primary") +
                                  ", want " + (want ? std::string{"primary"} : std::string{"not primary"}));
        // 两式同源：判定 == 折算后的 Ctrl 位是否置位。
        AURORA_TEST_CHECK_EQ(got, (static_cast<std::uint8_t>(fold_primary_modifier(s.mods)) &
                                   static_cast<std::uint8_t>(ModifierKey::Control)) != 0U);
    }
}

}  // namespace aurora::test_cases::utest_event
