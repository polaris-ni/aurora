#pragma once

// http_body.h —— 请求体的线上形态计划（§12 上传便捷面；仅库实现可见，不属公共 API）。
//
// 为何单列：一条请求的体走 `Content-Length` 还是 `Transfer-Encoding: chunked`，是同一条事实的两面——头部里
// 那句声明，与写完头部之后真正流出的字节，必须说同一件事。声明由 `Request::render_head()` 产出，字节由发起侧
// 的体泵产出；两处各自判一遍，任何一次改动（新增一种体形态、允许调用方自报长度、体源改判总长）都会让「声明
// 五个字节却发出分块」这类线上错位成为可能，而那是对端无从报错、只能等超时或错读到下一段的静默故障。本件把
// 判定收成一个函数，渲染侧与写出侧各问一次、得到同一份计划，故二者不可能分岔。
//
// 四条口径（规范性，落码与文档回写以此为准）：
// - **调用方写过的永远优先**：`Content-Length` 或 `Transfer-Encoding` 只要出现在请求字段里，计划就照它定，
//   本层不改写、不补齐、不加第二枚（与 `Accept-Encoding` / `X-Request-Id` 同一条纪律：协商与声明是调用方的
//   表达）。附带一条：声明了 `Content-Length` 的流式体按该长度写线上声明，写出侧仍以源的实际段为准——
//   「我知道这文件多大但我不想整份进内存」正是那种声明的用意。
// - **两枚字段并存即拒**：`RFC 9110 §8.6` 让转发方在冲突时以 `Transfer-Encoding` 为准并删掉长度，那是替别人
//   收拾报文；发起侧自己声明两枚是调用点写错，当场落 `http-request-invalid` 比替它猜哪枚算数更有价值。
// - **无体的判定看形态不看字节数**：长度为零且没有源也没有声明 = 无体，两枚字段都不补。零长内存体不补
//   `Content-Length: 0` 是刻意的：「没有体」与「体长为零」在线上等价，而前者不必让一条 `POST` 多出一种对端
//   可能另有解读的写法（`PUT` 一个零长资源同理）。
// - **分块只由「总长未知的源」触发**：内存体恒有确切长度；走分块的唯一条件是源自报总长未知且调用方没声明
//   长度。本库不会主动把一条有确切长度的请求改写成 `Transfer-Encoding: chunked`——那会改变中间层的转发行为，
//   而它不是调用方要求的事。
//
// @note Thread: single-threaded（每请求判定一次，无共享状态）
// @note Side-effects: none（只读 `Request`；源的 `total_size()` 由实现自定是否推进状态，本件约定它只读）

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {

/// @brief 写出泵一次交出的正文段上限（与传输层的写块粒度同值，故声明在此而非发起侧）。
///
/// 上限存在的理由是「体不必整份进内存」这条承诺只在分段成立时才兑现：一次 `next_segment()` 若把源的一整段
/// 原样搬上线，超大段会撞上传输层的待发字节上限（`StreamOptions::max_pending_bytes`）而当场失败。源自报的段
/// 若已超上限，泵替它切开——分块形态下切开只是多几个长度行，定长形态下切开不改变字节序列。
inline constexpr std::size_t AURORA_HTTP_WRITE_CHUNK = std::size_t{64} * 1024;

/// @brief 请求体的线上形态（与应答侧的 `wire::BodyFraming` 无关：那是收，这是发）。
enum class RequestBodyFraming : std::uint8_t {
    None,  ///< 本次请求没有消息体：两枚字段都不补，写出侧一段也不发
    Length,  ///< 确切长度：补（或沿用调用方声明的）`Content-Length`，体按原样连续写出
    Chunked,  ///< 长度未知：补（或沿用）`Transfer-Encoding: chunked`，体按分块写出并以终止块收尾
};

/// @brief 一次请求的体形态计划：判定一次，头部渲染与字节写出共用。
struct RequestBodyPlan {
    RequestBodyFraming framing{RequestBodyFraming::None};  ///< 线上形态
    std::optional<std::uint64_t> length;  ///< `framing == Length` 时的声明长度（字节），其余形态为空
    bool declared{false};  ///< 该形态出自调用方写下的字段（渲染侧据此决定「补齐」还是「原样」）
    bool has_body{false};  ///< 是否有待写出的体（`None` 恒假；决定 `Expect: 100-continue` 是否上线）
};

/// @brief 判定请求体的线上形态：`render_head()` 与体泵共用的唯一决策点。
/// @param request 待判定的请求（读它的 `body()` / `body_source()` / `headers()`）。
/// @return 成功为计划；两枚字段并存、声明的长度非纯十进制、或声明的传输编码不是 `chunked` 时为
///         `http-request-invalid`（detail 点名是哪一枚字段）。
[[nodiscard]] auto plan_request_body(const aurora::net::http::Request &request) -> Result<RequestBodyPlan>;

/// @brief 请求体的写出泵：把「一份内存体 / 一枚推送源」按计划切成可直接写出的线上字节段。
///
/// 它是 `plan_request_body()` 的第二位读者，也是**唯一**的产字节处：形态、分段、终止块都在这里定，发起侧
/// 只负责「拿一段、写一段、写完收摊」。三条纪律：
/// - **一段一问**：`next_segment()` 每次至多问源要一段（内存体按上限切片），故挂起的协程不会把整份体压在
///   栈上；返回空段即「此后再无字节」，调用方据此结束写循环，且空段不该被写给传输层（零长写没有语义）。
/// - **终止块由泵负责**：分块形态的 `0\r\n\r\n` 在源说结束之后由本泵补，绝不留给对端去猜一条流什么时候算完。
/// - **正文与框架分开计**：`payload_bytes()` 只数正文，分块的长度行与 CRLF 不计入——上传进度回答的是
///   「我的数据送到哪儿了」，不是「线上跑了多少字节」。
///
/// 每次尝试新建一只：游标不跨尝试，重放前要先把源 `restart()` 回原点（资格判据见 `BodySource`）。
/// @note Thread: single-threaded（随一条协程帧串行使用）
/// @note Side-effects: 经 `BodySource::next_chunk()` 读源（内置件读文件）
class RequestBodyPump {
  public:
    /// @brief 以请求与其形态计划构造（计划须由同一枚请求判出，否则形态与字节会各说一套）。
    /// @param request 待写出的请求（借引用：泵只读它的体，不延长其生命期）。
    /// @param plan 该请求的体形态计划。
    RequestBodyPump(const aurora::net::http::Request &request, const RequestBodyPlan &plan);

    RequestBodyPump(const RequestBodyPump &) = delete;
    auto operator=(const RequestBodyPump &) -> RequestBodyPump & = delete;
    RequestBodyPump(RequestBodyPump &&) = delete;
    auto operator=(RequestBodyPump &&) -> RequestBodyPump & = delete;
    ~RequestBodyPump() = default;

    /// @brief 交出下一段要写出的线上字节。
    /// @return 非空 = 原样写出这一段；空 = 已交完（含分块终止块已交出的情形）；失败 = 源读失败，原样上抛。
    ///         交完之后仍返回空段（幂等），调用方可放心循环到 `done()`。
    [[nodiscard]] auto next_segment() -> Result<std::vector<char>>;

    /// @brief 是否已交完（无待写字节）。
    /// @return 已交完为 true。
    [[nodiscard]] auto done() const -> bool { return stage_ == Stage::Finished; }

    /// @brief 是否有待写出的体（决定 `Expect: 100-continue` 的窗口要不要开）。
    /// @return 计划判定有体为 true。
    [[nodiscard]] auto has_body() const -> bool { return plan_.has_body; }

    /// @brief 已交出的**正文**字节数（不含分块框架）。
    /// @return 累计字节数。
    [[nodiscard]] auto payload_bytes() const -> std::uint64_t { return payload_bytes_; }

  private:
    /// @brief 泵的阶段：还在问来处要段 / 已全部交出（终止块交出即算全部）。
    enum class Stage : std::uint8_t { Pull, Finished };

    /// @brief 把一段正文按形态包成线上字节（分块形态走 `wire::render_request_chunk`，定长形态原样）。
    /// @param payload 正文段（非空）。
    /// @return 该写出的字节段。
    [[nodiscard]] auto frame(std::vector<char> payload) const -> std::vector<char>;

    /// @brief 正文见底时的收尾：该补终止块就补一枚，否则交出空段。
    /// @return 终止块字节或空段（同时把阶段推到 `Finished`）。
    [[nodiscard]] auto drain_to_end() -> std::vector<char>;

    const aurora::net::http::Request *request_;  ///< 体的来处（内存体或推送源，二者已由计划裁决）
    RequestBodyPlan plan_;  ///< 形态计划（复制一份：判定与写出读同一份事实）
    Stage stage_{Stage::Pull};  ///< 当前阶段
    std::size_t offset_{0};  ///< 内存体已写出的偏移
    std::vector<char> pending_;  ///< 源交出但超单段上限、还没写出的剩余正文
    bool terminator_pending_{false};  ///< 分块形态收尾时还欠一枚终止块
    std::uint64_t payload_bytes_{0};  ///< 已交出的正文字节数
};

}  // namespace aurora::net::detail
