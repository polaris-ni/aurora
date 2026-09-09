/// 测试类型: unit
/// 目标单元: include/aurora/render/image_cache.h
/// 测试说明: 覆盖 ImageCache 单例的命中/未命中路径、失败结果不入缓存、条目移除与清空、字节账目、
/// LRU 提升顺序与超限淘汰，以及单图超过总上限时不入缓存的降级

#include <cstddef>
#include <string>

#include "aurora/core/image.h"
#include "aurora/render/image_cache.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image_cache {

namespace {
/// @brief 构造 w×h 全零 RGBA8 图（内容由尺寸决定，便于区分条目）。
[[nodiscard]] auto make_image(int w, int h) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return img;
}
}  // namespace

/// @brief ImageCache 是进程级单例：每条用例前后必须清空并还原上限。
class CacheGuard : public ::aurora::testing::Fixture {
  protected:
    auto SetUp() -> void override {
        saved_max_ = ImageCache::instance().max_bytes();
        ImageCache::instance().clear();
    }
    auto TearDown() -> void override {
        ImageCache::instance().clear();
        ImageCache::instance().set_max_bytes(saved_max_);
    }

  private:
    std::size_t saved_max_ = 0;
};

AURORA_TEST_F(CacheGuard, put_then_get_hits_cache) {
    auto &cache = ImageCache::instance();
    cache.put("a.png", make_image(2, 2));

    AURORA_TEST_CHECK_TRUE(cache.contains("a.png"));
    AURORA_TEST_CHECK_EQ(cache.count(), 1U);

    const auto first = cache.get("a.png");
    AURORA_TEST_REQUIRE_TRUE(first.ok());
    AURORA_TEST_CHECK_EQ(first.value().width, 2);
    AURORA_TEST_CHECK_EQ(static_cast<std::size_t>(first.value().pixels.size()), 2U * 2U * 4U);

    const std::size_t hits_before = cache.hit_count();
    AURORA_TEST_CHECK_TRUE(cache.get("a.png").ok());
    AURORA_TEST_CHECK_GT(cache.hit_count(), hits_before);
}

AURORA_TEST_F(CacheGuard, missing_path_returns_error_and_is_not_cached) {
    auto &cache = ImageCache::instance();
    const auto loaded = cache.get("definitely-missing-file.png");

    AURORA_TEST_CHECK_FALSE(loaded.ok());
    AURORA_TEST_CHECK_FALSE(cache.contains("definitely-missing-file.png"));
    AURORA_TEST_CHECK_EQ(cache.count(), 0U);
}

AURORA_TEST_F(CacheGuard, put_overwrites_same_key) {
    auto &cache = ImageCache::instance();
    cache.put("a.png", make_image(2, 2));
    cache.put("a.png", make_image(4, 4));

    AURORA_TEST_CHECK_EQ(cache.count(), 1U);
    AURORA_TEST_CHECK_EQ(cache.get("a.png").value().width, 4);
    // 覆盖时旧条目字节数须被扣回，不重复累计。
    AURORA_TEST_CHECK_EQ(cache.current_bytes(), 4U * 4U * 4U);
}

AURORA_TEST_F(CacheGuard, remove_and_clear_drop_entries) {
    auto &cache = ImageCache::instance();
    cache.put("a.png", make_image(2, 2));
    cache.put("b.png", make_image(3, 3));

    cache.remove("a.png");
    AURORA_TEST_CHECK_FALSE(cache.contains("a.png"));
    AURORA_TEST_CHECK_TRUE(cache.contains("b.png"));

    cache.clear();
    AURORA_TEST_CHECK_EQ(cache.count(), 0U);
    AURORA_TEST_CHECK_EQ(cache.current_bytes(), 0U);
}

AURORA_TEST_F(CacheGuard, current_bytes_tracks_payload) {
    auto &cache = ImageCache::instance();
    cache.put("a.png", make_image(2, 2));  // 16 B
    cache.put("b.png", make_image(4, 4));  // 64 B

    AURORA_TEST_CHECK_EQ(cache.current_bytes(), 16U + 64U);
    cache.remove("a.png");
    AURORA_TEST_CHECK_EQ(cache.current_bytes(), 64U);
}

AURORA_TEST_F(CacheGuard, evicts_least_recently_used_when_over_limit) {
    auto &cache = ImageCache::instance();
    cache.set_max_bytes(100);

    cache.put("a.png", make_image(4, 4));  // 64 B
    cache.put("b.png", make_image(4, 4));  // 64 B → 128 > 100，淘汰最久未用的 a
    AURORA_TEST_CHECK_FALSE(cache.contains("a.png"));
    AURORA_TEST_CHECK_TRUE(cache.contains("b.png"));
    AURORA_TEST_CHECK_LE(cache.current_bytes(), 100U);
}

AURORA_TEST_F(CacheGuard, get_promotes_entry_and_shifts_eviction_order) {
    auto &cache = ImageCache::instance();
    // 预算 160 B 容纳两张 64 B 图，使「淘汰顺序」而非「立即超限」成为被测行为。
    cache.set_max_bytes(160);

    cache.put("a.png", make_image(4, 4));  // 64 B
    cache.put("b.png", make_image(4, 4));  // 64 B
    AURORA_TEST_REQUIRE_TRUE(cache.contains("a.png"));

    const auto reread = cache.get("a.png");  // 提升 a → b 变为最久未用
    AURORA_TEST_REQUIRE_TRUE(reread.ok());     // 提升不改变条目内容
    AURORA_TEST_CHECK_EQ(reread.value().width, 4);
    cache.put("c.png", make_image(4, 4));  // 192 > 160 → 淘汰 b

    AURORA_TEST_CHECK_TRUE(cache.contains("a.png"));
    AURORA_TEST_CHECK_FALSE(cache.contains("b.png"));
    AURORA_TEST_CHECK_TRUE(cache.contains("c.png"));
}

AURORA_TEST_F(CacheGuard, oversized_single_image_is_not_cached) {
    auto &cache = ImageCache::instance();
    cache.set_max_bytes(10);
    cache.put("big.png", make_image(8, 8));  // 256 B > 10 B

    // 单图超限：直接放弃缓存（由调用方持有），不触发死循环淘汰。
    AURORA_TEST_CHECK_FALSE(cache.contains("big.png"));
    AURORA_TEST_CHECK_EQ(cache.count(), 0U);
    AURORA_TEST_CHECK_EQ(cache.current_bytes(), 0U);
}

AURORA_TEST_F(CacheGuard, lowering_limit_triggers_immediate_eviction) {
    auto &cache = ImageCache::instance();
    cache.put("a.png", make_image(4, 4));  // 64 B
    cache.put("b.png", make_image(4, 4));  // 64 B
    AURORA_TEST_REQUIRE_EQ(cache.count(), 2U);

    cache.set_max_bytes(64);  // 立即按新上限淘汰
    AURORA_TEST_CHECK_LE(cache.current_bytes(), 64U);
    AURORA_TEST_CHECK_EQ(cache.count(), 1U);
}

}  // namespace aurora::test_cases::utest_image_cache
