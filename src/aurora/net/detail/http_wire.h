// http_wire.h —— HTTP/1.1 报文的字节层（仅库实现可见，不属公共 API）。
//
// 为何单列：「请求行与字段的序列化」和「应答状态行、头部块与分块体的解析」是纯编码与纯解析，
// 把它们从套接字里剥出来才能做到三件事——
// - 畸形输入的每条分岔可在零 socket、零线程的条件下逐值断言（真实远端不会配合你造出「chunk
//   长度写成八进制」这种现场，而这一类恰恰是最容易被漏掉的一支）。
// - 上层只管「把字节喂进去、把成品取出来」，不自己碰文法，也就没有第二套解析口径可漂。
// - 头部注入（值里带 CR/LF）这类安全判据落在这里才谈得上「发出之前就已拦住」，落到套接字层
//   就只能事后解释。
//
// 四条口径：
// - 解析产出沿用 `proxy_wire.h` 的三态：成功且有值 = 报文完整、成功且空 = 还没读够（那不是错误，
//   调用方据此继续喂字节）、失败 = 不合文法或超出上限。
// - **上限恒在**：头部块与单块长度都有硬上限，超限即判畸形而不是继续长（无界缓冲不是兼容性，
//   是拒绝服务）。
// - **`Content-Length` 与 `Transfer-Encoding` 并存一律判协议错**：两条长度声明同时成立时，链路上
//   每个中间代理各自采信哪一条是不可知的，报文走私（request smuggling）的全部本钱就在这条歧义上；
//   本库不猜，直接断开。
// - 字段名的原始拼写在解析时保留、比较时按 ASCII 大小写不敏感（`Headers` 的既有口径），序列化时
//   不改写调用方给的拼写——「发出去的头和写下来的头长得一样」是对端排查的前提。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"

namespace aurora::net::wire {

/// @brief 应答头部块（状态行 + 字段）的解析上限（字节）。
///
/// 取值依据：现实里最啰嗦的应答头（多枚 `Set-Cookie` 加安全策略字段）也在 16 KiB 量级，32 KiB 已
/// 是两倍余量；不设上限则一个持续不发空行、或把头部块切成 1 字节一片的对端能把读缓冲无界撑大。
inline constexpr std::size_t AURORA_HTTP_MAX_HEAD_BYTES = std::size_t{32} * 1024;

/// @brief 单个分块长度字段的上限（字节）：比它大的长度声明一律判畸形。
///
/// 这一枚不是「响应体上限」——体上限由调用方给（`http-body-too-large` 的判据）。它防的是长度字段
/// 本身写成一个巨大数（`FFFFFFFFFFFFFFFF`）时被解析成负数或回绕，那会让下游的缓冲算术出错。
inline constexpr std::uint64_t AURORA_HTTP_MAX_CHUNK_BYTES = std::uint64_t{64} * 1024 * 1024;

/// @brief 应答的头部块：状态行 + 字段；其后的字节属于消息体。
struct ResponseHead {
    std::uint16_t status{0};  ///< 三位状态码；只在头部块完整时给出
    std::string reason;  ///< 原因短语（原样保留，缺省为空——HTTP/2 风格的对端常常不给）
    Headers headers;  ///< 应答字段（同名保序、ASCII 大小写不敏感存取）
    std::size_t header_bytes{0};  ///< 状态行 + 头部块 + 空行占用的字节数，其后即消息体首字节
};

/// @brief 消息体的分帧形态，由头部块唯一决定。
enum class BodyFraming : std::uint8_t {
    None,  ///< 本应答没有消息体（1xx / 204 / 304，或对 HEAD 请求的应答）
    Length,  ///< `Content-Length` 给定确切长度
    Chunked,  ///< `Transfer-Encoding: chunked`，分块解码后长度未知
    UntilClose,  ///< 既无长度也非分块：体到对端关闭写向为止（HTTP/1.0 形态）
};

/// @brief 解析应答头部块（状态行 + 字段 + 空行）。
///
/// 只吃掉头部块自身占用的字节：`header_bytes` 之后属于消息体，绝不被本函数消费。状态行要求
/// `HTTP/1.x SP 3DIGIT [SP reason]` 形态；字段行允许 obs-fold（前导空格 / 制表符续行，
/// `RFC 9112 §5.2` 已废弃但仍会出现），按折行合并进上一条字段值。
/// @param buffered 已读入的连续字节（可以只到头部块中途）。
/// @return 成功且有值 = 头部块完整；成功且空 = 还需要更多字节；失败 = 不合文法或超出头部上限。
[[nodiscard]] auto parse_response_head(std::string_view buffered) -> Result<std::optional<ResponseHead>>;

/// @brief 由头部块判定消息体的分帧形态。
///
/// 判据次序是规范性的：先看 `Transfer-Encoding`（它优先于 `Content-Length` 且二者并存即判畸形），
/// 再按状态码排除无体形态，最后落到 `Content-Length` 或「读到关闭为止」。
/// @param head 已解析的头部块。
/// @param request_is_head 请求方法是否为 HEAD（HEAD 的应答恒无体，即便状态码带体）。
/// @return 分帧形态；歧义或本库不实现的形态为错误（`http-protocol-error`）。
[[nodiscard]] auto framing_of(const ResponseHead &head, bool request_is_head) -> Result<BodyFraming>;

/// @brief 序列化请求行与请求字段（不含消息体）。
///
/// 产出以 CRLF 结尾的空行收尾，可直接与消息体拼接后写出。函数负责三处校验与两处补齐：
/// - 方法文本必须是 token（未知方法在发起之前即拒，而不是让对端回一句 501 再解释）；
/// - 字段名必须是 token、字段值不得含 CR / LF（头部注入的唯一入口就是这两个字符）；
/// - 未给 `Host` 时按目标 URL 补齐（含非缺省端口），未给请求目标（path 为空）时补 `/`。
///
/// 请求目标有两种形态（`RFC 9112 §3.2.2` 把它列为 origin-form / absolute-form 等五种之一，客户端只用其中
/// 两种）：origin-form 是「连到目标本身」时的写法，absolute-form 是「把这条请求交给转发代理」时的写法。
/// @param method_text 方法文本（大写的 `GET` / `POST` 之类，取自 `http::method_text()`）。
/// @param target 已解析的目标 URL；只用 path / query 拼请求目标，用 host / port 拼 `Host`。
/// @param fields 请求字段（原始拼写原样写出，同名多条按登记次序全部写出）。
/// @param absolute_form 请求目标是否写成绝对形态（`scheme://authority/path?query`，恒不含片段与 userinfo）；
///                      假 = origin-form（缺省，直连与隧道两条路都是它）。
/// @return 请求字节；不合文法为 `http-request-invalid`。
[[nodiscard]] auto render_request_head(std::string_view method_text, const Url &target, const Headers &fields,
                                       bool absolute_form = false) -> Result<std::string>;

/// @brief 分块传输（`Transfer-Encoding: chunked`）的解码状态机。
///
/// 跨多次投递累积，故「块长度行被切成两半」「块数据尾部的 CRLF 落在下一次读里」这些形态都是
/// 常态而不是畸形。只解码块数据本身，块扩展参数（`;name=value`）按 `RFC 9112 §7.1` 忽略；
/// 终止块之后的头部块是 trailer，经 `trailers()` 取回。
/// @note Thread: single-threaded（每连接一个实例，随该连接的读回调串行使用）
/// @note Side-effects: none（全部状态在本对象内）
class ChunkedDecoder {
  public:
    /// @brief 送入一段字节并推进状态机。
    /// @param bytes 新读到的原始字节（可以是半行、半块）。
    /// @return 成功 = 状态机仍可继续；失败 = 文法不合（长度字段非十六进制、缺 CRLF、长度越上限）
    ///         或在终止块之后又收到字节（那是上一条消息的开头，本层不越界解释）。
    [[nodiscard]] auto push(std::string_view bytes) -> Result<void>;

    /// @brief 取走已解码出的明文（取完即从内部缓冲移除，因此不会重复交付）。
    /// @param out 追加目标；本函数只追加、不清空调用方的缓冲。
    auto drain(std::vector<char> &out) -> void;

    /// @brief 是否已见到终止块（长度为 0 的块）。
    /// @return 见到终止块为真；此后 `push()` 只接受 trailer 行。
    [[nodiscard]] auto complete() const -> bool { return done_; }

    /// @brief 已完整读入的 trailer 字段（多数对端没有，返回空表是常态）。
    /// @return trailer 字段表；终止块之前为空。
    [[nodiscard]] auto trailers() const -> const Headers & { return trailers_; }

  private:
    /// @brief 状态机的阶段，决定缓冲区头部那一段字节该按什么文法读。
    enum class Phase : std::uint8_t {
        SizeLine,  ///< 读块长度行（十六进制 + 可选块扩展 + CRLF）
        Data,  ///< 消费当前块的数据字节
        DataCrlf,  ///< 块数据之后那对 CRLF
        TrailerLine,  ///< 终止块之后的 trailer 行
        Ended,  ///< trailer 块已由空行收尾，本条消息结束
    };

    std::vector<char> buffer_;  ///< 尚未解析完的原始字节
    std::vector<char> pending_;  ///< 已解码、等待 `drain()` 取走的明文
    Headers trailers_;  ///< 终止块之后的 trailer 字段
    Phase phase_{Phase::SizeLine};  ///< 当前阶段
    std::uint64_t remaining_{0};  ///< 当前块还剩多少数据字节
    bool done_{false};  ///< 是否已见终止块（长度为 0 的块）
};

/// @brief 把「一段段读到的原始字节」装配成一条完整应答的接收侧状态机。
///
/// 它把本文件已有的三件（`parse_response_head` 的三态头部解析、`framing_of` 的分帧判定、
/// `ChunkedDecoder` 的分块解码）按线上次序串起来，只回答发起侧最关心的三问：
/// - **头部块全了吗**：未全前收到的字节都归头部缓冲（含 body 起始那一段，它可能和头部同批抵达）。
/// - **体分帧是什么**：头部一全即由 `framing_of` 定形态，`HEAD` / 1xx / 204 / 304 恒无体。
/// - **体读全了吗**：`Length` 按声明长度、`Chunked` 按终止块、`UntilClose` 按对端 FIN（经
///   `peer_closed()` 告知），任一形态触达上限即 `http-body-too-large`。
///
/// **范围是「一条应答」**：HTTP/1.1 流水线（pipelining）不在本库面内（见提案 §15），故一条连接上
/// 每个请求周期只装配一条应答，完整之后再收到字节即判畸形——与 `ChunkedDecoder`「终止块之后不收字节」
/// 是同一条纪律，把「读过头把下一条的开头吃进本条」这类分帧错位挡在本层。
///
/// 只做接收侧，不做发送侧、不碰套接字：故可在零网络下逐值断言畸形、越限与分帧错位。
/// @note Thread: single-threaded（每连接一个实例，随该连接的读回调串行使用）
/// @note Side-effects: none（全部状态在本对象内）
class ResponseAssembler {
  public:
    /// @brief 构造。
    /// @param max_body_bytes 物化正文上限（`std::nullopt` = 不设上限）；越限落 `http-body-too-large`。
    /// @param request_is_head 对应请求是否为 `HEAD`（决定应答恒无体的判定）。
    ResponseAssembler(std::optional<std::uint64_t> max_body_bytes, bool request_is_head);

    /// @brief 送入一段读到的原始字节并推进装配。
    /// @param bytes 新读到的连续字节（可只覆盖头部块中途、或同时覆盖头部尾与体首段）。
    /// @return 成功 = 仍可继续；失败 = 文法不合、分帧歧义、体越上限、或完整之后又收到字节。
    [[nodiscard]] auto push(std::string_view bytes) -> Result<void>;

    /// @brief 告知对端已关闭写向（FIN）：`UntilClose` 形态据此判定体已读全。
    /// @return 恒成功（除 `UntilClose` 外的形态下它是空操作）。
    [[nodiscard]] auto peer_closed() -> Result<void>;

    /// @brief 应答是否已装配完整（头部 + 按分帧的体全部到位）。
    /// @return 完整为真；此时 `head()` 与已解码体可取。
    [[nodiscard]] auto complete() const -> bool { return complete_; }

    /// @brief 已解析的头部块（仅 `head_ready()` 为真时有意义）。
    /// @return 头部块只读引用。
    [[nodiscard]] auto head() const -> const ResponseHead & { return head_; }

    /// @brief 头部块是否已解析完成。
    /// @return 已定为真。
    [[nodiscard]] auto head_ready() const -> bool { return head_ready_; }

    /// @brief 已确定的消息体分帧形态（`head_ready()` 之前为 `None`）。
    /// @return 分帧形态。
    [[nodiscard]] auto framing() const -> BodyFraming { return framing_; }

    /// @brief 取走已解码的明文体（取完即从内部移除，不重复交付）。
    /// @param out 追加目标；只追加、不清空调用方缓冲。
    auto drain_body(std::vector<char> &out) -> void;

  private:
    /// @brief 装配阶段。
    enum class Phase : std::uint8_t {
        Head,  ///< 仍在收集 / 解析头部块
        Body,  ///< 按分帧形态收集消息体
        Ended,  ///< 本条消息完整
    };

    /// @brief 头部块解析完成后，按分帧初始化体收集态并就地消费缓冲里已有的体字节。
    /// @return 分帧判定或上限核算的失败；成功为可继续。
    [[nodiscard]] auto enter_body_phase() -> Result<void>;

    /// @brief 把一段明文体并入交付缓冲并做累计上限核算。
    /// @param bytes 本段明文。
    /// @return 越限为 `http-body-too-large`；否则成功。
    [[nodiscard]] auto absorb(std::string_view bytes) -> Result<void>;

    std::optional<std::uint64_t> max_body_bytes_;  ///< 物化上限（nullopt = 不限）
    bool request_is_head_{false};  ///< HEAD 请求判定
    Phase phase_{Phase::Head};  ///< 当前阶段
    bool head_ready_{false};  ///< 头部块是否已解析
    bool complete_{false};  ///< 整条应答是否已装配完整
    std::string buffer_;  ///< 原始字节累积（头部未全前不喂解码器）
    ResponseHead head_;  ///< 已解析头部
    BodyFraming framing_{BodyFraming::None};  ///< 头部解析后确定的分帧形态
    ChunkedDecoder chunked_;  ///< Chunked 形态的解码器
    std::vector<char> pending_;  ///< 已解码、待 `drain_body()` 取走的明文
    std::uint64_t content_remaining_{0};  ///< Length 形态还剩多少体字节
    std::uint64_t produced_{0};  ///< 本条已解码的累计明文（上限核算唯一计数，不随 drain 回落）
};

}  // namespace aurora::net::wire
