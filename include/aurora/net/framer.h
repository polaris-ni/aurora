#pragma once

/// @brief 字节流 → 完整消息的分帧器（CHG-010 传输切片：SSE 行解析、本机 IPC 协议、远端终端协议共用）。
/// @file net/framer.h
///
/// `StreamTransport` 交付的是「这次读到了若干字节」，而绝大多数协议的语义单位是一条消息。本类型把那串
/// 字节切成一条完整消息，只做这一件事：**不含任何 socket、平台头与线程**，因此在无网络的条件下即可逐值
/// 断言其全部判据。
///
/// 三型分帧（提案 §11）：
/// - `lines()`：LF 或 CRLF 终止，终止符不进气泡给回调的帧体；空行是一条**空帧**而不是噪声（SSE 的事件
///   边界正是空行）。单独的 CR 不视为行终止符——SSE 那类需要兼容裸 CR 的来源由上层先归一化。
/// - `until(delimiter, …)`：任意非空分隔符，分隔符不进气泡帧体；连续两次命中之间就是空帧。跨 `feed()`
///   边界的分隔符照样能命中（扫描位置随内部缓冲保留）。
/// - `length_prefixed(…)`：定长长度字段（1..8 字节、大端或小端）声明其后帧体的字节数，超过
///   `max_frame_bytes` 的声明值一律拒收。
///
/// 拉模型契约（提案 §11 的「回调返回值决定是否继续投递」）：
/// - **每次 `feed()` 都吃掉它收到的全部字节**，交付多少条由回调返回值决定：返回 `true` 继续，返回
///   `false` 即暂停，未交付的字节留在内部缓冲里，等下一次 `feed()`（空 span 即「只续投不新增」）继续。
///   因此调用方的正确背压动作是「暂停期间不要再读该流」，而不是把字节再喂一遍。
/// - 内部缓冲有硬上限（`FrameOptions::max_buffered_bytes`）：越过即以 `net-buffer-limit` 失败，慢消费者
///   不可能造出无界缓冲。单条消息超上限则以 `net-framer-overflow` 失败。
/// - 两枚失败都是**终态**：此后所有操作原样回报同一条错误，直到 `reset()`（换一条连接复用本对象）。
///
/// 输入结束的语义随型别不同（`finish()`）：行 / 分隔符型把没有自带终止符的尾巴补成最后一帧，并以
/// `Frame::unterminated` 标明「它是输入结束补出来的」，上层据此决定收下还是丢弃（SSE 按规范丢弃、行协议
/// 通常收下）；长度前缀型里「长度承诺未兑现」的字节不构成一条消息，直接丢弃并在 `FinishReport` 报出字节数。
///
/// @code
/// auto framer = aurora::net::Framer::lines();
/// framer.feed(chunk, [](const auto &frame) {
///     use(frame.bytes);
///     return true;   // 继续投递
/// });
/// @endcode
/// @note Thread: 单线程使用（本类型不加锁；与 `StreamTransport` 的回调同线程串行即是预期用法）
/// @note Side-effects: none（纯内存换算）

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net {

namespace detail {
struct FramerState;  ///< 分帧器的全部状态（缓冲、口径、终态错误）；类型留在实现侧，公共头不暴露
}  // namespace detail

/// @brief 长度前缀的字节序（`Framer::length_prefixed()` 的入参）。
/// @note 与线上格式一一对应：`Big` 即网络字节序，`Little` 常见于 Windows 侧自定义协议。
enum class LengthByteOrder : std::uint8_t {
    Big,  ///< 大端（网络字节序）
    Little,  ///< 小端
};

/// @brief 一帧的交付形态：帧体只在回调期间有效，且可能由输入结束补出。
/// @note Thread: 单线程使用（视图指向本分帧器的内部缓冲）
/// @note Side-effects: none
struct Frame {
    /// @brief 帧体（不含终止符 / 分隔符 / 长度字段本身）。
    /// @note 有效期仅限本次回调之内：回调返回后内部缓冲会被重排，不得保存该视图。
    std::string_view bytes;
    /// @brief 该帧由 `finish()` 补出，即输入结束时它没有自带的终止标记。
    bool unterminated = false;
};

/// @brief 帧的出口：**返回 `false` 即暂停投递**（剩余字节留在内部缓冲，等下一次 `feed()`）。
using FrameSink = std::function<bool(const Frame &frame)>;

/// @brief 分帧口径（三型共用；缺省值即提案 §11 的「必须有上限」形态）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct FrameOptions {
    /// @brief 单条消息的字节上限：行 / 分隔符型里超出即拒收，长度前缀型里作为声明值的上限。零非法。
    std::size_t max_frame_bytes = std::size_t{1024} * 1024;
    /// @brief 内部缓冲的硬上限（含尚未交付的完整帧与凑不满一帧的残片）：越过即 `net-buffer-limit`。
    /// 零非法。它比 `max_frame_bytes` 大才谈得上「暂停期间还能接住一整帧」。
    std::size_t max_buffered_bytes = std::size_t{4} * 1024 * 1024;
    /// @brief 长度前缀的字节序（只被 `length_prefixed()` 读取）。
    LengthByteOrder byte_order = LengthByteOrder::Big;
    /// @brief 长度前缀的字节数，1..8（只被 `length_prefixed()` 读取）。
    std::size_t length_field_bytes = 4;
    /// @brief 长度字段的声明值是否把长度字段自身也算进去（某些协议把整帧长度记为含头）。
    bool length_counts_prefix = false;
};

/// @brief 一次 `feed()` 的结果。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct FeedReport {
    /// @brief 本次交付的帧数。
    std::size_t delivered = 0;
    /// @brief 是否因回调返回 `false` 而提前停止投递。
    bool sink_paused = false;
    /// @brief 停止后内部缓冲里剩下的字节数（含未交付的完整帧与残片）。
    std::size_t buffered_bytes = 0;
};

/// @brief 一次 `finish()` 的结果。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct FinishReport {
    /// @brief 本次补交付的帧数（行 / 分隔符型最多一枚）。
    std::size_t delivered = 0;
    /// @brief 是否因回调返回 `false` 而未交付完。
    bool sink_paused = false;
    /// @brief 因「长度承诺未兑现」而丢弃的字节数（只可能是长度前缀型）。
    std::size_t dropped_bytes = 0;
};

/// @brief 把 `StreamTransport` 的字节读事件产出为「一条完整消息」的分帧器。
///
/// 值语义不可拷贝（内部缓冲可能很大且被回调视图引用），可移动。构造只经三型工厂。
/// @note Thread: 单线程使用
/// @note Side-effects: none
class Framer {
  public:
    /// @brief 按行分帧（LF 或 CRLF 终止，终止符不进帧体）。
    /// @param options 分帧口径；`byte_order` 等长度前缀字段在本型不被读取。
    /// @return 成功的分帧器；口径非法（上限为 0）时失败为 `net-framer-config-invalid`。
    [[nodiscard]] static auto lines(FrameOptions options = {}) -> Result<Framer>;

    /// @brief 按分隔符分帧（任意非空字节串，分隔符不进帧体）。
    /// @param delimiter 分隔符；非空且长度不得超过 `options.max_frame_bytes`。
    /// @param options 分帧口径。
    /// @return 成功的分帧器；分隔符为空、超上限或口径非法时失败为 `net-framer-config-invalid`。
    [[nodiscard]] static auto until(std::string_view delimiter, FrameOptions options = {}) -> Result<Framer>;

    /// @brief 按长度前缀分帧（定长字段声明其后帧体的字节数）。
    /// @param options 分帧口径；用到 `max_frame_bytes`、`max_buffered_bytes`、`byte_order`、
    ///        `length_field_bytes` 与 `length_counts_prefix`。
    /// @return 成功的分帧器；`length_field_bytes` 不在 1..8 或口径非法时失败为
    ///         `net-framer-config-invalid`。
    [[nodiscard]] static auto length_prefixed(FrameOptions options = {}) -> Result<Framer>;

    Framer(const Framer &) = delete;
    auto operator=(const Framer &) -> Framer & = delete;
    /// @brief 移动构造：内部缓冲与终态错误随对象一并移交。
    /// @param other 被移动的源对象（移动后其缓冲为空，仍可安全调用 `feed()`）。
    Framer(Framer &&other) noexcept;
    /// @brief 移动赋值：先释放自身缓冲再接收对方的。
    /// @param other 被移动的源对象。
    /// @return 自身引用。
    auto operator=(Framer &&other) noexcept -> Framer &;
    /// @brief 析构：只归还内部缓冲，无系统资源（定义在实现侧，因状态类型不外露）。
    ~Framer();

    /// @brief 喂入一段字节并尽可能多地交付完整帧。
    ///
    /// 入参字节一律被吃掉（不再归调用方所有）；回调返回 `false` 后未交付的字节留在内部缓冲，
    /// 由下一次 `feed()`（可以是空 span）继续投递。
    /// @param bytes 新读到的字节；空 span 合法，语义是「只续投缓冲里的帧」。
    /// @param on_frame 帧出口；空回调视为「一条都不投」，即立即暂停，不改变缓冲。
    /// @return 成功为本次结果；终态失败为原样错误（`net-framer-overflow` / `net-buffer-limit` /
    ///         `net-framer-config-invalid`）。
    [[nodiscard]] auto feed(std::span<const char> bytes, const FrameSink &on_frame) -> Result<FeedReport>;

    /// @brief 宣告输入结束：交付尾巴（行 / 分隔符型）或丢弃残片（长度前缀型）。
    /// @param on_frame 帧出口；补交付的帧以 `Frame::unterminated == true` 标明形态。
    /// @return 成功为本次结果；终态失败为原样错误。
    [[nodiscard]] auto finish(const FrameSink &on_frame) -> Result<FinishReport>;

    /// @brief 清空缓冲与终态错误，回到刚构造的形态（换一条连接复用本对象时用）。
    auto reset() -> void;

    /// @brief 内部缓冲的当前字节数（未交付的完整帧 + 凑不满一帧的残片）。
    /// @return 缓冲字节数；对象处于终态时同样是真值，供诊断读取。
    [[nodiscard]] auto buffered_bytes() const -> std::size_t;

  private:
    /// @brief 唯一构造入口：三型工厂在口径校验通过后经此接管状态。
    /// @param state 已装配好的状态（缓冲、口径、终态错误）。
    explicit Framer(std::unique_ptr<detail::FramerState> state);

    std::unique_ptr<detail::FramerState> state_;  ///< 全部状态；缓冲形态只在实现侧可见
};

}  // namespace aurora::net
