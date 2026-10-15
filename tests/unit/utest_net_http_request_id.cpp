/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_request_id.h + src/aurora/net/detail/http_request_id.cpp
/// 测试说明: HTTP 链路标识的取值形状与判定回归（CHG-018 §12「X-Request-Id」的取值侧；零 socket、零发起侧）。
///           发起侧只在「三级取定」那一处问本件，故字段补齐与不覆盖的判据在 `utest_net_http_client`；这里
///           只断言交出的那一串本身：形状是 `RFC 9562` UUIDv4 的小写文本形态（36 字符、`8-4-4-4-12` 四段
///           以 `-` 相连、版本 nibble 恒 `4`、变体 nibble 落 `8`/`9`/`a`/`b`），随机位取自库内置的非密码学
///           序列故进程内不重复；`is_request_id_shape()` 作为判定侧要把所有「看着像但不是 v4」的形态挡住
///           （大写拼写、段长与分隔位不对、非十六进制、别的版本号、别的变体位、空串与前后多余字节）。
///           刻意不断言任何一枚具体取值——序列以时钟播种，逐值断言只会把实现细节钉成契约。

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include "aurora/net/detail/http_request_id.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_request_id {
namespace {

using aurora::net::detail::AURORA_HTTP_REQUEST_ID;
using aurora::net::detail::is_request_id_shape;
using aurora::net::detail::make_request_id;

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

}  // namespace

AURORA_TEST_CASE(the_field_name_is_the_canonical_spelling) {
    // 字段名只在这一处定义：发起侧补写、日志取值、用例断言都问它，故拼写漂移会在三个地方同时报警。
    AURORA_TEST_CHECK_EQ(AURORA_HTTP_REQUEST_ID, "X-Request-Id");
}

AURORA_TEST_CASE(generated_ids_are_lowercase_uuidv4_text) {
    for (std::size_t index = 0; index < 50; ++index) {
        const auto value = make_request_id();
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
        AURORA_TEST_CHECK_EQ(seen.insert(make_request_id()).second, true);
    }
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{500});
}

AURORA_TEST_CASE(shape_check_rejects_everything_that_is_not_uuidv4) {
    const auto valid = make_request_id();
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
