/// 测试类型: unit
/// 目标单元: src/aurora/app/uri_list.h
/// 测试说明: `text/uri-list` 编解码的纯逻辑——本地路径 ↔ `file://` URI 往返保真（空格、`%`、`#`、`?`、
/// 反斜杠与 UTF-8 多字节中文）、`file://` 与 `http://` 混合行的分类、CRLF / LF / CR 三种分隔符与空行
/// 容错、空清单编解码为空的往返，以及 MIME 常量表取值与编解码产物的对照

#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/app/uri_list.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_uri_list {

using aurora::uri_list::decode;
using aurora::uri_list::encode;

namespace {

/// @brief 一条带空格的 POSIX 形态路径（纯字符串，用例不触碰文件系统）。
constexpr std::string_view AURORA_PATH_SPACE = "/srv/aurora/aurora dir/report final.txt";

/// @brief 一条带盘符与反斜杠的 Windows 形态路径（同样只作字符串参与编解码）。
constexpr std::string_view AURORA_PATH_WINDOWS = "C:\\aurora dir\\report.txt";

}  // namespace

// 变异自证：把 `is_unreserved_byte` 的保留集去掉 `#` / `?` / `%`，或把 `decode` 里的 `%XX`
// 十六进制判读改成「无条件吞掉两个字符」，本用例即转红——这两处任一失守都会让往返不再保真。
AURORA_TEST_CASE(roundtrip_preserves_paths_with_reserved_characters) {
    const std::vector<std::string> paths{
        std::string{AURORA_PATH_SPACE},
        std::string{AURORA_PATH_WINDOWS},
        "/srv/aurora/100%.txt",  // 字面百分号：必须编成 %25 才能被解回 '%'
        "/srv/aurora/a#b?c.txt",  // 片断与查询的保留字符
        "/srv/aurora/plain_name-v1.0.txt",  // 保留集内的字符原样往返
    };
    AURORA_TEST_CHECK_EQ(decode(encode(paths)), paths);
}

// 变异自证：把 UTF-8 多字节排除出编码集（例如只编码 ASCII 可打印字符），解码侧会拿到截断字节，
// 往返比对即刻不等——本用例即为此而设。
AURORA_TEST_CASE(roundtrip_preserves_utf8_multibyte_paths) {
    // CJK-LITERAL: cjk-fixture - 汉字是 UTF-8 三字节序列，是「多字节必须整体百分号编码」的最小实证素材
    const std::vector<std::string> paths{"/srv/aurora/文档/report.txt", "/srv/aurora/报表.txt"};
    AURORA_TEST_CHECK_EQ(decode(encode(paths)), paths);
}

// 变异自证：把 `has_file_scheme` 的 scheme 比对删掉（改成「整行百分号解码」），`http://` 行会被
// 当成路径返回，本用例的条数与取值断言同时转红。
AURORA_TEST_CASE(decode_keeps_only_file_scheme_lines) {
    const std::string payload =
        "file:///srv/aurora/one.txt\r\nhttp://example.com/two.txt\r\nfile:///srv/aurora/three.txt\r\n";
    const std::vector<std::string> expected{"/srv/aurora/one.txt", "/srv/aurora/three.txt"};
    AURORA_TEST_CHECK_EQ(decode(payload), expected);
}

// 变异自证（已实测）：`split_lines` 里把 `c == '\r'` 从分隔符判读中去掉，CRLF 行会残留一个尾随
// `\r`（往返用例一并转红），CR 分支更退化成「一整行」；反过来把 CRLF 当成两个独立分隔符，
// 则会在每条之间多出一条空行。三种写法下本用例都会转红。
AURORA_TEST_CASE(decode_tolerates_crlf_lf_and_cr_separators) {
    const std::vector<std::string> expected{"/srv/aurora/one.txt", "/srv/aurora/two.txt"};
    AURORA_TEST_CHECK_EQ(decode("file:///srv/aurora/one.txt\r\nfile:///srv/aurora/two.txt\r\n"), expected);
    AURORA_TEST_CHECK_EQ(decode("file:///srv/aurora/one.txt\nfile:///srv/aurora/two.txt\n"), expected);
    AURORA_TEST_CHECK_EQ(decode("file:///srv/aurora/one.txt\rfile:///srv/aurora/two.txt\r"), expected);
}

// 变异自证：去掉 `split_lines` 的空行过滤（`if (i > start)` 改成无条件 emplace_back），
// 前导 / 重复 / 尾随分隔符都会产出空条目，本用例的清单比对立即转红。
AURORA_TEST_CASE(decode_ignores_blank_lines) {
    const std::vector<std::string> expected{"/srv/aurora/one.txt", "/srv/aurora/two.txt"};
    AURORA_TEST_CHECK_EQ(decode("\r\nfile:///srv/aurora/one.txt\r\n\r\nfile:///srv/aurora/two.txt\r\n\r\n"), expected);
    AURORA_TEST_CHECK_EQ(decode("\n\n"), std::vector<std::string>{});
    AURORA_TEST_CHECK_EQ(decode(""), std::vector<std::string>{});
}

// 变异自证：把行分隔符从 `"\r\n"` 改成 `"\n"`，或把 `file://` 前缀拼漏，本用例即转红——
// 它钉的是编码产物的**字面形**，而不只是「往返能回来」这一个侧面。
AURORA_TEST_CASE(encode_emits_one_file_uri_per_crlf_terminated_line) {
    AURORA_TEST_CHECK_STREQ(encode({"/srv/aurora/a b.txt"}), "file:///srv/aurora/a%20b.txt\r\n");
    AURORA_TEST_CHECK_STREQ(encode({"/srv/aurora/a#b?c.txt"}), "file:///srv/aurora/a%23b%3Fc.txt\r\n");
    AURORA_TEST_CHECK_STREQ(encode({"/srv/aurora/100%.txt"}), "file:///srv/aurora/100%25.txt\r\n");
    // 盘符路径不以 `/` 开头，编码侧补出空 authority 的第三个斜杠：`file:///C:%5Ca%20b.txt`。
    AURORA_TEST_CHECK_STREQ(encode({"C:\\a b.txt"}), "file:///C:%5Ca%20b.txt\r\n");
    AURORA_TEST_CHECK_STREQ(encode({"/srv/aurora/one.txt", "/srv/aurora/two.txt"}),
                            "file:///srv/aurora/one.txt\r\nfile:///srv/aurora/two.txt\r\n");
}

// 空清单编解码为空：空内容不是失败，也不该产生任何条目。
// 变异自证：把 `encode` 的空路径跳过改成照样产出 `file://` 行，本用例即转红。
AURORA_TEST_CASE(empty_list_roundtrips_to_empty) {
    AURORA_TEST_CHECK_STREQ(encode({}), "");
    AURORA_TEST_CHECK_STREQ(encode({""}), "");
    AURORA_TEST_CHECK_EQ(decode(""), std::vector<std::string>{});
    AURORA_TEST_CHECK_EQ(decode(encode({})), std::vector<std::string>{});
}

// 常量表是剪贴板与系统拖放的单一真源：这里钉住取值形态，编码产物再与之对照，
// 使「各处引用常量而非就地硬编码」这一点可被断言，而不只是靠评审。
// 变异自证：把 `AURORA_MIME_URI_LIST` 改成大写形或别的拼写，本用例与跨模块消费侧同时转红。
AURORA_TEST_CASE(uri_list_mime_constant_matches_interop_spelling) {
    AURORA_TEST_CHECK_STREQ(std::string{AURORA_MIME_URI_LIST}, "text/uri-list");
    AURORA_TEST_CHECK_STREQ(std::string{AURORA_MIME_TEXT_PLAIN}, "text/plain");
    AURORA_TEST_CHECK_STREQ(std::string{AURORA_MIME_IMAGE_PNG}, "image/png");
    AURORA_TEST_CHECK_STREQ(std::string{AURORA_MIME_TEXT_HTML}, "text/html");
    AURORA_TEST_CHECK_STREQ(std::string{AURORA_MIME_WIDGET}, "aurora/widget");

    AURORA_TEST_CHECK_TRUE(is_known_mime(AURORA_MIME_URI_LIST));
    AURORA_TEST_CHECK_TRUE(is_known_mime(AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_CHECK_TRUE(is_known_mime(AURORA_MIME_IMAGE_PNG));
    AURORA_TEST_CHECK_TRUE(is_known_mime(AURORA_MIME_TEXT_HTML));
    AURORA_TEST_CHECK_TRUE(is_known_mime(AURORA_MIME_WIDGET));
    AURORA_TEST_CHECK_FALSE(is_known_mime("application/x-custom"));
    AURORA_TEST_CHECK_FALSE(is_known_mime(""));

    // 编码产物以 `file://` 起头，正是 `text/uri-list` 这一格式所要求的条目形态。
    const std::string payload = encode({"/srv/aurora/one.txt"});
    AURORA_TEST_CHECK_TRUE(payload.starts_with("file://"));
}

}  // namespace aurora::test_cases::utest_uri_list
