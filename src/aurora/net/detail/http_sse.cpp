// http_sse.cpp —— `text/event-stream` 增量解析器的实现（口径见 `http_sse.h` 的文件头）。
#include "http_sse.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"

namespace aurora::net::detail {
namespace {

/// @brief 流首的 UTF-8 BOM（`EF BB BF`）：规范明写解码前剥掉它。
constexpr std::string_view AURORA_SSE_BOM{"\xEF\xBB\xBF"};

/// @brief 是否为 BOM 的**严格前缀**（1~2 枚）：判不出来的字节得挂住等下一段，不能先当正文切行。
/// @param bytes 手上已有的流首字节（长度小于 BOM 才有意义）。
/// @return 是前缀为 true。
[[nodiscard]] auto is_bom_prefix(std::string_view bytes) -> bool {
    return bytes.size() < AURORA_SSE_BOM.size() && AURORA_SSE_BOM.starts_with(bytes);
}

/// @brief 剥掉字段值的一个前导空格（`RFC`-系文法的通例：`data: x` 的值是 `x` 而不是 ` x`）。
/// @param value 冒号之后的原文。
/// @return 剥后视图；只剥一枚，第二个空格是正文。
[[nodiscard]] auto strip_one_space(std::string_view value) -> std::string_view {
    if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    return value;
}

/// @brief 解 `retry` 的窗口：只认纯 ASCII 数字，其余形态一律「当没给」。
///
/// 「解不出即忽略」是规范的原文（非数字的 retry 字段不改变重连时间），本层不猜也不报错：服务端写错一枚
/// 心跳字段不该让一条好端端的流断掉。上限 15 位是为了不撞 `int64` 的毫秒溢出——更长的那串本来就不像
/// 任何真实的重连窗口。
/// @param value 字段值原文。
/// @param out 解出的毫秒数（仅在返回真时有意义）。
/// @return 解出为 true。
[[nodiscard]] auto parse_retry_window(std::string_view value, std::int64_t &out) -> bool {
    if (value.empty() || value.size() > 15) {
        return false;
    }
    std::int64_t acc = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        acc = (acc * 10) + static_cast<std::int64_t>(ch - '0');
    }
    out = acc;
    return true;
}

}  // namespace

SseStreamParser::SseStreamParser(SseParserSinks sinks) : sinks_(std::move(sinks)) {}

SseStreamParser::SseStreamParser(SseStreamParser &&other) noexcept
    : sinks_(std::move(other.sinks_)),
      line_(std::move(other.line_)),
      data_(std::move(other.data_)),
      event_type_(std::move(other.event_type_)),
      failure_(std::move(other.failure_)),
      pending_cr_(other.pending_cr_),
      bom_checked_(other.bom_checked_),
      paused_(other.paused_) {
    other.pending_cr_ = false;
}

auto SseStreamParser::operator=(SseStreamParser &&other) noexcept -> SseStreamParser & {
    if (this == &other) {
        return *this;
    }
    sinks_ = std::move(other.sinks_);
    line_ = std::move(other.line_);
    data_ = std::move(other.data_);
    event_type_ = std::move(other.event_type_);
    failure_ = std::move(other.failure_);
    pending_cr_ = other.pending_cr_;
    bom_checked_ = other.bom_checked_;
    paused_ = other.paused_;
    other.pending_cr_ = false;
    return *this;
}

SseStreamParser::~SseStreamParser() = default;

auto SseStreamParser::push(std::string_view bytes) -> Result<void> {
    if (failure_.has_value()) {
        return Result<void>{*failure_};
    }
    // 流首 BOM 可能横跨两段：判不出来的那 1~2 枚先挂在本实例里，不提前当正文切行——否则
    // 「第一段只有 EF BB」会被切成一行假字段，宿主收到一条服务端从没写过的事件。
    std::string_view input = bytes;
    if (!bom_checked_) {
        head_.append(bytes);
        if (is_bom_prefix(head_)) {
            return Result<void>{};  // 还判不出来：等下一段再定案
        }
        if (head_.starts_with(AURORA_SSE_BOM)) {
            head_.erase(0, AURORA_SSE_BOM.size());
        }
        bom_checked_ = true;
        input = std::string_view{head_};
    }

    std::size_t offset = 0;
    while (offset < input.size()) {
        if (pending_cr_) {
            pending_cr_ = false;
            if (input[offset] == '\n') {
                ++offset;  // CRLF 的 LF 是同一枚终止符的另一半，不另起一行
                continue;
            }
        }
        const std::size_t next = input.find_first_of("\r\n", offset);
        const std::size_t stop = (next == std::string_view::npos) ? input.size() : next;
        line_.append(input.substr(offset, stop - offset));
        offset = stop;
        if (line_.size() > AURORA_SSE_MAX_LINE_BYTES) {
            return fail(make_error(
                ErrorCode::NetFramerOverflow,
                ErrorParams{{"detail", "event-stream line exceeds the built-in line limit of 1 MiB"}}));
        }
        if (next == std::string_view::npos) {
            break;  // 本段没有终止符了：剩下的字节属于正在凑的那一行
        }
        // 命中终止符：CR 当场结算一行并挂住「下一枚可能是 LF」，LF 直接结算。
        const bool was_cr = input[offset] == '\r';
        ++offset;
        auto consumed = consume_line(std::string_view{line_});
        line_.clear();
        if (!consumed) {
            return consumed;
        }
        if (was_cr) {
            pending_cr_ = true;
        }
    }
    head_.clear();
    return Result<void>{};
}

auto SseStreamParser::finish() -> Result<void> {
    if (failure_.has_value()) {
        return Result<void>{*failure_};
    }
    // 上一枚 CR 已经结算过它那一行，这里只把挂起位收干净。
    pending_cr_ = false;
    if (!line_.empty()) {
        // 末行缺终止符仍是一条**完整字段行**（服务端写完 `data: x` 就断了），按行分发是对的。
        auto consumed = consume_line(std::string_view{line_});
        line_.clear();
        if (!consumed) {
            return consumed;
        }
    }
    // 没有空行收尾的那条事件在这里丢弃：补发等于把半条消息当一条交出去。
    data_.clear();
    event_type_.clear();
    return Result<void>{};
}

auto SseStreamParser::buffered_bytes() const -> std::size_t {
    return line_.size() + data_.size() + event_type_.size() + head_.size();
}

auto SseStreamParser::consume_line(std::string_view line) -> Result<void> {
    if (line.empty()) {
        return dispatch();
    }
    if (line.front() == ':') {
        // 注释行（心跳用它）：值同样剥一个前导空格，其余原文照交。
        if (sinks_.on_comment) {
            sinks_.on_comment(strip_one_space(line.substr(1)));
        }
        return Result<void>{};
    }
    const std::size_t colon = line.find(':');
    const std::string_view field = (colon == std::string_view::npos) ? line : line.substr(0, colon);
    const std::string_view value =
        (colon == std::string_view::npos) ? std::string_view{} : strip_one_space(line.substr(colon + 1));
    if (field == "event") {
        event_type_ = std::string{value};
    } else if (field == "data") {
        // 规范的连接形态是「值 + 一枚 LF」，每行都加：只有一行裸 `data` 时缓冲是 "\n" 而非空串，
        // 于是那条事件照样派发、消息体为空串（本层若省掉这枚 LF 就会把它判成「没有 data 字段」而吞掉）。
        data_.append(value);
        data_.push_back('\n');
    } else if (field == "id") {
        // 含 NUL 的 id 整条忽略（规范的原文是「ignore the field」）：续传标识里塞进 NUL 之后线上没法表达。
        if (value.find('\0') == std::string_view::npos && sinks_.on_last_id) {
            sinks_.on_last_id(std::string{value});
        }
    } else if (field == "retry") {
        std::int64_t window = 0;
        if (parse_retry_window(value, window) && sinks_.on_retry) {
            sinks_.on_retry(std::chrono::milliseconds{window});
        }
    }
    // 未知字段忽略而不是报错：那是 text/event-stream 的向后兼容机制。
    const std::size_t event_bytes = data_.size() + event_type_.size();
    if (event_bytes > AURORA_SSE_MAX_EVENT_BYTES) {
        return fail(make_error(ErrorCode::NetBufferLimit,
                               ErrorParams{{"detail",
                                            "event-stream record exceeds the built-in "
                                            "record limit of 4 MiB before its blank line"}}));
    }
    return Result<void>{};
}

auto SseStreamParser::dispatch() -> Result<void> {
    if (data_.empty()) {
        // 数据缓冲为空即不派发（注释之后、或只有 event/id/retry 的那一段空行）；但事件类型要清回去，
        // 否则下一块会继承这一块没成形的类型名。
        event_type_.clear();
        return Result<void>{};
    }
    // 连接用的那枚 LF 是**行与行之间**的，最后一行后面没有另一行，故它不属于消息体（规范把「去掉末枚 LF」
    // 排在「缓冲为空即不派发」之后，顺序决定了裸 `data` 那一条仍然算一条事件、只是体为空串）。
    if (data_.back() == '\n') {
        data_.pop_back();
    }
    SseParsedEvent event;
    event.type = event_type_.empty() ? std::string{"message"} : std::move(event_type_);
    event.data = std::move(data_);
    event_type_.clear();
    data_.clear();
    if (!paused_ && sinks_.on_event) {
        if (!sinks_.on_event(event)) {
            paused_ = true;
        }
    }
    return Result<void>{};
}

auto SseStreamParser::fail(Error error) -> Result<void> {
    failure_ = std::move(error);
    return Result<void>{*failure_};
}

}  // namespace aurora::net::detail
