// http_sse.h —— `text/event-stream` 的增量解析器（`RFC 9110` 之外的 WHATWG SSE 语义本体）。
//
// 为何单列：一条事件流的边界判据全在**字节到行的切分**与**字段分发**上，而这两件事一旦写进 `Client::open_sse`
// 的协程体，就只能靠真连接与真对端才能测——那既测不出「裸 CR 也算行终止」，也测不出「连接收摊时未收尾的那条
// 事件必须丢弃」。落在这里，零 socket、零时钟、零线程，输入只是「这一次到手了若干字节」，于是每条判据都能
// 逐值断言且各腿同值。发起侧只在四处各叫一声：喂入读到的段、连接收摊时宣告输入结束、问宿主是否已收摊、
// 取解析器报出的字段效应（续传标识与重连窗口）。
//
// 五条口径（规范性，落码与文档回写以此为准）：
// - **行终止有三型**：CR、CRLF、LF 都算一条行的收尾。`net::Framer` 的行型只认 LF 与 CRLF（其头注释明写
//   「需要兼容裸 CR 的来源由上层先归一化，本层不猜」），而 SSE 的字段文法建立在「三型都算」之上，故本解析器
//   自带切行逻辑，跨段挂住一枚尾随 CR（下一段首字节是 LF 时不另起一行）。这一条是对提案 §11「SSE 行解析与
//   `Framer` 共用同一实现」的纠回，纠回理由就是上面那句：共用要么让 Framer 接管它刻意不接的形态，要么在两层
//   各留一份行状态。
// - **编码只认 UTF-8**：BOM（`EF BB BF`）在流首剥掉；非法字节序列**不替换也不报错**，按原始字节留在字段值里
//   交出去——事件流的字节属于载荷，本层没有权改写它（`Response::text()` 的 charset 解码是物化路径的事）。
// - **一条事件由空行收尾**：数据缓冲为空时遇空行**不派发**（那是注释或心跳之后的空行），只把事件类型缓冲
//   清回去；未以空行收尾的残留随连接收摊一起丢弃，绝不补发——补发等于让宿主收到一条服务端从未说完的事件。
// - **上限是本层的不变量**：单行 1 MiB（越限落 `net-framer-overflow`，与分帧器同一枚码同一句「这条流上的某帧
//   太长」），单条事件的缓冲 4 MiB（越限落 `net-buffer-limit`，与「本层拒绝把缓冲变成无界」同一条纪律）。两枚
//   都是库内置常量、不开旋钮：开旋钮等于让「不发疯」这件事变成调用方的责任。
// - **字段效应即时上报，不跟着事件派发**：`id` 与 `retry` 在服务端写下的那一刻就生效（DOM 的 `lastEventId`
//   取的是派发时刻该流的续传标识，不是本条带没带），故经出口交回发起侧持有；未知字段一律忽略而非报错，
//   这是 `text/event-stream` 的向后兼容机制，本层不得替服务端作废它。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 单行字节上限：越过即终态（缺省与 `Framer` 的 `max_frame_bytes` 同值，两层的「一帧太长」同口径）。
inline constexpr std::size_t AURORA_SSE_MAX_LINE_BYTES = std::size_t{1024} * 1024;

/// @brief 单条事件的缓冲上限（数据缓冲与事件类型缓冲之和；越过即终态）。
inline constexpr std::size_t AURORA_SSE_MAX_EVENT_BYTES = std::size_t{4} * 1024 * 1024;

/// @brief 解析器派发的一条完整事件（尚未拼上续传标识——那是发起侧持有的会话事实）。
/// @note Thread: 单线程使用（解析器不加锁；与交付线程串行即是预期用法）
/// @note Side-effects: none
struct SseParsedEvent {
    std::string type;  ///< 事件类型；服务端未命名时已是规范折出的 `message`
    std::string data;  ///< 消息体：多行 `data` 以 LF 连接，不含收尾空行；恒非空
};

/// @brief 解析器的出口束（四个都可缺；空出口 = 该种事实就地丢弃）。
///
/// `on_event` 的返回值是**宿主的收摊判定**：返回假即不再派发后续事件（本层不报错，收摊由发起侧译成成功结局，
/// 口径与 `SinkFlow::Stop` 一致）。其余三口只报事实，不参与判定。
/// @note Thread: 单线程使用（闭包在 `push()` / `finish()` 的调用线程上同步执行）
/// @note Side-effects: 由实现方决定；本层只约定调用时机
struct SseParserSinks {
    std::function<bool(const SseParsedEvent &event)> on_event;  ///< 一条事件成形（空行收尾）
    std::function<void(std::string_view text)> on_comment;  ///< 一行注释（含心跳用的空注释）
    std::function<void(const std::string &value)> on_last_id;  ///< `id` 字段落下即报（空值也报，那是清空）
    std::function<void(std::chrono::milliseconds window)> on_retry;  ///< `retry` 字段落下即报（非数字不报）
};

/// @brief 把 `text/event-stream` 的字节段增量解成「一条完整事件」。
///
/// 每（重）建一条连接即新建一个实例：行缓冲、数据缓冲与事件类型缓冲都属于**这一条流**，跨连接不残留
/// （续传标识与重连窗口由发起侧持有，正是为了把它们与流的边界分开）。值语义不可拷贝（内部缓冲可能很大），
/// 可移动。
/// @note Thread: 单线程使用
/// @note Side-effects: none（纯内存换算）
class SseStreamParser {
  public:
    /// @brief 以出口束构造。
    /// @param sinks 四个出口；缺哪个就不报哪种事实。
    explicit SseStreamParser(SseParserSinks sinks);

    SseStreamParser(const SseStreamParser &) = delete;
    auto operator=(const SseStreamParser &) -> SseStreamParser & = delete;
    /// @brief 移动构造：缓冲、终态错误与挂住的 CR 一并移交。
    /// @param other 被移动的源对象（移动后其缓冲为空，仍可安全 `push()`）。
    SseStreamParser(SseStreamParser &&other) noexcept;
    /// @brief 移动赋值：先释放自身缓冲再接收对方的。
    /// @param other 被移动的源对象。
    /// @return 自身引用。
    auto operator=(SseStreamParser &&other) noexcept -> SseStreamParser &;
    /// @brief 析构：只归还缓冲，无系统资源。
    ~SseStreamParser();

    /// @brief 喂入一段字节，切出尽可能多的完整行并按字段分发。
    ///
    /// 入参字节一律被吃掉。终态之后（越限）再喂同样交出**同一枚**错误，不做二次判定——那批字节属于谁已经
    /// 不可知，继续切行只会造出更多假事件。
    /// @param bytes 本段读到的字节；空 span 合法（什么都不结算，只把待判的 BOM 挂着）。
    /// @return 成功为空；单行越限为 `net-framer-overflow`，单条事件缓冲越限为 `net-buffer-limit`，
    ///         detail 点名越的是哪一枚内置上限。
    [[nodiscard]] auto push(std::string_view bytes) -> Result<void>;

    /// @brief 宣告输入结束（连接收摊）：把没有自带终止符的残尾按行结算，再丢弃未成形的那条事件。
    ///
    /// 「残尾按行结算」与「未成形的事件丢弃」是两件事：末行缺终止符仍是一条**完整字段行**（服务端写完
    /// `data: x` 就断了），按行分发是对的；而那条事件没有空行收尾，服务端从没说过它完——补发就是把半条
    /// 消息当一条交出去。
    /// @return 成功为空；结算末行时越限同样交出终态错误。
    [[nodiscard]] auto finish() -> Result<void>;

    /// @brief 宿主是否已回过「不再收」。
    /// @return 回过假为 true；此后 `push()` 仍会切行与报字段效应，但不再派发事件。
    [[nodiscard]] auto host_paused() const -> bool { return paused_; }

    /// @brief 当前缓冲里未成形的字节数（数据缓冲 + 事件类型缓冲 + 行缓冲）。
    /// @return 字节数；供诊断读取，不参与任何判定。
    [[nodiscard]] auto buffered_bytes() const -> std::size_t;

  private:
    /// @brief 结算一行：按注释 / 字段 / 空行三型分发。
    /// @param line 行体（不含终止符）。
    /// @return 成功为空；缓冲越限为终态错误。
    auto consume_line(std::string_view line) -> Result<void>;

    /// @brief 空行：按规范决定派发还是只清缓冲。
    /// @return 成功为空；派发出口未把缓冲清干净前的越限检查已完成，故此处不报错。
    auto dispatch() -> Result<void>;

    /// @brief 记下行终态错误。
    /// @param error 待记录的错误。
    auto fail(Error error) -> Result<void>;

    SseParserSinks sinks_;  ///< 四个出口（可缺）
    std::string head_;  ///< 流首待判 BOM 的挂起字节（判完即清）
    std::string line_;  ///< 正在凑的一行（不含终止符）
    std::string data_;  ///< 本条事件已攒的数据缓冲
    std::string event_type_;  ///< 本条事件的事件类型缓冲；空 = 未命名，派发时折成 `message`
    std::optional<Error> failure_;  ///< 终态错误；有值即本实例只重复交出它
    bool pending_cr_{false};  ///< 上一段以 CR 收尾：本段首字节是 LF 时不另起一行
    bool bom_checked_{false};  ///< 流首的 BOM 是否已判定（只判一次）
    bool paused_{false};  ///< 宿主是否已回过「不再收」
};

}  // namespace aurora::net::detail
