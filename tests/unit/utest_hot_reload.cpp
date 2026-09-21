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
    hr.set_loader([&current]() -> Json { return current; });

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
    hr.set_loader([&current]() -> Json { return current; });

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
    hr.set_loader([&current]() -> Json { return current; });

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
    hr.set_loader([&current]() -> Json { return current; });

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
    hr.set_loader([&current]() -> Json { return current; });

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

AURORA_TEST_CASE(state_is_preserved_for_props_the_json_does_not_declare) {
    // 热重载的核心价值：改结构不该把用户刚勾上的状态冲掉。
    Json current = Json::object();
    current["type"] = "Column";
    Json checkbox_node = Json::object();
    checkbox_node["type"] = "Checkbox";
    checkbox_node["props"] = Json::object();
    checkbox_node["props"]["checked"] = false;
    current["children"] = Json::array({checkbox_node});

    HotReload hr("ui.json");
    hr.set_loader([&current]() -> Json { return current; });

    auto root = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(root != nullptr);

    // 模拟「用户运行期勾上了它」。
    {
        Node checkbox = root->child_nodes().at(0);
        AURORA_TEST_REQUIRE_TRUE(Inspector::set_prop(checkbox.widget(), "checked", Json(true)).ok());
    }

    // 新 JSON 追加了一个兄弟，且**没有**声明 checked ⇒ 勾选状态应被保留。
    Json bare = Json::object();
    bare["type"] = "Checkbox";
    Json added = Json::object();
    added["type"] = "Text";
    added["props"] = Json::object();
    added["props"]["content"] = "added";
    current["children"] = Json::array({bare, added});

    auto rebuilt = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(rebuilt != nullptr);
    Node checkbox = rebuilt->child_nodes().at(0);
    Json props = Json::object();
    checkbox.widget().serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.value("checked", false));
}

AURORA_TEST_CASE(declared_props_win_over_preserved_state) {
    // 反面对照：源文件显式声明的值永远优先，热重载不得反过来覆盖用户的 JSON。
    Json current = Json::object();
    current["type"] = "Column";
    Json checkbox_node = Json::object();
    checkbox_node["type"] = "Checkbox";
    checkbox_node["props"] = Json::object();
    checkbox_node["props"]["checked"] = false;
    current["children"] = Json::array({checkbox_node});

    HotReload hr("ui.json");
    hr.set_loader([&current]() -> Json { return current; });
    auto root = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(root != nullptr);

    {
        Node checkbox = root->child_nodes().at(0);
        AURORA_TEST_REQUIRE_TRUE(Inspector::set_prop(checkbox.widget(), "checked", Json(true)).ok());
    }

    // 这次新 JSON **显式写了** checked=false ⇒ 必须落到 false。
    // 注意：JSON 必须与上一轮**确有不同**，否则 HotReload 按「无变化」直接跳过（返回 nullptr），
    // 那是在测另一条分支。这里追加一个无关兄弟来制造差异。
    Json redeclared = Json::object();
    redeclared["type"] = "Checkbox";
    redeclared["props"] = Json::object();
    redeclared["props"]["checked"] = false;
    Json unrelated = Json::object();
    unrelated["type"] = "Text";
    unrelated["props"] = Json::object();
    unrelated["props"]["content"] = "unrelated";
    current["children"] = Json::array({redeclared, unrelated});

    auto rebuilt = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(rebuilt != nullptr);
    Node checkbox = rebuilt->child_nodes().at(0);
    Json props = Json::object();
    checkbox.widget().serialize_props(props);
    AURORA_TEST_CHECK_FALSE(props.value("checked", true));
}

AURORA_TEST_CASE(set_state_key_and_deferred_loader_injection) {
    // 仅路径构造（不触文件系统），随后注入 loader 与状态 key 均可生效。
    HotReload hr("unused.json");
    hr.set_state_key("id");

    Json current = text_tree("v1");
    hr.set_loader([&current]() -> Json { return current; });

    auto root = hr.try_sync();
    AURORA_TEST_REQUIRE_TRUE(root != nullptr);
    AURORA_TEST_CHECK_STREQ(root->type_name(), "Text");
}

}  // namespace aurora::test_cases::utest_hot_reload
