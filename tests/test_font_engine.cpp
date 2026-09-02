// FontEngine 单元测试：验证真实字体度量、选中原语（caret_x / hit_test_char）
// 的语义，以及无 TTF 回退路径下依然可用。
// ── API 覆盖映射 ─────────────────────────────
// core/font.h(Font/FontStyle/FontWeight 数据模型，经 FontEngine 用例行使)。

#include <cmath>
#include <iostream>
#include <string>

#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Font;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::render::FontEngine;
using aurora::render::TextAAMode;

namespace {
auto approx(float a, float b, const float eps = 1e-3f) -> bool { return std::fabs(a - b) < eps; }

// UTF-8 码点计数（与 FontEngine 内部一致）。
auto cp_count(const std::string &s) -> std::size_t {
    std::size_t i = 0;
    std::size_t n = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c < 0x80U) {
            len = 1;
        } else if ((c >> 5U) == 0x6U) {
            len = 2;
        } else if ((c >> 4U) == 0xEU) {
            len = 3;
        } else if ((c >> 3U) == 0x1EU) {
            len = 4;
        }
        i += len;
        ++n;
    }
    return n;
}
} // namespace

AURORA_TEST() {
    auto &fe = FontEngine::instance();

    const Font f14{ .size_pt = 14.0f };
    const Font f20{ .size_pt = 20.0f };

    // 1) 高度为正且随字号单调
    {
        AURORA_TEST_CHECK(fe.measure_height(f14) > 0.0f);
        AURORA_TEST_CHECK(fe.measure_height(f20) > fe.measure_height(f14));
        AURORA_LOG_INFO("test", "[1] measure_height OK");
    }

    // 2) 空串宽度为 0
    {
        AURORA_TEST_CHECK(fe.measure_width("", f14) == 0.0f);
        AURORA_LOG_INFO("test", "[2] empty width == 0 OK");
    }

    // 3) 宽度随串长单调
    {
        const float w1 = FontEngine::measure_width("Hell", f14);
        const float w2 = FontEngine::measure_width("Hello", f14);
        AURORA_TEST_CHECK(w2 > w1);
        AURORA_LOG_INFO("test", "[3] width monotonic OK");
    }

    // 4) caret_x 端点：第 0 码点在前，整串末尾等于 measure_width
    {
        const std::string s = "Hello, FontEngine!";
        AURORA_TEST_CHECK(fe.caret_x(s, 0, f14) == 0.0f);
        const std::size_t n = cp_count(s);
        AURORA_TEST_CHECK(approx(fe.caret_x(s, n, f14), fe.measure_width(s, f14)));
        AURORA_LOG_INFO("test", "[4] caret_x endpoints OK");
    }

    // 5) hit_test_char 端点：x<=0 → 0；足够大 → 码点数
    {
        const std::string s = "Hello, FontEngine!";
        const std::size_t n = cp_count(s);
        AURORA_TEST_CHECK(fe.hit_test_char(s, 0.0f, f14) == 0);
        AURORA_TEST_CHECK(fe.hit_test_char(s, fe.measure_width(s, f14) + 999.0f, f14) == n);
        AURORA_LOG_INFO("test", "[5] hit_test_char endpoints OK");
    }

    // 6) 选中原语往返：caret_x(i) 经 hit_test_char 回到 i（UTF-8 安全，含 CJK）
    {
        const std::string samples[] = { "Hello", "Hello, World!", "你好，世界", "A你B好C", "mix 中英文 ok" };
        for (const auto &s : samples) {
            const std::size_t n = cp_count(s);
            AURORA_TEST_CHECK(approx(fe.caret_x(s, n, f14), fe.measure_width(s, f14)));
            for (std::size_t i = 0; i <= n; ++i) {
                const float x = FontEngine::caret_x(s, i, f14);
                AURORA_TEST_CHECK(fe.hit_test_char(s, x, f14) == i);
            }
        }
        AURORA_LOG_INFO("test", "[6] caret<->hit_test round-trip OK (incl. CJK)");
    }

    // 7) hit_test_char 落在中间返回合理下标
    {
        const std::string s = "Hello, World!";
        const std::size_t n = cp_count(s);
        const float mid = FontEngine::measure_width(s, f14) * 0.5f;
        const std::size_t idx = FontEngine::hit_test_char(s, mid, f14);
        AURORA_TEST_CHECK(idx > 0 && idx < n);
        AURORA_LOG_INFO("test", "[7] hit_test_char midpoint OK");
    }

    // 8) 文本抗锯齿策略切换
    {
        const auto prev = FontEngine::text_aa_mode();
        FontEngine::set_text_aa_mode(TextAAMode::Supersample);
        AURORA_TEST_CHECK(FontEngine::text_aa_mode() == TextAAMode::Supersample);
        FontEngine::set_text_aa_mode(TextAAMode::ClearType);
        AURORA_TEST_CHECK(FontEngine::text_aa_mode() == TextAAMode::ClearType);
        FontEngine::set_text_aa_mode(prev);
        AURORA_LOG_INFO("test", "[8] text_aa_mode switch OK");
    }

    // 9) 超采样 / ClearType 均渲染出文本且存在抗锯齿过渡（含中文）
    {
        const auto prev = FontEngine::text_aa_mode();
        const Font f{ .family = "sans-serif", .size_pt = 14.0f, .weight = 400 };
        const std::string text = "Aurora 中文";
        for (const auto mode : { TextAAMode::Supersample, TextAAMode::ClearType }) {
            FontEngine::set_text_aa_mode(mode);
            Painter p;
            p.begin(240, 60);
            p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 240, .height = 60 } },
                        Color{ 255, 255, 255, 255 });
            FontEngine::instance().draw_text(
                p, Rect{ .origin = Point{ .x = 10, .y = 10 }, .size = Size{ .width = 220, .height = 40 } }, text, f,
                Color{ 0, 0, 0, 255 });
            int dark = 0;
            int edge = 0;
            for (int y = 0; y < p.height(); ++y) {
                for (int x = 0; x < p.width(); ++x) {
                    const Color c = p.get_pixel(x, y);
                    const int l = (c.m_r + c.m_g + c.m_b) / 3;
                    if (l < 32) {
                        ++dark;
                    } else if (l > 16 && l < 239) {
                        ++edge;
                    }
                }
            }
            AURORA_TEST_CHECK(dark > 0); // 渲染出文本
            AURORA_TEST_CHECK(edge > 0); // 存在抗锯齿过渡
        }
        FontEngine::set_text_aa_mode(prev);
        AURORA_LOG_INFO("test", "[9] supersample/cleartype render + AA edges OK");
    }

    // 10) 半透明文本在 ClearType 下回退超采样且不崩溃、仍渲染
    {
        const auto prev = FontEngine::text_aa_mode();
        FontEngine::set_text_aa_mode(TextAAMode::ClearType);
        Painter p;
        p.begin(200, 50);
        p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 200, .height = 50 } },
                    Color{ 255, 255, 255, 255 });
        const Font f{ .family = "sans-serif", .size_pt = 14.0f, .weight = 400 };
        FontEngine::instance().draw_text(
            p, Rect{ .origin = Point{ .x = 10, .y = 10 }, .size = Size{ .width = 180, .height = 30 } }, "中文 AA", f,
            Color{ 0, 0, 0, 128 });
        int changed = 0;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if ((c.m_r + c.m_g + c.m_b) / 3 < 200) {
                    ++changed; // 半透明黑字使白底变灰
                }
            }
        }
        AURORA_TEST_CHECK(changed > 0);
        FontEngine::set_text_aa_mode(prev);
        AURORA_LOG_INFO("test", "[10] semitransparent fallback OK");
    }

    AURORA_LOG_INFO("test", "ALL FONT ENGINE TESTS PASSED");
}
