/// 测试类型: unit
/// 目标单元: include/aurora/app/hot_reload.h
/// 测试说明: 覆盖 HotReload 的 JSON 注入 loader 全纯逻辑路径——首次同步建树并缓存根、
/// 相同 JSON 跳过重建、变更 JSON 重建新树、loader 抛异常/空 JSON/非法结构的安全回退
/// （根与 last_json 不被失败污染）、set_loader/set_state_key 配置语义；不触达真实文件系统

#include <stdexcept>
#include <string>

#include "aurora/app/hot_reload.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_hot_reload {

namespace {

/// 可 from_json 重建的最小 Text 树。
auto text_tree(const char *text_value) -> Json {
    Json j;
    j["type"] = "Text";
    j["props"]["text"] = text_value;
    return j;
}

/// 可 from_json 重建的最小 Button 树。
auto button_tree(const char *label) -> Json {
    Json j;
    j["type"] = "Button";
    j["props"]["label"] = label;
    return j;
}

}  // namespace

AURORA_TEST_CASE(first_sync_builds_and_caches_root) {
    Json current = text_tree("v1");
    HotReload hr("ui.json");
    hr.set_loader([&current] { return current; });

    // 首次同步前无根。
    AURORA_TEST_CHECK_TRUE(hr.root() == nullptr);

    auto first = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(first != nullptr);
    AURORA_TEST_CHECK_STREQ(first->type_name(), "Text");
    // 根缓存与返回值共享同一对象。
    AURORA_TEST_CHECK_TRUE(hr.root() == first);
}

AURORA_TEST_CASE(unchanged_json_returns_nullptr) {
    Json current = text_tree("v1");
    HotReload hr("ui.json");
    hr.set_loader([&current] { return current; });

    auto first = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(first != nullptr);

    // 相同 JSON：无变化 → nullptr，根保持不变。
    auto second = hr.try_sync();
    AURORA_TEST_CHECK_TRUE(second == nullptr);
    AURORA_TEST_CHECK_TRUE(hr.root() == first);
}

AURORA_TEST_CASE(changed_json_rebuilds_new_tree) {
    Json current = text_tree("v1");
    HotReload hr("ui.json");
    hr.set_loader([&current] { return current; });

    auto first = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(first != nullptr);
    AURORA_TEST_CHECK_STREQ(first->type_name(), "Text");

    // 变更类型 → 重建出 Button 新根。
    current = button_tree("v2");
    auto second = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(second != nullptr);
    AURORA_TEST_CHECK_STREQ(second->type_name(), "Button");
    AURORA_TEST_CHECK_TRUE(hr.root() == second);
    AURORA_TEST_CHECK_TRUE(hr.root() != first);
}

AURORA_TEST_CASE(loader_throw_returns_nullptr) {
    HotReload hr("ui.json");
    hr.set_loader([]() -> Json { throw std::runtime_error("boom"); });

    // loader 抛出 → 安全回退 nullptr，不外溢异常、不产生根。
    auto r = hr.try_sync();
    AURORA_TEST_CHECK_TRUE(r == nullptr);
    AURORA_TEST_CHECK_TRUE(hr.root() == nullptr);
}

AURORA_TEST_CASE(empty_json_returns_nullptr_and_keeps_root) {
    Json current = Json{};  // null → empty
    HotReload hr("ui.json");
    hr.set_loader([&current] { return current; });

    // 空 JSON：直接视为无变化。
    AURORA_TEST_CHECK_TRUE(hr.try_sync() == nullptr);

    // 成功同步后变空：根保留旧树。
    current = text_tree("v1");
    auto first = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(first != nullptr);

    current = Json{};
    AURORA_TEST_CHECK_TRUE(hr.try_sync() == nullptr);
    AURORA_TEST_CHECK_TRUE(hr.root() == first);
}

AURORA_TEST_CASE(invalid_structure_returns_nullptr_and_keeps_root) {
    Json current = text_tree("v1");
    HotReload hr("ui.json");
    hr.set_loader([&current] { return current; });

    auto first = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(first != nullptr);

    // 非空但缺 type 的 JSON：from_json 失败 → nullptr，根与 last_json 不被污染。
    current = Json{1, 2};  // 非空数组：节点必须是对象
    AURORA_TEST_CHECK_TRUE(hr.try_sync() == nullptr);
    current = Json{{"foo", 1}};  // 非空对象：缺 "type" 字段
    AURORA_TEST_CHECK_TRUE(hr.try_sync() == nullptr);
    AURORA_TEST_CHECK_TRUE(hr.root() == first);

    // 失败后恢复与 last_json 相同的内容：仍按「无变化」跳过（而非重建）。
    current = text_tree("v1");
    AURORA_TEST_CHECK_TRUE(hr.try_sync() == nullptr);
    AURORA_TEST_CHECK_TRUE(hr.root() == first);
}

AURORA_TEST_CASE(set_state_key_and_deferred_loader_injection) {
    // 仅路径构造（不触文件系统），随后注入 loader 与状态 key 均可生效。
    HotReload hr("unused.json");
    hr.set_state_key("id");

    Json current = text_tree("v1");
    hr.set_loader([&current] { return current; });

    auto root = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(root != nullptr);
    AURORA_TEST_CHECK_STREQ(root->type_name(), "Text");
}

}  // namespace aurora::test_cases::utest_hot_reload
