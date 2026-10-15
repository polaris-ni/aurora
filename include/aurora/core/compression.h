#pragma once

/// @brief 通用解压能力：按格式入参的 gzip / zlib / deflate 解压（CHG-018 §12「内容编码」的 core 级落地）。
/// @file core/compression.h
///
/// 本头回答一个问题：**「把一份按某种格式压缩的字节流还原成原始字节」**。它是 core 层共用基础件
/// （与 `Headers` / `Url` 同格局），首个消费者是网络栈 HTTP 发起侧的 `Content-Encoding` 透明解压，
/// 任何需要「按格式解压」的模块都应复用本能力而不是各自 vendored 一份 inflate。
///
/// 三条形态约束（规范性，落码与文档回写以此为准）：
///
/// - **格式显式入参，不做内容嗅探**：调用方声明 `CompressionFormat`，库不猜。格式与线上包装的
///   对应是精确的——`Gzip` = RFC 1952 包装、`Zlib` = RFC 1950 包装、`Deflate` = RFC 1951 裸
///   DEFLATE。HTTP 的 `Content-Encoding: deflate` 名义上是 zlib 格式但存在历史服务器发裸流的
///   歧义，该歧义的消解（先试 Zlib、失败回落 Deflate）属 HTTP 层编排，不进本通用 API。
/// - **解压炸弹防护是调用方的显式限额**：`DecompressOptions::max_output_bytes` 缺省不限；HTTP
///   发起侧恒把物化体上限传下来。任何「压缩几十倍」的载荷在不设限额时都会按比例放大内存，
///   限额由知道业务上限的那一层决定。
/// - **压缩方向只设计不实现**：`compress()` 恒落 `compression-unavailable`。仓内复用件 wuffs
///   对压缩格式只提供解码器（无任何编码器符号），本批不引入新 vendored 编码库、亦不手写 deflate
///   压缩机；待出现真实消费者时另立提案补齐。接口先立是为了让调用点的形状稳定。
///
/// 构建开关：`AURORA_ENABLE_COMPRESSION`（默认 ON）门控本能力；关闭时全部入口恒落
/// `compression-unavailable`（诚实降级，不静默把压缩字节当明文交出）。
///
/// @code
/// // 一次性物化
/// auto out = aurora::decompress(aurora::CompressionFormat::Gzip, gzipped_bytes);
/// if (out) { use(out.value()); }
///
/// // 流式：大文件不物化，逐段经 Sink 消费
/// auto dec = aurora::Decompressor::create(aurora::CompressionFormat::Gzip, {.max_output_bytes = 64 << 20});
/// if (dec) { (void)dec.value().push(chunk, [](std::span<const char> d) { consume(d); }); }
/// @endcode
/// @note Thread: `decompress` 线程安全（无共享状态）；`Decompressor` 实例非线程安全（单实例单线程驱动）
/// @note Side-effects: 解压分配内存；无 I/O、无全局状态

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 压缩格式枚举：值精确对应线上包装形态，不做内容嗅探。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class CompressionFormat : std::uint8_t {
    Gzip,  ///< gzip 包装（RFC 1952）：HTTP `Content-Encoding: gzip` 的对应格式
    Zlib,  ///< zlib 包装（RFC 1950）：HTTP `Content-Encoding: deflate` 的名义对应格式
    Deflate,  ///< 裸 DEFLATE 流（RFC 1951，无包装头/校验尾）：HTTP `deflate` 歧义的回落形态
};

/// @brief 解压输出消费回调：每段解压产物到达时调用一次（可能零次或多次）。
/// @note Thread: 在驱动 `push` / `finish` 的线程上同步调用
/// @note Side-effects: 由回调实现决定
using DecompressSink = std::function<void(std::span<const char>)>;

/// @brief 解压选项。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct DecompressOptions {
    /// @brief 解压产物字节上限（解压炸弹防护）；`std::nullopt` = 不设限。
    ///
    /// 越限落 `compression-output-too-large`。知道业务上限的调用方（如 HTTP 发起侧的物化体上限）
    /// 应恒设此值；缺省不限是「调用方自担风险」而非推荐用法。
    std::optional<std::uint64_t> max_output_bytes;
};

/// @brief 流式解压器：逐段喂入压缩字节、逐段交出解压产物，大文件全程不物化。
///
/// 生命周期：`create()` 取得实例 → 若干次 `push()` → 一次 `finish()`。`finish()` 之后再
/// `push()` 落 `compression-data-corrupt`（单条流作用域，与 `wire::ResponseAssembler` 的同纪律）。
/// 实例 move-only（持 wuffs 解码器句柄），不可拷贝。
/// @note Thread: 非线程安全（单实例单线程驱动；`push` / `finish` 内同步调用 Sink）
/// @note Side-effects: 解压分配内存；无 I/O
class Decompressor {
  public:
    /// @brief 创建解压器并初始化底层解码器。
    /// @param format 压缩格式（显式声明，不嗅探）。
    /// @param options 解压选项（上限等）。
    /// @return 成功携就绪实例；构建未编入本能力（`AURORA_ENABLE_COMPRESSION=OFF`）或解码器
    ///         分配失败时落 `compression-unavailable`。
    [[nodiscard]] static auto create(CompressionFormat format, const DecompressOptions &options = {})
        -> Result<Decompressor>;

    /// @brief 喂入一段压缩字节，解压产物经 Sink 零次或多次交出。
    /// @param src 本段压缩字节（可空跨段：允许把一条流切成任意切分喂入）。
    /// @param sink 解压产物消费回调；同步调用，不得在回调内再驱动本实例。
    /// @return 成功为 `Result<void>{}`；流损坏/截断落 `compression-data-corrupt`，解压总量越
    ///         `max_output_bytes` 落 `compression-output-too-large`。
    auto push(std::span<const char> src, const DecompressSink &sink) -> Result<void>;

    /// @brief 宣告输入结束，排出解码器内残留产物（如 gzip 尾块前的缓冲）。
    /// @param sink 解压产物消费回调；同步调用。
    /// @return 成功为 `Result<void>{}`；流未完整终结（缺尾块/校验和不符）落
    ///         `compression-data-corrupt`。
    auto finish(const DecompressSink &sink) -> Result<void>;

    /// @brief 析构：释放底层解码器（未 `finish()` 即析构 = 放弃该条流，无额外副作用）。
    ~Decompressor();

    /// @brief 移动构造：接管底层解码器，源实例置空（再使用源落 `compression-data-corrupt`）。
    /// @param other 源实例（移动后置空）。
    Decompressor(Decompressor &&other) noexcept;

    /// @brief 移动赋值：释放自持解码器后接管对方的。
    /// @param other 源实例（移动后置空）。
    /// @return 自身引用。
    auto operator=(Decompressor &&other) noexcept -> Decompressor &;

    Decompressor(const Decompressor &) = delete;
    auto operator=(const Decompressor &) -> Decompressor & = delete;

  private:
    struct Impl;
    explicit Decompressor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

/// @brief 一次性解压：把整份压缩字节流还原为原始字节。
///
/// `Decompressor` 的便利包装（内部 create → push 全量 → finish 收集），语义逐字等价；
/// 需要「边解边用」的大载荷请改用 `Decompressor`。
/// @param format 压缩格式（显式声明，不嗅探）。
/// @param input 压缩字节（整条流）。
/// @param options 解压选项（上限等）。
/// @return 成功携解压产物；失败落 `compression-unavailable`（未编入/分配失败）、
///         `compression-data-corrupt`（流损坏/截断）、`compression-output-too-large`（越上限）。
[[nodiscard]] auto decompress(CompressionFormat format, std::span<const char> input,
                              const DecompressOptions &options = {}) -> Result<std::vector<char>>;

/// @brief 一次性压缩：**本批只设计不实现**，恒落 `compression-unavailable`。
///
/// 留出接口是为了让调用点形状稳定；仓内复用件 wuffs 无压缩编码器，本批不引入新 vendored
/// 编码库、亦不手写 deflate 压缩机，待出现真实消费者时另立提案补齐。错误 detail 会点明
/// 所请求格式，便于调用方分流。
/// @param format 压缩格式（显式声明）。
/// @param input 待压缩字节。
/// @return 恒为 `compression-unavailable` 失败态（压缩方向未实现）。
[[nodiscard]] auto compress(CompressionFormat format, std::span<const char> input) -> Result<std::vector<char>>;

}  // namespace aurora
