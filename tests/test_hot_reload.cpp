// HotReload 验证：注入 JSON 加载器，首次同步构建新根、内容不变返回 nullptr、
// 内容变化返回新根、非法/空 JSON 返回 nullptr 并保留旧根。不依赖 GUI 后端。
// 使用 tests/test_harness.h 的 AURORA_TEST_CHECK（NDEBUG 安全）。

#include <memory>
#include <string>

#include "aurora/app/hot_reload.h"
#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::HotReload;
using aurora::Json;

AURORA_TEST() {
    std::string content = R"({"type":"Button","props":{"text":"Hi"}})";
    HotReload hr("dummy.json");
    hr.set_loader([&]() -> Json { return Json::parse(content); });

    // 首次同步：构建新根
    const auto w1 = hr.try_sync();
    AURORA_TEST_CHECK(static_cast<bool>(w1));
    AURORA_TEST_CHECK(hr.root() == w1);

    // 内容未变：返回 nullptr（无新根）
    const auto w2 = hr.try_sync();
    AURORA_TEST_CHECK(!w2);

    // 内容变化：返回新根（与旧根不同，root() 同步更新）
    content = R"({"type":"Text","props":{"text":"X"}})";
    const auto w3 = hr.try_sync();
    AURORA_TEST_CHECK(static_cast<bool>(w3));
    AURORA_TEST_CHECK(w3 != w1);
    AURORA_TEST_CHECK(hr.root() == w3);

    // 非法 JSON（空字符串）：parse 抛异常被吞，返回 nullptr 并保留旧根
    content = "";
    const auto w4 = hr.try_sync();
    AURORA_TEST_CHECK(!w4);
    AURORA_TEST_CHECK(static_cast<bool>(hr.root()));

    // 合法 JSON 但缺 type 字段：from_json 失败 → nullptr，旧根保留
    content = R"({"foo":1})";
    const auto w5 = hr.try_sync();
    AURORA_TEST_CHECK(!w5);
    AURORA_TEST_CHECK(static_cast<bool>(hr.root()));

    // set_state_key 不崩溃，root 仍有效
    hr.set_state_key("id");
    AURORA_TEST_CHECK(static_cast<bool>(hr.root()));
}
