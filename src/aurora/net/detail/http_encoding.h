// http_encoding.h —— HTTP 应答 `Content-Encoding` 的发起侧解压管线（CHG-018 §12「内容编码」的 HTTP 接入；
// 仅库实现可见，不属公共 API）。
//
// 为何单列：本文件里是三条各自有独立判据的纯逻辑——**编码名的文法与支持集的判定**（一个字节都不碰）、
// **按编码链逐层还原字节**（core 能力 `aurora::decompress` 的物化编排）与**同一链条的流式串联**
// （core 能力 `aurora::Decompressor` 的逐段编排，`open_stream` 用）。把它们写进 `Client::send` 的协程体，
// 两条都只能靠真连接才能测；把它们写进 `core/compression.h`，又把 HTTP 特有的歧义渗进了通用 API——那里
// 明确写了「`deflate` 名义 zlib、历史服务器发裸流的歧义，其消解属 HTTP 层编排，不进本通用 API」。
// 落在这里，零 socket、零平台头即可逐值断言畸形、回落与错误映射。
//
// 五条口径（规范性，落码与文档回写以此为准）：
// - **支持集就是宣告出去的那两枚**：`Accept-Encoding` 只宣告 `gzip, deflate`；链上出现任何别的名（`br` /
//   `zstd` / 自定义）落 `http-content-encoding-unsupported` 并在文本里点名是哪一枚。构建未编入解压能力
//   （`AURORA_ENABLE_COMPRESSION=OFF`）时发起侧连宣告都不发，而服务器强行压缩同样落这一枚码——静默把压缩
//   字节当明文交出是最坏的一种失败，它污染的是调用方拿到的每一个字节。
// - **书写次序是应用次序，解压走反序**（`RFC 9110 §8.4`：`Content-Encoding: gzip, deflate` 表示载荷先经
//   gzip 再经 deflate 加工，故还原从右往左——先剥 deflate 再剥 gzip；按书写次序正向解只会解出垃圾）。
// - **`deflate` 的两型歧义在本层消解**：先按 `Zlib`（RFC 1950 包装）解，**仅当**底层报
//   `compression-data-corrupt` 才回落裸 `Deflate`（RFC 1951）；底层报越限或能力缺席时不回落——回落只会把
//   真成因盖掉。两型皆拒即 `http-body-corrupt`，文本写明两条都试过。
// - **core 码一律翻成 HTTP 出口码**：`compression-unavailable` → `http-content-encoding-unsupported`、
//   `compression-data-corrupt` → `http-body-corrupt`、`compression-output-too-large` →
//   `http-body-too-large`（文本点名越的是 `max_decompressed_bytes` 而非线上的 `max_body_bytes`）。调用方在
//   HTTP 面只面对 `http-*` 一族、旋钮与码一一对应，底层成因保留在文本里而不是第二族错误码。
// - **空值与 `identity` 都不算环节**：字段缺失、值为空、值为 `identity` 一律回空链（调用方直通原始字节）；
//   但列表里出现**空的环节**（`gzip,,deflate` 一类）是报文文法问题，落 `http-protocol-error` 而不是被忽略。
// - **流式路径的 `deflate` 歧义按包装头判定，不按「先试再回落」**：物化路径手里有全部字节，解不开可以换一
//   种格式重播；流式路径的字节一旦交给宿主就收不回来，重播等于让宿主重写已写下的那一段。故 `Deflate` 环节
//   只看**本环节最前两枚输入字节**是否构成合法 zlib 头（`RFC 1950 §2.2` 的三条硬约束），据此把格式锁死到
//   整条流。判定的代价是一条「头恰好通过 zlib 校验、体却按裸流编」的病态载荷会被报成损坏而不是被回落救回——
//   这种载荷在现实中不存在（两型的首两字节由编码器写出，不会同时满足另一型的校验），而被省下的正是「把整条
//   流留在内存里再试一次」这笔开销。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "aurora/core/compression.h"
#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 受支持的内容编码环节（枚举值即本层唯一认得的两枚编码名）。
///
/// 不含 `identity`：它按定义是「没有环节」，解析阶段就归入空链，留一个枚举值只会多一条不该分支的路径。
enum class Coding : std::uint8_t {
    Gzip,  ///< `gzip`：RFC 1952 包装，对应 `CompressionFormat::Gzip`
    Deflate,  ///< `deflate`：名义 RFC 1950（zlib）、历史服务器发 RFC 1951 裸流，两型依次尝试
};

/// @brief 发起侧应当宣告的 `Accept-Encoding` 字段值（与上面的支持集同源）。
inline constexpr std::string_view AURORA_HTTP_ACCEPT_ENCODING = "gzip, deflate";

/// @brief 本构建是否具备解压能力（`AURORA_ENABLE_COMPRESSION` 的编译期事实）。
///
/// 它是「发不发 `Accept-Encoding`」的唯一判据：不具备时恒假，发起侧据此**不宣告**任何编码，而不是宣告后
/// 再在接收侧报错。
/// @return 编入解压能力为 true，否则 false。
[[nodiscard]] auto compression_available() -> bool;

/// @brief 解析 `Content-Encoding` 字段值为编码链（保持线上书写次序，即应用次序）。
///
/// 环节之间以逗号分隔，允许首尾与分隔符周围的空白；编码名按 ASCII 大小写不敏感匹配（`RFC 9110 §8.4` 的
/// coding-token 是不敏感的）。`identity` 与空值不产环节，支持集外的名字一律失败而不是被跳过。环节名**整段
/// 匹配**，带 `;q=` 一类参数的写法不产环节而是落不支持码——参数只属于 `Accept-Encoding` 的协商语境，应答侧
/// 出现它本身就是对端越界，把它剥掉再解是替对端编造意图。
/// @param value 字段值原文（多条同名字段由调用方先按逗号合并，本函数只看一条完整列表）。
/// @return 成功携应用次序的编码链（可为空 = 无需解压）；空的列表环节落 `http-protocol-error`，未知编码名
///         落 `http-content-encoding-unsupported`。
[[nodiscard]] auto parse_content_encoding(std::string_view value) -> Result<std::vector<Coding>>;

/// @brief 按编码链逐层还原字节（反序应用），把 core 解压失败翻成 HTTP 出口码。
///
/// 空链直通原始字节（一次拷贝，语义是「没有环节要解」）；`Deflate` 环节内部先试 zlib、仅流损坏时回落裸
/// DEFLATE。逐层都按 `max_output_bytes` 设上限，故中间环节同样受炸弹防护约束。
/// @param chain 已解析的编码链（应用次序，本函数自右向左逐层还原）。
/// @param input 线上消息体字节（分帧解码后的原始载荷）。
/// @param max_output_bytes 解压产物字节上限（`std::nullopt` = 不设限）；越限落 `http-body-too-large`。
/// @return 成功携还原后的字节；失败携对应 `http-*` 码（`http-content-encoding-unsupported` /
///         `http-body-corrupt` / `http-body-too-large`），底层 `compression-*` 成因写在其文本里。
[[nodiscard]] auto decode_content_encoding(const std::vector<Coding> &chain, std::span<const char> input,
                                           std::optional<std::uint64_t> max_output_bytes) -> Result<std::vector<char>>;

/// @brief 流式还原的产出答复：宿主收了这一段之后还要不要下一段。
///
/// 与公共面的 `SinkFlow` 同语义而不复用同一枚枚举：本层属库实现，它可以被 `open_stream` 之外的流式消费者
/// 使用（SSE 切片就在路上），把 HTTP 公共类型拽进这层依赖方向就反了。
enum class EmitFlow : std::uint8_t {
    Continue,  ///< 宿主还要字节：链继续还原
    Stop,  ///< 宿主已收摊：链就此停住，剩余字节不再还原
};

/// @brief 流式还原的产出出口：每段最终明文到达时调用一次，返回值决定链是否继续。
/// @note Thread: 在驱动 `push()` / `finish()` 的线程上同步调用；回调内不得再驱动同一实例
using StreamingEmit = std::function<Result<EmitFlow>(std::span<const char>)>;

/// @brief 编码链的流式还原器：把 core 的 `Decompressor` 按链反序串成一条逐段管线。
///
/// 与物化的 `decode_content_encoding()` 同口径（书写次序是应用次序、还原从右往左、逐层都按
/// `max_output_bytes` 设上限、core 码一律翻成 `http-*` 出口码），区别只在字节何时交出：本类的每一环收到
/// 多少还原产物就往下一环喂多少，最后一环经 `StreamingEmit` 直交宿主，故整条载荷不必同时存在于内存。
///
/// 生命周期与 core 同纪律：`create()` → 若干次 `push()` → 一次 `finish()`。宿主中途回 `Stop` 时链即停住，
/// 此后**不得**再调 `finish()`——那是在宣告一条被自己掐断的流「应当完整」，只会凭空造出一枚损坏错误；
/// 收摊事实由调用方的出口值表达。
/// @note Thread: single-threaded（单实例单线程驱动；`push()` / `finish()` 内同步调用产出出口）
/// @note Side-effects: 解压分配内存（上限由解码器内部窗口与本链环数决定）；无 I/O
class StreamingDecoder {
  public:
    /// @brief 按编码链建链：每环一枚 core 解压器，末环接产出出口。
    ///
    /// 空链合法（直通：`push()` 的字节原样交给产出出口）。链上任一环需要解压而本构建未编入能力时，
    /// **在建链这一步**就落 `http-content-encoding-unsupported`，而不是等字节流进半程才失败——宣告与
    /// 支持集同源，本构建根本不该收到带编码的载荷，来了就是要么畸形要么对端越界，两种都早失败为好。
    /// @param chain 已解析的编码链（应用次序；本函数自右向左建环）。
    /// @param max_output_bytes 逐层的解压产物上限（`std::nullopt` = 不设限）；越限落 `http-body-too-large`。
    /// @param emit 产出出口；空函数即 `general-invalid-argument`（用法错误，不当协议错误报）。
    /// @return 成功携就绪实例；失败携对应 `http-*` / `general-*` 码。
    [[nodiscard]] static auto create(const std::vector<Coding> &chain, std::optional<std::uint64_t> max_output_bytes,
                                     StreamingEmit emit) -> Result<StreamingDecoder>;

    /// @brief 喂入一段线上字节（分帧解出后的载荷），还原产物经产出出口零次或多次交出。
    /// @param input 本段输入字节（可为空 = 无事发生；切分点由调用方决定，与环节边界无关）。
    /// @return 成功携宿主答复（`Stop` 时调用方应立即停止喂字节且不再 `finish()`）；失败携对应 `http-*` 码。
    [[nodiscard]] auto push(std::span<const char> input) -> Result<EmitFlow>;

    /// @brief 宣告输入结束：排出各环残留产物并逐环验证流完整终结。
    ///
    /// 只有在 `push()` 一路都回 `Continue` 时才该调用它（见类注释的「掐断不 finish」纪律）。截断与校验
    /// 不符落 `http-body-corrupt`，越上限落 `http-body-too-large`。
    /// @return 成功携宿主答复；失败携对应 `http-*` 码。
    [[nodiscard]] auto finish() -> Result<EmitFlow>;

  private:
    /// @brief 链上一环：编码名、（`deflate` 才有的）格式判定态、以及本环的 core 解压器。
    struct Stage {
        Coding coding{Coding::Gzip};  ///< 环节名：错误文本与格式判定都取之于它
        bool needs_probe{false};  ///< 是否为 `Deflate`：首两枚输入字节决定 zlib / 裸流
        std::optional<CompressionFormat> format;  ///< 已锁定的包装格式（`Deflate` 未定期为空）
        std::optional<Decompressor> decoder;  ///< 本环解压器（格式未定或一个字节都没收到时为空）
        std::vector<char> preamble;  ///< 格式未定期攒下的输入字节（两枚即可判定，故至多攒两枚）
    };

    /// @brief 本环的产出往下一环（或宿主）喂去；`index` 为本环在链里的下标。
    /// @param index 链上下标（`stages_.size()` 即宿主）。
    /// @param bytes 本环还原出的一段明文。
    /// @return 成功携宿主答复；失败携对应 `http-*` 码。
    [[nodiscard]] auto emit_from(std::size_t index, std::span<const char> bytes) -> Result<EmitFlow>;

    /// @brief 收摊一环并把它排出的产物喂下去，随后递归收摊下一环。
    /// @param index 链上下标。
    /// @return 成功携宿主答复；失败携对应 `http-*` 码。
    [[nodiscard]] auto settle(std::size_t index) -> Result<EmitFlow>;

    /// @brief 按已锁定的格式开出本环的 core 解压器。
    /// @param stage 待开环（就地写入 `decoder`）。
    /// @return 成功为 `Result<void>{}`；失败携 **core 层** 成因（由调用方翻成 HTTP 出口码）。
    [[nodiscard]] auto open(Stage &stage) const -> Result<void>;

    /// @brief 记下链内首个中止事实（幂等：后来的中止不覆盖先到的成因）。
    /// @param error 已翻成 `http-*` 码的失败原因。
    auto note_halt(Error error) -> void;

    /// @brief 链是否已中止：宿主收摊优先于错误（先到者先定，见本类注释的掐断纪律）。
    /// @return 中止原因以 `Result` 形式交出；未中止为空。
    [[nodiscard]] auto halted() const -> std::optional<Result<EmitFlow>>;

    std::vector<Stage> stages_;  ///< 还原次序的链（与 `chain` 反序）
    std::optional<std::uint64_t> max_output_bytes_;  ///< 逐层的解压产物上限
    StreamingEmit emit_;  ///< 末环之后的产出出口
    bool aborted_by_host_{false};  ///< 宿主是否已回 `Stop`
    std::optional<Error> abort_error_;  ///< 链内首个失败原因（已是 `http-*` 码，不二次换算）
};

}  // namespace aurora::net::detail
