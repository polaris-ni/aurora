// core_test.cpp — 覆盖 core 基础类型与工具的单测（原缺口模块）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// core/types.h / math.h / dimension.h / duration.h / time.h / color.h / debug.h(check_render_purity)
//   / diagnostics.h(基础上报) / immutable.h / utf8.h → 本文件既有用例直接行使；
// core/literals.h → UDL 声明头（_dp/_px 等行为由 dimension UDL 用例行使）；
// core/assert.h → AURORA_ASSERT 宏：Debug 断言、Release 编译剥离，行为断言豁免（同类路径见 test_strict_mode）。

#include <cmath>
#include <cstdint>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/math.h"
#include "aurora/core/string_util.h"
#include "aurora/core/utf8.h"
#include "test_harness.h"

// ---- Result / Error ----
static void test_result() {
    {
        au::Result ok = 42;
        AURORA_TEST_CHECK_MSG(ok.ok(), "Result: ok() true for value");
        AURORA_TEST_CHECK_MSG(static_cast<bool>(ok), "Result: operator bool true for value");
        AURORA_TEST_CHECK_MSG(ok.value() == 42, "Result: value() returns stored value");
    }
    {
        au::Result<int> err = au::make_error(aurora::ErrorCode::GeneralUnknown, "something broke");
        AURORA_TEST_CHECK_MSG(!err.ok(), "Result: ok() false for error");
        AURORA_TEST_CHECK_MSG(!static_cast<bool>(err), "Result: operator bool false for error");
        AURORA_TEST_CHECK_MSG(err.error().code == "general-unknown", "Result: error().code is slug");
        AURORA_TEST_CHECK_MSG(err.error().message == "something broke", "Result: error().message");
    }
    {
        au::Result<int> e3 = make_error(aurora::ErrorCode::ValidationFailed, "msg", "fix it", "docs#x", "file:1");
        AURORA_TEST_CHECK_MSG(e3.error().suggestion == "fix it", "Result: make_error suggestion");
        AURORA_TEST_CHECK_MSG(e3.error().docs == "docs#x", "Result: make_error docs");
        AURORA_TEST_CHECK_MSG(e3.error().where == "file:1", "Result: make_error where");
    }
    {
        au::Error er{.code = "code", .message = "m"};
        std::string js = er.to_json();
        AURORA_TEST_CHECK_MSG(!js.empty(), "Error: to_json() non-empty");
        AURORA_TEST_CHECK_MSG(js.find("\"code\"") != std::string::npos, "Error: to_json contains code field");
    }
    {
        bool threw = false;
        au::Result ok = 5;
        try {
            (void)ok.unwrap();
        } catch (...) {
            threw = true;
        }
        AURORA_TEST_CHECK_MSG(!threw, "Result: unwrap() does not throw on success");
        au::Result<int> bad = make_error(aurora::ErrorCode::GeneralUnknown, "fail");
        bool threw2 = false;
        try {
            static_cast<void>(bad.unwrap());
        } catch (const std::exception &) {
            threw2 = true;
        }
        AURORA_TEST_CHECK_MSG(threw2, "Result: unwrap() throws on failure");
    }
}

// ---- 几何类型 Point / Size / Rect / EdgeInsets / Length / Constraints ----
static void test_geometry() {
    {
        constexpr au::Point a{.x = 1, .y = 2};
        constexpr au::Point b{.x = 3, .y = 4};
        constexpr au::Point c = a + b;
        AURORA_TEST_CHECK_MSG(c.x == 4 && c.y == 6, "Point: operator+");
        constexpr au::Point d = b - a;
        AURORA_TEST_CHECK_MSG(d.x == 2 && d.y == 2, "Point: operator-");
    }
    {
        constexpr au::Size inf = au::Size::infinity();
        AURORA_TEST_CHECK_MSG(!inf.is_finite(), "Size: infinity is not finite");
        constexpr au::Size s{.width = 10, .height = 20};
        AURORA_TEST_CHECK_MSG(s.is_finite(), "Size: finite is_finite");
        constexpr au::Size s2 = s * 2.0F;
        AURORA_TEST_CHECK_MSG(s2.width == 20 && s2.height == 40, "Size: operator* scalar");
        constexpr au::Size s3 = s + au::Size{.width = 1, .height = 1};
        AURORA_TEST_CHECK_MSG(s3.width == 11 && s3.height == 21, "Size: operator+");
    }
    {
        constexpr au::Rect r{.origin = au::Point{.x = 0, .y = 0}, .size = au::Size{.width = 10, .height = 20}};
        AURORA_TEST_CHECK_MSG(near_f(r.right(), 10.0F), "Rect: right()");
        AURORA_TEST_CHECK_MSG(near_f(r.bottom(), 20.0F), "Rect: bottom()");
        AURORA_TEST_CHECK_MSG(r.contains(au::Point{.x = 5, .y = 5}), "Rect: contains inside");
        AURORA_TEST_CHECK_MSG(r.contains(au::Point{.x = 0, .y = 0}), "Rect: contains origin (inclusive)");
        AURORA_TEST_CHECK_MSG(!r.contains(au::Point{.x = 11, .y = 5}), "Rect: contains outside x");
        AURORA_TEST_CHECK_MSG(!r.contains(au::Point{.x = 5, .y = 21}), "Rect: contains outside y");
    }
    {
        constexpr au::EdgeInsets ins{.left = 1, .top = 2, .right = 3, .bottom = 4};
        AURORA_TEST_CHECK_MSG(near_f(ins.horizontal(), 4.0F), "EdgeInsets: horizontal");
        AURORA_TEST_CHECK_MSG(near_f(ins.vertical(), 6.0F), "EdgeInsets: vertical");
    }
    {
        constexpr au::Length wrap = au::Length::wrap();
        constexpr au::Length fill_l = au::Length::expand();
        constexpr au::Length fixed = au::Length::fixed(120);
        constexpr au::Length ratio = au::Length::ratio(0.5F);
        AURORA_TEST_CHECK_MSG(wrap.kind == au::LengthKind::WrapContent, "Length: wrap kind");
        AURORA_TEST_CHECK_MSG(fill_l.kind == au::LengthKind::Expand, "Length: expand kind");
        AURORA_TEST_CHECK_MSG(fixed.kind == au::LengthKind::Fixed && near_f(fixed.value, 120.0F), "Length: fixed");
        AURORA_TEST_CHECK_MSG(ratio.kind == au::LengthKind::Fraction && near_f(ratio.value, 0.5F), "Length: ratio");
    }
    {
        constexpr au::Constraints c{.min = au::Size{.width = 0, .height = 0},
                                    .max = au::Size{.width = 100, .height = 100}};
        const au::Size r1 = c.constrain(au::Size{.width = 50, .height = 50});
        AURORA_TEST_CHECK_MSG(r1.width == 50 && r1.height == 50, "Constraints: constrain within range");
        const au::Size r2 = c.constrain(au::Size{.width = 200, .height = 30});
        AURORA_TEST_CHECK_MSG(r2.width == 100 && r2.height == 30, "Constraints: constrain clamps max");
        const au::Size r3 = c.constrain(au::Size{.width = -5, .height = 30});
        AURORA_TEST_CHECK_MSG(r3.width == 0 && r3.height == 30, "Constraints: constrain clamps min");
    }
}

// ---- 强类型尺寸工厂 + 字面量 ----
static void test_dimension() {
    constexpr au::Length p = au::px(120);
    AURORA_TEST_CHECK_MSG(p.kind == au::LengthKind::Fixed && near_f(p.value, 120.0F), "dimension: px");
    constexpr au::Length d = au::dp(80);
    AURORA_TEST_CHECK_MSG(d.kind == au::LengthKind::Fixed && near_f(d.value, 80.0F), "dimension: dp");
    constexpr au::Length pc = au::percent(0.25F);
    AURORA_TEST_CHECK_MSG(pc.kind == au::LengthKind::Fraction && near_f(pc.value, 0.25F), "dimension: percent");
    constexpr au::Length f = au::fill();
    AURORA_TEST_CHECK_MSG(f.kind == au::LengthKind::Expand, "dimension: fill");
    constexpr au::Length a = au::auto_length();
    AURORA_TEST_CHECK_MSG(a.kind == au::LengthKind::WrapContent, "dimension: auto_length");

    AURORA_TEST_CHECK_MSG(to_string(au::px(10)) == "px(10.000000)", "dimension: to_string px");
    AURORA_TEST_CHECK_MSG(to_string(au::fill()) == "fill", "dimension: to_string fill");
    AURORA_TEST_CHECK_MSG(to_string(aurora::percent(0.5F)).find("percent") != std::string::npos,
                          "dimension: to_string percent");

    {
        using aurora::literals::operator""_dp;
        using aurora::literals::operator""_px;
        constexpr auto d1 = 120_dp;
        constexpr auto p1 = 8_px;
        AURORA_TEST_CHECK_MSG(d1.kind == au::LengthKind::Fixed && near_f(d1.value, 120.0F), "dimension literal: _dp");
        AURORA_TEST_CHECK_MSG(p1.kind == au::LengthKind::Fixed && near_f(p1.value, 8.0F), "dimension literal: _px");
    }
}

// ---- Duration ----
static void test_duration() {
    constexpr au::Duration s = au::Duration::from_seconds(2.0);
    AURORA_TEST_CHECK_MSG(near_f(s.seconds, 2.0), "Duration: from_seconds");
    constexpr au::Duration m = au::Duration::from_ms(250);
    AURORA_TEST_CHECK_MSG(near_f(m.seconds, 0.25), "Duration: from_ms");
    AURORA_TEST_CHECK_MSG(m == au::Duration::from_ms(250), "Duration: operator==");
    AURORA_TEST_CHECK_MSG(m != au::Duration::from_ms(300), "Duration: operator!= via ==");
    AURORA_TEST_CHECK_MSG(m.to_chrono().count() == 0.25, "Duration: to_chrono");

    {
        using au::literals::operator""_ms;
        constexpr auto lit = 500_ms;
        AURORA_TEST_CHECK_MSG(near_f(lit.seconds, 0.5), "Duration literal: _ms");
    }
}

// ---- Color ----
static void test_color() {
    constexpr au::Color c = au::Color::from_rgba(10, 20, 30, 40);
    AURORA_TEST_CHECK_MSG(c.m_r == 10 && c.m_g == 20 && c.m_b == 30 && c.m_a == 40, "Color: from_rgba");
    AURORA_TEST_CHECK_MSG(au::Color::white() == au::Color{255, 255, 255}, "Color: white");
    AURORA_TEST_CHECK_MSG(au::Color::black() == au::Color{0, 0, 0}, "Color: black");
    AURORA_TEST_CHECK_MSG(au::Color::red() == au::Color{255, 0, 0}, "Color: red");
    AURORA_TEST_CHECK_MSG(au::Color::transparent() == au::Color{0, 0, 0, 0}, "Color: transparent");
    AURORA_TEST_CHECK_MSG(au::colors::AURORA_RED == au::Color::red(), "colors::Red alias");
    AURORA_TEST_CHECK_MSG(au::colors::AURORA_BLUE == au::Color::blue(), "colors::Blue alias");

    {
        using au::literals::operator""_rgb;
        using au::literals::operator""_rgba;
        constexpr auto rgb = 0xFF0000_rgb;
        AURORA_TEST_CHECK_MSG(rgb == au::Color{255, 0, 0}, "Color literal: _rgb");
        constexpr auto rgba = 0x0000FFFF_rgba;
        AURORA_TEST_CHECK_MSG(rgba == au::Color{0, 0, 255, 255}, "Color literal: _rgba");
    }
}

// ---- Immutable / Mutable ----
static void test_immutable() {
    au::State s{7};
    const au::Immutable im{s};
    AURORA_TEST_CHECK_MSG(im.get() == 7, "Immutable: get()");
    AURORA_TEST_CHECK_MSG(im.scope().empty(), "Immutable: default scope empty");
    const au::Immutable im2{s, "reader"};
    AURORA_TEST_CHECK_MSG(im2.scope() == "reader", "Immutable: named scope");

    au::Mutable m{s};
    m.set(11);
    AURORA_TEST_CHECK_MSG(s.get() == 11, "Mutable: set() propagates to State");
    AURORA_TEST_CHECK_MSG(m.get() == 11, "Mutable: get()");
}

// ---- StrictMode ----
static void test_strict_mode() {
    const au::StrictMode prev = au::strict_mode();
    set_strict_mode(au::StrictMode::On);
    AURORA_TEST_CHECK_MSG(aurora::strict_mode() == au::StrictMode::On, "StrictMode: set On");
    set_strict_mode(au::StrictMode::Off);
    AURORA_TEST_CHECK_MSG(aurora::strict_mode() == au::StrictMode::Off, "StrictMode: set Off");
    set_strict_mode(prev);
}

// ---- time / debug / 渲染纯度 ----
static void test_time_debug() {
    const uint64_t ts = au::current_timestamp();  // 绘制上下文外：g_paint_depth==0，守卫通过
    // ts 为毫秒级 epoch 时间戳（uint64），应远大于 2000 年（1e12 ms），避免无符号 >0 的恒真告警。
    AURORA_TEST_CHECK_MSG(ts > 1'000'000'000'000ULL, "current_timestamp returns sane epoch ms");
#ifdef AURORA_ENABLE_DEBUG
    {
        au::debug::PaintPurityGuard pg;  // 进入绘制上下文（g_paint_depth = 1）
        au::debug::check_render_purity();  // g_paint_depth > 0，断言通过
        AURORA_TEST_CHECK_MSG(true, "debug::check_render_purity within paint context");
    }
    (void)aurora::current_timestamp();  // 离开绘制上下文：g_paint_depth==0，守卫通过
    AURORA_TEST_CHECK_MSG(true, "current_timestamp outside paint ok");
#endif
}

// 验证 Widget::paint 期间 g_paint_depth 被正确置位（时钟纯度守卫的前提）；
// 并验证 layout 阶段（非 paint）不在绘制上下文中。整套纯度机制 AURORA_ENABLE_DEBUG 门控，
// 故本测试仅在 debug 构建编译 / 运行。
#ifdef AURORA_ENABLE_DEBUG
namespace {
struct PurityProbe : au::Widget {
    int depth_in_paint_ = -1;
    int depth_in_layout_ = -1;
    [[nodiscard]] auto type_name() const -> const char * override { return "PurityProbe"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        (void)ctx;
        depth_in_layout_ = au::debug::g_paint_depth;  // layout 不在绘制上下文中，应为 0
        return c.max;
    }
    auto on_paint(au::Painter & /*p*/, const au::Rect & /*bounds*/, const au::BuildContext & /*ctx*/) -> void override {
        depth_in_paint_ = au::debug::g_paint_depth;  // paint 中应为 >0
    }
};
}  // namespace

static void test_paint_purity_flag() {
    PurityProbe w;
    constexpr au::BuildContext ctx;
    w.mount(ctx);
    au::Constraints cc;
    cc.min = au::Size{.width = 0.0F, .height = 0.0F};
    cc.max = au::Size{.width = 100.0F, .height = 100.0F};
    w.layout(cc, ctx);
    AURORA_TEST_CHECK_MSG(w.depth_in_layout_ == 0, "g_paint_depth == 0 during layout (not in paint)");
    au::Painter p;
    p.begin(100, 100);
    w.paint(p, au::Rect{.origin = au::Point{.x = 0.0F, .y = 0.0F}, .size = au::Size{.width = 100.0F, .height = 100.0F}},
            ctx);
    AURORA_TEST_CHECK_MSG(w.depth_in_paint_ > 0, "g_paint_depth > 0 during Widget::paint (purity guard active)");
    AURORA_TEST_CHECK_MSG(aurora::debug::g_paint_depth == 0, "g_paint_depth restored to 0 after paint");
}
#endif  // AURORA_ENABLE_DEBUG

// ---- EventStream ----
static void test_event_stream() {
    au::EventStream<int> es;
    int sum = 0;
    auto sub = es.subscribe([&](const int &v) -> void { sum += v; });
    es.emit(5);
    es.emit(10);
    AURORA_TEST_CHECK_MSG(sum == 15, "EventStream: emit to subscriber");
    sub.reset();
    es.emit(20);
    AURORA_TEST_CHECK_MSG(sum == 15, "EventStream: reset subscription stops delivery");

    {
        au::EventStream<std::string> es2;
        int hits = 0;
        {
            auto s2 = es2.subscribe([&](const std::string &) -> void { ++hits; });
            es2.emit("a");
            AURORA_TEST_CHECK_MSG(hits == 1, "EventStream: scoped subscription active");
        }
        es2.emit("b");
        AURORA_TEST_CHECK_MSG(hits == 1, "EventStream: dtor unsubscribes");
    }
}

// ---- Diagnostics ----
static void test_diagnostics() {
    set_strict_mode(au::StrictMode::Off);  // degraded 在严格模式下会触发断言
    // 使用 g_error_table 中已注册的 Layout 类 slug：Diagnostics::report 按 slug 表驱动
    // 注入 severity/category/code_enum；未注册的 slug 会退化为 General/Warning。
    au::Diagnostics::warn("minor issue", "layout", "layout-invalid-constraints");
    const auto last = au::Diagnostics::get_last_diagnostics();
    AURORA_TEST_CHECK_MSG(!last.empty(), "Diagnostics: get_last_diagnostics non-empty after warn");
    AURORA_TEST_CHECK_MSG(last.back().severity == au::ErrorSeverity::Warning, "Diagnostics: warn severity");
    AURORA_TEST_CHECK_MSG(last.back().message == "minor issue", "Diagnostics: warn message");
    AURORA_TEST_CHECK_MSG(last.back().category == au::ErrorCategory::Layout, "Diagnostics: warn category");

    au::Diagnostics::degraded("bad color", "paint", "render-degraded");
    AURORA_TEST_CHECK_MSG(au::Diagnostics::count() >= 1, "Diagnostics: count increments on degraded");

    const auto taken = au::Diagnostics::take();
    AURORA_TEST_CHECK_MSG(!taken.empty(), "Diagnostics: take returns accumulated");
    AURORA_TEST_CHECK_MSG(au::Diagnostics::count() == 0, "Diagnostics: take clears count");

    const std::string exp = au::Diagnostics::explain_diagnostic("nav-depth-exceeded");
    AURORA_TEST_CHECK_MSG(!exp.empty(), "Diagnostics: explain_diagnostic known code non-empty");
    const std::string exp2 = au::Diagnostics::explain_diagnostic("totally-unknown-code");
    AURORA_TEST_CHECK_MSG(!exp2.empty(), "Diagnostics: explain_diagnostic fallback non-empty");

    const au::Diagnostic d{.severity = au::ErrorSeverity::Warning,
                           .category = au::ErrorCategory::Layout,
                           .message = "m",
                           .where = "where",
                           .code = "code",
                           .code_enum = au::ErrorCode::GeneralUnknown,
                           .fix = std::nullopt};
    const std::string line = d.to_json_line();
    AURORA_TEST_CHECK_MSG(!line.empty(), "Diagnostic: to_json_line non-empty");
    AURORA_TEST_CHECK_MSG(line.find("\"severity\"") != std::string::npos, "Diagnostic: to_json_line has severity");
}

// ---- MainThreadOnly ----
static void test_main_thread_only() {
    au::MainThreadOnly mto{42};
    AURORA_TEST_CHECK_MSG(mto.get() == 42, "MainThreadOnly: get");
    mto.set(99);
    AURORA_TEST_CHECK_MSG(mto.get() == 99, "MainThreadOnly: set");
    au::MainThreadOnly<int, false> mto2{5};
    AURORA_TEST_CHECK_MSG(mto2.get() == 5, "MainThreadOnly<false>: get");
}

// ---- Font ----
static void test_font() {
    const au::Font f;
    AURORA_TEST_CHECK_MSG(f.family == "sans-serif", "Font: default family");
    AURORA_TEST_CHECK_MSG(near_f(f.size_pt, 14.0F), "Font: default size");
    AURORA_TEST_CHECK_MSG(f.weight == 400, "Font: default weight");
    const au::Font f2{.family = "serif", .size_pt = 20.0F, .weight = 700};
    AURORA_TEST_CHECK_MSG(f2.family == "serif" && near_f(f2.size_pt, 20.0F) && f2.weight == 700, "Font: custom ctor");
}

// ---- Image (结构，解码见 png_image_test) ----
static void test_image_struct() {
    const au::Image img;
    AURORA_TEST_CHECK_MSG(img.width == 0 && img.height == 0, "Image: default zero size");
    AURORA_TEST_CHECK_MSG(img.pixels.empty(), "Image: default empty pixels");
}

// ---- UTF-8 工具 (core/utf8.h) ----
static void test_utf8() {
    AURORA_TEST_CHECK_MSG(aurora::utf8_encode(0x41U) == "A", "utf8 ascii");
    AURORA_TEST_CHECK_MSG(aurora::utf8_encode(0xC0U) == std::string("\xC3\x80", 2), "utf8 2-byte");
    AURORA_TEST_CHECK_MSG(aurora::utf8_encode(0x20ACU) == std::string("\xE2\x82\xAC", 3), "utf8 3-byte euro");
    AURORA_TEST_CHECK_MSG(aurora::utf8_encode(0x1F600U) == std::string("\xF0\x9F\x98\x80", 4), "utf8 4-byte emoji");

    const std::string s = "A\xE2\x82\xAC\xF0\x9F\x98\x80";  // A + euro + emoji
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_len('A') == 1U, "cp_len ascii");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_len(0xE2U) == 3U, "cp_len 3-byte head");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_len(0xF0U) == 4U, "cp_len 4-byte head");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_count(s) == 3U, "cp_count mixed");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_slice(s, 0U, 1U) == "A", "cp_slice first");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_slice(s, 1U, 1U) == std::string("\xE2\x82\xAC", 3), "cp_slice euro");
    AURORA_TEST_CHECK_MSG(aurora::utf8_cp_slice(s, 2U, 1U) == std::string("\xF0\x9F\x98\x80", 4), "cp_slice emoji");
}

// ---- string_format (core/string_util.h, internal) ----
static void test_string_format() {
    AURORA_TEST_CHECK_MSG(aurora::internal::string_format("hello %d", 42) == "hello 42", "string_format int");
    AURORA_TEST_CHECK_MSG(aurora::internal::string_format("%s-%s", "a", "b") == "a-b", "string_format concat");
    AURORA_TEST_CHECK_MSG(aurora::internal::string_format("%.2f", 3.14159) == "3.14", "string_format float precision");
    const std::string big(200, 'x');
    AURORA_TEST_CHECK_MSG(aurora::internal::string_format("%s", big.c_str()) == big, "string_format long buffer");
    AURORA_TEST_CHECK_MSG(aurora::internal::string_format(nullptr).empty(), "string_format null fmt");
}

// ---- saturate / saturate_u8 (core/math.h) ----
static void test_math_saturate() {
    // saturate: 钳到 [0,1]，负数→0、大于 1→1、区间内不变。
    AURORA_TEST_CHECK_MSG(near_f(au::saturate(-0.5F), 0.0F), "saturate clamps negative to 0");
    AURORA_TEST_CHECK_MSG(near_f(au::saturate(2.0F), 1.0F), "saturate clamps >1 to 1");
    AURORA_TEST_CHECK_MSG(near_f(au::saturate(0.0F), 0.0F), "saturate keeps 0");
    AURORA_TEST_CHECK_MSG(near_f(au::saturate(1.0F), 1.0F), "saturate keeps 1");
    AURORA_TEST_CHECK_MSG(near_f(au::saturate(0.37F), 0.37F), "saturate keeps in-range");
    // saturate_u8: 先 clamp 到 [0,255] 再转 uint8，保留 0/255 边界与零值约定。
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(-1.0F) == 0, "saturate_u8 clamps negative to 0");
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(300.0F) == 255, "saturate_u8 clamps >255 to 255");
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(0.0F) == 0, "saturate_u8 keeps 0");
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(255.0F) == 255, "saturate_u8 keeps 255");
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(127.6F) == 127, "saturate_u8 truncates via cast (127)");
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(100.4F) == 100, "saturate_u8 truncates via cast (100)");
    // 与原手写 std::clamp 等价性微验证。
    constexpr float v = 42.9F;
    AURORA_TEST_CHECK_MSG(aurora::saturate_u8(v) == static_cast<std::uint8_t>(std::clamp(v, 0.0F, 255.0F)),
                          "saturate_u8 matches std::clamp cast");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== core_test ===\n");
    test_result();
    test_geometry();
    test_dimension();
    test_duration();
    test_color();
    test_immutable();
    test_strict_mode();
    test_time_debug();
#ifdef AURORA_ENABLE_DEBUG
    test_paint_purity_flag();
#endif
    test_event_stream();
    test_diagnostics();
    test_main_thread_only();
    test_font();
    test_image_struct();
    test_utf8();
    test_string_format();
    test_math_saturate();
}
