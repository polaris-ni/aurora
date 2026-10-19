// http_request_id.cpp —— 链路标识的取值与形状判定（口径见同名头文件）。
#include "aurora/net/detail/http_request_id.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "aurora/net/detail/noncrypto_random.h"

namespace aurora::net::detail {
namespace {

/// @brief 小写十六进制表（本库交出的标识恒小写，判定也只认小写——两型并存会让「原样写回」变得含糊）。
constexpr std::array<char, 16> AURORA_LOWER_HEX{'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

/// @brief 把 `value` 的低 `width` 位十六进制追加到 `out`（不足位补零，超出的高位丢弃）。
/// @param out 目标串（就地追加）。
/// @param value 待编码的整数。
/// @param width 写出的 nibble 数（4 位一段即 width = 4）。
/// @return 无返回值（就地追加）。
auto append_hex(std::string &out, std::uint64_t value, std::size_t width) -> void {
    for (std::size_t index = width; index > 0; --index) {
        out.push_back(AURORA_LOWER_HEX.at((value >> ((index - 1) * 4U)) & 0xFU));
    }
}

/// @brief 判定一字符是否小写十六进制 nibble。
/// @param unit 待判字符。
/// @return 属 `[0-9a-f]` 为 true。
[[nodiscard]] auto is_hex_nibble(char unit) -> bool {
    const bool digit = unit >= '0' && unit <= '9';
    const bool letter = unit >= 'a' && unit <= 'f';
    return digit || letter;
}

}  // namespace

auto make_request_id() -> std::string {
    const std::uint64_t head = noncrypto_u64();  // 前 64 位：time_low + time_mid + time_hi_and_version
    const std::uint64_t tail = noncrypto_u64();  // 后 64 位：clock_seq + node
    // 版本位压在第三段的首 nibble、变体位压在第四段的首 nibble（`RFC 9562 §4.4`）：先清再置，
    // 于是随机位只填其余 122 位，交出的文本恒是合法 UUIDv4。
    const std::uint64_t version = (head & ~(UINT64_C(0xF) << 12)) | (UINT64_C(4) << 12);
    const std::uint64_t variant = (tail & ~(UINT64_C(0x3) << 62)) | (UINT64_C(0x2) << 62);
    std::string out;
    out.reserve(36);
    append_hex(out, version >> 32, 8);
    out.push_back('-');
    append_hex(out, version >> 16, 4);
    out.push_back('-');
    append_hex(out, version, 4);  // 首 nibble 已是 `4`
    out.push_back('-');
    append_hex(out, variant >> 48, 4);  // 首 nibble 已落 `8`..`b`
    out.push_back('-');
    append_hex(out, variant & ((UINT64_C(1) << 48) - UINT64_C(1)), 12);
    return out;
}

auto is_request_id_shape(std::string_view value) -> bool {
    if (value.size() != 36) {
        return false;
    }
    // 分隔位恒在 8 / 13 / 18 / 23（`8-4-4-4-12` 的段界），其余位恒是小写十六进制 nibble。
    constexpr std::array<std::size_t, 4> separator_positions{8, 13, 18, 23};
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = std::ranges::find(separator_positions, index) != separator_positions.end();
        if (separator ? value.at(index) != '-' : !is_hex_nibble(value.at(index))) {
            return false;
        }
    }
    // 版本与变体两段是 UUIDv4 的身份位：形状合格不等于它们是 v4，调用方自填的其它版本一律判假。
    return value.at(14) == '4' &&
           (value.at(19) == '8' || value.at(19) == '9' || value.at(19) == 'a' || value.at(19) == 'b');
}

}  // namespace aurora::net::detail
