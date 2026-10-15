#pragma once

/// @brief `multipart/form-data` 请求体的构造器：内存部件与惰性文件部件（CHG-018 §12 上传便捷面）。
/// @file net/http_multipart.h
///
/// 「把一份表单连同几个文件发出去」是 HTTP 侧最常被写坏的一件事：边界字符串手拼、部件头漏一行空行、
/// 文件整份读进内存、文件名里的引号把 `Content-Disposition` 顶穿。本类把这些收成一个 `BodySource`
/// 实现——线上字节只从 `next_chunk()` 一处产出，故头部里的 `Content-Type` 与实际流出去的体不可能各说
/// 一套（体形态的判定见 `detail::plan_request_body()`，那是渲染侧与写出侧共用的唯一决策点）。
///
/// 四条设计约束值得在写代码前知道：
///
/// - **文件部件惰性读**：`file()` 只登记路径，读出发生在 `next_chunk()` 里、一次至多一
///   `detail::AURORA_HTTP_WRITE_CHUNK`（与写出泵的同一段长上限），故上传一份大文件不会先把它整份搬进
///   内存。这是 `BodySource` 存在的头号理由，也是本类不给「先把文件读成 `std::vector<char>` 再交出去」
///   那条路的原因。
/// - **失败一律推迟到取字节时**：追加部件的方法返回自身引用（链式写法要能连下去），故它们不报错；
///   部件名里的 CR/LF、非法边界、边界与内存部件正文相撞、文件打不开或读到一半失败，都在
///   `next_chunk()` 的返回值上如实交出。要发起前就判一次，用 `validate()`——它同是那条纪律的唯一执行处，
///   不是另建一套校验。
/// - **边界由库生成，且可覆写**：缺省取一取自库内非密码学序列（`detail::noncrypto_u64()`，与重试抖动、
///   `X-Request-Id` 同源）的定长令牌，逐次构造互不相同；`boundary(std::string)` 供测试逐值断言线上字节、
///   也供对接固定格式的第三方。覆写值当场校验（`RFC 2046 §5.1` 的字符集与 70 字符上限），不合格者在
///   `validate()` 上如实失败，不会被写成一句对端无从解析的 `Content-Type`。
/// - **`total_size()` 交出的是完整线上长度**：含每段的边界行、部件头与收尾 CRLF，故它恒等于
///   `next_chunk()` 逐段交出之字节数之和；任一文件部件的长度取不到就返回 `nullopt`，本次改走
///   `Transfer-Encoding: chunked`——宁可少一句声明，也不报一个与实际写出对不上的长度。
///
/// @code
/// aurora::net::http::MultipartWriter form;
/// form.field("title", "夜间快照")
///     .file("image", "/tmp/shot.png")
///     .bytes("signature", std::vector<char>{'a', 'b'});
/// auto req = aurora::net::http::Request::post("https://example.com/upload");
/// form.apply_to(req);                      // 挂上 Content-Type 与本对象作为体源
/// @endcode
/// @note Thread: single-threaded（一次交换的体源由同一条协程串行推进；不可跨线程共享同一实例）
/// @note Side-effects: 文件部件按需读盘（`std::ifstream`），无其他外部效应

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/http.h"

namespace aurora::net::http {

/// @brief `multipart/form-data` 体的构造器：按追加次序把部件写成一条合法的 MIME 体流。
///
/// 继承 `BodySource` 因而不可拷贝、不可移动，且 `Request` 只持引用：本对象的生命期须覆盖整次交换，
/// 含重定向的每一跳与重试的每一次尝试（写出泵在协程下一次恢复时还要问它要第二段）。
/// @note Thread: single-threaded（随一条协程帧串行推进）
/// @note Side-effects: 文件部件按需读盘
class MultipartWriter final : public BodySource {
  public:
    /// @brief 一个部件的描述：字段名、可选的文件名与媒体类型，以及正文的来处。
    ///
    /// 这是查看用的值视图（Inspector 与测试据此核对「加过什么」），不复制正文之外的任何东西。
    /// 正文的来处由 `path` 是否为空区分：非空 = 文件部件（惰性读，`data` 恒空），空 = 内存部件。
    struct Part {
        std::string name;  ///< 部件的表单字段名（`Content-Disposition` 的 `name` 参数）
        std::optional<std::string> filename;  ///< 文件名参数；只有文件部件与非空 `bytes()` 才有
        std::optional<std::string> content_type;  ///< 部件的 `Content-Type`；未写则该部件不发这行
        std::string path;  ///< 文件部件的路径；内存部件为空
        std::vector<char> data;  ///< 内存部件的正文；文件部件为空
    };

    /// @brief 构造一具空表单，并当场生成边界。
    MultipartWriter();

    MultipartWriter(const MultipartWriter &) = delete;
    auto operator=(const MultipartWriter &) -> MultipartWriter & = delete;
    MultipartWriter(MultipartWriter &&) = delete;
    auto operator=(MultipartWriter &&) -> MultipartWriter & = delete;
    /// @brief 析构：随 `std::ifstream` 成员自行收尾，打开着的文件句柄就此关闭（不留待迟的读取）。
    ~MultipartWriter() override;

    /// @brief 追加一个文本部件（线上不带 `Content-Type`，由 `RFC 7578 §4.2` 的缺省 `text/plain` 接管）。
    /// @param name 字段名，原样写进 `Content-Disposition`；不可为空，且不得含 CR / LF。
    /// @param value 正文（UTF-8 文本按字节交出，本类不转码）。
    /// @return 自身引用，便于链式追加。
    auto field(std::string_view name, std::string_view value) -> MultipartWriter &;

    /// @brief 追加一个内存部件（已在手里的字节：小文件、签名片段、序列化出来的 JSON 块）。
    /// @param name 字段名；约束同 `field()`。
    /// @param data 正文；零长合法（表示一个空值部件）。
    /// @param filename 文件名参数；空串 = 不带（对端按普通字段而非文件处理）。
    /// @param content_type 媒体类型；空串 = 由本类补 `application/octet-stream`——任意字节被缺省成
    ///        `text/plain`（`RFC 7578 §4.2` 对无文件名部件的缺省判）比被当成二进制更容易出错。
    /// @return 自身引用，便于链式追加。
    auto bytes(std::string_view name, std::vector<char> data, std::string_view filename = {},
               std::string_view content_type = {}) -> MultipartWriter &;

    /// @brief 追加一个文件部件：只登记路径，读出留到取字节时按段进行。
    ///
    /// 文件名取路径的末段（`filename` 参数），媒体类型按扩展名查一张极简表（识别不了则
    /// `application/octet-stream`）。要送别的媒体类型用 `bytes()` 自己带正文。
    /// @param name 表单字段名；约束同 `field()`。
    /// @param path 文件路径；此处不校验可读性（打不开在 `next_chunk()` 如实失败）。
    /// @return 自身引用，便于链式追加。
    auto file(std::string_view name, std::string_view path) -> MultipartWriter &;

    /// @brief 覆写边界字符串（缺省是库生成的定长令牌）。
    ///
    /// 只在确有外部契约要求时使用：固定边界让测试可以逐字节断言线上形态，某些遗留服务端也要求边界
    /// 落在其白名单里。合法性（字符集与长度）当场记入待校验状态，`validate()` 与 `next_chunk()` 都据此拒。
    /// @param value 边界字符串；须落在 `RFC 2046 §5.1` 允许的字符集内、不超过 70 字符且不含 CR / LF。
    /// @return 自身引用，便于链式调用。
    auto boundary(std::string value) -> MultipartWriter &;

    /// @brief 查看边界字符串。
    /// @return 当前生效的边界（覆写过即覆写值，否则是构造时生成的那枚）。
    [[nodiscard]] auto boundary() const -> const std::string & { return boundary_; }

    /// @brief 本次体应挂的 `Content-Type` 值。
    /// @return `multipart/form-data; boundary=<边界>`（边界由字符集校验保证无需引用，故不带引号）。
    [[nodiscard]] auto content_type() const -> std::string;

    /// @brief 查看已登记的部件（按追加次序）。
    /// @return 部件描述序列的只读引用。
    [[nodiscard]] auto parts() const -> const std::vector<Part> & { return parts_; }

    /// @brief 校验整张表单能否成为一条合法体流：`next_chunk()` 失败处的同一份判据。
    ///
    /// 拒的条件：没有部件（一条空体不是合法的 `multipart`）、字段名为空或含 CR / LF、边界字符集或长度
    /// 越界、边界出现在任一部件头文本或内存部件正文里（那会让对端提前看见终止边界，把体读成另一份）。
    /// 文件的可读性不在这里判——它属取字节时才知道的事实。本函数每次调用都从头算（含对内存部件正文的
    /// 相撞扫描），是发起前的检查口；`next_chunk()` 用同一份判据但记住上次结果，不逐段重扫。
    /// @return 成功态表示整张表单可写成合法体流；失败持有点名原因的 `http-request-invalid`。
    [[nodiscard]] auto validate() const -> Result<void>;

    /// @brief 把本表单挂上一请求：置 `Content-Type` 并以自身为流式体源。
    ///
    /// 只动这两处，其余字段与旋钮留给调用方（`Client` 的鉴权、重试、超时一概不碰）。`Content-Type`
    /// 用 `set_header` 而非追加：同一个请求挂两枚边界是线上事故。
    /// @param request 目标请求。
    auto apply_to(Request &request) -> void;

    /// @brief 交出体流的下一段（部件头、文件正文的一段、收尾边界行各算一段）。
    /// @return 成功非空 = 本段线上字节；成功空 = 整条体流已交完；失败 = 校验或文件读取不过，
    ///         发起侧当场中止本次交换。零长正文的部件不产出空段（空段的含义只有「交完了」）。
    [[nodiscard]] auto next_chunk() -> Result<std::vector<char>> override;

    /// @brief 整条体流的线上长度（含边界行、部件头与 CRLF）。
    /// @return 全部文件部件的长度都取得到时返回确切总长；任一取不到（文件不存在、不可读、不是常规文件）
    ///         或校验不过时返回 `nullopt`，本次因此改走分块。
    [[nodiscard]] auto total_size() const -> std::optional<std::uint64_t> override;

    /// @brief 把游标放回原点：关掉打开着的文件、复位部件与段内偏移。
    ///
    /// 恒为 true——本类的状态是一列可重放的部件描述加一个游标，内存部件的正文与文件部件的路径都还在，
    /// 故挂 multipart 体的请求始终留在重试与重定向带体重放的白名单里（文件在两次尝试之间被换掉或删掉，
    /// 表现为下一次读取失败而非不可重放）。
    /// @return 恒 true。
    auto restart() -> bool override;

  private:
    /// @brief 体流游标所在段：部件头 / 部件正文 / 部件收尾 CRLF / 结束边界行 / 交完。
    enum class Stage : std::uint8_t { PartHead, PartBody, PartTail, Epilogue, Done };

    /// @brief 一句「请求方的声明本身有问题」的载荷（与 `Request::render_head()` 同族同码）。
    /// @param detail 点名是哪一处、为什么不能用。
    /// @return `http-request-invalid` 的错误对象。
    [[nodiscard]] static auto form_invalid(const std::string &detail) -> Error;

    /// @brief 把第 `index` 个部件的边界行与部件头拼成一段线上字节（恒非空）。
    /// @param index 部件下标。
    /// @return `--<边界>` 行、部件头各行，再加收尾的那一行 CRLF。
    [[nodiscard]] auto render_part_head(std::size_t index) const -> std::vector<char>;

    /// @brief 为文件部件打开读句柄（就地持有 `file_stream_`）。
    /// @param part 目标部件（`path` 非空）。
    /// @return 成功为 `Result<void>` 的成功态；打不开或路径指向目录时为 `io-file-not-found`。
    [[nodiscard]] auto open_file(const Part &part) -> Result<void>;

    /// @brief 从已打开的句柄读出一段正文（读出后不推进 `body_offset_`，由调用方负责）。
    /// @param part 目标部件（只取其路径用于失败细节）。
    /// @param length 期望段长（上限 `detail::AURORA_HTTP_WRITE_CHUNK`）。
    /// @return 成功为实际读到的字节（到文件尾则为剩余不足一段的长度，读尽时为空段）；读到一半失败为
    ///         `io-file-not-found`。
    [[nodiscard]] auto read_open_file(const Part &part, std::size_t length) -> Result<std::vector<char>>;

    /// @brief 取一个文件部件的长度（`Content-Length` 与分块判定都据它）。
    /// @param path 文件路径。
    /// @return 常规文件且长度可取则返回之；否则 `nullopt`（目录、不存在、非常规文件同一路径）。
    [[nodiscard]] static auto file_length(std::string_view path) -> std::optional<std::uint64_t>;

    std::vector<Part> parts_;  ///< 按追加次序登记的部件
    std::string boundary_;  ///< 生效的边界字符串
    Stage stage_{Stage::PartHead};  ///< 游标所在段
    std::size_t part_index_{0};  ///< 当前部件下标
    std::uint64_t body_offset_{0};  ///< 当前部件正文已交出的字节数
    std::ifstream file_stream_;  ///< 当前文件部件的读句柄（离开该部件即关）
    bool failure_checked_{false};  ///< `failure_` 是否是当前形态下算出来的
    std::optional<Error> failure_;  ///< 上次校验得出的推迟失败（空 = 合格）
};

}  // namespace aurora::net::http
