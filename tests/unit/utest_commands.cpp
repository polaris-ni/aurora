/// 测试类型: unit
/// 目标单元: include/aurora/commands.h
/// 测试说明: 覆盖 Command 模型默认值、CommandRegistry 的注册/解绑/查找/启停/调用/检索与序列化
/// 信封（含可选字段省略与覆盖开关叠加语义），以及 command_fuzzy_score 的匹配判定与加权序关系

#include <string>
#include <utility>
#include <vector>

#include "aurora/commands.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_commands {

using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto make_command(const std::string &id, const std::string &title) -> Command {
    Command cmd;
    cmd.id = id;
    cmd.title = title;
    return cmd;
}

}  // namespace

AURORA_TEST_CASE(command_defaults_are_empty_and_condition_agnostic) {
    const Command cmd;
    AURORA_TEST_CHECK_EQ(cmd.id, std::string{});
    AURORA_TEST_CHECK_EQ(cmd.title, std::string{});
    AURORA_TEST_CHECK_EQ(cmd.icon, std::string{});
    AURORA_TEST_CHECK_EQ(cmd.category, std::string{});
    AURORA_TEST_CHECK_EQ(cmd.when_label, std::string{});
    AURORA_TEST_CHECK_FALSE(cmd.default_binding.has_value());
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(cmd.action));
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(cmd.enabled));
    AURORA_TEST_CHECK(cmd.scope == ShortcutScope::Global);
}

AURORA_TEST_CASE(registry_add_find_remove_keeps_registration_order) {
    CommandRegistry reg;
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(reg.contains("a"));
    AURORA_TEST_CHECK(reg.find("a") == nullptr);

    AURORA_TEST_CHECK_EQ(reg.add(make_command("a", "Alpha")), 1);
    AURORA_TEST_CHECK_EQ(reg.add(make_command("b", "Beta")), 2);
    AURORA_TEST_CHECK_EQ(reg.add(make_command("c", "Gamma")), 3);
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{3});
    AURORA_TEST_CHECK_TRUE(reg.contains("b"));
    AURORA_TEST_CHECK(reg.find("b") != nullptr);
    AURORA_TEST_CHECK_EQ(reg.find("b")->title, std::string{"Beta"});

    std::vector<std::string> ids;
    for (const Command &cmd : reg.all()) {
        ids.push_back(cmd.id);
    }
    AURORA_TEST_CHECK_EQ(ids.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(ids[0], std::string{"a"});
    AURORA_TEST_CHECK_EQ(ids[1], std::string{"b"});
    AURORA_TEST_CHECK_EQ(ids[2], std::string{"c"});

    // 解绑中间项：其余保持注册序，重复解绑返回 false。
    AURORA_TEST_CHECK_TRUE(reg.remove("b"));
    AURORA_TEST_CHECK_FALSE(reg.remove("b"));
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{2});
    AURORA_TEST_CHECK_FALSE(reg.contains("b"));
    AURORA_TEST_CHECK_EQ(reg.all()[0].id, std::string{"a"});
    AURORA_TEST_CHECK_EQ(reg.all()[1].id, std::string{"c"});

    reg.clear();
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(reg.contains("a"));
}

AURORA_TEST_CASE(registry_add_same_id_overwrites_in_place) {
    CommandRegistry reg;
    reg.add(make_command("x", "First"));
    reg.add(make_command("y", "Second"));
    reg.add(make_command("x", "Replaced"));

    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(reg.find("x")->title, std::string{"Replaced"});
    // 覆盖保持注册序（x 仍在首位）。
    AURORA_TEST_CHECK_EQ(reg.all()[0].id, std::string{"x"});
    AURORA_TEST_CHECK_EQ(reg.all()[0].title, std::string{"Replaced"});
    AURORA_TEST_CHECK_EQ(reg.all()[1].id, std::string{"y"});
}

AURORA_TEST_CASE(registry_rejects_empty_id) {
    CommandRegistry reg;
    AURORA_TEST_CHECK_EQ(reg.add(make_command("", "NoId")), -1);
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(reg.contains(""));
}

AURORA_TEST_CASE(registry_enabled_predicate_and_override_stack) {
    CommandRegistry reg;
    reg.add(make_command("always", "Always"));

    Command gated = make_command("gated", "Gated");
    gated.enabled = []() -> bool { return false; };
    reg.add(std::move(gated));

    AURORA_TEST_CHECK_TRUE(reg.is_enabled("always"));
    AURORA_TEST_CHECK_FALSE(reg.is_enabled("gated"));
    AURORA_TEST_CHECK_FALSE(reg.is_enabled("missing"));  // 未命中 = 不启用

    // 覆盖为 true 不能越过为假的谓词（两者取合）。
    AURORA_TEST_CHECK_TRUE(reg.set_enabled("gated", true));
    AURORA_TEST_CHECK_FALSE(reg.is_enabled("gated"));

    // 覆盖为 false 可强制禁用恒启用命令。
    AURORA_TEST_CHECK_TRUE(reg.set_enabled("always", false));
    AURORA_TEST_CHECK_FALSE(reg.is_enabled("always"));

    AURORA_TEST_CHECK_FALSE(reg.set_enabled("missing", true));
}

AURORA_TEST_CASE(registry_invoke_success_and_guards) {
    CommandRegistry reg;
    int calls = 0;
    Command runnable = make_command("run", "Run");
    runnable.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(runnable));
    reg.add(make_command("placeholder", "Placeholder"));  // 无 action

    AURORA_TEST_CHECK_TRUE(reg.invoke("run"));
    AURORA_TEST_CHECK_EQ(calls, 1);

    AURORA_TEST_CHECK_FALSE(reg.invoke("missing"));  // 未命中
    AURORA_TEST_CHECK_FALSE(reg.invoke("placeholder"));  // 无 action

    reg.set_enabled("run", false);
    AURORA_TEST_CHECK_FALSE(reg.invoke("run"));  // 未启用
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(registry_search_filters_ranks_and_respects_enabled) {
    CommandRegistry reg;
    reg.add(make_command("open", "Open File"));
    reg.add(make_command("copy", "Copy"));

    Command closed = make_command("close", "Close");
    closed.enabled = []() -> bool { return false; };
    reg.add(std::move(closed));

    // 空查询：全部启用者。
    AURORA_TEST_CHECK_EQ(reg.search("").size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(reg.search("", false).size(), std::size_t{3});

    // "op" 同时命中 Open File 与 Copy：词首命中者排前。
    const auto hits = reg.search("op");
    AURORA_TEST_CHECK_EQ(hits.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(hits[0]->id, std::string{"open"});
    AURORA_TEST_CHECK_EQ(hits[1]->id, std::string{"copy"});

    // 非子序列：无命中。
    AURORA_TEST_CHECK_EQ(reg.search("zzz").size(), std::size_t{0});

    // 未启用者被默认过滤，显式放开后可见。
    AURORA_TEST_CHECK_EQ(reg.search("clo").size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(reg.search("clo", false).size(), std::size_t{1});
}

AURORA_TEST_CASE(registry_search_is_deterministic_on_ties) {
    CommandRegistry reg;
    reg.add(make_command("dup1", "Same"));
    reg.add(make_command("dup2", "Same"));

    const auto hits = reg.search("same");
    AURORA_TEST_CHECK_EQ(hits.size(), std::size_t{2});
    // 同分同标题：稳定排序保持注册序。
    AURORA_TEST_CHECK_EQ(hits[0]->id, std::string{"dup1"});
    AURORA_TEST_CHECK_EQ(hits[1]->id, std::string{"dup2"});
}

AURORA_TEST_CASE(registry_to_json_envelope_and_optional_fields) {
    CommandRegistry reg;

    const KeyCombo combo{ModifierKey::Control, KeyCode::O};
    Command full = make_command("file.open", "Open File");
    full.icon = "folder";
    full.category = "File";
    full.when_label = "editorFocused";
    full.action = []() -> void {};
    full.default_binding = combo;
    reg.add(std::move(full));

    reg.add(make_command("noop", "No-op"));  // 无图标/分组/条件/动作/绑定

    Command gated = make_command("gone", "Gone");
    gated.enabled = []() -> bool { return false; };
    reg.add(std::move(gated));

    const Json out = reg.to_json();
    AURORA_TEST_CHECK_TRUE(out.contains("commands"));
    const auto *const items = require_child(out, "commands");
    AURORA_TEST_CHECK_EQ(items->size(), std::size_t{3});

    const auto *const first = require_child_at(*items, 0);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "id"), "file.open");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "title"), "Open File");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "icon"), "folder");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "category"), "File");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "when"), "editorFocused");
    AURORA_TEST_CHECK_TRUE(require_field<bool>(*first, "enabled"));
    AURORA_TEST_CHECK_TRUE(require_field<bool>(*first, "invocable"));
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first, "default_binding"), combo.to_string());

    // 可选字段非空才输出。
    const auto *const second = require_child_at(*items, 1);
    AURORA_TEST_CHECK_FALSE(second->contains("icon"));
    AURORA_TEST_CHECK_FALSE(second->contains("category"));
    AURORA_TEST_CHECK_FALSE(second->contains("when"));
    AURORA_TEST_CHECK_FALSE(second->contains("default_binding"));
    AURORA_TEST_CHECK_FALSE(require_field<bool>(*second, "invocable"));  // 无 action

    // 启用状态如实反映谓词求值。
    AURORA_TEST_CHECK_FALSE(require_field<bool>(*require_child_at(*items, 2), "enabled"));
}

AURORA_TEST_CASE(fuzzy_score_empty_query_and_non_match) {
    AURORA_TEST_CHECK_EQ(command_fuzzy_score("", "Anything"), 0);
    AURORA_TEST_CHECK_EQ(command_fuzzy_score("", ""), 0);
    AURORA_TEST_CHECK_EQ(command_fuzzy_score("zz", "Open File"), -1);
    AURORA_TEST_CHECK_EQ(command_fuzzy_score("elif", "File"), -1);  // 顺序不符
}

AURORA_TEST_CASE(fuzzy_score_is_case_insensitive_subsequence) {
    AURORA_TEST_CHECK(command_fuzzy_score("open", "Open File") >= 0);
    AURORA_TEST_CHECK(command_fuzzy_score("OPEN", "open file") >= 0);
    AURORA_TEST_CHECK(command_fuzzy_score("of", "Open File") >= 0);
    AURORA_TEST_CHECK(command_fuzzy_score("efile", "Open File") >= 0);
    AURORA_TEST_CHECK(command_fuzzy_score("ofile", "Open File") >= 0);
}

AURORA_TEST_CASE(fuzzy_score_prefers_prefix_and_consecutive_hits) {
    // 词首 + 连续命中优于词中离散命中。
    AURORA_TEST_CHECK(command_fuzzy_score("op", "Open File") > command_fuzzy_score("op", "Copy"));

    // 连续命中优于跨词离散命中。
    AURORA_TEST_CHECK(command_fuzzy_score("ile", "File") > command_fuzzy_score("ile", "I like elephants"));
}

AURORA_TEST_CASE(fuzzy_score_valid_match_never_uses_the_no_match_sentinel) {
    // 长间隔 + 长标题仍为有效匹配（得分夹取到 ≥ 0），不与「不匹配 = -1」混淆。
    const int far_apart = command_fuzzy_score("ab", "a________________b");
    AURORA_TEST_CHECK(far_apart >= 0);
    AURORA_TEST_CHECK_EQ(command_fuzzy_score("ab", "b"), -1);
}

// set_binding 为什么必须「只改绑定」：此前改绑定只有 add(Command) 一条路，而它是整条覆盖语义——
// 宿主重述一遍就得把 enabled 谓词这类「取得到、复制不了语义」的字段抄全，漏抄即静默的能力丢失。
AURORA_TEST_CASE(set_binding_rewrites_only_the_default_binding) {
    CommandRegistry reg;
    bool gate = true;
    int calls = 0;
    Command cmd = make_command("file.open", "Open");
    cmd.icon = "folder";
    cmd.category = "File";
    cmd.default_binding = KeyCombo{ModifierKey::Control, KeyCode::O};
    cmd.scope = ShortcutScope::Focus;
    cmd.when_label = "hasEditor";
    cmd.enabled = [&gate]() -> bool { return gate; };
    cmd.action = [&calls]() -> void { ++calls; };
    reg.add(cmd);

    const Command before = *reg.find("file.open");
    AURORA_TEST_CHECK_TRUE(
        reg.set_binding("file.open", KeyCombo{ModifierKey::Control | ModifierKey::Shift, KeyCode::P}));

    const Command *after = reg.find("file.open");
    AURORA_TEST_REQUIRE_NOT_NULL(after);
    AURORA_TEST_CHECK_STREQ(after->id, before.id);
    AURORA_TEST_CHECK_STREQ(after->title, before.title);
    AURORA_TEST_CHECK_STREQ(after->icon, before.icon);
    AURORA_TEST_CHECK_STREQ(after->category, before.category);
    AURORA_TEST_CHECK_STREQ(after->when_label, before.when_label);
    AURORA_TEST_CHECK_EQ(static_cast<int>(after->scope), static_cast<int>(before.scope));  // 作用域未被动
    AURORA_TEST_CHECK_STREQ(aurora::testing::require_value(after->default_binding).to_string(), "Ctrl+Shift+P");

    // 谓词与动作体仍在（std::function 比不了对象身份，比求值结果与可调用性）。
    gate = false;
    AURORA_TEST_CHECK_FALSE(reg.is_enabled("file.open"));
    gate = true;
    AURORA_TEST_CHECK_TRUE(reg.invoke("file.open"));
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 注册次序与条目数不变。
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(reg.all().front().id, "file.open");

    // 未注册 id：返回 false、不入表、不动既有条目。
    AURORA_TEST_CHECK_FALSE(reg.set_binding("app.quit", KeyCombo{KeyCode::F5}));
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{1});
    AURORA_TEST_CHECK_NULL(reg.find("app.quit"));
    AURORA_TEST_CHECK_STREQ(aurora::testing::require_value(reg.find("file.open")->default_binding).to_string(),
                            "Ctrl+Shift+P");
}

}  // namespace aurora::test_cases::utest_commands
