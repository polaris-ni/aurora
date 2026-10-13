// framer.cpp —— 字节流 → 完整消息的分帧三型实现（CHG-010 传输切片）。
//
// 本文件不出现 socket、平台头与线程：全部判据都是「缓冲里的字节怎么切」，因此在无网络条件下即可逐值
// 断言（`StreamTransport` 的交付只是它的字节来源之一）。
//
// 形态三则（规范性）：
// 1. **只有一枚缓冲**：`feed()` 先原样追加输入，再从缓冲前端逐帧交付。`Frame::bytes` 是这件缓冲的视图，
//    而缓冲的前缀只在回调返回（或抛出）之后才被抹掉，故「视图仅在回调期间有效」是实现的直接结果，
//    不需要调用方额外遵守什么。
// 2. **拉模型**：回调返回 `false` 即暂停，未交付的字节留在缓冲里由下一次 `feed()` 续投。本层不排队
//    帧、不复制整份消息，慢消费者的代价只是这件有硬上限的缓冲。
// 3. **失败即终态**：`net-framer-overflow` 与 `net-buffer-limit` 一旦置位，此后所有操作原样回报同一条
//    错误直到 `reset()`。分帧器不在超限处重同步——协议里一条超长的消息足以说明字节流已不可信。

#include "aurora/net/framer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.gen.h"
#include "aurora/core/result.h"

namespace aurora::net {

namespace detail {

/// @brief 分帧型别：决定一帧的边界到哪里。
enum class FramerKind : std::uint8_t {
    Line,  ///< LF 或 CRLF 终止
    Delimiter,  ///< 任意非空分隔符终止
    LengthPrefixed,  ///< 定长长度字段声明帧体字节数
};

/// @brief 分帧器的全部可变状态（口径 + 缓冲 + 终态错误）。
struct FramerState {
    /// @brief 装配：口径已由工厂校验，此处只接管型别与分隔符副本。
    /// @param type 分帧型别。
    /// @param config 已校验的分帧口径。
    /// @param separator 分隔符（仅 `Delimiter` 型读取）。
    FramerState(FramerKind type, FrameOptions config, std::string separator)
        : kind(type), options(config), delimiter(std::move(separator)) {}

    FramerKind kind;  ///< 分帧型别
    FrameOptions options;  ///< 上限与长度字段口径
    std::string delimiter;  ///< `Delimiter` 型的分隔符
    std::vector<char> buffer;  ///< 未交付字节：完整帧前缀 + 凑不满一帧的残片
    std::size_t scan_from = 0;  ///< 下一次边界搜索起点（跨 `feed()` 保留，避免重扫已确认无边界的前缀）
    std::optional<Error> terminal;  ///< 终态错误；置位后所有操作原样回报它
};

}  // namespace detail

namespace {

/// @brief 一帧在缓冲里的位置描述。
struct Boundary {
    std::size_t skip_before = 0;  ///< 帧体之前要跳过的字节数（长度前缀型的长度字段）
    std::size_t body_bytes = 0;  ///< 帧体字节数（不含任何标记）
    std::size_t consumed = 0;  ///< 整帧占用的字节数（含标记），即要从缓冲前端抹掉的前缀长度
};

/// @brief 一次边界搜索的结果：要么给出成形帧，要么给出「暂无进展」，要么给出终态失败。
struct Attempt {
    std::optional<Boundary> frame;  ///< 已成形的帧；尚未成形时为空
    std::optional<Error> failure;  ///< 终态失败（消息超出上限）
};

/// @brief 回调抛出时也要抹掉已消费的帧前缀，否则异常展开后同一帧会被二次投递。
struct ConsumeOnExit {
    ConsumeOnExit(std::vector<char> &target, std::size_t count) : buffer(target), bytes(count) {}
    ConsumeOnExit(const ConsumeOnExit &other) = delete;
    ConsumeOnExit(ConsumeOnExit &&other) = delete;
    auto operator=(const ConsumeOnExit &other) -> ConsumeOnExit & = delete;
    auto operator=(ConsumeOnExit &&other) -> ConsumeOnExit & = delete;
    ~ConsumeOnExit() { buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes)); }

    std::vector<char> &buffer;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) 守卫抹的是缓冲本体
    std::size_t bytes;  ///< 前缀长度
};

/// @brief 构造 `net-framer-config-invalid`。
/// @param reason 口径为何非法（ASCII）。
/// @return 结构化错误。
auto config_invalid(std::string_view reason) -> Error {
    return make_error(ErrorCode::NetFramerConfigInvalid, ErrorParams{{"detail", std::string(reason)}});
}

/// @brief 构造 `net-framer-overflow`。
/// @param reason 超限的具体形态（ASCII）。
/// @return 结构化错误。
auto frame_overflow(std::string_view reason) -> Error {
    return make_error(ErrorCode::NetFramerOverflow, ErrorParams{{"detail", std::string(reason)}});
}

/// @brief 构造 `net-buffer-limit`（缓冲越过硬上限）。
/// @param state 分帧状态（读当前缓冲与上限，用于报出数字）。
/// @return 结构化错误。
auto buffer_limit(const detail::FramerState &state) -> Error {
    const std::string detail = "buffered " + std::to_string(state.buffer.size()) + " bytes over cap " +
                               std::to_string(state.options.max_buffered_bytes);
    return make_error(ErrorCode::NetBufferLimit, ErrorParams{{"detail", detail}});
}

/// @brief 按口径解码缓冲最前的长度字段。
/// @param state 分帧状态（缓冲前部至少含整个长度字段）。
/// @return 声明值；上限由调用方裁决。
auto decode_length(const detail::FramerState &state) -> std::uint64_t {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < state.options.length_field_bytes; ++i) {
        const auto byte = static_cast<std::uint64_t>(static_cast<std::uint8_t>(state.buffer.at(i)));
        if (state.options.byte_order == LengthByteOrder::Big) {
            value = (value << 8U) | byte;
        } else {
            value |= byte << (i * 8U);
        }
    }
    return value;
}

/// @brief 找缓冲前端下一帧的边界；同时把「尚未成形」时的前缀搜索起点推进到本型的最低必要位置。
/// @param state 分帧状态（就地更新 `scan_from`）。
/// @return 成形帧 / 暂无进展 / 终态失败三者之一。
auto find_boundary(detail::FramerState &state) -> Attempt {
    const std::string_view view{state.buffer.data(), state.buffer.size()};
    const std::size_t frame_cap = state.options.max_frame_bytes;

    if (state.kind == detail::FramerKind::LengthPrefixed) {
        const std::size_t width = state.options.length_field_bytes;
        if (view.size() < width) {
            return {};  // 连长度字段都没读全，谈不上超限
        }
        const std::uint64_t declared = decode_length(state);
        std::uint64_t body = declared;
        if (state.options.length_counts_prefix) {
            if (declared < width) {
                return Attempt{.failure = frame_overflow("length prefix declares fewer bytes than its own width")};
            }
            body = declared - width;
        }
        if (body > frame_cap) {
            return Attempt{.failure = frame_overflow("declared frame body exceeds max_frame_bytes")};
        }
        const std::size_t needed = width + static_cast<std::size_t>(body);
        if (view.size() < needed) {
            return {};  // 帧体还没读全
        }
        return Attempt{
            .frame = Boundary{.skip_before = width, .body_bytes = static_cast<std::size_t>(body), .consumed = needed}};
    }

    // 终止标记命中位置：行型找 LF（CRLF 的 CR 记在帧体末尾之外），分隔符型找整段分隔符。
    const std::size_t hit = (state.kind == detail::FramerKind::Line) ? view.find('\n', state.scan_from)
                                                                     : view.find(state.delimiter, state.scan_from);
    if (hit == std::string_view::npos) {
        // 未命中：扫过的部分确认不含边界。行型可整段跳过；分隔符型的尾部若干字节可能是分隔符的前缀，
        // 必须留在下次的搜索范围里（分隔符长度减一即「还差最后一字节」的最坏情形）。
        const std::size_t overlap =
            state.kind == detail::FramerKind::Line ? 0 : state.delimiter.size() - std::size_t{1};
        state.scan_from = view.size() > overlap ? view.size() - overlap : 0;
        if (view.size() > frame_cap) {
            return Attempt{.failure = frame_overflow("unterminated message exceeds max_frame_bytes")};
        }
        return {};
    }

    std::size_t body_bytes = hit;
    std::size_t frame_end = hit + 1;
    if (state.kind == detail::FramerKind::Line) {
        if (hit > 0 && view[hit - 1] == '\r') {
            body_bytes = hit - 1;  // CRLF：CR 属于终止标记，不进帧体
        }
    } else {
        frame_end = hit + state.delimiter.size();
    }
    if (body_bytes > frame_cap) {
        return Attempt{.failure = frame_overflow("message exceeds max_frame_bytes")};
    }
    return Attempt{.frame = Boundary{.body_bytes = body_bytes, .consumed = frame_end}};
}

/// @brief 从缓冲前端尽可能多地交付已成形帧，直到暂无进展、回调暂停或超限。
/// @param state 分帧状态（就地消费缓冲前缀）。
/// @param on_frame 帧出口；空回调视为「一条都不投」。
/// @param report 出参：本次交付计数与暂停标志。
/// @return 终态失败（若有）；否则空。
auto drain(detail::FramerState &state, const FrameSink &on_frame, FeedReport &report) -> std::optional<Error> {
    if (!on_frame) {
        report.sink_paused = true;
        return {};
    }
    while (true) {
        const auto attempt = find_boundary(state);
        if (attempt.failure) {
            return attempt.failure;
        }
        if (!attempt.frame) {
            return {};
        }
        const Boundary &frame = *attempt.frame;
        // 视图指向缓冲本体：前缀抹除发生在回调结束（或抛出）之后，故回调期间始终有效。
        const auto body_slice = std::span<const char>(state.buffer).subspan(frame.skip_before, frame.body_bytes);
        const std::string_view body{body_slice.data(), body_slice.size()};
        bool keep = false;
        {
            const ConsumeOnExit consumed{state.buffer, frame.consumed};
            keep = on_frame(Frame{.bytes = body, .unterminated = false});
        }
        state.scan_from = 0;
        ++report.delivered;
        if (!keep) {
            report.sink_paused = true;
            return {};
        }
    }
}

/// @brief 公共口径校验：上限不得为 0，缓冲必须装得下一整帧。
/// @param options 待校验口径。
/// @return 非法时给出错误；合法时为空。
auto check_limits(const FrameOptions &options) -> std::optional<Error> {
    if (options.max_frame_bytes == 0) {
        return config_invalid("max_frame_bytes is 0");
    }
    if (options.max_buffered_bytes == 0) {
        return config_invalid("max_buffered_bytes is 0");
    }
    if (options.max_buffered_bytes < options.max_frame_bytes) {
        return config_invalid("max_buffered_bytes is smaller than max_frame_bytes");
    }
    return {};
}

}  // namespace

auto Framer::lines(FrameOptions options) -> Result<Framer> {
    if (const auto invalid = check_limits(options)) {
        return *invalid;
    }
    return Framer{std::make_unique<detail::FramerState>(detail::FramerKind::Line, options, std::string{})};
}

auto Framer::until(std::string_view delimiter, FrameOptions options) -> Result<Framer> {
    if (const auto invalid = check_limits(options)) {
        return *invalid;
    }
    if (delimiter.empty()) {
        return config_invalid("delimiter is empty");
    }
    if (delimiter.size() > options.max_frame_bytes) {
        return config_invalid("delimiter is longer than max_frame_bytes");
    }
    return Framer{std::make_unique<detail::FramerState>(detail::FramerKind::Delimiter, options,
                                                        std::string(delimiter))};
}

auto Framer::length_prefixed(FrameOptions options) -> Result<Framer> {
    if (const auto invalid = check_limits(options)) {
        return *invalid;
    }
    if (options.length_field_bytes < 1 || options.length_field_bytes > 8) {
        return config_invalid("length_field_bytes is outside 1..8");
    }
    return Framer{
        std::make_unique<detail::FramerState>(detail::FramerKind::LengthPrefixed, options, std::string{})};
}

Framer::Framer(std::unique_ptr<detail::FramerState> state) : state_(std::move(state)) {}

Framer::Framer(Framer &&other) noexcept = default;

auto Framer::operator=(Framer &&other) noexcept -> Framer & = default;

Framer::~Framer() = default;

auto Framer::feed(std::span<const char> bytes, const FrameSink &on_frame) -> Result<FeedReport> {
    if (!state_) {
        return Result<FeedReport>{FeedReport{}};  // 移动后的落点：没有缓冲可切，成功空报告而不是错误
    }
    if (state_->terminal) {
        return Result<FeedReport>{*state_->terminal};  // 终态：原样回报同一条错误，直到 reset()
    }
    detail::FramerState &state = *state_;
    // 入参一律被吃掉：先追加，再由交付循环决定投出多少条。
    state.buffer.insert(state.buffer.end(), bytes.begin(), bytes.end());

    FeedReport report;
    if (const auto failure = drain(state, on_frame, report)) {
        state.terminal = *failure;
        return Result<FeedReport>{*failure};
    }
    if (state.buffer.size() > state.options.max_buffered_bytes) {
        // 暂停期间接不下更多：慢消费者不得把这件缓冲变成无界的。
        state.terminal = buffer_limit(state);
        return Result<FeedReport>{*state.terminal};
    }
    report.buffered_bytes = state.buffer.size();
    return Result<FeedReport>{report};
}

auto Framer::finish(const FrameSink &on_frame) -> Result<FinishReport> {
    if (!state_) {
        return Result<FinishReport>{FinishReport{}};
    }
    if (state_->terminal) {
        return Result<FinishReport>{*state_->terminal};
    }
    detail::FramerState &state = *state_;
    FinishReport report;
    if (state.kind == detail::FramerKind::LengthPrefixed) {
        // 长度承诺未兑现的残片不构成一条消息：按型别语义丢弃并报出字节数。
        report.dropped_bytes = state.buffer.size();
        state.buffer.clear();
        state.scan_from = 0;
        return Result<FinishReport>{report};
    }
    if (state.buffer.empty()) {
        return Result<FinishReport>{report};
    }
    if (!on_frame) {
        report.sink_paused = true;
        return Result<FinishReport>{report};
    }
    const std::string_view tail{state.buffer.data(), state.buffer.size()};
    if (on_frame(Frame{.bytes = tail, .unterminated = true})) {
        state.buffer.clear();
        state.scan_from = 0;
        report.delivered = 1;
    } else {
        report.sink_paused = true;
    }
    return Result<FinishReport>{report};
}

auto Framer::reset() -> void {
    if (!state_) {
        return;
    }
    state_->buffer.clear();
    state_->scan_from = 0;
    state_->terminal.reset();
}

auto Framer::buffered_bytes() const -> std::size_t { return state_ ? state_->buffer.size() : 0; }

}  // namespace aurora::net
