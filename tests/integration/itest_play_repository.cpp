/// 测试类型: integration
/// 目标单元: examples/app/google_play/google_play_data.h
/// 测试说明: Google Play 数据层（HOOK 驱动、不联网）——默认本地目录规模
///           （4 类目 × 48 项 = 192）、featured / list_by_category /
///           list_by_subcategory / search（大小写不敏感跨字段）/ detail / reviews /
///           screenshots 数据契约，以及可替换 DataHook 数据源注入

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "google_play_data.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_play_repository {

namespace {

auto count_all(const gp::PlayRepository &repo) -> int {
    static constexpr std::array<const char *, 4> kCategories = {"apps", "games", "movies", "books"};
    int n = 0;
    for (const auto *c : kCategories) {
        n += static_cast<int>(repo.list_by_category(c).size());
    }
    return n;
}

}  // namespace

AURORA_TEST_CASE(default_catalog_scale_and_category_filters) {
    const gp::PlayRepository repo;

    // 默认 hook 合成 4 大类目 × 48 项 = 192 项。
    AURORA_TEST_CHECK_EQ(count_all(repo), 192);

    // featured 非空。
    const auto feat = repo.featured();
    AURORA_TEST_CHECK(!feat.empty());

    // 按类目过滤正确：apps 恒 48 项且类目字段全匹配。
    const auto apps = repo.list_by_category("apps");
    AURORA_TEST_CHECK(!apps.empty());
    AURORA_TEST_CHECK_EQ(apps.size(), static_cast<std::size_t>(48));
    bool all_apps = true;
    for (const auto &a : apps) {
        if (a.category != "apps") {
            all_apps = false;
        }
    }
    AURORA_TEST_CHECK(all_apps);

    // 子类目过滤：games 的子类目非空，过滤结果全部属于该子类目。
    const auto games = repo.list_by_category("games");
    AURORA_TEST_CHECK(!games.empty());
    const auto subs = gp::subcategories_of("games");
    AURORA_TEST_CHECK(!subs.empty());
    const auto sub_filtered = repo.list_by_subcategory("games", subs.front());
    AURORA_TEST_CHECK(!sub_filtered.empty());
    bool ok_sub = true;
    for (const auto &g : sub_filtered) {
        if (g.subcategory != subs.front()) {
            ok_sub = false;
        }
    }
    AURORA_TEST_CHECK(ok_sub);
}

AURORA_TEST_CASE(search_is_case_insensitive_and_cross_field) {
    const gp::PlayRepository repo;

    // 合成名称含 "Chat" 等，小写查询必有命中（大小写不敏感）。
    const auto hits = repo.search("chat");
    AURORA_TEST_CHECK(!hits.empty());

    // 跨字段：开发者 "Aurora Labs" 也匹配。
    const auto hits_dev = repo.search("aurora");
    AURORA_TEST_CHECK(!hits_dev.empty());

    // 无关查询返回空。
    const auto none = repo.search("zzz_no_such_app_zzz");
    AURORA_TEST_CHECK(none.empty());
}

AURORA_TEST_CASE(detail_reviews_and_screenshots_contract) {
    const gp::PlayRepository repo;
    const auto apps = repo.list_by_category("apps");
    AURORA_TEST_REQUIRE(!apps.empty());
    const std::string id = apps.front().id;

    // detail：返回与 id 匹配且图标非空的项。
    const auto d = repo.detail(id);
    AURORA_TEST_CHECK(d.id == id);
    AURORA_TEST_CHECK(d.icon.width > 0 && d.icon.height > 0);
    AURORA_TEST_CHECK(!d.icon.pixels.empty());

    // 评价：每个 app 至少 3 条。
    const auto rev = repo.reviews(id);
    AURORA_TEST_CHECK(rev.size() >= static_cast<std::size_t>(3));

    // 截图：默认 4 张，均为非空 RGBA 图像（像素数 = w*h*4）。
    const auto shots = repo.screenshots_for(id);
    AURORA_TEST_CHECK_EQ(shots.size(), static_cast<std::size_t>(4));
    for (const auto &s : shots) {
        AURORA_TEST_CHECK(s.width > 0 && s.height > 0);
        AURORA_TEST_CHECK_EQ(s.pixels.size(),
                             static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) * 4U);
    }
}

AURORA_TEST_CASE(data_hook_replaces_catalog_source) {
    gp::PlayRepository repo;

    // 可替换 DataHook：返回一个被 hook 截断的目录，验证数据源可替换（不联网）。
    bool called = false;
    repo.set_data_hook([&called](const gp::DataRequest &) -> gp::CatalogPtr {
        called = true;
        auto v = std::make_shared<std::vector<gp::AppItem>>();
        gp::AppItem a;
        a.id = "hooked";
        a.name = "Hooked";
        a.category = "apps";
        a.is_app = true;
        v->push_back(a);
        return v;
    });

    const auto hooked = repo.list_by_category("apps");
    AURORA_TEST_CHECK(called);
    AURORA_TEST_CHECK_EQ(hooked.size(), static_cast<std::size_t>(1));
    AURORA_TEST_CHECK(!hooked.empty() && hooked.front().id == "hooked");
}

}  // namespace aurora::test_cases::itest_play_repository
