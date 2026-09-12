/// 测试类型: unit
/// 目标单元: include/aurora/render/bidi.h (+ src/aurora/render/font_engine.cpp 的跨 run 重排接入)
/// 测试说明: 覆盖 A2 混排 UBA 切片的段落级 run 视觉重排：
///   - 纯函数：guess_paragraph_direction（首个强方向字符定基准，UBA P2/P3）与
///     bidi_visual_run_order（RTL 段落整体右→左翻转 run 顺序，LTR 恒等）。
///   - 集成：注册真实双字体 Roboto(拉丁)+Amiri(阿拉伯) 到同一 family（find_glyph 跨面回退，
///     自然形成两个 face-run），绘制「ab مرحبا」RTL 段落，按墨迹分布桶比对验证最左 run 为阿拉伯文
///     （即跨 run 重排生效；未重排时最左应为拉丁 "ab"，分布必不同）。

#include <filesystem>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/bidi.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_bidi {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

[[nodiscard]] auto count_opaque(const Painter &p) -> int {
    int count = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            if (p.get_pixel(x, y).a > 0) {
                ++count;
            }
        }
    }
    return count;
}

// 某列是否存在「墨迹」（任意 alpha 超过阈值即算，避免抗锯齿强度差异造成的逐像素抖动）。
[[nodiscard]] auto column_has_ink(const Painter &p, int x) -> bool {
    for (int y = 0; y < p.height(); ++y) {
        if (p.get_pixel(x, y).a > 32) {
            return true;
        }
    }
    return false;
}
}  // namespace

// ---------------- 纯函数：guess_paragraph_direction ----------------
AURORA_TEST_CASE(guess_paragraph_direction_arabic_is_rtl) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("مرحبا") == aurora::TextDirection::RTL);
}

AURORA_TEST_CASE(guess_paragraph_direction_hebrew_is_rtl) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("שלום") == aurora::TextDirection::RTL);
}

AURORA_TEST_CASE(guess_paragraph_direction_latin_is_ltr) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("Hello") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_digits_fallback_ltr) {
    // 全数字/中性：无强方向字符，回退 LTR。
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("12345") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_empty_is_ltr) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_first_strong_wins) {
    // 首个强方向字符定基准：以拉丁开头 → LTR（即便后接阿拉伯文）。
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("Hi مرحبا") == aurora::TextDirection::LTR);
}

// ---------------- 纯函数：bidi_visual_run_order ----------------
AURORA_TEST_CASE(run_order_ltr_keeps_logical_order) {
    const std::vector<std::size_t> expect{0, 1, 2};
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(3, aurora::TextDirection::LTR) == expect);
}

AURORA_TEST_CASE(run_order_rtl_reverses_runs) {
    const std::vector<std::size_t> expect{2, 1, 0};
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(3, aurora::TextDirection::RTL) == expect);
}

AURORA_TEST_CASE(run_order_single_run_is_identity) {
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(1, aurora::TextDirection::RTL) ==
                     std::vector<std::size_t>{0});
}

AURORA_TEST_CASE(run_order_zero_runs_is_empty) {
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(0, aurora::TextDirection::RTL).empty());
}

// ---------------- 集成：真实双字体跨 run 重排 ----------------
AURORA_TEST_CASE(mixed_rtl_reorders_runs_by_paragraph_direction) {
    const auto root = aurora::testing::isolation::repo_root();
    if (root.empty()) {
        AURORA_TEST_SKIP("repo_root 不可用，跳过真实字体集成测试");
    }
    const std::filesystem::path roboto =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf";
    const std::filesystem::path amiri =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Amiri-Regular.ttf";
    if (!std::filesystem::exists(roboto) || !std::filesystem::exists(amiri)) {
        AURORA_TEST_SKIP("Roboto/Amiri TTF 不在仓库内，跳过真实字体集成测试");
    }

    // 同一 family 注册两种面：find_glyph 缺字时跨面回退 → 自然形成两个 face-run。
    aurora::render::FontEngine::register_font("__bidi_mix", roboto.string());
    aurora::render::FontEngine::register_font("__bidi_mix", amiri.string());

    const aurora::Font font{.family = "__bidi_mix", .size_pt = 32.0F};
    const aurora::render::TextLayoutOpts rtl{.direction = aurora::TextDirection::RTL};

    // 混合串：拉丁 "ab" + 空格 + 阿拉伯 "مرحبا"，显式 RTL 段落。
    const std::string mixed = "ab مرحبا";
    Painter p;
    p.begin(400, 80);
    aurora::render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 400.0F, 80.0F), mixed, font, aurora::Color::black(), rtl);
    AURORA_TEST_REQUIRE(count_opaque(p) > 0);  // 两词都落笔

    // 阿拉伯词单独绘制（同 family/direction/size），作为「最左 run 应为阿拉伯文」的参照。
    const std::string ar = "مرحبا";
    const float w_ar = aurora::render::FontEngine::measure_width(ar, font, rtl);
    AURORA_TEST_REQUIRE(w_ar > 0.0F);

    Painter q;
    q.begin(400, 80);
    aurora::render::FontEngine::draw_text(q, rect_at(0.0F, 0.0F, 400.0F, 80.0F), ar, font, aurora::Color::black(), rtl);

    // 在最左 w_ar 像素区间内按桶比对墨迹分布：正确重排下 Arabic 是最左 run，
    // 其墨迹分布应与单独绘制的阿拉伯词逐桶一致；若未重排（最左为拉丁 "ab"），分布必不同。
    const int buckets = 16;
    const int span = static_cast<int>(std::ceil(w_ar));
    AURORA_TEST_REQUIRE(span > 0 && span < 400);
    bool all_match = true;
    for (int b = 0; b < buckets; ++b) {
        const int x0 = span * b / buckets;
        const int x1 = span * (b + 1) / buckets;
        bool in_p = false;
        bool in_q = false;
        for (int x = x0; x < x1; ++x) {
            if (column_has_ink(p, x)) {
                in_p = true;
            }
            if (column_has_ink(q, x)) {
                in_q = true;
            }
        }
        if (in_p != in_q) {
            all_match = false;
            break;
        }
    }
    AURORA_TEST_CHECK(all_match);
}

}  // namespace aurora::test_cases::utest_bidi
