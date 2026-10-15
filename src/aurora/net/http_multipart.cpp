// http_multipart.cpp —— `multipart/form-data` 体流的产出（口径见同名公共头）。
//
// 本文件是那条体流的**唯一**产字节处：部件头、正文段、收尾 CRLF 与结束边界行都在这里拼，而
// `total_size()` 只是把同一套拼法在不读正文的前提下加一遍。两处的行序与换行必须逐字节对齐，否则
// `Content-Length` 与实际流出的长度错位——对端要么截断要么把后续请求读进体里，而那是无从报错的线上故障。
// 因此正文长度只有一条来源（`file_length()` 与内存部件的 `size()`），框架字节只有一条来源
// （`render_part_head()` 与写死的两处 CRLF 常量），单测据同一判据把「声明 == 流出」钉死。
#include "aurora/net/http_multipart.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/net/detail/http_body.h"
#include "aurora/net/detail/noncrypto_random.h"

namespace aurora::net::http {
namespace {

/// @brief 一行 CRLF（部件头收尾与正文收尾都用它；MIME 的行的终止符不是 `\n`）。
constexpr std::string_view AURORA_CRLF = "\r\n";

/// @brief 边界前缀：让线上抓包一眼认出这枚边界出自本库，也保证首字符落在合法字符集内。
constexpr std::string_view AURORA_BOUNDARY_PREFIX = "aurora-";

/// @brief 边界里随机段的十六进制宽度（一整个 64 位随机数即 16 枚 nibble）。
constexpr std::size_t AURORA_BOUNDARY_RANDOM_WIDTH = 16;

/// @brief 边界字符串的长度上限（`RFC 2046 §5.1`：1..70 字符）。
constexpr std::size_t AURORA_BOUNDARY_MAX_LENGTH = 70;

/// @brief 扩展名的最长可判定长度（再长的大概率不是扩展名，按未知类型处理）。
constexpr std::size_t AURORA_EXTENSION_MAX_LENGTH = 12;

/// @brief 结束边界行里除边界本身之外的框架字节：开头的 `--`、结尾的 `--` 与收尾 CRLF，共六个字符。
constexpr std::size_t AURORA_EPILOGUE_OVERHEAD = 6;

/// @brief 每个部件正文之后的收尾 CRLF 长度。
constexpr std::size_t AURORA_PART_TAIL_LENGTH = 2;

/// @brief 通用二进制类型：表外扩展名与未给类型的内存部件的缺省。
constexpr std::string_view AURORA_MEDIA_OCTET_STREAM = "application/octet-stream";

/// @brief 小写十六进制表（与 `detail::http_request_id` 同一套字形，边界的随机段据此写出）。
constexpr std::array<char, 16> AURORA_LOWER_HEX{'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

/// @brief 扩展名 → 媒体类型的最小表（只登记上传里真会碰到的那些，按字母序排列便于查重复）。
///
/// 表外的扩展名一律落 `application/octet-stream`：宁可让对端把部件当二进制，也不凭猜测贴一个错类型。
/// 真需要别的类型时用 `bytes()` 自带 `content_type`，那条路不查表。
constexpr std::array<std::pair<std::string_view, std::string_view>, 36> AURORA_MEDIA_TYPE_TABLE{
    {
        {"avi", "video/x-msvideo"},
        {"avif", "image/avif"},
        {"bmp", "image/bmp"},
        {"css", "text/css"},
        {"csv", "text/csv"},
        {"flac", "audio/flac"},
        {"gif", "image/gif"},
        {"gz", "application/gzip"},
        {"htm", "text/html"},
        {"html", "text/html"},
        {"ico", "image/vnd.microsoft.icon"},
        {"jpeg", "image/jpeg"},
        {"json", "application/json"},
        {"m4a", "audio/mp4"},
        {"md", "text/markdown"},
        {"mov", "video/quicktime"},
        {"mp3", "audio/mpeg"},
        {"mp4", "video/mp4"},
        {"mpeg", "video/mpeg"},
        {"ogg", "audio/ogg"},
        {"pdf", "application/pdf"},
        {"png", "image/png"},
        {"svg", "image/svg+xml"},
        {"tar", "application/x-tar"},
        {"tif", "image/tiff"},
        {"tiff", "image/tiff"},
        {"tsv", "text/tab-separated-values"},
        {"txt", "text/plain"},
        {"wav", "audio/wav"},
        {"wasm", "application/wasm"},
        {"webm", "video/webm"},
        {"webp", "image/webp"},
        {"xml", "application/xml"},
        {"yaml", "application/yaml"},
        {"yml", "application/yaml"},
        {"zip", "application/zip"},
    },
};

/// @brief ASCII 单字节小写化；非大写字母原样返回（规避 locale 与负值 `char` 相关的 UB）。
/// @param unit 原始字节。
/// @return 折叠后的字节。
auto to_lower_ascii(char unit) -> char {
    const auto raw = static_cast<unsigned char>(unit);
    if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
    }
    return unit;
}

/// @brief 整串 ASCII 小写化（扩展名比较按大小写不敏感处理）。
/// @param text 原始文本。
/// @return 小写化后的副本。
[[nodiscard]] auto to_lower_ascii_str(std::string_view text) -> std::string {
    std::string out{text};
    for (char &unit : out) {
        unit = to_lower_ascii(unit);
    }
    return out;
}

/// @brief 文本里是否混着 CR 或 LF。
///
/// 部件头的行终止符由本层写死，故凡要写到那几行上的文本都不许带这俩字符——否则一段名字里塞进 `\r\n`
/// 就能把部件头顶开一行，后面的内容被对端当头部字段解析（这是 MIME 版的请求走私）。
/// @param text 待判文本。
/// @return 含 `\r` 或 `\n` 为 true。
[[nodiscard]] auto has_line_terminator(std::string_view text) -> bool {
    return text.find('\r') != std::string_view::npos || text.find('\n') != std::string_view::npos;
}

/// @brief 该字符是否属 `RFC 2046 §5.1` 给边界串准备的字符集。
///
/// 收的是字母、数字与 `Marks`（`'()+_,-./:=?`）。**空格一律不收**：那条规则允许空格出现在串中间，但
/// 空格进了 `Content-Type` 的 `boundary` 参数就得引用起来，而引用形态的兼容性正是各家服务端最容易分岔
/// 的地方；少一点随机空间换来「写出去就是合法参数」，值。
/// @param unit 待判字符。
/// @return 允许为 true。
[[nodiscard]] auto is_boundary_char(char unit) -> bool {
    static constexpr std::string_view AURORA_BOUNDARY_MARKS = "'()+_,-./:=?";
    const bool alnum = (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z');
    return alnum || AURORA_BOUNDARY_MARKS.find(unit) != std::string_view::npos;
}

/// @brief 把 `value` 的低 `width` 位十六进制追加到 `out`（不足位补零，故定长）。
/// @param out 目标串（就地追加）。
/// @param value 待编码的整数。
/// @param width 写出的 nibble 数。
auto append_hex(std::string &out, std::uint64_t value, std::size_t width) -> void {
    for (std::size_t index = width; index > 0; --index) {
        out.push_back(AURORA_LOWER_HEX.at((value >> ((index - 1) * 4U)) & 0xFU));
    }
}

/// @brief 生成一枚边界：定长前缀加一整个 64 位随机数的十六进制形。
/// @return 长度 23、字符全合法、逐次构造互不相同的令牌。
[[nodiscard]] auto make_boundary() -> std::string {
    std::string out{AURORA_BOUNDARY_PREFIX};
    append_hex(out, detail::noncrypto_u64(), AURORA_BOUNDARY_RANDOM_WIDTH);
    return out;
}

/// @brief 把一段文本写成 MIME 的 `quoted-string`（`RFC 2183 §2`：反斜杠与双引号各自转义）。
/// @param text 原始文本（调用方已保证不含 CR / LF）。
/// @return 带首尾双引号的参数值。
[[nodiscard]] auto quote(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char unit : text) {
        if (unit == '"' || unit == '\\') {
            out.push_back('\\');
        }
        out.push_back(unit);
    }
    out.push_back('"');
    return out;
}

/// @brief 取路径的末段作为文件部件的 `filename` 参数值。
///
/// 分隔符按本机平台口径认（Windows 上 `\` 与 `/` 同为分隔符，POSIX 上只有 `/`）——路径本就是本机文件系统
/// 给的，本类不做跨平台的重新解释。末段为空（路径以分隔符结尾）时返回空串，交由校验按「文件名不可为空」
/// 之外的口径处理：那种路径本就取不到正文，`total_size()` 先一步交 `nullopt`。
/// @param path 文件路径。
/// @return 末段文本。
[[nodiscard]] auto basename_of(std::string_view path) -> std::string {
    return std::filesystem::path{path}.filename().string();
}

/// @brief 取文件名的扩展名（小写、不含点）。
/// @param filename 文件名文本。
/// @return 末段点号之后、长度不超过判定上限的小写扩展名；无点、点在最末或扩展名过长时为空串。
[[nodiscard]] auto extension_of(std::string_view filename) -> std::string {
    const auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= filename.size()) {
        return {};
    }
    const auto tail = filename.substr(dot + 1);
    if (tail.size() > AURORA_EXTENSION_MAX_LENGTH) {
        return {};
    }
    // 扩展名里出现分隔符说明那个点落在目录段（调用方可能直接塞一段路径进来当文件名）。
    if (tail.find('/') != std::string_view::npos || tail.find('\\') != std::string_view::npos) {
        return {};
    }
    return to_lower_ascii_str(tail);
}

/// @brief 查扩展名对应的媒体类型。
/// @param extension 小写扩展名（不含点）。
/// @return 命中表内项则返回之，否则 `application/octet-stream`。
[[nodiscard]] auto media_type_for(std::string_view extension) -> std::string_view {
    if (extension.empty()) {
        return AURORA_MEDIA_OCTET_STREAM;
    }
    for (const auto &[candidate, type] : AURORA_MEDIA_TYPE_TABLE) {
        if (candidate == extension) {
            return type;
        }
    }
    return AURORA_MEDIA_OCTET_STREAM;
}

/// @brief 一句「文件部件此刻取不到字节」的载荷。
///
/// 与图像侧读文件同一枚码（`io-file-not-found`）：同一族事实不该有两个名字，而 open 失败与读到一半失败
/// 对调用方是同一个动作——换个路径再来一次。
/// @param detail 现场细节（含路径与失败环节）。
/// @return `io-file-not-found` 的错误对象。
[[nodiscard]] auto file_unreadable(const std::string &detail) -> Error {
    return make_error(ErrorCode::IOFileNotFound, detail);
}

/// @brief 第 `index` 个部件在失败细节里的名字。
/// @param index 部件下标。
/// @return `multipart part <n>` 文本（让失败可定位到具体哪一次追加）。
[[nodiscard]] auto part_label(std::size_t index) -> std::string { return "multipart part " + std::to_string(index); }

}  // namespace

auto MultipartWriter::form_invalid(const std::string &detail) -> Error {
    return make_error(ErrorCode::HttpRequestInvalid, detail);
}

auto MultipartWriter::file_length(std::string_view path) -> std::optional<std::uint64_t> {
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path{path}, ec);
    if (ec) {
        return std::nullopt;  // 不存在、是目录、非常规文件、无权限——一律按「长度未知」处理
    }
    return static_cast<std::uint64_t>(size);
}

MultipartWriter::MultipartWriter() : boundary_{make_boundary()} {}

MultipartWriter::~MultipartWriter() = default;

auto MultipartWriter::field(std::string_view name, std::string_view value) -> MultipartWriter & {
    parts_.push_back(Part{
        .name = std::string{name},
        .filename = std::nullopt,
        .content_type = std::nullopt,  // 不发这行，由 `RFC 7578 §4.2` 的缺省 `text/plain` 接管
        .path = {},
        .data = std::vector<char>{value.begin(), value.end()},
    });
    failure_checked_ = false;
    return *this;
}

auto MultipartWriter::bytes(std::string_view name, std::vector<char> data, std::string_view filename,
                            std::string_view content_type) -> MultipartWriter & {
    parts_.push_back(Part{
        .name = std::string{name},
        .filename = filename.empty() ? std::nullopt : std::optional<std::string>{std::string{filename}},
        .content_type = std::string{content_type.empty() ? AURORA_MEDIA_OCTET_STREAM : content_type},
        .path = {},
        .data = std::move(data),
    });
    failure_checked_ = false;
    return *this;
}

auto MultipartWriter::file(std::string_view name, std::string_view path) -> MultipartWriter & {
    auto filename = basename_of(path);
    // 类型查的是「写进部件头的那枚文件名」，故用本地量而非等 `push_back` 之后回读。
    const auto type = media_type_for(extension_of(filename));
    parts_.push_back(Part{
        .name = std::string{name},
        .filename = filename.empty() ? std::nullopt : std::optional<std::string>{std::move(filename)},
        .content_type = std::string{type},
        .path = std::string{path},
        .data = {},
    });
    failure_checked_ = false;
    return *this;
}

auto MultipartWriter::boundary(std::string value) -> MultipartWriter & {
    boundary_ = std::move(value);
    failure_checked_ = false;  // 覆写的那枚可能撞进正文，之前「合格」的结论不再算数
    return *this;
}

auto MultipartWriter::content_type() const -> std::string {
    // 边界不带引号：`validate()` 的字符集把需要引用的形态（空格、双引号）都挡在外面了。
    return "multipart/form-data; boundary=" + boundary_;
}

auto MultipartWriter::validate() const -> Result<void> {
    if (parts_.empty()) {
        return form_invalid("multipart body has no part: a multipart/form-data body carries at least one part");
    }
    if (boundary_.empty() || boundary_.size() > AURORA_BOUNDARY_MAX_LENGTH ||
        std::any_of(boundary_.begin(), boundary_.end(), [](char unit) { return !is_boundary_char(unit); })) {
        return form_invalid("multipart boundary must be 1.." + std::to_string(AURORA_BOUNDARY_MAX_LENGTH) +
                            " characters drawn from letters, digits and '()+_,-./:=? (a space is not accepted: "
                            "it would have to be quoted inside the Content-Type parameter)");
    }
    for (std::size_t index = 0; index < parts_.size(); ++index) {
        const auto &part = parts_.at(index);
        const auto label = part_label(index);
        if (part.name.empty()) {
            return form_invalid(label +
                                " has an empty name: RFC 7578 §4.2 requires the name parameter on every "
                                "part of a multipart/form-data body");
        }
        if (has_line_terminator(part.name)) {
            return form_invalid(label +
                                " name contains CR or LF: it is written onto the part's header line, so "
                                "one of those would end that line and let the rest be read as headers");
        }
        if (part.filename.has_value() && (part.filename->empty() || has_line_terminator(*part.filename))) {
            return form_invalid(label +
                                " filename is empty or contains CR or LF: the parameter shares the part's "
                                "header line, and an empty filename is not a valid file part");
        }
        if (part.content_type.has_value() && has_line_terminator(*part.content_type)) {
            return form_invalid(label + " content type contains CR or LF");
        }
        // 边界撞进部件头：那一行会被对端读成分隔行，体的结构与内容就此分家。
        const auto collides = [this](std::string_view text) {
            return !text.empty() && text.find(boundary_) != std::string_view::npos;
        };
        if (collides(part.name) || collides(part.filename.value_or(std::string{})) ||
            collides(part.content_type.value_or(std::string{}))) {
            return form_invalid(label +
                                " repeats the boundary inside its header text: the peer would see a "
                                "delimiter line and stop reading this part");
        }
        // 内存部件逐字节查一次；文件部件不查——那要先读完整份，与惰性读的承诺正相反。库生成的边界与
        // 文件内容相撞的概率约等于零，用固定边界上传含该字符串的文件则是调用方的契约，不在这里兜。
        if (part.path.empty() &&
            std::search(part.data.begin(), part.data.end(), boundary_.begin(), boundary_.end()) != part.data.end()) {
            return form_invalid(label +
                                " carries a payload that contains the boundary: the peer would treat that "
                                "occurrence as a delimiter line and cut the body short");
        }
    }
    return {};
}

auto MultipartWriter::apply_to(Request &request) -> void {
    request.set_header("Content-Type", content_type());
    request.body(*this);  // 只挂引用：本对象的生命期须覆盖整次交换（口径见公共头）
}

auto MultipartWriter::render_part_head(std::size_t index) const -> std::vector<char> {
    const auto &part = parts_.at(index);
    std::string out;
    out.append("--");
    out.append(boundary_);
    out.append(AURORA_CRLF);
    out.append("Content-Disposition: form-data; name=");
    out.append(quote(part.name));
    if (part.filename.has_value()) {
        out.append("; filename=");
        out.append(quote(*part.filename));
    }
    out.append(AURORA_CRLF);
    if (part.content_type.has_value()) {
        out.append("Content-Type: ");
        out.append(*part.content_type);
        out.append(AURORA_CRLF);
    }
    out.append(AURORA_CRLF);  // 部件头与正文之间那行空行：缺它整条体就不合 MIME
    return std::vector<char>{out.begin(), out.end()};
}

auto MultipartWriter::open_file(const Part &part) -> Result<void> {
    file_stream_.close();
    // 目录必须先拦：POSIX 下 open(O_RDONLY) 对目录会成功，读取时才报 EISDIR，而 libstdc++ 的
    // basic_filebuf::underflow 对此无条件抛出（不看流的异常掩码）。不拦的话异常会穿过 `next_chunk()`。
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path{part.path}, ec)) {
        return file_unreadable("multipart: part path is a directory " + part.path);
    }
    try {
        file_stream_.open(part.path, std::ios::in | std::ios::binary);
    } catch (const std::ios_base::failure &) {
        return file_unreadable("multipart: cannot open " + part.path);
    }
    if (!file_stream_.is_open()) {
        return file_unreadable("multipart: cannot open " + part.path);
    }
    return {};
}

auto MultipartWriter::read_open_file(const Part &part, std::size_t length) -> Result<std::vector<char>> {
    std::vector<char> buf(length, '\0');
    std::streamsize got = 0;
    try {
        file_stream_.read(buf.data(), static_cast<std::streamsize>(length));
        got = file_stream_.gcount();
    } catch (const std::ios_base::failure &) {
        // 读到一半的 I/O 失败（EISDIR / EIO / EACCES 之类）同样降级为 Result 错误，不外抛。
        file_stream_.close();
        return file_unreadable("multipart: read failed partway through " + part.path);
    }
    buf.resize(static_cast<std::size_t>(got));
    if (got == 0 && file_stream_.bad()) {
        file_stream_.close();
        return file_unreadable("multipart: read failed on " + part.path);
    }
    return buf;
}

auto MultipartWriter::next_chunk() -> Result<std::vector<char>> {
    if (!failure_checked_) {  // 同一份判据只算一次：逐段重扫内存正文会让长体流变成平方复杂度
        const auto check = validate();
        failure_ = check ? std::nullopt : std::optional<Error>{check.error()};
        failure_checked_ = true;
    }
    if (failure_.has_value()) {
        return Result<std::vector<char>>{*failure_};
    }
    while (true) {
        switch (stage_) {
            case Stage::PartHead: {
                auto piece = render_part_head(part_index_);
                body_offset_ = 0;
                stage_ = Stage::PartBody;
                return piece;  // 部件头恒非空（边界行加空行就有好几十字节）
            }
            case Stage::PartBody: {
                const auto &part = parts_.at(part_index_);
                if (!part.path.empty()) {
                    if (body_offset_ == 0) {
                        const auto opened = open_file(part);
                        if (!opened) {
                            return Result<std::vector<char>>{opened.error()};
                        }
                    }
                    auto piece = read_open_file(part, detail::AURORA_HTTP_WRITE_CHUNK);
                    if (!piece) {
                        return piece;
                    }
                    if (piece.value().empty()) {  // 零长文件与读完的判据相同：本段没有正文，直接收尾
                        file_stream_.close();
                        stage_ = Stage::PartTail;
                        continue;
                    }
                    body_offset_ += static_cast<std::uint64_t>(piece.value().size());
                    return piece;
                }
                if (body_offset_ >= part.data.size()) {
                    stage_ = Stage::PartTail;
                    continue;  // 零长正文不产出空段：空段的含义只有「整条体流交完了」
                }
                const auto rest = part.data.size() - static_cast<std::size_t>(body_offset_);
                const auto take = std::min(rest, detail::AURORA_HTTP_WRITE_CHUNK);
                const auto begin = part.data.begin() + static_cast<std::ptrdiff_t>(body_offset_);
                body_offset_ += static_cast<std::uint64_t>(take);
                return std::vector<char>{begin, begin + static_cast<std::ptrdiff_t>(take)};
            }
            case Stage::PartTail: {
                ++part_index_;
                stage_ = part_index_ < parts_.size() ? Stage::PartHead : Stage::Epilogue;
                body_offset_ = 0;
                return std::vector<char>{AURORA_CRLF.begin(), AURORA_CRLF.end()};
            }
            case Stage::Epilogue: {
                stage_ = Stage::Done;
                std::string out;
                out.append("--");
                out.append(boundary_);
                out.append("--");
                out.append(AURORA_CRLF);
                return std::vector<char>{out.begin(), out.end()};
            }
            case Stage::Done:
                return std::vector<char>{};  // 幂等：交完之后恒交空段
        }
    }
}

auto MultipartWriter::total_size() const -> std::optional<std::uint64_t> {
    // 校验不过时长度毫无意义，而给出长度会让发起侧写出 `Content-Length`；交 `nullopt` 把本次推到分块形态
    // 上，`next_chunk()` 仍会在写出第一段落之前如实失败——宁可少一句声明，也不报一个对不上的长度。
    if (!validate().ok()) {
        return std::nullopt;
    }
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < parts_.size(); ++index) {
        const auto &part = parts_.at(index);
        total += static_cast<std::uint64_t>(render_part_head(index).size());
        total += AURORA_PART_TAIL_LENGTH;  // 正文之后的收尾 CRLF
        if (part.path.empty()) {
            total += part.data.size();
            continue;
        }
        const auto size = file_length(part.path);
        if (!size.has_value()) {
            return std::nullopt;
        }
        total += *size;
    }
    return total + AURORA_EPILOGUE_OVERHEAD + boundary_.size();  // `--` + 边界 + `--` + CRLF
}

auto MultipartWriter::restart() -> bool {
    file_stream_.close();
    stage_ = Stage::PartHead;
    part_index_ = 0;
    body_offset_ = 0;
    return true;
}

}  // namespace aurora::net::http
