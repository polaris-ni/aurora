/// @file utest_json.cpp
/// 测试类型: unit
/// 目标单元: include/aurora/core/json.h
/// 测试说明: 覆盖自研 JSON 库的语法合规矩阵（合法 / 非法字面量与结构）、边界语义（前导 BOM、内嵌 NUL、
/// 代理对、超域数字保真、顶层标量）、键序与重复键、全类型 dump↔parse 往返、错误位置与占位符填充、
/// 类型判别与内部存储的同构、封闭读类型集、保真数字不参与数值转换、严格相等语义、容器读写与迭代视图、
/// 以及序列化的转义 / ASCII 化 / 缩进 / 非有限值拒绝

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/json.h"
#include "framework/aurora_test.h"

// 断言失败时把 JSON 值渲染为其紧凑文本，替代框架默认的容器遍历（对 Object / String 会退化成空）。
namespace aurora::testing {

template <>
struct ValuePrinter<aurora::json::Value> {
    static auto print(const aurora::json::Value &value) -> std::string {
        const auto text = aurora::json::dump(value);
        return text.ok() ? text.value() : std::string{"<unserializable>"};
    }
};

}  // namespace aurora::testing

namespace aurora::test_cases::utest_json {

namespace aj = aurora::json;

using aurora::ErrorCode;

namespace {

/// @brief parse → dump → parse 后与原值相等（全类型往返的公共检查）。
auto check_round_trip(std::string_view text) -> void {
    const auto first = aj::parse(text);
    AURORA_TEST_REQUIRE(first.ok());
    const auto dumped = aj::dump(first.value());
    AURORA_TEST_REQUIRE(dumped.ok());
    const auto second = aj::parse(dumped.value());
    AURORA_TEST_REQUIRE(second.ok());
    AURORA_TEST_CHECK_EQ(second.value(), first.value());
}

/// @brief 收紧后的 parse 失败检查：错误码恒为 json-parse-error。
auto check_parse_fails(std::string_view text) -> void {
    const auto r = aj::parse(text);
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::JsonParseError);
}

}  // namespace

// ============================================================================
// 语法合规
// ============================================================================

AURORA_TEST_CASE(parses_every_top_level_form) {
    AURORA_TEST_CHECK(aj::parse("null").value().is_null());
    AURORA_TEST_CHECK(aj::parse("true").value().is_bool());
    AURORA_TEST_CHECK(aj::parse("false").value().is_bool());
    AURORA_TEST_CHECK(aj::parse("42").value().is_int());
    AURORA_TEST_CHECK(aj::parse("-42").value().is_int());
    AURORA_TEST_CHECK(aj::parse("\"x\"").value().is_string());
    AURORA_TEST_CHECK(aj::parse("[]").value().is_array());
    AURORA_TEST_CHECK(aj::parse("{}").value().is_object());
    // RFC 8259 允许顶层为任意值（非仅容器），此处连标量一并锚定。
    AURORA_TEST_CHECK(aj::parse("3.5").value().is_double());
    // 首尾空白（含各类空白字符）被完整跳过。
    AURORA_TEST_CHECK(aj::parse("  \n\t 7 \r\n ").value().is_int());
    // 空容器内部允许空白。
    AURORA_TEST_CHECK(aj::parse("[ ]").value().is_array());
    AURORA_TEST_CHECK(aj::parse("{ }").value().is_object());
}

AURORA_TEST_CASE(parses_nested_containers) {
    const auto doc = aj::parse(R"({"a":{"b":[1,{"c":[]}]},"d":[[[]]]})");
    AURORA_TEST_REQUIRE(doc.ok());
    const auto &root = doc.value();
    AURORA_TEST_CHECK(root.is_object());
    const auto *inner = root.find("a");
    AURORA_TEST_REQUIRE(inner != nullptr);
    const auto *list = inner->find("b");
    AURORA_TEST_REQUIRE(list != nullptr);
    AURORA_TEST_CHECK_EQ(list->size(), 2U);
    AURORA_TEST_CHECK_EQ(list->at(0)->as_or<std::int64_t>(-1), 1);

    const auto *nested = list->at(1);
    AURORA_TEST_REQUIRE(nested != nullptr);
    AURORA_TEST_CHECK(nested->is_object());
    const auto *empty_list = nested->find("c");
    AURORA_TEST_REQUIRE(empty_list != nullptr);
    AURORA_TEST_CHECK(empty_list->is_array());
    AURORA_TEST_CHECK(empty_list->empty());
}

AURORA_TEST_CASE(rejects_malformed_literals) {
    check_parse_fails("tru");
    check_parse_fails("nul");
    check_parse_fails("fals");
    check_parse_fails("TRUE");
    check_parse_fails("Null");
    check_parse_fails("nulll");
}

AURORA_TEST_CASE(rejects_malformed_numbers) {
    check_parse_fails("01");  // 前导零
    check_parse_fails("-01");  // 负号后前导零
    check_parse_fails("-");  // 只有符号
    check_parse_fails("1.");  // 小数点后无数字
    check_parse_fails("1e");  // 指数无数字
    check_parse_fails("1e+");  // 指数符号后无数字
    check_parse_fails(".5");  // 缺整数部分
    check_parse_fails("+1");  // 不允许显式正号
    check_parse_fails("1.2.3");  // 多个小数点 → 尾随内容
    check_parse_fails("0x1F");  // 十六进制非 JSON 语法
}

AURORA_TEST_CASE(rejects_structural_errors) {
    check_parse_fails("");
    check_parse_fails("   ");
    check_parse_fails("{} {}");  // 顶层多值
    check_parse_fails("42 43");  // 标量尾随内容
    check_parse_fails("{");  // 未闭合对象
    check_parse_fails("[1,");  // 未闭合数组
    check_parse_fails("[1,]");  // 尾随逗号（数组）
    check_parse_fails(R"({"a":1,})");  // 尾随逗号（对象）
    check_parse_fails(R"({"a"})");  // 缺冒号
    check_parse_fails(R"({"a":})");  // 缺值
    check_parse_fails("{a:1}");  // 键未加引号
    check_parse_fails("[1 2]");  // 缺逗号
    check_parse_fails("\"unterminated");
    check_parse_fails(R"({"a":1)");  // 括号类型不匹配
}

AURORA_TEST_CASE(rejects_leading_bom_and_bad_escapes) {
    const auto bom = aj::parse("\xEF\xBB\xBF{}");
    AURORA_TEST_REQUIRE_FALSE(bom.ok());
    AURORA_TEST_CHECK_EQ(bom.error().code_enum, ErrorCode::JsonParseError);
    AURORA_TEST_CHECK(bom.error().message.find("BOM") != std::string::npos);

    check_parse_fails(R"("\q")");  // 未知转义
    check_parse_fails(R"("\u12")");  // \u 位数不足
    check_parse_fails(R"("\uZZZZ")");  // 非法十六进制位
    check_parse_fails("\"\x01\"");  // 裸控制字符
    check_parse_fails("\"\n\"");  // 裸换行
}

// ============================================================================
// 边界语义
// ============================================================================

AURORA_TEST_CASE(keeps_embedded_nul_and_decodes_surrogate_pairs) {
    const auto nul = aj::parse(R"("a\u0000b")");
    AURORA_TEST_REQUIRE(nul.ok());
    const auto nul_text = nul.value().as_string();
    AURORA_TEST_REQUIRE(nul_text.has_value());
    AURORA_TEST_CHECK_EQ(nul_text->size(), 3U);
    AURORA_TEST_CHECK_EQ(*nul_text, std::string_view{"a\0b", 3});

    const auto emoji = aj::parse(R"("\uD83D\uDE00")");
    AURORA_TEST_REQUIRE(emoji.ok());
    const auto emoji_text = emoji.value().as_string();
    AURORA_TEST_REQUIRE(emoji_text.has_value());
    AURORA_TEST_CHECK_EQ(*emoji_text, std::string_view{"\xF0\x9F\x98\x80"});

    // 直接写入的 4 字节 UTF-8 与 \u 转义路径结果一致。
    const auto literal = aj::parse("\"\xF0\x9F\x98\x80\"");
    AURORA_TEST_REQUIRE(literal.ok());
    AURORA_TEST_CHECK_EQ(literal.value(), emoji.value());
}

AURORA_TEST_CASE(rejects_lone_surrogates) {
    check_parse_fails(R"("\uD800")");  // 孤立高代理项
    check_parse_fails(R"("\uDC00")");  // 孤立低代理项
    check_parse_fails(R"("\uD800x")");  // 高代理项后无低代理项
    check_parse_fails(R"("\uD800\u0041")");  // 次项不是低代理项
}

AURORA_TEST_CASE(validates_utf8_in_strings_and_honours_the_option) {
    const auto strict = aj::parse("\"\xFF\"");
    AURORA_TEST_REQUIRE_FALSE(strict.ok());
    AURORA_TEST_CHECK_EQ(strict.error().code_enum, ErrorCode::JsonParseError);
    AURORA_TEST_CHECK(strict.error().message.find("UTF-8") != std::string::npos);

    // 过长编码（0xC0 0x80 表示 NUL 的非法两字节形式）同样被拒。
    check_parse_fails("\"\xC0\x80\"");
    // 截断的 3 字节序列。
    check_parse_fails("\"\xE4\xB8\"");

    aj::ParseOptions lax;
    lax.validate_utf8 = false;
    const auto permissive = aj::parse("\"\xFF\"", lax);
    AURORA_TEST_REQUIRE(permissive.ok());
    const auto bytes = permissive.value().as_string();
    AURORA_TEST_REQUIRE(bytes.has_value());
    AURORA_TEST_CHECK_EQ(bytes->size(), 1U);
}

AURORA_TEST_CASE(dispatches_numbers_across_int_uint_double_and_raw) {
    // 整数域边界：int64 上下界与溢出到 uint64 的一个单位。
    AURORA_TEST_CHECK_EQ(aj::parse("9223372036854775807").value().type(), aj::Type::Int);
    AURORA_TEST_CHECK_EQ(aj::parse("-9223372036854775808").value().type(), aj::Type::Int);
    AURORA_TEST_CHECK_EQ(aj::parse("9223372036854775808").value().type(), aj::Type::UInt);
    AURORA_TEST_CHECK_EQ(aj::parse("18446744073709551615").value().type(), aj::Type::UInt);

    // 超 uint64 上界 → 保真数字。
    AURORA_TEST_CHECK_EQ(aj::parse("18446744073709551616").value().type(), aj::Type::RawNumber);

    // JSON 无负零整数语义：-0 归零。
    AURORA_TEST_CHECK_EQ(aj::parse("-0").value().type(), aj::Type::Int);
    AURORA_TEST_CHECK_EQ(aj::parse("-0").value().as_or<std::int64_t>(-9), 0);

    // 小数与指数域：可精确往返者落 Double。
    AURORA_TEST_CHECK_EQ(aj::parse("1.5").value().type(), aj::Type::Double);
    AURORA_TEST_CHECK_EQ(aj::parse("1e2").value().type(), aj::Type::Double);
    AURORA_TEST_CHECK_EQ(aj::parse("1e2").value().as_or<double>(-1.0), 100.0);
    AURORA_TEST_CHECK_EQ(aj::parse("-1.5E-3").value().type(), aj::Type::Double);

    // 上溢 / 下溢 / 往返失真 → 保真数字（原字面量一字不改）。
    AURORA_TEST_CHECK_EQ(aj::parse("1e999").value().type(), aj::Type::RawNumber);
    AURORA_TEST_CHECK_EQ(aj::parse("1e-999").value().type(), aj::Type::RawNumber);
    AURORA_TEST_CHECK_EQ(aj::parse("0.1000000000000000055511151231257827").value().type(), aj::Type::RawNumber);
    AURORA_TEST_CHECK_EQ(aj::parse("1e999").value().as_raw_number().value(), "1e999");
}

AURORA_TEST_CASE(preserves_key_insertion_order_and_last_wins) {
    const auto doc = aj::parse(R"({"b":1,"a":2,"c":3})");
    AURORA_TEST_REQUIRE(doc.ok());

    std::vector<std::string> keys;
    for (const auto &entry : doc.value().entries()) {
        keys.emplace_back(entry.key);
    }
    AURORA_TEST_REQUIRE_EQ(keys.size(), 3U);
    AURORA_TEST_CHECK_EQ(keys[0], "b");
    AURORA_TEST_CHECK_EQ(keys[1], "a");
    AURORA_TEST_CHECK_EQ(keys[2], "c");

    // 重复键：后值胜出，但位置保持首次插入处。
    const auto dup = aj::parse(R"({"b":1,"a":2,"b":3})");
    AURORA_TEST_REQUIRE(dup.ok());
    AURORA_TEST_CHECK_EQ(dup.value().size(), 2U);
    std::vector<std::string> dup_keys;
    std::vector<std::int64_t> dup_values;
    for (const auto &entry : dup.value().entries()) {
        dup_keys.emplace_back(entry.key);
        dup_values.push_back(entry.value.as_or<std::int64_t>(-1));
    }
    AURORA_TEST_REQUIRE_EQ(dup_keys.size(), 2U);
    AURORA_TEST_CHECK_EQ(dup_keys[0], "b");
    AURORA_TEST_CHECK_EQ(dup_keys[1], "a");
    AURORA_TEST_CHECK_EQ(dup_values[0], 3);
    AURORA_TEST_CHECK_EQ(dup_values[1], 2);
}

AURORA_TEST_CASE(round_trips_every_type_through_dump_and_parse) {
    check_round_trip("null");
    check_round_trip("true");
    check_round_trip("false");
    check_round_trip("0");
    check_round_trip("-1");
    check_round_trip("9223372036854775807");
    check_round_trip("9223372036854775808");
    check_round_trip("18446744073709551615");
    check_round_trip("18446744073709551616");  // 保真数字闭环
    check_round_trip("1e999");  // 上溢保真
    check_round_trip("1e-999");  // 下溢保真
    check_round_trip("0.1");
    check_round_trip("3.141592653589793");
    check_round_trip("-1.5e-3");
    check_round_trip("\"text\"");
    check_round_trip("\"\"");
    check_round_trip("[]");
    check_round_trip("{}");
    check_round_trip("[1,2,3]");
    check_round_trip(R"({"a":1,"b":[true,null],"c":{"d":"e"}})");
    check_round_trip(R"("\u4e2d\u6587")");
    check_round_trip("[1e999,9223372036854775808,-0.0]");
}

// ============================================================================
// 错误路径
// ============================================================================

AURORA_TEST_CASE(reports_parse_error_position) {
    const auto bad = aj::parse("{\n  \"a\" 1}");
    AURORA_TEST_REQUIRE_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::JsonParseError);
    AURORA_TEST_CHECK_EQ(bad.error().code, "json-parse-error");
    // 行 / 列均自 1 起：错位字符是第 2 行第 7 列的 '1'。
    AURORA_TEST_CHECK(bad.error().message.find("line 2") != std::string::npos);
    AURORA_TEST_CHECK(bad.error().message.find("column 7") != std::string::npos);
    AURORA_TEST_CHECK(bad.error().hint.find("Near:") != std::string::npos);
}

AURORA_TEST_CASE(enforces_depth_limit_with_all_placeholders_filled) {
    aj::ParseOptions opts;
    opts.max_depth = 2;
    AURORA_TEST_CHECK(aj::parse("[[1]]", opts).ok());

    const auto too_deep = aj::parse("[[[1]]]", opts);
    AURORA_TEST_REQUIRE_FALSE(too_deep.ok());
    AURORA_TEST_CHECK_EQ(too_deep.error().code_enum, ErrorCode::JsonDepthExceeded);
    AURORA_TEST_CHECK_EQ(too_deep.error().code, "json-depth-exceeded");
    // {max} / {line} / {column} 三个占位符必须全部被填充，不留花括号字面残留。
    AURORA_TEST_CHECK_EQ(too_deep.error().message.find('{'), std::string::npos);
    AURORA_TEST_CHECK(too_deep.error().message.find("2") != std::string::npos);

    // 默认上限 512：600 层数组触发同一错误码。
    std::string deep;
    deep.reserve(1201);
    for (int i = 0; i < 600; ++i) {
        deep.push_back('[');
    }
    deep.push_back('1');
    for (int i = 0; i < 600; ++i) {
        deep.push_back(']');
    }
    const auto overflow = aj::parse(deep);
    AURORA_TEST_REQUIRE_FALSE(overflow.ok());
    AURORA_TEST_CHECK_EQ(overflow.error().code_enum, ErrorCode::JsonDepthExceeded);
    AURORA_TEST_CHECK_EQ(overflow.error().message.find('{'), std::string::npos);
    AURORA_TEST_CHECK(overflow.error().message.find("512") != std::string::npos);
}

AURORA_TEST_CASE(reports_type_mismatch_with_all_placeholders_filled) {
    const aj::Value text = aj::Value("hello");
    const auto strict = text.as<std::int64_t>();
    AURORA_TEST_REQUIRE_FALSE(strict.ok());
    AURORA_TEST_CHECK_EQ(strict.error().code_enum, ErrorCode::JsonTypeMismatch);
    AURORA_TEST_CHECK_EQ(strict.error().code, "json-type-mismatch");
    AURORA_TEST_CHECK_EQ(strict.error().message.find('{'), std::string::npos);
    AURORA_TEST_CHECK(strict.error().message.find("int64") != std::string::npos);
    AURORA_TEST_CHECK(strict.error().message.find("string") != std::string::npos);
}

// ============================================================================
// 类型系统
// ============================================================================

AURORA_TEST_CASE(exposes_type_ordinals_isomorphic_with_storage_index) {
    // 枚举量序数冻结：与内部 variant 的 alternative index 强制同构。
    static_assert(static_cast<std::uint8_t>(aj::Type::Null) == 0);
    static_assert(static_cast<std::uint8_t>(aj::Type::Bool) == 1);
    static_assert(static_cast<std::uint8_t>(aj::Type::Int) == 2);
    static_assert(static_cast<std::uint8_t>(aj::Type::UInt) == 3);
    static_assert(static_cast<std::uint8_t>(aj::Type::Double) == 4);
    static_assert(static_cast<std::uint8_t>(aj::Type::RawNumber) == 5);
    static_assert(static_cast<std::uint8_t>(aj::Type::String) == 6);
    static_assert(static_cast<std::uint8_t>(aj::Type::Array) == 7);
    static_assert(static_cast<std::uint8_t>(aj::Type::Object) == 8);

    // 逐类型构造 → type() 反查一致（即 index 与 Type 未错位）。
    AURORA_TEST_CHECK_EQ(aj::Value().type(), aj::Type::Null);
    AURORA_TEST_CHECK_EQ(aj::Value(nullptr).type(), aj::Type::Null);
    AURORA_TEST_CHECK_EQ(aj::Value(true).type(), aj::Type::Bool);
    AURORA_TEST_CHECK_EQ(aj::Value(1).type(), aj::Type::Int);
    AURORA_TEST_CHECK_EQ(aj::Value(std::numeric_limits<std::uint64_t>::max()).type(), aj::Type::UInt);
    AURORA_TEST_CHECK_EQ(aj::Value(1.5).type(), aj::Type::Double);
    AURORA_TEST_CHECK_EQ(aj::Value::raw_number("1e999").type(), aj::Type::RawNumber);
    AURORA_TEST_CHECK_EQ(aj::Value("s").type(), aj::Type::String);
    AURORA_TEST_CHECK_EQ(aj::Value::array().type(), aj::Type::Array);
    AURORA_TEST_CHECK_EQ(aj::Value::object().type(), aj::Type::Object);
}

AURORA_TEST_CASE(classifies_types_through_predicates) {
    AURORA_TEST_CHECK(aj::Value().is_null());
    AURORA_TEST_CHECK(aj::Value(false).is_bool());
    AURORA_TEST_CHECK(aj::Value(1).is_int());
    AURORA_TEST_CHECK(aj::Value(std::numeric_limits<std::uint64_t>::max()).is_uint());
    AURORA_TEST_CHECK(aj::Value(1.0).is_double());
    AURORA_TEST_CHECK(aj::Value::raw_number("1").is_raw_number());
    AURORA_TEST_CHECK(aj::Value("s").is_string());
    AURORA_TEST_CHECK(aj::Value::array().is_array());
    AURORA_TEST_CHECK(aj::Value::object().is_object());

    // is_number 覆盖四种数值形态，is_integer 仅覆盖两种整型形态。
    AURORA_TEST_CHECK(aj::Value(1).is_number());
    AURORA_TEST_CHECK(aj::Value(std::numeric_limits<std::uint64_t>::max()).is_number());
    AURORA_TEST_CHECK(aj::Value(1.0).is_number());
    AURORA_TEST_CHECK(aj::Value::raw_number("1").is_number());
    AURORA_TEST_CHECK_FALSE(aj::Value("s").is_number());
    AURORA_TEST_CHECK_FALSE(aj::Value(true).is_number());
    AURORA_TEST_CHECK(aj::Value(1).is_integer());
    AURORA_TEST_CHECK(aj::Value(std::numeric_limits<std::uint64_t>::max()).is_integer());
    AURORA_TEST_CHECK_FALSE(aj::Value(1.0).is_integer());
    AURORA_TEST_CHECK_FALSE(aj::Value::raw_number("1").is_integer());

    // 判别互斥：每个值恰有一个 is_* 为真。
    const aj::Value sample[] = {aj::Value(),    aj::Value(true),    aj::Value(1),        aj::Value(1.0),
                                aj::Value("s"), aj::Value::array(), aj::Value::object(), aj::Value::raw_number("1")};
    for (const auto &v : sample) {
        const int hits = static_cast<int>(v.is_null()) + static_cast<int>(v.is_bool()) + static_cast<int>(v.is_int()) +
                         static_cast<int>(v.is_uint()) + static_cast<int>(v.is_double()) +
                         static_cast<int>(v.is_raw_number()) + static_cast<int>(v.is_string()) +
                         static_cast<int>(v.is_array()) + static_cast<int>(v.is_object());
        AURORA_TEST_CHECK_EQ(hits, 1);
    }
}

AURORA_TEST_CASE(reads_through_the_closed_type_set) {
    const aj::Value int_value = aj::Value(42);
    AURORA_TEST_CHECK_EQ(int_value.as_or<std::int64_t>(-1), 42);
    AURORA_TEST_CHECK_EQ(int_value.as_or<int>(-1), 42);
    AURORA_TEST_CHECK_EQ(int_value.as_or<std::uint64_t>(0), std::uint64_t{42});
    AURORA_TEST_CHECK_EQ(int_value.as_or<double>(-1.0), 42.0);
    AURORA_TEST_CHECK_EQ(int_value.as_or<float>(-1.0F), 42.0F);
    AURORA_TEST_CHECK_EQ(int_value.as_or<std::size_t>(0), std::size_t{42});

    // 跨类别不转换：数值不读作布尔/字符串，布尔不读作数值。
    AURORA_TEST_CHECK_EQ(int_value.as_or<bool>(false), false);
    AURORA_TEST_CHECK_EQ(int_value.as_or<std::string>("-"), std::string{"-"});
    AURORA_TEST_CHECK_EQ(aj::Value(true).as_or<std::int64_t>(-1), -1);
    AURORA_TEST_CHECK_EQ(aj::Value("7").as_or<std::int64_t>(-1), -1);

    // 窄化边界：int 域外的 int64 值不落 int；非整值 double 不落整型；负值不落无符号。
    AURORA_TEST_CHECK_EQ(aj::Value(std::int64_t{2147483648}).as_or<int>(-1), -1);
    AURORA_TEST_CHECK_EQ(aj::Value(std::int64_t{2147483647}).as_or<int>(-1), 2147483647);
    AURORA_TEST_CHECK_EQ(aj::Value(2.5).as_or<int>(-1), -1);
    AURORA_TEST_CHECK_EQ(aj::Value(2.0).as_or<int>(-1), 2);
    AURORA_TEST_CHECK_EQ(aj::Value(-1).as_or<std::uint64_t>(999), std::uint64_t{999});

    // 严格路径：成功取值 / 失败给结构化错误。
    const auto ok_int = int_value.as<std::int64_t>();
    AURORA_TEST_REQUIRE(ok_int.ok());
    AURORA_TEST_CHECK_EQ(ok_int.value(), std::int64_t{42});

    const auto bad_bool = int_value.as<bool>();
    AURORA_TEST_REQUIRE_FALSE(bad_bool.ok());
    AURORA_TEST_CHECK_EQ(bad_bool.error().code_enum, ErrorCode::JsonTypeMismatch);
}

AURORA_TEST_CASE(reads_object_fields_by_key) {
    const auto doc = aj::parse(R"({"n":7,"s":"x","b":true})");
    AURORA_TEST_REQUIRE(doc.ok());
    const auto &obj = doc.value();

    AURORA_TEST_CHECK_EQ(obj.as_or<std::int64_t>("n", -1), 7);
    AURORA_TEST_CHECK_EQ(obj.as_or<std::string>("s", "-"), std::string{"x"});
    AURORA_TEST_CHECK_EQ(obj.as_or<bool>("b", false), true);

    // 缺键 / 类型不符 / 非 Object 上取子键，一律回退到 fallback。
    AURORA_TEST_CHECK_EQ(obj.as_or<std::int64_t>("missing", -1), -1);
    AURORA_TEST_CHECK_EQ(obj.as_or<std::int64_t>("s", -1), -1);
    AURORA_TEST_CHECK_EQ(aj::Value(1).as_or<std::int64_t>("n", -1), -1);
    AURORA_TEST_CHECK_EQ(aj::Value::array().as_or<std::int64_t>("n", -1), -1);

    const auto got = obj.get<std::int64_t>("n");
    AURORA_TEST_REQUIRE(got.ok());
    AURORA_TEST_CHECK_EQ(got.value(), std::int64_t{7});

    const auto absent = obj.get<std::int64_t>("missing");
    AURORA_TEST_REQUIRE_FALSE(absent.ok());
    AURORA_TEST_CHECK_EQ(absent.error().code_enum, ErrorCode::JsonTypeMismatch);
    AURORA_TEST_CHECK(absent.error().message.find("missing") != std::string::npos);
}

AURORA_TEST_CASE(keeps_raw_numbers_out_of_numeric_conversion) {
    const aj::Value raw = aj::Value::raw_number("1e999");
    AURORA_TEST_CHECK(raw.is_raw_number());
    AURORA_TEST_CHECK(raw.is_number());
    AURORA_TEST_CHECK_FALSE(raw.is_string());
    AURORA_TEST_CHECK_FALSE(raw.is_double());
    // 保真数字不参与任何数值转换，也不被当作字符串读出。
    AURORA_TEST_CHECK_FALSE(raw.as_int().has_value());
    AURORA_TEST_CHECK_FALSE(raw.as_double().has_value());
    AURORA_TEST_CHECK_FALSE(raw.as_string().has_value());
    AURORA_TEST_CHECK_EQ(raw.as_or<double>(-1.0), -1.0);
    AURORA_TEST_CHECK_EQ(raw.as_or<std::string>("-"), std::string{"-"});

    // 保真数字只经 as_raw_number 暴露原字面量；size() 给文本字节数。
    const auto text = raw.as_raw_number();
    AURORA_TEST_REQUIRE(text.has_value());
    AURORA_TEST_CHECK_EQ(*text, "1e999");
    AURORA_TEST_CHECK_EQ(raw.size(), 5U);

    const auto strict = raw.as<double>();
    AURORA_TEST_REQUIRE_FALSE(strict.ok());
    AURORA_TEST_CHECK_EQ(strict.error().code_enum, ErrorCode::JsonTypeMismatch);
}

AURORA_TEST_CASE(compares_strictly_within_the_same_type) {
    AURORA_TEST_CHECK(aj::Value(1) == aj::Value(1));
    AURORA_TEST_CHECK(aj::Value(1) != aj::Value(2));
    AURORA_TEST_CHECK(aj::Value() == aj::Value(nullptr));
    AURORA_TEST_CHECK(aj::Value(true) == aj::Value(true));
    AURORA_TEST_CHECK(aj::Value("a") == aj::Value(std::string_view{"a"}));
    AURORA_TEST_CHECK(aj::Value(std::string{"a"}) == aj::Value("a"));

    // 跨数值类型不等：Int(1) 与 Double(1.0) 分属两个 Type。
    AURORA_TEST_CHECK(aj::Value(1) != aj::Value(1.0));
    AURORA_TEST_CHECK(aj::Value(0) != aj::Value(false));
    AURORA_TEST_CHECK(aj::Value(1) != aj::Value(true));
    AURORA_TEST_CHECK(aj::Value(std::numeric_limits<std::uint64_t>::max()) != aj::Value(-1));

    // 保真数字按文本比较，且与同文本的 String 不同型。
    AURORA_TEST_CHECK(aj::Value::raw_number("1") == aj::Value::raw_number("1"));
    AURORA_TEST_CHECK(aj::Value::raw_number("1") != aj::Value::raw_number("1.0"));
    AURORA_TEST_CHECK(aj::Value::raw_number("1") != aj::Value(1));
    AURORA_TEST_CHECK(aj::Value::raw_number("1") != aj::Value("1"));

    // 容器按元素 / 条目逐项比较，顺序敏感。
    AURORA_TEST_CHECK(aj::parse("[1,2]").value() == aj::parse("[1,2]").value());
    AURORA_TEST_CHECK(aj::parse("[1,2]").value() != aj::parse("[2,1]").value());
    AURORA_TEST_CHECK(aj::parse(R"({"a":1})").value() == aj::parse(R"({"a":1})").value());
    AURORA_TEST_CHECK(aj::parse(R"({"a":1})").value() != aj::parse(R"({"a":2})").value());
    AURORA_TEST_CHECK(aj::parse(R"({"a":1})").value() != aj::parse(R"({"b":1})").value());
}

// ============================================================================
// 容器读写与迭代视图
// ============================================================================

AURORA_TEST_CASE(looks_up_without_mutating) {
    auto doc = aj::parse(R"({"a":1})");
    AURORA_TEST_REQUIRE(doc.ok());
    const auto &obj = doc.value();

    // 缺失键只返回空指针，绝不隐式插入（nlohmann operator[] 的历史陷阱）。
    AURORA_TEST_CHECK(obj.find("missing") == nullptr);
    AURORA_TEST_CHECK_FALSE(obj.contains("missing"));
    AURORA_TEST_CHECK_EQ(obj.size(), 1U);

    AURORA_TEST_CHECK(obj.contains("a"));
    AURORA_TEST_REQUIRE(obj.find("a") != nullptr);
    AURORA_TEST_CHECK_EQ(obj.find("a")->as_or<std::int64_t>(-1), 1);
}

AURORA_TEST_CASE(addresses_keys_and_indexes_through_at_overloads) {
    auto doc = aj::parse(R"({"a":{"b":7},"c":[10,20]})");
    AURORA_TEST_REQUIRE(doc.ok());
    auto &root = doc.value();
    const auto &croot = root;

    // 键族 at 是 find 的等价入口：命中同址，缺失同为 nullptr（不抛、不突变）。
    AURORA_TEST_REQUIRE(root.at("a") != nullptr);
    AURORA_TEST_CHECK(root.at("a") == root.find("a"));
    AURORA_TEST_CHECK(croot.at("a") == croot.find("a"));
    AURORA_TEST_CHECK(root.at("missing") == nullptr);
    AURORA_TEST_CHECK_EQ(croot.size(), 2U);

    // 键族可与索引族串联：Object → Array → 元素。
    AURORA_TEST_REQUIRE(root.at("c") != nullptr);
    AURORA_TEST_REQUIRE(root.at("c")->at(1) != nullptr);
    AURORA_TEST_CHECK_EQ(root.at("c")->at(1)->as_or<std::int64_t>(-1), 20);

    // 索引越界 → nullptr（无异常，调用方自判空）。
    AURORA_TEST_CHECK(root.at("c")->at(2) == nullptr);

    // 容器类型不符：Object 上取索引、Array 上取键都返回 nullptr。
    AURORA_TEST_CHECK(root.at(0) == nullptr);
    AURORA_TEST_CHECK(croot.at(0) == nullptr);
    AURORA_TEST_CHECK(root.at("c")->at("b") == nullptr);

    // 非 const 重载返回可写指针，可就地改写。
    AURORA_TEST_REQUIRE(root.at("a")->at("b") != nullptr);
    root.at("a")->set("b", aj::Value(8));
    AURORA_TEST_CHECK_EQ(root.at("a")->at("b")->as_or<std::int64_t>(-1), 8);
}

AURORA_TEST_CASE(mutates_containers_and_reports_misses) {
    aj::Value obj = aj::Value::object();
    obj.set("a", aj::Value(1));
    obj.set("b", aj::Value("x"));
    AURORA_TEST_CHECK_EQ(obj.size(), 2U);

    // 覆盖保持首次插入位置。
    obj.set("a", aj::Value(2));
    AURORA_TEST_CHECK_EQ(obj.size(), 2U);
    auto it = obj.entries().begin();
    AURORA_TEST_CHECK_EQ((*it).key, "a");
    AURORA_TEST_CHECK_EQ((*it).value.as_or<std::int64_t>(-1), 2);
    ++it;
    AURORA_TEST_CHECK_EQ((*it).key, "b");

    AURORA_TEST_CHECK(obj.erase("a"));
    AURORA_TEST_CHECK_FALSE(obj.erase("a"));
    AURORA_TEST_CHECK_EQ(obj.size(), 1U);
    AURORA_TEST_CHECK_FALSE(obj.contains("a"));

    aj::Value arr = aj::Value::array();
    arr.reserve(4);
    arr.push_back(aj::Value(1));
    arr.push_back(aj::Value(2.5));
    arr.push_back(aj::Value("z"));
    AURORA_TEST_CHECK_EQ(arr.size(), 3U);
    AURORA_TEST_CHECK_EQ(arr.at(0)->as_or<std::int64_t>(-1), 1);
    AURORA_TEST_CHECK_EQ(arr.at(2)->as_or<std::string>("-"), std::string{"z"});

    // 越界与非数组一律空指针，不做越界读。
    AURORA_TEST_CHECK(arr.at(3) == nullptr);
    AURORA_TEST_CHECK(arr.at(100) == nullptr);
    AURORA_TEST_CHECK(aj::Value(1).at(0) == nullptr);
    AURORA_TEST_CHECK_EQ(aj::Value(1).as_or_at<std::int64_t>(0, -1), -1);
    AURORA_TEST_CHECK_EQ(arr.as_or_at<double>(1, -1.0), 2.5);
    AURORA_TEST_CHECK_EQ(arr.as_or_at<double>(2, -1.0), -1.0);  // 字符串元素 → 回退
    AURORA_TEST_CHECK_EQ(arr.as_or_at<double>(9, -1.0), -1.0);  // 越界 → 回退

    AURORA_TEST_CHECK(arr.erase_at(0));
    AURORA_TEST_CHECK_FALSE(arr.erase_at(9));
    AURORA_TEST_CHECK_EQ(arr.size(), 2U);
    AURORA_TEST_CHECK(arr.at(0)->is_double());

    arr.clear();
    AURORA_TEST_CHECK(arr.empty());
    AURORA_TEST_CHECK_EQ(arr.size(), 0U);

    obj.clear();
    AURORA_TEST_CHECK(obj.empty());
    AURORA_TEST_CHECK_EQ(obj.size(), 0U);
}

AURORA_TEST_CASE(iterates_entries_and_elements) {
    const auto arr = aj::parse("[1,2,3]");
    AURORA_TEST_REQUIRE(arr.ok());
    std::int64_t sum = 0;
    std::size_t count = 0;
    for (const aj::Value &elem : arr.value()) {
        sum += elem.as_or<std::int64_t>(0);
        ++count;
    }
    AURORA_TEST_CHECK_EQ(count, 3U);
    AURORA_TEST_CHECK_EQ(sum, 6);

    // 非容器上的 entries() / 迭代一律为空 range，不产生悬垂访问。
    const aj::Value scalar = aj::Value(1);
    AURORA_TEST_CHECK(scalar.entries().begin() == scalar.entries().end());
    AURORA_TEST_CHECK(scalar.begin() == scalar.end());
    AURORA_TEST_CHECK(aj::Value::array().begin() == aj::Value::array().end());
    AURORA_TEST_CHECK(aj::Value("s").begin() == aj::Value("s").end());

    // 空对象与空数组的 entries() 亦为空 range。
    AURORA_TEST_CHECK(aj::Value::object().entries().begin() == aj::Value::object().entries().end());
}

AURORA_TEST_CASE(reports_container_queries_consistently) {
    AURORA_TEST_CHECK_EQ(aj::Value("abc").size(), 3U);  // 字符串按字节数
    AURORA_TEST_CHECK_EQ(aj::Value("").size(), 0U);
    AURORA_TEST_CHECK_EQ(aj::Value(1).size(), 0U);  // 标量恒 0
    AURORA_TEST_CHECK_EQ(aj::Value().size(), 0U);
    AURORA_TEST_CHECK_EQ(aj::parse(R"({"a":1,"b":2})").value().size(), 2U);
    AURORA_TEST_CHECK_EQ(aj::parse("[1,2,3,4]").value().size(), 4U);

    AURORA_TEST_CHECK(aj::Value().empty());
    AURORA_TEST_CHECK(aj::Value::array().empty());
    AURORA_TEST_CHECK(aj::Value::object().empty());
    AURORA_TEST_CHECK(aj::Value("").empty());
    AURORA_TEST_CHECK_FALSE(aj::Value(0).empty());
    AURORA_TEST_CHECK_FALSE(aj::Value(false).empty());
    AURORA_TEST_CHECK_FALSE(aj::Value("x").empty());
    AURORA_TEST_CHECK_FALSE(aj::parse("[0]").value().empty());

    // find / at / contains 在非容器上的空安全契约。
    AURORA_TEST_CHECK(aj::Value().find("k") == nullptr);
    AURORA_TEST_CHECK(aj::Value::array().find("k") == nullptr);
    AURORA_TEST_CHECK_FALSE(aj::Value::array().contains("k"));
    AURORA_TEST_CHECK(aj::Value().at(0) == nullptr);
    AURORA_TEST_CHECK(aj::Value::object().at(0) == nullptr);

    // at 的键族与索引族都是「不抛、缺失即 nullptr」：互相的容器类型不符也一律空指针。
    AURORA_TEST_CHECK(aj::Value().at("k") == nullptr);
    AURORA_TEST_CHECK(aj::Value(1).at("k") == nullptr);
    AURORA_TEST_CHECK(aj::Value::array().at("k") == nullptr);
    AURORA_TEST_CHECK(aj::Value::object().at("k") == nullptr);
    AURORA_TEST_CHECK(aj::Value::array().at(0) == nullptr);  // 空数组：无越界异常
    AURORA_TEST_CHECK(aj::Value::object().at(0) == nullptr);
    AURORA_TEST_CHECK(aj::Value("x").at(0) == nullptr);
}

// ============================================================================
// 序列化
// ============================================================================

AURORA_TEST_CASE(dumps_scalars_with_exact_text) {
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value()).value(), std::string{"null"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(nullptr)).value(), std::string{"null"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(true)).value(), std::string{"true"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(false)).value(), std::string{"false"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(0)).value(), std::string{"0"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(-7)).value(), std::string{"-7"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(std::int64_t{9223372036854775807})).value(),
                         std::string{"9223372036854775807"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(std::numeric_limits<std::uint64_t>::max())).value(),
                         std::string{"18446744073709551615"});
    // 整值 Double 补 .0 后缀，使类型在文本层可辨。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(2.0)).value(), std::string{"2.0"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(2.5)).value(), std::string{"2.5"});
    // 保真数字原样输出。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value::raw_number("1e999")).value(), std::string{"1e999"});
}

AURORA_TEST_CASE(dumps_strings_with_the_escape_matrix) {
    const aj::Value special = aj::Value(std::string{"a\"b\\c\n\t\r\b\f"});
    AURORA_TEST_CHECK_EQ(aj::dump(special).value(), std::string{R"("a\"b\\c\n\t\r\b\f")"});

    // 斜杠不转义（RFC 惯例）。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value("a/b")).value(), std::string{R"("a/b")"});
    // 其余控制字符走 \u00XX（小写十六进制）。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(std::string(1, '\x01'))).value(), std::string{R"("\u0001")"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(std::string(1, '\x1F'))).value(), std::string{R"("\u001f")"});
    // 内嵌 NUL 转义为 \u0000，不截断。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value(std::string{"a\0b", 3})).value(), std::string{R"("a\u0000b")"});
}

AURORA_TEST_CASE(dumps_with_optional_ascii_escaping) {
    const aj::Value chinese = aj::Value("\xE4\xB8\xAD");
    const aj::Value emoji = aj::Value("\xF0\x9F\x98\x80");

    // 默认保留原 UTF-8 字节。
    AURORA_TEST_CHECK_EQ(aj::dump(chinese).value(), std::string{"\"\xE4\xB8\xAD\""});
    AURORA_TEST_CHECK_EQ(aj::dump(emoji).value(), std::string{"\"\xF0\x9F\x98\x80\""});

    aj::DumpOptions ascii;
    ascii.ensure_ascii = true;
    AURORA_TEST_CHECK_EQ(aj::dump(chinese, ascii).value(), std::string{R"("\u4e2d")"});
    // 辅助平面字符转成代理对形式的两个 \u 序列。
    AURORA_TEST_CHECK_EQ(aj::dump(emoji, ascii).value(), std::string{R"("\ud83d\ude00")"});
    // ASCII 内容不受该选项影响。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value("plain"), ascii).value(), std::string{R"("plain")"});
}

AURORA_TEST_CASE(formats_dump_with_and_without_indent) {
    const auto doc = aj::parse(R"({"a":[1,2],"b":{}})");
    AURORA_TEST_REQUIRE(doc.ok());

    AURORA_TEST_CHECK_EQ(aj::dump(doc.value()).value(), std::string{R"({"a":[1,2],"b":{}})"});

    aj::DumpOptions pretty;
    pretty.indent = 2;
    AURORA_TEST_CHECK_EQ(aj::dump(doc.value(), pretty).value(),
                         std::string{"{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {}\n}"});

    // 缩进模式仅影响容器展开，空容器仍写为紧凑形式。
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value::array(), pretty).value(), std::string{"[]"});
    AURORA_TEST_CHECK_EQ(aj::dump(aj::Value::object(), pretty).value(), std::string{"{}"});
}

AURORA_TEST_CASE(appends_into_a_reused_buffer) {
    std::string buffer = "prefix:";
    const auto appended = aj::dump_into(aj::Value(1), buffer);
    AURORA_TEST_REQUIRE(appended.ok());
    AURORA_TEST_CHECK_EQ(buffer, std::string{"prefix:1"});

    const auto again = aj::dump_into(aj::Value("x"), buffer);
    AURORA_TEST_REQUIRE(again.ok());
    AURORA_TEST_CHECK_EQ(buffer, std::string{R"(prefix:1"x")"});
}

AURORA_TEST_CASE(refuses_to_serialize_non_finite_doubles) {
    const aj::Value nan_value = aj::Value(std::numeric_limits<double>::quiet_NaN());
    const auto nan_dump = aj::dump(nan_value);
    AURORA_TEST_REQUIRE_FALSE(nan_dump.ok());
    AURORA_TEST_CHECK_EQ(nan_dump.error().code_enum, ErrorCode::JsonValueNotSerializable);
    AURORA_TEST_CHECK_EQ(nan_dump.error().code, "json-value-not-serializable");
    AURORA_TEST_CHECK_EQ(nan_dump.error().message.find('{'), std::string::npos);
    AURORA_TEST_CHECK(nan_dump.error().message.find("NaN") != std::string::npos);

    const aj::Value inf_value = aj::Value(std::numeric_limits<double>::infinity());
    const auto inf_dump = aj::dump(inf_value);
    AURORA_TEST_REQUIRE_FALSE(inf_dump.ok());
    AURORA_TEST_CHECK_EQ(inf_dump.error().code_enum, ErrorCode::JsonValueNotSerializable);
    AURORA_TEST_CHECK(inf_dump.error().message.find("Infinity") != std::string::npos);

    // 嵌套在容器内同样被拦截（错误沿递归路径上抛，不产出半截文本契约）。
    aj::Value nested = aj::Value::object();
    nested.set("bad", nan_value);
    const auto nested_dump = aj::dump(nested);
    AURORA_TEST_REQUIRE_FALSE(nested_dump.ok());
    AURORA_TEST_CHECK_EQ(nested_dump.error().code_enum, ErrorCode::JsonValueNotSerializable);

    aj::Value list = aj::Value::array();
    list.push_back(aj::Value(1));
    list.push_back(aj::Value(std::numeric_limits<double>::infinity()));
    const auto list_dump = aj::dump(list);
    AURORA_TEST_REQUIRE_FALSE(list_dump.ok());
    AURORA_TEST_CHECK_EQ(list_dump.error().code_enum, ErrorCode::JsonValueNotSerializable);
}

// ============================================================================
// JSON Pointer（RFC 6901 最小集）
// ============================================================================

AURORA_TEST_CASE(addresses_members_through_pointers) {
    const auto root = aj::parse(R"({"a":{"b":[10,20]},"~x":1,"k/1":2})");
    AURORA_TEST_REQUIRE(root.ok());
    const aj::Value &doc = root.value();

    // 空 pointer 指向文档自身。
    AURORA_TEST_CHECK(aj::find_pointer(doc, "") == &doc);

    // 对象键与数组下标混合路径。
    const aj::Value *nested = aj::find_pointer(doc, "/a/b/1");
    AURORA_TEST_REQUIRE(nested != nullptr);
    AURORA_TEST_CHECK_EQ(nested->as_or<std::int64_t>(0), 20);

    // 段转义：~0 还原为 '~'、~1 还原为 '/'（RFC 6901 §3）。
    const aj::Value *tilde = aj::find_pointer(doc, "/~0x");
    AURORA_TEST_REQUIRE(tilde != nullptr);
    AURORA_TEST_CHECK_EQ(tilde->as_or<std::int64_t>(0), 1);

    const aj::Value *with_slash = aj::find_pointer(doc, "/k~11");
    AURORA_TEST_REQUIRE(with_slash != nullptr);
    AURORA_TEST_CHECK_EQ(with_slash->as_or<std::int64_t>(0), 2);
}

AURORA_TEST_CASE(pointer_misses_report_null) {
    const auto root = aj::parse(R"({"a":[1],"s":"txt"})");
    AURORA_TEST_REQUIRE(root.ok());
    const aj::Value &doc = root.value();

    AURORA_TEST_CHECK(aj::find_pointer(doc, "a") == nullptr);  // 缺前导 '/'
    AURORA_TEST_CHECK(aj::find_pointer(doc, "/missing") == nullptr);  // 键不存在
    AURORA_TEST_CHECK(aj::find_pointer(doc, "/a/9") == nullptr);  // 数组越界
    AURORA_TEST_CHECK(aj::find_pointer(doc, "/a/x") == nullptr);  // 数组段非数字
    AURORA_TEST_CHECK(aj::find_pointer(doc, "/s/x") == nullptr);  // 标量下取子项
}

AURORA_TEST_CASE(resolves_write_paths_and_creates_missing_layers) {
    aj::Value doc = aj::Value::object();

    // 空 pointer 落在根自身。
    const auto at_root = aj::resolve_for_write(doc, "");
    AURORA_TEST_REQUIRE(at_root.ok());
    AURORA_TEST_CHECK(at_root.value() == &doc);

    // 缺失中间层按下一段形态建容器：数字段 → Array。
    auto list_slot = aj::resolve_for_write(doc, "/list/0");
    AURORA_TEST_REQUIRE(list_slot.ok());
    *list_slot.value() = aj::Value(7);
    const aj::Value *list = aj::find_pointer(doc, "/list");
    AURORA_TEST_REQUIRE(list != nullptr);
    AURORA_TEST_CHECK_TRUE(list->is_array());

    // 非数字段 → Object。
    auto name_slot = aj::resolve_for_write(doc, "/meta/name");
    AURORA_TEST_REQUIRE(name_slot.ok());
    *name_slot.value() = aj::Value("aurora");
    const aj::Value *meta = aj::find_pointer(doc, "/meta");
    AURORA_TEST_REQUIRE(meta != nullptr);
    AURORA_TEST_CHECK_TRUE(meta->is_object());

    // 数组上 `-` 与「索引 == 长度」都按追加处理。
    auto dash_slot = aj::resolve_for_write(doc, "/list/-");
    AURORA_TEST_REQUIRE(dash_slot.ok());
    *dash_slot.value() = aj::Value(8);
    auto append_slot = aj::resolve_for_write(doc, "/list/2");
    AURORA_TEST_REQUIRE(append_slot.ok());
    *append_slot.value() = aj::Value(9);
    AURORA_TEST_CHECK_EQ(aj::find_pointer(doc, "/list")->size(), 3U);

    // 非 const 重载返回可写槽位。
    aj::Value *writable = aj::find_pointer(doc, "/list/0");
    AURORA_TEST_REQUIRE(writable != nullptr);
    *writable = aj::Value(70);
    AURORA_TEST_CHECK_EQ(aj::find_pointer(doc, "/list/0")->as_or<std::int64_t>(0), 70);
}

AURORA_TEST_CASE(rejects_invalid_write_paths) {
    aj::Value doc = aj::Value::object();

    // 语法非法：非空且不以 '/' 开头。
    const auto bad_syntax = aj::resolve_for_write(doc, "a/b");
    AURORA_TEST_REQUIRE_FALSE(bad_syntax.ok());
    AURORA_TEST_CHECK_EQ(bad_syntax.error().code_enum, ErrorCode::JsonParseError);

    // 在标量下继续下探。
    doc.set("s", aj::Value("text"));
    const auto into_scalar = aj::resolve_for_write(doc, "/s/x");
    AURORA_TEST_REQUIRE_FALSE(into_scalar.ok());
    AURORA_TEST_CHECK_EQ(into_scalar.error().code_enum, ErrorCode::JsonTypeMismatch);

    // 数组索引越界（超出可追加位）。
    doc.set("arr", aj::Value::array());
    const auto out_of_range = aj::resolve_for_write(doc, "/arr/3");
    AURORA_TEST_REQUIRE_FALSE(out_of_range.ok());
    AURORA_TEST_CHECK_EQ(out_of_range.error().code_enum, ErrorCode::JsonTypeMismatch);

    // `-` 与「索引 == 长度」都是追加位，只能作末段：出现在中间段一律拒绝。
    const auto dash_not_last = aj::resolve_for_write(doc, "/arr/-/x");
    AURORA_TEST_REQUIRE_FALSE(dash_not_last.ok());
    AURORA_TEST_CHECK_EQ(dash_not_last.error().code_enum, ErrorCode::JsonTypeMismatch);

    const auto append_not_last = aj::resolve_for_write(doc, "/arr/0/x");  // arr 为空，0 == size
    AURORA_TEST_REQUIRE_FALSE(append_not_last.ok());
    AURORA_TEST_CHECK_EQ(append_not_last.error().code_enum, ErrorCode::JsonTypeMismatch);
}

AURORA_TEST_CASE(erases_members_through_pointers) {
    auto root = aj::parse(R"({"a":1,"b":[10,20,30]})");
    AURORA_TEST_REQUIRE(root.ok());
    aj::Value &doc = root.value();

    AURORA_TEST_CHECK_TRUE(aj::erase_pointer(doc, "/a").value());
    AURORA_TEST_CHECK(aj::find_pointer(doc, "/a") == nullptr);

    AURORA_TEST_CHECK_TRUE(aj::erase_pointer(doc, "/b/1").value());
    AURORA_TEST_CHECK_EQ(aj::find_pointer(doc, "/b")->size(), 2U);

    // 未命中不算错误。
    AURORA_TEST_CHECK_FALSE(aj::erase_pointer(doc, "/nope").value());
    AURORA_TEST_CHECK_FALSE(aj::erase_pointer(doc, "/b/9").value());

    // 根不可删除：空 pointer 是错误。
    const auto on_root = aj::erase_pointer(doc, "");
    AURORA_TEST_REQUIRE_FALSE(on_root.ok());
    AURORA_TEST_CHECK_EQ(on_root.error().code_enum, ErrorCode::JsonParseError);
}

}  // namespace aurora::test_cases::utest_json
