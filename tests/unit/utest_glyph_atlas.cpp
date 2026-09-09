/// 测试类型: unit
/// 目标单元: include/aurora/render/glyph_atlas.h
/// 测试说明: 覆盖 GlyphAtlas 的未命中返回空指针、插入后字段保真（含 Gray/Lcd 两种缓冲语义）、
/// 同键覆盖、clear 全清，以及容量上限触发的 LRU 淘汰与访问提升对淘汰顺序的影响

#include <cstddef>
#include <cstdint>
#include <vector>

#include "aurora/render/glyph_atlas.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glyph_atlas {

namespace {
[[nodiscard]] auto make_entry(render::GlyphAtlas::Mode mode, int width, int rows) -> render::GlyphAtlas::Entry {
    render::GlyphAtlas::Entry entry;
    entry.mode = mode;
    entry.left = 1;
    entry.top = 7;
    entry.width = width;
    entry.rows = rows;
    entry.pitch = width;
    entry.advance = 5.5F;
    const std::size_t factor = (mode == render::GlyphAtlas::Mode::Lcd) ? 3U : 1U;
    entry.buf.assign(static_cast<std::size_t>(width) * rows * factor, 0xAB);
    return entry;
}
}  // namespace

AURORA_TEST_CASE(miss_returns_nullptr) {
    render::GlyphAtlas atlas;
    AURORA_TEST_CHECK_NULL(atlas.find(0x0123'4567'89AB'CDEFULL));
}

AURORA_TEST_CASE(insert_then_find_preserves_gray_entry) {
    render::GlyphAtlas atlas;
    atlas.insert(42, make_entry(render::GlyphAtlas::Mode::Gray, 4, 6));

    const auto *found = atlas.find(42);
    AURORA_TEST_REQUIRE_NOT_NULL(found);
    AURORA_TEST_CHECK_EQ(static_cast<int>(found->mode), static_cast<int>(render::GlyphAtlas::Mode::Gray));
    AURORA_TEST_CHECK_EQ(found->left, 1);
    AURORA_TEST_CHECK_EQ(found->top, 7);
    AURORA_TEST_CHECK_EQ(found->width, 4);
    AURORA_TEST_CHECK_EQ(found->rows, 6);
    AURORA_TEST_CHECK_NEAR(found->advance, 5.5, 1e-6);
    AURORA_TEST_CHECK_EQ(found->buf.size(), 4U * 6U);
    AURORA_TEST_CHECK_EQ(static_cast<int>(found->buf.front()), 0xAB);
}

AURORA_TEST_CASE(insert_then_find_preserves_lcd_entry) {
    // LCD 子像素缓冲每行 3 字节/列：Buf 长度 = 3 * width * rows。
    render::GlyphAtlas atlas;
    atlas.insert(7, make_entry(render::GlyphAtlas::Mode::Lcd, 3, 5));

    const auto *found = atlas.find(7);
    AURORA_TEST_REQUIRE_NOT_NULL(found);
    AURORA_TEST_CHECK_EQ(static_cast<int>(found->mode), static_cast<int>(render::GlyphAtlas::Mode::Lcd));
    AURORA_TEST_CHECK_EQ(found->buf.size(), 3U * 3U * 5U);
}

AURORA_TEST_CASE(insert_overwrites_same_key) {
    render::GlyphAtlas atlas;
    atlas.insert(1, make_entry(render::GlyphAtlas::Mode::Gray, 2, 2));
    atlas.insert(1, make_entry(render::GlyphAtlas::Mode::Gray, 8, 8));

    const auto *found = atlas.find(1);
    AURORA_TEST_REQUIRE_NOT_NULL(found);
    AURORA_TEST_CHECK_EQ(found->width, 8);
    AURORA_TEST_CHECK_EQ(found->rows, 8);
}

AURORA_TEST_CASE(clear_drops_all_entries) {
    render::GlyphAtlas atlas;
    atlas.insert(1, make_entry(render::GlyphAtlas::Mode::Gray, 2, 2));
    atlas.insert(2, make_entry(render::GlyphAtlas::Mode::Gray, 2, 2));
    atlas.clear();

    AURORA_TEST_CHECK_NULL(atlas.find(1));
    AURORA_TEST_CHECK_NULL(atlas.find(2));
}

AURORA_TEST_CASE(exceeding_capacity_evicts_least_recently_used) {
    render::GlyphAtlas atlas;
    const std::uint64_t capacity = render::GlyphAtlas::AURORA_MAX_ENTRIES;
    for (std::uint64_t key = 0; key < capacity; ++key) {
        atlas.insert(key, make_entry(render::GlyphAtlas::Mode::Gray, 1, 1));
    }
    // 注意：`find` 本身会 LRU 提升，故「填满」阶段只能访问最新键（已在队首），
    // 否则断言会顺带改变淘汰顺序，把 key 0 救下。
    AURORA_TEST_CHECK_NOT_NULL(atlas.find(capacity - 1));  // 恰好填满：尚无淘汰

    atlas.insert(capacity, make_entry(render::GlyphAtlas::Mode::Gray, 1, 1));
    AURORA_TEST_CHECK_NULL(atlas.find(0));  // 最久未用者被淘汰
    AURORA_TEST_CHECK_NOT_NULL(atlas.find(capacity));
}

AURORA_TEST_CASE(find_promotes_entry_and_shifts_eviction_order) {
    render::GlyphAtlas atlas;
    const std::uint64_t capacity = render::GlyphAtlas::AURORA_MAX_ENTRIES;
    for (std::uint64_t key = 0; key < capacity; ++key) {
        atlas.insert(key, make_entry(render::GlyphAtlas::Mode::Gray, 1, 1));
    }

    AURORA_TEST_CHECK_NOT_NULL(atlas.find(0));  // 提升 key 0 → key 1 成为最久未用
    atlas.insert(capacity, make_entry(render::GlyphAtlas::Mode::Gray, 1, 1));

    AURORA_TEST_CHECK_NOT_NULL(atlas.find(0));
    AURORA_TEST_CHECK_NULL(atlas.find(1));
}

}  // namespace aurora::test_cases::utest_glyph_atlas
