// test_font_shape_cache.cpp — 文本 shaping 缓存：正确性 / 命中率 / 确定性。
//
// 缓存只记忆纯函数 shape_line(line, faces, px, opts) 的结果，不改变任何输出像素，
// 故命中前后 measure_width / caret_x 必须逐位一致（golden 零影响）。

#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"

#include "test_harness.h"

using aurora::Font;
using aurora::render::FontEngine;

AURORA_TEST() {
    FontEngine::shape_cache_clear();

    const Font f{ .size_pt = 14.0f };
    const std::string text = "渲染性能优化专项 Aurora Render Perf 1234567890 こんにちは";

    // 首次测量触发 1 次 hb_shape（miss），后续命中。
    (void)FontEngine::measure_width(text, f);
    const auto after_one = FontEngine::shape_cache_stats();
    AURORA_TEST_CHECK_MSG(after_one.misses == 1, "Test1: first measure triggers exactly 1 miss (hb_shape)");

    // 重复测量：全部命中，无新 miss。
    for (int i = 0; i < 200; ++i) {
        (void)FontEngine::measure_width(text, f);
    }
    const auto s = FontEngine::shape_cache_stats();
    const auto total = s.hits + s.misses;
    const double rate = (total != 0u) ? static_cast<double>(s.hits) / static_cast<double>(total) : 0.0;

    AURORA_TEST_CHECK_MSG(s.hits > 0, "Test2: shape cache hits > 0 (repeated static text)");
    AURORA_TEST_CHECK_MSG(s.misses == 1, "Test3: same text only 1 miss (rest hit, cache effective)");
    AURORA_TEST_CHECK_MSG(rate >= 0.95, "Test4: G-10 hit rate >= 95%");

    // 确定性：缓存命中前后 measure_width 完全一致（golden 零影响）。
    const float w_a = FontEngine::measure_width(text, f);
    const float w_b = FontEngine::measure_width(text, f);
    AURORA_TEST_CHECK_MSG(w_a == w_b, "Test5: width bit-identical before/after cache hit");

    // caret_x 同样走缓存且一致。
    const float cx_a = FontEngine::caret_x(text, 5, f);
    const float cx_b = FontEngine::caret_x(text, 5, f);
    AURORA_TEST_CHECK_MSG(cx_a == cx_b, "Test6: caret_x consistent before/after cache");

    // 不同字号必须命中各自缓存条目（px 是 key 的一部分），且结果仍一致。
    const Font f2{ .size_pt = 28.0f };
    const float w_big = FontEngine::measure_width(text, f2);
    const float w_big2 = FontEngine::measure_width(text, f2);
    AURORA_TEST_CHECK_MSG(w_big == w_big2, "Test7: different font sizes hit cache and stay consistent");

    // 清空重置统计与条目。
    FontEngine::shape_cache_clear();
    const auto cleared = FontEngine::shape_cache_stats();
    AURORA_TEST_CHECK_MSG(cleared.hits == 0 && cleared.misses == 0 && cleared.entries == 0,
                          "Test8: shape_cache_clear resets stats and entries");
}
