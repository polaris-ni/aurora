/// 测试类型: unit
/// 目标单元: src/aurora/widget/descriptor.cpp
/// 测试说明: descriptor.cpp 提供两个序列化函数：descriptor_to_json(PropDescriptor) 与
/// descriptor_to_json(WidgetDescriptor)。两者均按「字段非空才输出」的契约生成 JSON；构造全字段填满
/// 的描述符可覆盖所有分支（含 enum/min/max/pattern/constraint/requires_props/conflicts_with 与
/// allowed_child_types/invariants），构造极简描述符可覆盖「跳过可选字段」的分支。
/// descriptor.h 另提供 LocalizedString ↔ props JSON 双形态通道（string 字面 / `$msg` 完整），
/// 一并在此验证保真往返与旧字面形态的读侧兼容。
/// 判据：全字段描述符的 JSON 含全部非空键；极简描述符的 JSON 不含可选键；`$msg` 往返逐字段还原。

#include <string>

#include "aurora/i18n/builtin_messages.gen.h"
#include "aurora/widget/descriptor.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_descriptor {

AURORA_TEST_CASE(prop_descriptor_full_fields_serializes_everything) {
    PropDescriptor p;
    p.name = "header";
    p.type = "string";
    p.default_value = "\"\"";
    p.required = true;
    p.note = "Title text";
    p.json_type = "string";
    p.enum_values = {"compact", "expanded"};
    p.min_value = "0";
    p.max_value = "100";
    p.pattern = "^[a-z]+$";
    p.constraint = "non-empty";
    p.requires_props = {"title"};
    p.conflicts_with = {"footer"};

    const Json j = aurora::descriptor_to_json(p);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "header");
    AURORA_TEST_CHECK_EQ(j.at("type")->as_or<std::string>(""), "string");
    AURORA_TEST_CHECK_EQ(j.at("default")->as_or<std::string>(""), "\"\"");
    AURORA_TEST_CHECK_TRUE(j.at("required")->as_or<bool>(false));
    AURORA_TEST_CHECK_TRUE(j.contains("note"));
    AURORA_TEST_CHECK_TRUE(j.contains("json_type"));
    AURORA_TEST_CHECK_TRUE(j.contains("enum"));
    AURORA_TEST_CHECK_TRUE(j.contains("minimum"));
    AURORA_TEST_CHECK_TRUE(j.contains("maximum"));
    AURORA_TEST_CHECK_TRUE(j.contains("pattern"));
    AURORA_TEST_CHECK_TRUE(j.contains("constraint"));
    AURORA_TEST_CHECK_TRUE(j.contains("requires_props"));
    AURORA_TEST_CHECK_TRUE(j.contains("conflicts_with"));
}

AURORA_TEST_CASE(prop_descriptor_minimal_skips_optional_keys) {
    PropDescriptor p;
    p.name = "count";
    p.type = "int";
    p.default_value = "0";
    p.required = false;

    const Json j = aurora::descriptor_to_json(p);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "count");
    AURORA_TEST_CHECK_FALSE(j.contains("note"));
    AURORA_TEST_CHECK_FALSE(j.contains("enum"));
    AURORA_TEST_CHECK_FALSE(j.contains("minimum"));
    AURORA_TEST_CHECK_FALSE(j.contains("pattern"));
    AURORA_TEST_CHECK_FALSE(j.contains("requires_props"));
}

AURORA_TEST_CASE(widget_descriptor_full_and_minimal) {
    WidgetDescriptor full;
    full.name = "ExpansionPanel";
    full.ns = "aurora";
    full.properties.emplace_back(
        PropDescriptor{.name = "header", .type = "string", .default_value = R"("")", .required = true});
    full.events.emplace_back("on_toggle");
    full.children_policy = "single";
    full.examples.emplace_back(R"(au::ExpansionPanel("x", au::Text("y")))");
    full.allowed_child_types = {"Text", "Image"};
    full.invariants = {"header non-empty"};

    const Json j = aurora::descriptor_to_json(full);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "ExpansionPanel");
    AURORA_TEST_CHECK_EQ(j.at("namespace")->as_or<std::string>(""), "aurora");
    AURORA_TEST_CHECK_TRUE(j.contains("properties"));
    AURORA_TEST_CHECK_TRUE(j.contains("events"));
    AURORA_TEST_CHECK_EQ(j.at("children_policy")->as_or<std::string>(""), "single");
    AURORA_TEST_CHECK_TRUE(j.contains("examples"));
    AURORA_TEST_CHECK_TRUE(j.contains("allowed_child_types"));
    AURORA_TEST_CHECK_TRUE(j.contains("invariants"));

    // 极简：仅必填字段，扩展字段不输出。
    WidgetDescriptor minimal;
    minimal.name = "Leaf";
    const Json jm = aurora::descriptor_to_json(minimal);
    AURORA_TEST_CHECK_EQ(jm.at("name")->as_or<std::string>(""), "Leaf");
    AURORA_TEST_CHECK_FALSE(jm.contains("allowed_child_types"));
    AURORA_TEST_CHECK_FALSE(jm.contains("invariants"));
}

AURORA_TEST_CASE(localizable_json_msg_roundtrip_preserves_id_and_args) {
    // `$msg` 完整形态：Id 经默认目录反查规范名，三类标量参数逐字段往返。
    const auto ls =
        aurora::LocalizedString::tr(aurora::msg::AURORA_COMMAND_PALETTE_PLACEHOLDER,
                                    {{"who", std::string{"Ada"}}, {"count", std::int64_t{7}}, {"ratio", 1.5}});
    const Json j = aurora::localizable_to_json(ls);
    AURORA_TEST_REQUIRE(j.is_object());
    const Json *msg = j.at("$msg");
    AURORA_TEST_REQUIRE(msg != nullptr);
    AURORA_TEST_CHECK_EQ(msg->at("id")->as_or<std::string>(""), std::string("command_palette.placeholder"));
    const Json *args = msg->at("args");
    AURORA_TEST_REQUIRE(args != nullptr);
    AURORA_TEST_CHECK_EQ(args->at("who")->as_or<std::string>(""), std::string("Ada"));
    AURORA_TEST_CHECK_EQ(args->at("count")->as_or<std::int64_t>(0), std::int64_t{7});
    AURORA_TEST_CHECK_EQ(args->at("ratio")->as_or<double>(0.0), 1.5);

    const auto back = aurora::localizable_from_json(j);
    AURORA_TEST_CHECK(back.localize);
    AURORA_TEST_CHECK_EQ(back.id, aurora::msg::AURORA_COMMAND_PALETTE_PLACEHOLDER);
    AURORA_TEST_REQUIRE(back.args.size() == 3);
    AURORA_TEST_CHECK_EQ(back.args[0].name, std::string("who"));
    AURORA_TEST_CHECK_EQ(std::get<std::string>(back.args[0].value), std::string("Ada"));
    AURORA_TEST_CHECK_EQ(std::get<std::int64_t>(back.args[1].value), std::int64_t{7});
    AURORA_TEST_CHECK_EQ(std::get<double>(back.args[2].value), 1.5);
    // 序列化幂等：还原值再序列化仍落在同一规范名与同一组参数上。
    const Json again = aurora::localizable_to_json(back);
    AURORA_TEST_REQUIRE(again.at("$msg") != nullptr);
    AURORA_TEST_CHECK_EQ(again.at("$msg")->at("id")->as_or<std::string>(""),
                         std::string("command_palette.placeholder"));
    const Json *args_again = again.at("$msg")->at("args");
    AURORA_TEST_REQUIRE(args_again != nullptr);
    AURORA_TEST_CHECK(args_again->contains("who"));
    AURORA_TEST_CHECK(args_again->contains("count"));
    AURORA_TEST_CHECK(args_again->contains("ratio"));
    AURORA_TEST_CHECK_EQ(args_again->at("count")->as_or<std::int64_t>(0), std::int64_t{7});
}

AURORA_TEST_CASE(localizable_json_literal_and_degraded_id_value) {
    // 非本地化值仍为 string 形态；未登记词表的 Id 无规范名可查 → 降级 id_value 数值往返。
    const Json plain = aurora::localizable_to_json(aurora::LocalizedString{"Hello"});
    AURORA_TEST_CHECK(plain.is_string());
    AURORA_TEST_CHECK_EQ(plain.as_or<std::string>(""), std::string("Hello"));
    const auto from_plain = aurora::localizable_from_json(plain);
    AURORA_TEST_CHECK(!from_plain.localize);
    AURORA_TEST_CHECK_EQ(from_plain.text, std::string("Hello"));

    const auto orphan = aurora::LocalizedString::tr(aurora::MessageId{987654});
    const Json j = aurora::localizable_to_json(orphan);
    const Json *msg = j.at("$msg");
    AURORA_TEST_REQUIRE(msg != nullptr);
    AURORA_TEST_CHECK(!msg->contains("id"));
    AURORA_TEST_CHECK_EQ(msg->at("id_value")->as_or<std::int64_t>(0), std::int64_t{987654});
    const auto back = aurora::localizable_from_json(j);
    AURORA_TEST_CHECK(back.localize);
    AURORA_TEST_CHECK_EQ(back.id, aurora::MessageId{987654});
}

AURORA_TEST_CASE(localizable_json_rejects_unknown_name_and_bad_shape) {
    // 未知规范名 / 非 $msg 对象 / 非法形态 → 默认构造值或验证错误（不抛不崩）。
    Json unknown = Json::object();
    Json inner = Json::object();
    inner.set("id", Json{"no.such.canonical.name"});
    unknown.set("$msg", std::move(inner));
    const auto dropped = aurora::localizable_from_json(unknown);
    AURORA_TEST_CHECK(!dropped.localize);
    AURORA_TEST_CHECK_EQ(dropped.text, std::string(""));

    Json nested = Json::object();
    nested.set("$msg", Json{"not an object"});
    AURORA_TEST_CHECK(!aurora::localizable_from_json(nested).localize);

    const PropDescriptor desc{.name = "content", .json_type = "string|$msg"};
    const auto ok_string = validate_prop<LocalizedString>(Json{"plain"}, desc);
    AURORA_TEST_REQUIRE(ok_string);
    AURORA_TEST_CHECK_EQ(ok_string.value().text, std::string("plain"));
    const auto ok_msg = validate_prop<LocalizedString>(unknown, desc);
    AURORA_TEST_CHECK(ok_msg.ok());  // 未知名走降级默认值，形态本身合法
    const auto bad_number = validate_prop<LocalizedString>(Json{std::int64_t{7}}, desc);
    AURORA_TEST_REQUIRE(!bad_number);
    AURORA_TEST_CHECK_EQ(bad_number.error().code_enum, aurora::ErrorCode::WidgetInvalidProp);
}

}  // namespace aurora::test_cases::utest_descriptor
