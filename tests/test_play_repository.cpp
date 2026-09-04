// test_play_repository.cpp — 校验 Google Play 数据层（HOOK 驱动、不联网）。
// 覆盖：默认本地目录规模、featured/list_by_category/list_by_subcategory/search/detail/reviews
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <cstdint>

#include "aurora/aurora.h"
#include "google_play_data.h"
#include "test_harness.h"

namespace au = aurora;

static auto count_all(const gp::PlayRepository &repo) -> int {
    int n = 0;
    const std::string cats[] = {"apps", "games", "movies", "books"};
    for (const auto &c : cats) {
        n += static_cast<int>(repo.list_by_category(c).size());
    }
    return n;
}

AURORA_TEST() {
    gp::PlayRepository repo;

    // 默认 hook 合成 4 大类目 x 48 项 = 192 项。
    AURORA_TEST_CHECK(count_all(repo) == 192);

    // featured 非空。
    auto feat = repo.featured();
    AURORA_TEST_CHECK(!feat.empty());

    // 按类目过滤正确。
    auto apps = repo.list_by_category("apps");
    AURORA_TEST_CHECK(!apps.empty());
    AURORA_TEST_CHECK(apps.size() == 48);
    bool all_apps = true;
    for (const auto &a : apps) {
        if (a.category != "apps") {
            all_apps = false;
        }
    }
    AURORA_TEST_CHECK(all_apps);

    // 子类目过滤。
    auto games = repo.list_by_category("games");
    AURORA_TEST_CHECK(!games.empty());
    auto subs = gp::subcategories_of("games");
    AURORA_TEST_CHECK(!subs.empty());
    auto sub_filtered = repo.list_by_subcategory("games", subs.front());
    AURORA_TEST_CHECK(!sub_filtered.empty());
    bool ok_sub = true;
    for (const auto &g : sub_filtered) {
        if (g.subcategory != subs.front()) {
            ok_sub = false;
        }
    }
    AURORA_TEST_CHECK(ok_sub);

    // 搜索：不区分大小写、跨字段匹配。合成名称含 "Chat" 等，必有命中。
    auto hits = repo.search("chat");
    AURORA_TEST_CHECK(!hits.empty());
    // 跨字段：开发者 "Aurora Labs" 也匹配。
    auto hits_dev = repo.search("aurora");
    AURORA_TEST_CHECK(!hits_dev.empty());
    auto none = repo.search("zzz_no_such_app_zzz");
    AURORA_TEST_CHECK(none.empty());

    // detail：返回与 id 匹配且图片非空的项。
    const std::string id = apps.front().id;
    auto d = repo.detail(id);
    AURORA_TEST_CHECK(d.id == id);
    AURORA_TEST_CHECK(d.icon.width > 0 && d.icon.height > 0);
    AURORA_TEST_CHECK(!d.icon.pixels.empty());

    // 评价：每个 app 至少 3 条。
    auto rev = repo.reviews(id);
    AURORA_TEST_CHECK(rev.size() >= 3);

    // 截图：默认 4 张，均为非空 RGBA 图像。
    auto shots = repo.screenshots_for(id);
    AURORA_TEST_CHECK(shots.size() == 4);
    for (const auto &s : shots) {
        AURORA_TEST_CHECK(s.width > 0 && s.height > 0);
        AURORA_TEST_CHECK(s.pixels.size() == static_cast<size_t>(s.width) * s.height * 4);
    }

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
    auto hooked = repo.list_by_category("apps");
    AURORA_TEST_CHECK(called);
    AURORA_TEST_CHECK(hooked.size() == 1);
    AURORA_TEST_CHECK(hooked.front().id == "hooked");
}
