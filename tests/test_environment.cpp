// environment_test.cpp — 覆盖环境系统（Environment 链式 with/get/set_local、BuildContext、MediaQuery）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Environment;
using aurora::MediaQuery;
using aurora::Theme;

static void test_environment() {
    Environment root;
    root.set_local<Theme>(Theme::dark());
    const auto *t = root.get<Theme>();
    AURORA_TEST_CHECK_MSG(t != nullptr && t->background == Theme::dark().background,
                          "Environment: set_local/get Theme");

    // 子环境：本地键优先，缺失键沿父链继承
    const Environment child = root.with<int>(42);
    const int *v = child.get<int>();
    AURORA_TEST_CHECK_MSG(v != nullptr && *v == 42, "Environment: child local value");
    const auto *inherited = child.get<Theme>();
    AURORA_TEST_CHECK_MSG(inherited != nullptr && inherited->background == Theme::dark().background,
                          "Environment: inherits from parent");

    // 子环境覆盖同名键
    const Environment overridden = root.with<Theme>(Theme::light());
    const auto *o = overridden.get<Theme>();
    AURORA_TEST_CHECK_MSG(o != nullptr && o->background == Theme::light().background,
                          "Environment: child overrides parent key");

    // set_local 在根上设置
    Environment e3;
    e3.set_local<Theme>(Theme::dark());
    const auto *t3 = e3.get<Theme>();
    AURORA_TEST_CHECK_MSG(t3 != nullptr && t3->background == Theme::dark().background, "Environment: set_local");
}

static void test_media_query() {
    const MediaQuery mq = MediaQuery::of(2.0F);
    Environment env;
    env.set_local<MediaQuery>(mq);
    const auto *q = env.get<MediaQuery>();
    AURORA_TEST_CHECK_MSG(q != nullptr && near_f(q->scale_factor, 2.0F), "Environment: MediaQuery round-trips");
    AURORA_TEST_CHECK_MSG(q->size.width == 0.0F, "MediaQuery: default size zero");

    BuildContext ctx;
    ctx.env = &env;
    const auto *from_ctx = ctx.environment<MediaQuery>();
    AURORA_TEST_CHECK_MSG(from_ctx != nullptr && near_f(from_ctx->scale_factor, 2.0F),
                          "BuildContext::environment<MediaQuery>");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== environment_test ===\n");
    test_environment();
    test_media_query();
}
