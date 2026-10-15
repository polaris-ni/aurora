/// 测试类型: unit
/// 目标单元: include/aurora/net/http_multipart.h + src/aurora/net/http_multipart.cpp
/// 测试说明: `multipart/form-data` 体流的逐字节回归（CHG-018 §12「大文件流式上传」的构造面）。覆盖固定
///           边界下线上字节的逐字符形态、库生成边界的合法性与逐实例互不相同、`total_size()` 与实抽字节
///           恒等（这条等式是 `Content-Length` 不说谎的前提）、文件部件按段惰性读出并可整条重放、零长正文
///           不产出空段（空段只表示「交完了」）、`quoted-string` 的转义、部件媒体类型按扩展名查表与二进制
///           缺省、推迟失败的四种形态（空名、含 CR/LF、边界越界或撞进正文、无部件）都在取字节处如实交出、
///           文件取不到时长度退回未知而错误留到打开那一刻、`apply_to()` 只动两枚事实。
/// 全用例零 socket：文件部件用框架接管的用例临时目录造真文件，故三平台同值；wasm 腿同样跑得起来（MEMFS）。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/net/detail/http_body.h"
#include "aurora/net/http.h"
#include "aurora/net/http_multipart.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_multipart {
namespace {

using aurora::net::http::MultipartWriter;
using aurora::net::http::Request;
using aurora::testing::require_value;

/// @brief 写出泵与部件正文共用的单段上限（段长断言与实现读同一个常量，不写死数字）。
constexpr std::size_t AURORA_WRITE_CHUNK = aurora::net::detail::AURORA_HTTP_WRITE_CHUNK;

/// @brief 一条体流抽完之后的观测值：线上字节、非空段数、最长一段、以及中途失败（若有）。
struct Drained {
    std::string bytes;  ///< 逐段拼起来的线上字节
    std::size_t segments{0};  ///< 交出的非空段数
    std::size_t largest{0};  ///< 最长一段的字节数
    std::optional<aurora::Error> failure;  ///< 有值 = 在交完之前失败
};

/// @brief 把一条体流抽到底（交完之后恒交空段，故不会无限循环）。
/// @param writer 待抽取的表单。
/// @return 观测值；失败时 `bytes` 只含失败之前交出的部分。
[[nodiscard]] auto drain(MultipartWriter &writer) -> Drained {
    Drained out;
    while (true) {
        auto piece = writer.next_chunk();
        if (!piece) {
            out.failure = piece.error();
            return out;
        }
        if (piece.value().empty()) {
            return out;
        }
        out.bytes.append(piece.value().data(), piece.value().size());
        out.largest = out.largest > piece.value().size() ? out.largest : piece.value().size();
        ++out.segments;
    }
}

/// @brief 把文本变成部件正文的字节形。
/// @param text 文本。
/// @return 字节向量。
[[nodiscard]] auto chars(std::string_view text) -> std::vector<char> {
    return std::vector<char>{text.begin(), text.end()};
}

/// @brief 在本用例的临时目录里造一个子目录并先清场（幂等，跨用例互不干扰）。
/// @param tag 子目录名。
/// @return 目录路径。
[[nodiscard]] auto case_dir(std::string_view tag) -> std::filesystem::path {
    const auto dir = std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_net_multipart" /
                     std::filesystem::path{tag};
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

/// @brief 在本用例的临时目录里写一份内容已知的真文件。
/// @param dir 目标目录（已存在）。
/// @param name 文件名。
/// @param payload 文件内容。
/// @return 文件路径。
[[nodiscard]] auto make_file(const std::filesystem::path &dir, std::string_view name, const std::string &payload)
    -> std::filesystem::path {
    const auto path = dir / std::filesystem::path{name};
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.close();
    return path;
}

/// @brief 断言一次校验失败并回读错误码。
/// @param writer 待判定的表单。
/// @return 失败码（校验通过即致命失败）。
[[nodiscard]] auto validation_code(const MultipartWriter &writer) -> aurora::ErrorCode {
    const auto check = writer.validate();
    AURORA_TEST_REQUIRE_MSG(!bool(check), check ? std::string{"expected a refusal"} : std::string{});
    return check.error().code_enum;
}

}  // namespace

AURORA_TEST_CASE(a_fixed_boundary_renders_the_form_byte_for_byte) {
    MultipartWriter writer;
    writer.boundary("AuroraBoundary").field("title", "night").bytes("blob", chars("ab"), "x.bin", "application/x-bin");

    // 部件次序 = 追加次序；每段 = 边界行 + 部件头 + 空行 + 正文 + CRLF，末尾另加一行结束边界。
    const auto expected = std::string{
        "--AuroraBoundary\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "night\r\n"
        "--AuroraBoundary\r\n"
        "Content-Disposition: form-data; name=\"blob\"; filename=\"x.bin\"\r\n"
        "Content-Type: application/x-bin\r\n"
        "\r\n"
        "ab\r\n"
        "--AuroraBoundary--\r\n"};
    const auto flowed = drain(writer);
    AURORA_TEST_CHECK_TRUE(!bool(flowed.failure));
    AURORA_TEST_CHECK_EQ(flowed.bytes, expected);
    // 声明与流出必须逐字节相等：这是 `Content-Length` 不说谎的唯一保证。
    const auto total = writer.total_size();
    AURORA_TEST_REQUIRE_TRUE(total.has_value());
    AURORA_TEST_CHECK_EQ(total.value_or(std::uint64_t{0}), static_cast<std::uint64_t>(flowed.bytes.size()));
    // 两枚部件各交出头、正文、收尾三段，再加一枚结束边界 = 七段。
    AURORA_TEST_CHECK_EQ(flowed.segments, std::size_t{7});

    // 重放：句柄关掉、游标归零，第二次抽出的字节与第一次逐字节相同。
    AURORA_TEST_CHECK_TRUE(writer.restart());
    const auto replay = drain(writer);
    AURORA_TEST_CHECK_EQ(replay.bytes, expected);
}

AURORA_TEST_CASE(the_generated_boundary_is_legal_and_unique_per_writer) {
    MultipartWriter first;
    MultipartWriter second;
    first.field("a", "1");
    second.field("a", "1");

    // 库生成的边界：定长前缀 + 一整个 64 位随机数的十六进制形，逐实例互不相同。
    AURORA_TEST_CHECK_EQ(first.boundary().size(), std::size_t{23});
    AURORA_TEST_CHECK_TRUE(first.boundary().rfind("aurora-", 0) == 0);
    AURORA_TEST_CHECK_TRUE(first.boundary() != second.boundary());
    AURORA_TEST_CHECK_TRUE(bool(first.validate()));
    AURORA_TEST_CHECK_EQ(first.content_type(), "multipart/form-data; boundary=" + first.boundary());

    // 体流里的边界与 `Content-Type` 里的必须是同一枚（两处读的是同一个字段，没有第二份真值源）。
    const auto flowed = drain(first);
    AURORA_TEST_CHECK_TRUE(flowed.bytes.find("--" + first.boundary() + "\r\n") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(flowed.bytes.find("--" + first.boundary() + "--\r\n") != std::string::npos);
}

AURORA_TEST_CASE(part_descriptions_carry_names_types_and_the_payload_source) {
    const auto dir = case_dir("parts");
    const auto path = make_file(dir, "shot.png", "binary-ish");
    MultipartWriter writer;
    writer.field("title", "night")
        .bytes("raw", chars("xy"))
        .bytes("named", chars("z"), "note.dat")
        .file("image", path.string());

    AURORA_TEST_CHECK_EQ(writer.parts().size(), std::size_t{4});
    // 文本部件不发 `Content-Type` 行（缺省即 text/plain），也不带 filename。
    AURORA_TEST_CHECK_TRUE(!writer.parts()[0].content_type.has_value());
    AURORA_TEST_CHECK_TRUE(!writer.parts()[0].filename.has_value());
    // 内存部件没给类型时补通用二进制：任意字节被缺省成 text/plain 比当成二进制更危险。
    AURORA_TEST_CHECK_EQ(writer.parts()[1].content_type.value_or(std::string{}), "application/octet-stream");
    AURORA_TEST_CHECK_TRUE(writer.parts()[1].filename.has_value() == false);
    AURORA_TEST_CHECK_EQ(writer.parts()[2].filename.value_or(std::string{}), "note.dat");
    // 文件部件留着原路径，文件名取末段，类型按扩展名查表。
    AURORA_TEST_CHECK_EQ(writer.parts()[3].path, path.string());
    AURORA_TEST_CHECK_EQ(writer.parts()[3].filename.value_or(std::string{}), "shot.png");
    AURORA_TEST_CHECK_EQ(writer.parts()[3].content_type.value_or(std::string{}), "image/png");
    AURORA_TEST_CHECK_TRUE(writer.parts()[3].data.empty());
}

AURORA_TEST_CASE(media_types_follow_the_extension_and_unknown_ones_go_binary) {
    const auto dir = case_dir("media");
    const auto png = make_file(dir, "a.PNG", "1");
    const auto json = make_file(dir, "b.json", "2");
    const auto weird = make_file(dir, "c.unknownext", "3");
    const auto noext = make_file(dir, "plainfile", "4");

    MultipartWriter writer;
    writer.file("png", png.string())
        .file("json", json.string())
        .file("weird", weird.string())
        .file("noext", noext.string());
    AURORA_TEST_CHECK_EQ(writer.parts()[0].content_type.value_or(std::string{}), "image/png");
    AURORA_TEST_CHECK_EQ(writer.parts()[1].content_type.value_or(std::string{}), "application/json");
    AURORA_TEST_CHECK_EQ(writer.parts()[2].content_type.value_or(std::string{}), "application/octet-stream");
    AURORA_TEST_CHECK_EQ(writer.parts()[3].content_type.value_or(std::string{}), "application/octet-stream");

    // 查表只认文件名最后一段的点：目录名里出现点不该把类型判错。
    const auto dotted = dir / "v1.2" / "report.csv";
    std::filesystem::create_directories(dotted.parent_path());
    {
        std::ofstream out{dotted, std::ios::binary};
        out << "a,b";
    }
    MultipartWriter second;
    second.file("csv", dotted.string());
    AURORA_TEST_CHECK_EQ(second.parts()[0].filename.value_or(std::string{}), "report.csv");
    AURORA_TEST_CHECK_EQ(second.parts()[0].content_type.value_or(std::string{}), "text/csv");
}

AURORA_TEST_CASE(a_file_part_streams_in_segments_and_replays_identically) {
    // 内容长度刻意超过单段上限，才能看见「惰性读」真的在切段而不是整份搬出来。
    std::string payload((AURORA_WRITE_CHUNK * 2U) + 7U, 'A');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>('A' + static_cast<int>(index % 26U));
    }
    const auto path = make_file(case_dir("stream"), "big.bin", payload);

    MultipartWriter writer;
    writer.boundary("BigBoundary").field("note", "ok").file("blob", path.string());

    const auto first = drain(writer);
    AURORA_TEST_CHECK_TRUE(!bool(first.failure));
    AURORA_TEST_CHECK_TRUE(first.largest <= AURORA_WRITE_CHUNK);
    // 文本部件三段 + 文件部件（头 + 三正文段 + 收尾）五段 + 结束边界 = 九段。
    AURORA_TEST_CHECK_EQ(first.segments, std::size_t{9});
    AURORA_TEST_CHECK_TRUE(first.bytes.find(payload) != std::string::npos);
    const auto total = writer.total_size();
    AURORA_TEST_REQUIRE_TRUE(total.has_value());
    AURORA_TEST_CHECK_EQ(total.value_or(std::uint64_t{0}), static_cast<std::uint64_t>(first.bytes.size()));

    // 重放：文件句柄关掉、游标归零，第二次抽出的字节与第一次逐字节相同。
    AURORA_TEST_CHECK_TRUE(writer.restart());
    const auto again = drain(writer);
    AURORA_TEST_CHECK_EQ(again.bytes, first.bytes);

    // 只有文件部件时同样成立：框架字节只有一条来源，故声明与流出不会分岔。
    MultipartWriter sized;
    sized.boundary("BigBoundary").file("blob", path.string());
    const auto flowed = drain(sized);
    AURORA_TEST_CHECK_EQ(sized.total_size().value_or(std::uint64_t{0}),
                         static_cast<std::uint64_t>(flowed.bytes.size()));
}

AURORA_TEST_CASE(zero_length_parts_do_not_emit_empty_segments) {
    MultipartWriter writer;
    writer.boundary("Z").field("empty", "").bytes("blank", std::vector<char>{});

    const auto flowed = drain(writer);
    AURORA_TEST_CHECK_TRUE(!bool(flowed.failure));
    // 每枚部件只交出头与收尾两段（正文为空便不产出段），加结束边界共五段。
    AURORA_TEST_CHECK_EQ(flowed.segments, std::size_t{5});
    AURORA_TEST_CHECK_EQ(flowed.bytes, std::string{"--Z\r\nContent-Disposition: form-data; name=\"empty\"\r\n\r\n\r\n"
                                                   "--Z\r\nContent-Disposition: form-data; name=\"blank\"\r\n"
                                                   "Content-Type: application/octet-stream\r\n\r\n\r\n--Z--\r\n"});
    AURORA_TEST_CHECK_EQ(writer.total_size().value_or(std::uint64_t{0}),
                         static_cast<std::uint64_t>(flowed.bytes.size()));
}

AURORA_TEST_CASE(quotes_and_backslashes_in_part_names_are_escaped) {
    MultipartWriter writer;
    writer.boundary("Q").field("we\"ird\\name", "v");
    const auto flowed = drain(writer);
    AURORA_TEST_CHECK_TRUE(!bool(flowed.failure));
    AURORA_TEST_CHECK_TRUE(flowed.bytes.find("name=\"we\\\"ird\\\\name\"") != std::string::npos);
    // 转义只加反斜杠，长度算的是转义后的实际字节，故声明与流出仍相等。
    AURORA_TEST_CHECK_EQ(writer.total_size().value_or(std::uint64_t{0}),
                         static_cast<std::uint64_t>(flowed.bytes.size()));
}

AURORA_TEST_CASE(bad_forms_are_refused_before_a_single_byte_leaves) {
    // 没有部件：一条空体不是合法的 multipart 体。
    MultipartWriter bare;
    AURORA_TEST_CHECK_EQ(validation_code(bare), aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK_TRUE(!bare.total_size().has_value());
    const auto refused = bare.next_chunk();
    AURORA_TEST_REQUIRE_TRUE(!bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);

    // 字段名不可为空，也不含 CR / LF——那两字符会把部件头顶开一行（MIME 版的请求走私）。
    MultipartWriter blank_name;
    blank_name.field("", "v");
    AURORA_TEST_CHECK_EQ(validation_code(blank_name), aurora::ErrorCode::HttpRequestInvalid);
    MultipartWriter injected;
    injected.field("bad\r\nname", "v");
    AURORA_TEST_CHECK_EQ(validation_code(injected), aurora::ErrorCode::HttpRequestInvalid);
    const auto mid_stream = injected.next_chunk();
    AURORA_TEST_REQUIRE_TRUE(!bool(mid_stream));
    AURORA_TEST_CHECK_EQ(mid_stream.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);

    // 边界：空、含空格、含必须引用的字符、超长、字符集外，四种都不上线。
    for (const auto &candidate : {std::string{}, std::string{"has space"}, std::string{"with\"quote"},
                                  std::string(71, 'b'), std::string{"with;semicolon"}, std::string{"with@at"}}) {
        MultipartWriter writer;
        writer.boundary(candidate).field("a", "1");
        AURORA_TEST_CHECK_EQ(validation_code(writer), aurora::ErrorCode::HttpRequestInvalid);
    }

    // 边界撞进内存正文：对端会把那一处当分隔行，体在半途被截。
    MultipartWriter collision;
    collision.boundary("NEEDLE").field("a", "prefix NEEDLE suffix");
    AURORA_TEST_CHECK_EQ(validation_code(collision), aurora::ErrorCode::HttpRequestInvalid);
    // 撞进部件头（字段名里含边界）同样拒。
    MultipartWriter in_name;
    in_name.boundary("NEEDLE").field("aNEEDLEb", "1");
    AURORA_TEST_CHECK_EQ(validation_code(in_name), aurora::ErrorCode::HttpRequestInvalid);
    // 合法字符集里的 Marks（`'()+_,-./:=?`）不该被误拒。
    MultipartWriter marks;
    marks.boundary("a'b()+,-./:=?z").field("a", "1");
    AURORA_TEST_CHECK_TRUE(bool(marks.validate()));
}

AURORA_TEST_CASE(unavailable_files_leave_the_length_unknown_and_fail_at_the_open) {
    const auto dir = case_dir("files");
    const auto missing = dir / "gone.bin";
    MultipartWriter writer;
    writer.boundary("F").field("note", "ok").file("blob", missing.string());

    // 校验不看文件可读性（那是取字节时才知道的事实），故形态退回「长度未知」而非报错。
    AURORA_TEST_CHECK_TRUE(bool(writer.validate()));
    AURORA_TEST_CHECK_TRUE(!writer.total_size().has_value());

    // 前面的部件照常交出，错误落在打开文件的那一刻。
    const auto flowed = drain(writer);
    AURORA_TEST_REQUIRE_TRUE(bool(flowed.failure));
    AURORA_TEST_CHECK_EQ(require_value(flowed.failure).code_enum, aurora::ErrorCode::IOFileNotFound);
    AURORA_TEST_CHECK_TRUE(flowed.bytes.find("Content-Disposition") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(flowed.bytes.find("ok\r\n") != std::string::npos);

    // 目录当文件送：POSIX 下 open 会成功、读取才报 EISDIR，故这里靠前置判定拦下来。
    const auto as_dir_path = dir / "a-directory";
    std::filesystem::create_directories(as_dir_path);
    MultipartWriter as_dir;
    as_dir.boundary("F").file("blob", as_dir_path.string());
    AURORA_TEST_CHECK_TRUE(!as_dir.total_size().has_value());
    const auto second = drain(as_dir);
    AURORA_TEST_REQUIRE_TRUE(bool(second.failure));
    AURORA_TEST_CHECK_EQ(require_value(second.failure).code_enum, aurora::ErrorCode::IOFileNotFound);
}

AURORA_TEST_CASE(apply_to_sets_only_the_content_type_and_the_source) {
    const auto path = make_file(case_dir("apply"), "data.json", "{}");
    MultipartWriter writer;
    writer.boundary("Apply").field("note", "ok").file("blob", path.string());

    auto request = Request::post("https://example.com/upload");
    writer.apply_to(request);
    AURORA_TEST_CHECK_EQ(request.headers().get("Content-Type").value_or(std::string{}), writer.content_type());
    AURORA_TEST_CHECK_TRUE(request.body_source() == &writer);
    AURORA_TEST_CHECK_TRUE(request.body().empty());

    // 头部里的 `Content-Length` 就是 `total_size()`：两处读同一份计划，不可能各说一套。
    const auto head = request.render_head();
    AURORA_TEST_REQUIRE_TRUE(bool(head));
    const auto length = writer.total_size().value_or(std::uint64_t{0});
    AURORA_TEST_CHECK_TRUE(head.value().find("Content-Length: " + std::to_string(length) + "\r\n") !=
                           std::string::npos);
    AURORA_TEST_CHECK_TRUE(head.value().find("Transfer-Encoding") == std::string::npos);

    // 长度取不到时改走分块，而不是报一个对不上的 `Content-Length`。
    const auto missing = case_dir("apply") / "missing.bin";
    MultipartWriter streaming;
    streaming.boundary("Apply").file("blob", missing.string());
    auto other = Request::post("https://example.com/upload");
    streaming.apply_to(other);
    const auto other_head = other.render_head();
    AURORA_TEST_REQUIRE_TRUE(bool(other_head));
    AURORA_TEST_CHECK_TRUE(other_head.value().find("Transfer-Encoding: chunked") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(other_head.value().find("Content-Length") == std::string::npos);
}

AURORA_TEST_CASE(a_rewritten_boundary_revalidates_the_form) {
    // 推迟失败算一次就记住（逐段重扫内存正文会让长体流变成平方复杂度），但覆写边界会让结论作废重算。
    MultipartWriter writer;
    writer.boundary("").field("name", "v");
    const auto refused = writer.next_chunk();
    AURORA_TEST_REQUIRE_TRUE(!bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);

    writer.boundary("Fixed");
    AURORA_TEST_CHECK_TRUE(bool(writer.validate()));
    const auto flowed = drain(writer);
    AURORA_TEST_CHECK_TRUE(!bool(flowed.failure));

    // 新边界撞进已有正文：旧的「合格」结论不再算数，重算之后当场拒。
    writer.boundary("v");
    AURORA_TEST_CHECK_EQ(validation_code(writer), aurora::ErrorCode::HttpRequestInvalid);
    const auto stuck = drain(writer);
    AURORA_TEST_REQUIRE_TRUE(bool(stuck.failure));
}

}  // namespace aurora::test_cases::utest_net_http_multipart
