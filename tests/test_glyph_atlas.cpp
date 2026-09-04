// GlyphAtlas 单元测试：验证 LRU 命中/淘汰、模式隔离与覆盖度数据保真。
#include <cstdint>
#include <iostream>
#include <vector>

#include "aurora/render/glyph_atlas.h"
#include "test_harness.h"

using aurora::render::GlyphAtlas;

AURORA_TEST() {
    GlyphAtlas atlas;

    // 1) 插入后命中且覆盖度数据保真
    {
        GlyphAtlas::Entry e;
        e.mode = GlyphAtlas::Mode::Gray;
        e.left = -1;
        e.top = 8;
        e.width = 4;
        e.rows = 2;
        e.advance = 5.0F;
        e.buf = {0, 128, 255, 64, 16, 32, 96, 200};
        atlas.insert(1, std::move(e));
        const GlyphAtlas::Entry *got = atlas.find(1);
        AURORA_TEST_CHECK(got != nullptr);
        AURORA_TEST_CHECK(got->width == 4 && got->rows == 2);
        AURORA_TEST_CHECK(got->left == -1 && got->top == 8);
        AURORA_TEST_CHECK(got->advance == 5.0F);
        AURORA_TEST_CHECK(got->buf.size() == 8U);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(got->buf[2] == 255);
        AURORA_LOG_INFO("test", "[1] insert/find/roundtrip OK");
    }

    // 2) 同 base key 的 Gray / Lcd 互不覆盖
    {
        GlyphAtlas::Entry g;
        g.mode = GlyphAtlas::Mode::Gray;
        g.width = 2;
        g.rows = 1;
        g.buf = {10, 20};
        atlas.insert(100, std::move(g));
        GlyphAtlas::Entry l;
        l.mode = GlyphAtlas::Mode::Lcd;
        l.width = 2;
        l.rows = 1;
        l.buf = {30, 40, 50, 60, 70, 80};
        atlas.insert(101, std::move(l));
        const GlyphAtlas::Entry *gg = atlas.find(100);
        const GlyphAtlas::Entry *ll = atlas.find(101);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(gg != nullptr && gg->mode == GlyphAtlas::Mode::Gray && gg->buf[1] == 20);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ll != nullptr && ll->mode == GlyphAtlas::Mode::Lcd && ll->buf[5] == 80);
        AURORA_LOG_INFO("test", "[2] gray/lcd separation OK");
    }

    // 3) 超出容量后最久未用被淘汰
    {
        GlyphAtlas small;
        constexpr std::size_t n = GlyphAtlas::AURORA_MAX_ENTRIES + 200;
        for (std::size_t i = 0; i < n; ++i) {
            GlyphAtlas::Entry e;
            e.width = 1;
            e.rows = 1;
            e.buf = {static_cast<std::uint8_t>(i & 0xFFU)};
            small.insert(i, std::move(e));
        }
        // 最早插入的 key=0 应已被淘汰
        AURORA_TEST_CHECK(small.find(0) == nullptr);
        // 最近插入的仍存在
        AURORA_TEST_CHECK(small.find(n - 1) != nullptr);
        // 容量受控（条目数不超过 kMaxEntries 太多）
        std::size_t live = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (small.find(i) != nullptr) {
                ++live;
            }
        }
        AURORA_TEST_CHECK(live <= GlyphAtlas::AURORA_MAX_ENTRIES);
        AURORA_LOG_INFO("test", "[3] LRU eviction OK (live=", live, ")");
    }

    // 4) clear 清空全部
    {
        atlas.clear();
        AURORA_TEST_CHECK(atlas.find(1) == nullptr);
        AURORA_TEST_CHECK(atlas.find(100) == nullptr);
        AURORA_LOG_INFO("test", "[4] clear OK");
    }
}