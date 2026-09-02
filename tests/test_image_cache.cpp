// 验证 ImageCache：LRU 语义、内存上限淘汰、put/get/remove/clear、命中计数。
#include <cstdio>

#include "aurora/render/image_cache.h"

#include "test_harness.h"

using aurora::Image;
using aurora::ImageCache;

namespace {

/// 构造指定像素尺寸的纯色测试图（bytes = w*h*4）。
auto make_image(int w, int h) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0xFF);
    return img;
}

} // namespace

AURORA_TEST() {
    // ---- 1. put/contains/count ----
    {
        ImageCache cache;
        AURORA_TEST_CHECK(cache.count() == 0);

        cache.put("a.png", make_image(10, 10)); // 400B
        AURORA_TEST_CHECK(cache.contains("a.png"));
        AURORA_TEST_CHECK(cache.count() == 1);
        AURORA_TEST_CHECK(cache.current_bytes() == 400);
    }

    // ---- 2. get 命中返回缓存 + 命中计数 ----
    {
        ImageCache cache;
        cache.put("b.png", make_image(5, 5));
        AURORA_TEST_CHECK(cache.hit_count() == 0);

        auto r = cache.get("b.png");
        AURORA_TEST_CHECK(r.ok());
        AURORA_TEST_CHECK(r.value().width == 5);
        AURORA_TEST_CHECK(cache.hit_count() == 1);
    }

    // ---- 3. get 未命中且文件不存在返回错误（不缓存失败）----
    {
        ImageCache cache;
        auto r = cache.get("__no_such_file__.png");
        AURORA_TEST_CHECK(!r.ok());
        AURORA_TEST_CHECK(!cache.contains("__no_such_file__.png"));
    }

    // ---- 4. 覆盖同 key 不重复计数 ----
    {
        ImageCache cache;
        cache.put("c.png", make_image(10, 10)); // 400B
        cache.put("c.png", make_image(20, 20)); // 1600B 覆盖
        AURORA_TEST_CHECK(cache.count() == 1);
        AURORA_TEST_CHECK(cache.current_bytes() == 1600);
    }

    // ---- 5. LRU 淘汰：超限时最久未用先出 ----
    {
        ImageCache cache;
        cache.set_max_bytes(1000);           // 上限 1000B
        cache.put("x1", make_image(10, 10)); // 400B
        cache.put("x2", make_image(10, 10)); // 400B（共 800）
        // 访问 x1 提升为最近使用
        (void)cache.get("x1");
        // 放入 x3 超限：淘汰最久未用的 x2
        cache.put("x3", make_image(10, 10)); // 400B（共 1200 > 1000）
        AURORA_TEST_CHECK(cache.contains("x1"));
        AURORA_TEST_CHECK(!cache.contains("x2"));
        AURORA_TEST_CHECK(cache.contains("x3"));
        AURORA_TEST_CHECK(cache.current_bytes() == 800);
    }

    // ---- 6. 单图超过总上限不缓存 ----
    {
        ImageCache cache;
        cache.set_max_bytes(100);
        cache.put("big", make_image(50, 50)); // 10000B > 100
        AURORA_TEST_CHECK(!cache.contains("big"));
        AURORA_TEST_CHECK(cache.current_bytes() == 0);
    }

    // ---- 7. set_max_bytes 立即淘汰 ----
    {
        ImageCache cache;
        cache.put("y1", make_image(10, 10));
        cache.put("y2", make_image(10, 10));
        AURORA_TEST_CHECK(cache.count() == 2);

        cache.set_max_bytes(500); // 只容得下 1 张
        AURORA_TEST_CHECK(cache.count() == 1);
        AURORA_TEST_CHECK(!cache.contains("y1")); // 最久未用先出
        AURORA_TEST_CHECK(cache.contains("y2"));
    }

    // ---- 8. remove / clear ----
    {
        ImageCache cache;
        cache.put("z1", make_image(4, 4));
        cache.put("z2", make_image(4, 4));

        cache.remove("z1");
        AURORA_TEST_CHECK(!cache.contains("z1"));
        AURORA_TEST_CHECK(cache.count() == 1);

        cache.clear();
        AURORA_TEST_CHECK(cache.count() == 0);
        AURORA_TEST_CHECK(cache.current_bytes() == 0);
    }

    // ---- 9. 单例可用 ----
    {
        auto &c1 = ImageCache::instance();
        auto &c2 = ImageCache::instance();
        AURORA_TEST_CHECK(&c1 == &c2);
    }
}
