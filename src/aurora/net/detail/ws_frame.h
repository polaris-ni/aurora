// ws_frame.h —— WebSocket 的线上帧编解码与分片重组（`RFC 6455` §5），纯内存：零 socket、零时钟、零线程。
//
// 为何单列：一条连接的边界判据全叠在**同一趟字节扫描**上——长度前缀随载荷长度在 7 / 16 / 64 位之间变宽、掩码位的
// 有无决定这一帧合不合法、FIN 与 opcode 共同决定「这条消息说完没有」、控制帧既不许跨分片又不许超过 125 字节、
// 文本还要在整条消息上判 UTF-8。这五件事拆到发起侧就只能靠真连接与真对端才测得动，写在这里则每条判据都能逐值
// 断言且各腿同值。发起侧只在三处各叫一声：喂入读到的段、连接收摊时宣告输入结束、把成形的消息与控制帧交给宿主。
//
// 为何不复用 `net::Framer`：`Framer` 的三型罩不住上面那五件事的叠加形态（它的定长型连变宽前缀都要调用方先知道
// 长度），硬套等于把一条协议的特例写进通用件的例外分支——通用件的例外分支是别的调用方的坑。
//
// 七条口径（规范性，落码与文档回写以此为准）：
// - **对端帧必须不带掩码**：掩码是客户端→服务端方向的义务（`RFC 6455` §5.1），服务端把自己当成客户端发一枚掩码帧
//   就是协议违例，本层**不解掩**、直接落 `ws-protocol-error`。少这一条会白养一条永不成立的解掩代码路径，还会把
//   「对端发疯」读成「载荷长了 4 字节」。
// - **发送侧恒带掩码，且密钥属于帧头而不属于载荷**：零长载荷照样写四字节密钥（`RFC 6455` §5.3 的 ABNF 把
//   `MASKING-KEY` 排在 `FRAME-HEADER` 里）。密钥每帧新取一枚，走 §8.8.27 的进程级系统熵；取不到熵就交出失败、
//   不回落伪随机——协议规定它不可预测，可预测的掩码等于把「规避缓存代理的内容过滤」这条机制废掉。
// - **长度前缀取最小档**：7 位档装得下就不许用 16 位档，16 位档装得下就不许用 64 位档，64 位档的最高位恒为 0。
//   这是规范的「MUST」而不是风格：非最小编码让同一份载荷有两种线上表示，而 §8 的帧序判定必须只有一种读法。
// - **控制帧自成一条消息**：`opcode >= 8` 即 FIN 必须为 1 且载荷 ≤ 125 字节，且它可以插在一条分片消息的两片之间
//   （规范明许）。`close` 的载荷只允许 0 字节或 ≥ 2 字节：一枚字节拼不出状态码，直接判违例；带码时状态码必须落在
//   线上合法集内，随行的原因文本必须是合法 UTF-8。
// - **分片状态机只有两种合法开场**：`continuation` 无头帧即违例（那是半条不属于任何消息的数据），头帧未说完又来
//   一枚 `text` / `binary` 同样违例（两条消息在一条连接上不许交叠）。控制帧不参与分片，故它不打断也不被这个状态机打断。
// - **上限是两枚可覆盖的旋钮，但恒有界**：单帧缺省 1 MiB、重组后的整条消息缺省 4 MiB——两枚取值与 `FrameOptions`
//   的两枚内置上限同值（同源裁决见提案 §18），为的是「这条流上的某一帧太长 / 某一条消息太长」这句话在分帧器、SSE
//   与本层三处说法一致。越单帧落 `ws-protocol-error`，越总帧同样落它而 detail 点名「是重组量」——这两枚码相同而句
//   不同，是为了让「对端发了个大家伙」与「对端把一条消息撑到装不下」在日志里可分。声明长度一到就判，不等字节收齐：
//   那 4 MiB 不该为了「也许对端会自己收手」而先分配出来。单帧那枚管的是**帧**，控制帧也不例外：协议给控制帧的
//   125 字节恒在缺省上限之内，故这条分岔只在调用方把旋钮收到 125 以下时才可见。
// - **文本的 UTF-8 在整条消息上判**：一条分片消息可以把一个多字节序列劈在两片的交界处，逐片判会把合法的续字节
//   序列当成违例。文本不合格不设独立错误码（裁决见提案 §16 第 39 项第六处），归 `ws-protocol-error` 并在 detail 点名。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 单帧载荷的缺省上限：越过即终态（与 `SseStreamParser` 的单行上限同量级，同一条「不发疯」纪律）。
inline constexpr std::size_t AURORA_WS_MAX_FRAME_BYTES = std::size_t{1024} * 1024;

/// @brief 重组后整条消息的缺省上限（所有分片之和）。
inline constexpr std::size_t AURORA_WS_MAX_MESSAGE_BYTES = std::size_t{4} * 1024 * 1024;

/// @brief 控制帧载荷的协议上限：`RFC 6455` §5.5 写死 125 字节，不是旋钮、不可覆盖。
inline constexpr std::size_t AURORA_WS_MAX_CONTROL_BYTES = 125;

/// @brief 帧头里掩码密钥的字节数：`RFC 6455` §5.3 写死 4 字节，且它属于帧头而不属于载荷。
inline constexpr std::size_t AURORA_WS_MASK_KEY_BYTES = 4;

/// @brief 帧头里的 4 位操作码。
///
/// 只列出线上允许出现的取值：`0x3`~`0x7` 与 `0xB`~`0xF` 是保留位，解到即违例，故本枚举不为其留值——把「不合法的
/// 操作码」表达成「没有对应取值」，判据就不会漏掉某一行 switch。
enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,  ///< 一条分片消息的后续片
    Text = 0x1,  ///< 文本消息（整条必须是合法 UTF-8）
    Binary = 0x2,  ///< 二进制消息（字节属于载荷，本层不解读）
    Close = 0x8,  ///< 关闭：载荷是 0 字节或「两字节大端状态码 + UTF-8 原因」
    Ping = 0x9,  ///< 探活：恒自动回一枚同载荷的 pong（口径见发起侧）
    Pong = 0xA,  ///< 探活应答
};

/// @brief 两枚上限的载体（发起侧从公共旋钮折进来；本层只认这两枚，不再自带第三枚）。
/// @note Thread: 构造后只读
/// @note Side-effects: none
struct WsFrameLimits {
    std::size_t max_frame_bytes = AURORA_WS_MAX_FRAME_BYTES;  ///< 单帧载荷上限
    std::size_t max_message_bytes = AURORA_WS_MAX_MESSAGE_BYTES;  ///< 重组后整条消息上限
};

/// @brief 解出的一条完整消息（已重组、已判 UTF-8；`opcode` 只会是 `Text` 或 `Binary`）。
/// @note Thread: 单线程使用（随出口回调一同活在交付线程上）
/// @note Side-effects: none
struct WsParsedMessage {
    WsOpcode opcode;  ///< 消息类型
    std::string payload;  ///< 载荷原文；分片已按序拼接
};

/// @brief 解出的一枚 `close` 帧。
///
/// `code` 为空是对端的事实（它没带状态码），不是本层的兜底——把「没给」折成 `1000`（正常关闭）会让一次没有
/// 说完话的收场冒充成一次好端端的道别。
/// @note Thread: 单线程使用
/// @note Side-effects: none
struct WsParsedClose {
    std::optional<std::uint16_t> code;  ///< 线上带来的状态码；空 = 对端没带
    std::string reason;  ///< 原因文本（合法 UTF-8，已在解帧时判过）
};

/// @brief 读侧的出口束（四个都可缺；空出口 = 该种事实就地丢弃）。
///
/// `on_message` 的返回值是**宿主的收摊判定**：返回假即不再派发后续消息（本层不报错，收摊由发起侧译成结局），
/// 口径与 `SseParserSinks::on_event` 一致。其余三口只报事实，不参与判定。
/// @note Thread: 单线程使用（闭包在 `push()` 的调用线程上同步执行）
/// @note Side-effects: 由实现方决定；本层只约定调用时机
struct WsFrameSinks {
    std::function<bool(WsParsedMessage message)> on_message;  ///< 一条消息成形（FIN 落到数据帧上）
    std::function<void(std::string_view payload)> on_ping;  ///< 对端的探活帧（载荷原样交回，供发起侧回 pong）
    std::function<void(std::string_view payload)> on_pong;  ///< 对端的探活应答
    std::function<void(WsParsedClose close)> on_close;  ///< 对端发起关闭
};

/// @brief 把字节段增量解成「一条完整消息 + 若干控制帧」。
///
/// 每条连接一个实例：帧缓冲与分片状态都属于**这一条流**，跨连接不残留。值语义不可拷贝（内部缓冲可达 MiB 级），
/// 可移动。终态之后（协议违例或越限）再喂同样交出**同一枚**错误，不做二次判定——那批字节属于谁已经不可知，
/// 继续解只会造出更多假消息。
/// @note Thread: 单线程使用
/// @note Side-effects: none（纯内存换算）
class WsFrameReader {
  public:
    /// @brief 以出口束与上限构造。
    /// @param sinks 四个出口；缺哪个就不报哪种事实。
    /// @param limits 两枚上限（越限即终态）。
    explicit WsFrameReader(WsFrameSinks sinks, WsFrameLimits limits = {});

    WsFrameReader(const WsFrameReader &) = delete;
    auto operator=(const WsFrameReader &) -> WsFrameReader & = delete;
    /// @brief 移动构造：帧缓冲、重组状态、终态错误与挂起的收摊判定一并移交。
    /// @param other 被移动的源对象（移动后其缓冲为空，仍可安全 `push()`）。
    WsFrameReader(WsFrameReader &&other) noexcept;
    /// @brief 移动赋值：先释放自身缓冲再接收对方的。
    /// @param other 被移动的源对象。
    /// @return 自身引用。
    auto operator=(WsFrameReader &&other) noexcept -> WsFrameReader &;
    /// @brief 析构：只归还缓冲，无系统资源。
    ~WsFrameReader();

    /// @brief 喂入一段字节，解出尽可能多的完整帧并逐帧走判定与派发。
    /// @param bytes 本段读到的字节；空 span 合法（什么都不结算）。
    /// @return 成功为空；协议违例与越限为 `ws-protocol-error`（终态，detail 点名违的是哪一条），detail 里出现
    ///         `reassembled` 者为总帧越限、出现 `frame size` 者为单帧越限。
    [[nodiscard]] auto push(std::string_view bytes) -> Result<void>;

    /// @brief 宣告输入结束（连接收摊）：丢掉没凑齐的半个帧头与没说完的那条消息，不补发。
    ///
    /// 「补发半条消息」是把对端从没说完的话当成一条交出去，与 §8.8.27 的 SSE 收摊同一条纪律。这里不报错：
    /// 「收摊时停在半帧」是一条**连接级**事实，归发起侧译成 `ws-closed` 的 detail，帧层没有那份上下文。
    /// @return 成功为空；若此前已进终态则交出那枚终态错误。
    [[nodiscard]] auto finish() -> Result<void>;

    /// @brief 收摊时是否还挂着一条没说完的消息。
    /// @return 有为 true；供发起侧写 detail 与判据读取，不参与任何判定。
    [[nodiscard]] auto message_in_progress() const -> bool { return fragment_open_; }

    /// @brief 宿主是否已回过「不再收」。
    /// @return 回过假为 true；此后 `push()` 仍会解帧、报控制帧与判协议形态，但不再派发消息。
    [[nodiscard]] auto host_paused() const -> bool { return paused_; }

    /// @brief 当前挂起的字节数（未凑齐的帧头与部分载荷 + 正在重组的消息）。
    /// @return 字节数；供诊断读取，不参与任何判定。
    [[nodiscard]] auto buffered_bytes() const -> std::size_t;

  private:
    /// @brief 结算一帧：判定形态、走分片状态机或报控制帧。
    /// @param final FIN 位。
    /// @param opcode 已核过的操作码。
    /// @param payload 已切出的载荷（属于 `pending_` 的那一段）。
    /// @return 成功为空；形态违例与越限为终态错误。
    auto consume_frame(bool final, WsOpcode opcode, std::string_view payload) -> Result<void>;

    /// @brief 派发一条成形消息（含文本的 UTF-8 判定与宿主的收摊判定）。
    /// @param opcode 头帧带来的类型。
    /// @param payload 整条载荷。
    /// @return 成功为空；文本不合格为终态错误。
    auto dispatch(WsOpcode opcode, std::string payload) -> Result<void>;

    /// @brief 记下行终态错误。
    /// @param error 待记录的错误。
    auto fail(Error error) -> Result<void>;

    WsFrameSinks sinks_;  ///< 四个出口（可缺）
    WsFrameLimits limits_;  ///< 两枚上限
    std::string pending_;  ///< 尚未凑齐一帧的字节（帧头 + 已到的一部分载荷）
    std::string message_;  ///< 正在重组的消息（未开分片时恒空）
    std::optional<WsOpcode> fragment_opcode_;  ///< 本条分片消息的头帧类型；空 = 未开
    std::optional<Error> failure_;  ///< 终态错误；有值即本实例只重复交出它
    bool fragment_open_{false};  ///< 是否有一条分片消息正在重组
    bool paused_{false};  ///< 宿主是否已回过「不再收」
};

/// @brief 写出一帧的线上字节：客户端侧恒带掩码。
///
/// 长度前缀按载荷长度取最小档（7 / 16 / 64 位）。本函数**不查**两枚上限——它们是收侧对「对端能发多大」的约束，
/// 我方要发的量自有发送队列那枚闸管着（口径见发起侧与提案 §16 第 39 项第三处）。
/// @param out 目标串：帧字节追加在尾部（调用方按批写完后一次性交给传输层）。
/// @param opcode 操作码。
/// @param final FIN 位；一帧一消息的写法传 `true`。
/// @param payload 载荷原文（本函数不改它，写出的是掩码后的副本）。
/// @param key 指定掩码密钥；缺省空 = **每帧新取一枚**系统熵。这个口只为判据而存在：逐字节比对的用例需要一枚已知
///        密钥，而发起侧恒不传（传了就是把一枚可猜的密钥交回调用方手上）。
/// @return 成功为空；取不到系统熵为 `net-platform-unavailable`（此处不回落伪随机）；控制帧载荷超过 125 字节或
///         控制帧被要求分片为 `ws-protocol-error`——本层拒写一枚自己不认的帧，绝不把违例推给对端去判。
[[nodiscard]] auto write_ws_frame(std::string &out, WsOpcode opcode, bool final, std::string_view payload,
                                  std::optional<std::array<std::byte, AURORA_WS_MASK_KEY_BYTES>> key = std::nullopt)
    -> Result<void>;

/// @brief 拼出一枚 `close` 帧的载荷：两字节大端状态码 + 原因文本（原因为空时只剩那两字节）。
/// @param code 线上状态码（调用方先用 `is_close_code_on_wire()` 核过）。
/// @param reason 原因文本（须是合法 UTF-8；本函数不判，判它的是发起侧的入参闸）。
/// @return 载荷串。
[[nodiscard]] auto close_frame_payload(std::uint16_t code, std::string_view reason) -> std::string;

/// @brief 状态码能不能上线（收发两向同一份合法集）。
///
/// 排除的是保留与「只有实现自己知道」的那几枚：`0`~`999`（保留、未定义）、`1004`（保留）、`1005` / `1006`
/// （表示「没带码」与「连接异常断开」，只活在实现内部、上线即是撒谎）、`1015`（保留给 TLS 失败）、
/// `1016`~`2999`（保留）、`5000`~`65535`（保留不可用）。合法集因此是 `{1000..1003} ∪ {1007..1014} ∪ {3000..4999}`。
/// @param code 待核的状态码。
/// @return 可上线为 true。
[[nodiscard]] auto is_close_code_on_wire(std::uint16_t code) -> bool;

/// @brief 找出第一个不合法的 UTF-8 字节位置（`Unicode 15.0 §3.9` 表 3.7 的良构形态）。
///
/// 收发两侧共用这一份判定，为的是「同一串字节在两条腿上给出同一个答案」：读侧在**整条消息**上判（口径见
/// 本文件头第七条），发出侧在**写出之前**判文本帧——写出一枚对端会判成违例的文本帧，等于把同一份坏字节
/// 交给一条已经建立的会话去踩刹车。判的是整段：截在串尾的半截序列同样算不合格。越码（`C0` / `C1`）、
/// 代理对区（`ED A0+`）与超过 `U+10FFFF` 的四字节形（`F5+` 与 `F4 90+`）都在这张表外，故不靠
/// 「解出码点再判范围」那种两趟写法。
/// @param bytes 待判的字节。
/// @return 首个非法字节的下标；整段合格为空。
[[nodiscard]] auto first_invalid_utf8(std::string_view bytes) -> std::optional<std::size_t>;

}  // namespace aurora::net::detail
