/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_request_id.h + src/aurora/net/detail/http_request_id.cpp
/// 测试说明: HTTP 链路标识的取值形状与判定回归（§12「X-Request-Id」的取值侧；零 socket、零发起侧）。
///           发起侧只在「三级取定」那一处问本件，故字段补齐与不覆盖的判据在 `utest_net_http_client`；这里
///           只断言交出的那一串本身：形状是 `RFC 9562` UUIDv4 的小写文本形态（36 字符、`8-4-4-4-12` 四段
///           以 `-` 相连、版本 nibble 恒 `4`、变体 nibble 落 `8`/`9`/`a`/`b`），随机位取自进程级系统熵源，
///           故取不到熵时这条路交出失败而不是发一枚可猜的——用例把「本腿该取得到」也当成一条断言；
///           `is_request_id_shape()` 作为判定侧要把所有「看着像但不是 v4」的形态挡住
///           （大写拼写、段长与分隔位不对、非十六进制、别的版本号、别的变体位、空串与前后多余字节）。
///           刻意不断言任何一枚具体取值——系统熵没有可复现的种子，逐值断言只会把实现细节钉成契约。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/net/detail/http_request_id.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_request_id {
namespace {

using aurora::net::detail::AURORA_HTTP_REQUEST_ID;
using aurora::net::detail::is_request_id_shape;
using aurora::net::detail::make_request_id;

/// @brief 取一枚标识文本；本腿取不到系统熵时交出一串明显不合格的字符。
/// @return 成功为 UUIDv4 文本；失败为 36 枚 `z`——失败本身由 `AURORA_TEST_CHECK` 记下，形状断言随后如实报红。
[[nodiscard]] auto draw_id() -> std::string {
    const auto id = make_request_id();
    AURORA_TEST_CHECK(static_cast<bool>(id));
    if (!id) {
        // 先落一枚局部量再交出：`return std::string(36, 'z')` 逐字重复了声明的返回类型，命中
        // modernize-return-braced-init-list，而该检查建议的花括号形态走的是 `initializer_list`
        // 重载，会把「36 枚 z」变成两枚字符——故此处改形而不照建议修。
        auto absent = std::string(36, 'z');
        return absent;
    }
    return id.value();
}

/// @brief 判定一字符是否小写十六进制 nibble（与实现同一条判据，写成测试侧的独立一份以免自证）。
/// @param unit 待判字符。
/// @return 属 `[0-9a-f]` 为 true。
[[nodiscard]] auto is_lower_hex(char unit) -> bool {
    return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'f');
}

/// @brief 按 UUID 文本形态逐位校验：段分隔位在 8 / 13 / 18 / 23，其余全为小写十六进制。
/// @param value 待验文本。
/// @return 形状合格为 true（不含版本与变体位的判定）。
[[nodiscard]] auto has_uuid_text_layout(std::string_view value) -> bool {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool is_separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (is_separator) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!is_lower_hex(value[index])) {
            return false;
        }
    }
    return true;
}

/// @brief 独立实现的十六进制解码（只用于从取到的文本里还原随机位，不与实现共用一份代码以免自证）。
/// @param value 一段小写十六进制文本。
/// @return 解码出的无符号值；含任何非 nibble 字符即失败。
[[nodiscard]] auto hex_value(std::string_view value) -> std::optional<std::uint64_t> {
    std::uint64_t out = 0;
    for (const char unit : value) {
        int nibble = -1;
        if (unit >= '0' && unit <= '9') {
            nibble = unit - '0';
        } else if (unit >= 'a' && unit <= 'f') {
            nibble = unit - 'a' + 10;
        }
        if (nibble < 0) {
            return std::nullopt;
        }
        out = (out << 4U) | static_cast<std::uint64_t>(nibble);
    }
    return out;
}

/// @brief 从一整套三段文本里还原第三段的那 12 位随机位。
///
/// 段界在 8 / 13 / 18 / 23，故第三段的 4 枚 nibble 落在 14..17，其首字符是规范强制覆盖的版本位（恒 `4`）。
/// 清掉它之后交出的就是纯随机段——拼接退化成一枚常数、或某一段根本没参与组装，都会在这里现形。
/// @param value 形状已合格的 UUIDv4 文本。
/// @return 第三段的随机取值；解不出 nibble 即失败。
[[nodiscard]] auto middle_random_field(std::string_view value) -> std::optional<std::uint64_t> {
    std::string rebuilt{value.substr(14, 4)};
    rebuilt[0] = '0';  // 版本 nibble 是规范强制覆盖位，不参与随机性判定
    return hex_value(rebuilt);
}

}  // namespace

AURORA_TEST_CASE(the_field_name_is_the_canonical_spelling) {
    // 字段名只在这一处定义：发起侧补写、日志取值、用例断言都问它，故拼写漂移会在三个地方同时报警。
    AURORA_TEST_CHECK_EQ(AURORA_HTTP_REQUEST_ID, "X-Request-Id");
}

AURORA_TEST_CASE(generated_ids_are_lowercase_uuidv4_text) {
    for (std::size_t index = 0; index < 50; ++index) {
        const auto value = draw_id();
        AURORA_TEST_CHECK(has_uuid_text_layout(value));
        AURORA_TEST_CHECK_EQ(value[14], '4');  // 版本 nibble：UUIDv4 的身份位
        const char variant = value[19];
        AURORA_TEST_CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
        AURORA_TEST_CHECK_EQ(is_request_id_shape(value), true);
    }
}

AURORA_TEST_CASE(generated_ids_do_not_repeat_within_one_process) {
    std::set<std::string> seen;
    for (std::size_t index = 0; index < 500; ++index) {
        AURORA_TEST_CHECK_EQ(seen.insert(draw_id()).second, true);
    }
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{500});
}

AURORA_TEST_CASE(the_random_bits_reach_every_segment_of_the_id) {
    // 拼接形态若退化——两枚 64 位取值写成同一枚、或某一枚恒零——只有「段与段各自散开」这一条判据能现形：
    // 全 122 位互异性会被版本/变体的强制位掩盖，而首段与末段各自的取值分布不会。
    std::vector<std::uint64_t> heads;
    std::vector<std::uint64_t> middles;
    std::vector<std::uint64_t> tails;
    std::vector<std::string> draws;
    heads.reserve(64);
    middles.reserve(64);
    tails.reserve(64);
    draws.reserve(64);
    for (std::size_t index = 0; index < 64; ++index) {
        const auto value = draw_id();
        const auto head = hex_value(value.substr(0, 8));
        const auto middle = middle_random_field(value);
        const auto tail = hex_value(value.substr(24, 12));
        AURORA_TEST_CHECK(head.has_value() && middle.has_value() && tail.has_value());
        if (!head || !middle || !tail) {
            return;
        }
        heads.push_back(*head);
        middles.push_back(*middle);
        tails.push_back(*tail);
        draws.push_back(value);
    }
    // 每一段都该有自己的分布：64 枚里出现过的互异取值不少于 60——某段被写成常数时这里只有 1。
    AURORA_TEST_CHECK_GE(std::set(heads.begin(), heads.end()).size(), std::size_t{60});
    AURORA_TEST_CHECK_GE(std::set(middles.begin(), middles.end()).size(), std::size_t{60});
    AURORA_TEST_CHECK_GE(std::set(tails.begin(), tails.end()).size(), std::size_t{60});
    // 段与段之间不相关：首段与末段逐枚相等只可能来自同一枚取值被拼了两遍。
    for (std::size_t index = 0; index < heads.size(); ++index) {
        AURORA_TEST_CHECK_NE(heads.at(index), tails.at(index));
    }
    // 两枚 64 位取值若写成同一枚（`head` 拼了两遍），第二段的四枚 nibble 必然与末段中间那四位逐枚相等：
    // 段界是 8 / 13 / 18 / 23，故第二段的 4 枚在 9..12，末段（12 枚，起于 24）的第 5..8 位在 28..31，
    // 两处取的正是同一组 16 位。独立取值下这一撞每枚概率 2^-16，64 枚里期望 0.001 次，故逐枚判不等站得住。
    for (const std::string &draw : draws) {
        const std::string_view value = draw;
        AURORA_TEST_CHECK_NE(value.substr(9, 4), value.substr(28, 4));
    }
}

AURORA_TEST_CASE(shape_check_rejects_everything_that_is_not_uuidv4) {
    const auto valid = draw_id();
    AURORA_TEST_CHECK_EQ(is_request_id_shape(valid), true);

    // 看着像 UUID 但不是 v4：版本号位与变体位是身份位，别的版本一律判假。
    auto version_one = valid;
    version_one[14] = '1';
    AURORA_TEST_CHECK_EQ(is_request_id_shape(version_one), false);
    auto variant_c = valid;
    variant_c[19] = 'c';
    AURORA_TEST_CHECK_EQ(is_request_id_shape(variant_c), false);

    // 拼写与文法形态：大写、缺分隔、多分隔、长度越界、非十六进制、空串。
    std::string upper;
    for (const char unit : valid) {
        upper.push_back(unit >= 'a' && unit <= 'f' ? static_cast<char>(unit - 32) : unit);
    }
    AURORA_TEST_CHECK_EQ(is_request_id_shape(upper), false);
    AURORA_TEST_CHECK_EQ(is_request_id_shape(valid.substr(0, 35)), false);
    AURORA_TEST_CHECK_EQ(is_request_id_shape(valid + "0"), false);
    AURORA_TEST_CHECK_EQ(is_request_id_shape(std::string_view{}), false);
    auto lost_separator = valid;
    lost_separator[8] = 'x';
    AURORA_TEST_CHECK_EQ(is_request_id_shape(lost_separator), false);
    auto not_hex = valid;
    not_hex[9] = 'g';
    AURORA_TEST_CHECK_EQ(is_request_id_shape(not_hex), false);

    // 业务键形态：短、可读、不含 nibble 结构——判假，但它仍是合法字段值（发起侧照原样上线）。
    AURORA_TEST_CHECK_EQ(is_request_id_shape("trace-42"), false);
}

}  // namespace aurora::test_cases::utest_net_http_request_id
