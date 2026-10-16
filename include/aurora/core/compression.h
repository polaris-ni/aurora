#pragma once

/// @brief 压缩解压编解码器与注册表：字符串格式 ID、可替换实现、注入路径恒在
///
/// 本头回答两个问题：**「把一份按某种格式压缩的字节流还原成原始字节」**与**「应用如何换掉或新增
/// 某个格式的实现」**。它是 core 层共用基础件（与 `Headers` / `Url` 同格局），首个消费者是网络栈
/// HTTP 发起侧的 `Content-Encoding` 透明解压；任何需要「按格式压缩 / 解压」的模块都应复用本能力
/// 而不是各自 vendored 一份 inflate。
///
/// 四条形态约束（规范性，落码与文档回写以此为准）：
///
/// - **格式 ID 是小写字符串，显式入参，不做内容嗅探**：格式与线上包装的对应是精确的——
///   `AURORA_COMPRESS_FORMAT_GZIP` = RFC 1952 包装、`AURORA_COMPRESS_FORMAT_ZLIB` = RFC 1950 包装、
///   `AURORA_COMPRESS_FORMAT_DEFLATE` = RFC 1951 裸 DEFLATE。内置三格式经预定义常量暴露（先例：
///   `net/detail/http_encoding.h` 的 `AURORA_HTTP_ACCEPT_ENCODING`）；自定义格式（zstd / brotli /
///   应用私有 ID）由应用自选字符串注册。按字节精确匹配、大小写敏感：拼错的 ID 在运行期落
///   `compression-unavailable` 且 detail 点名（诚实失败，不静默回落）。
/// - **实现可替换、注册即用**：全部入口经 `CompressionCodecRegistry` 按格式 ID 分派；同一格式 ID
///   后注册者覆盖先注册者（内置 wuffs 实现即被覆盖对象）。编解码器只写流式解压即自动获得一次性
///   入口（框架派生）；编解码器方法返回的错误原样透传（不包装、不改码）。
/// - **解压炸弹防护双层**：实现层收到的 `DecompressOptions` 即产出上限，实现应自行止步；框架层
///   再以计数装饰器兜底——越 `max_output_bytes` 即止交并落 `compression-output-too-large`，不依赖
///   实现是否自觉。限额的单一事实源是 `DecompressOptions`，装饰器是兜底而非第二真源。
/// - **注入路径恒在**：`AURORA_ENABLE_COMPRESSION` 只门控内置 wuffs 编解码器的注册；抽象、注册表
///   与自由函数恒编入——关闭宏的构建里注册自定义编解码器即可用。
///
/// 构建开关：`AURORA_ENABLE_COMPRESSION`（默认 ON，见 `BUILD_OPTIONS.md`）。
///
/// @code
/// // 一次性物化（内置 gzip）
/// auto out = aurora::decompress(aurora::AURORA_COMPRESS_FORMAT_GZIP, gzipped_bytes);
/// if (out) { use(out.value()); }
///
/// // 流式：大文件不物化，逐段经 Sink 消费
/// auto dec = aurora::create_decompressor(aurora::AURORA_COMPRESS_FORMAT_GZIP,
///                                        {.max_output_bytes = 64 << 20});
/// if (dec) { (void)dec.value()->push(chunk, [](std::span<const char> d) { consume(d); }); }
///
/// // 自定义格式一行接入（编解码器只写流式即自动获得一次性入口）
/// aurora::register_codec("zstd", std::make_shared<ZstdCodec>());
/// auto raw = aurora::decompress("zstd", packed);
/// @endcode
/// @note Thread: 注册表与全部自由函数线程安全（互斥量护槽位、锁外调用实现方代码）；
///       `Decompressor` 实例非线程安全（单实例单线程驱动）
/// @note Side-effects: 解压 / 压缩分配内存；注册表持进程级状态

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

/// @brief 内置格式 ID 常量：gzip 包装（RFC 1952），HTTP `Content-Encoding: gzip` 的对应格式。
inline constexpr std::string_view AURORA_COMPRESS_FORMAT_GZIP = "gzip";
/// @brief 内置格式 ID 常量：zlib 包装（RFC 1950），HTTP `Content-Encoding: deflate` 的名义对应格式。
inline constexpr std::string_view AURORA_COMPRESS_FORMAT_ZLIB = "zlib";
/// @brief 内置格式 ID 常量：裸 DEFLATE 流（RFC 1951，无包装头/校验尾），HTTP `deflate` 歧义的回落形态。
inline constexpr std::string_view AURORA_COMPRESS_FORMAT_DEFLATE = "deflate";

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

/// @brief 压缩选项：框架只透传不解释，数值语义由编解码器实现定义。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct CompressOptions {
    /// @brief 压缩级别；`std::nullopt` = 编解码器自定义默认。
    ///
    /// 数值语义由实现定义（如 zlib 0–9、zstd 1–22），框架不做范围校验、不跨实现归一——不为猜测中的
    /// 编码器预造旋钮阵。
    std::optional<int> level;
};

/// @brief 流式解压器抽象基类：逐段喂入压缩字节、逐段交出解压产物，大文件全程不物化。
///
/// 由 `CompressionCodec::create_decompressor` 产出，实例经 `std::unique_ptr` 持有。生命周期：
/// 取得实例 → 若干次 `push()` → 一次 `finish()`。单条流作用域（`finish()` 之后再 `push()` 落
/// `compression-data-corrupt`，与 `wire::ResponseAssembler` 的同纪律；`finish()` 幂等）由注册表
/// 交出的实例**在框架装饰器层统一收口**，编解码器实现可免于此。moved-from 即空 `unique_ptr`，
/// 经其调用属调用方用法错误（与任何 `unique_ptr` 所有权语义一致，库不另设「已移动」状态）。
/// 经基类指针删除，析构虚。
/// @note Thread: 非线程安全（单实例单线程驱动；`push` / `finish` 内同步调用 Sink）
/// @note Side-effects: 解压分配内存；无 I/O
class Decompressor {
  public:
    /// @brief 析构：释放底层解码器（未 `finish()` 即析构 = 放弃该条流，无额外副作用）。
    virtual ~Decompressor() = default;

    Decompressor(const Decompressor &) = delete;
    auto operator=(const Decompressor &) -> Decompressor & = delete;
    Decompressor(Decompressor &&) = delete;
    auto operator=(Decompressor &&) -> Decompressor & = delete;

    /// @brief 喂入一段压缩字节，解压产物经 Sink 零次或多次交出。
    /// @param src 本段压缩字节（可空跨段：允许把一条流切成任意切分喂入）。
    /// @param sink 解压产物消费回调；同步调用，不得在回调内再驱动本实例。
    /// @return 成功为 `Result<void>{}`；流损坏/截断落 `compression-data-corrupt`，解压总量越
    ///         `max_output_bytes` 落 `compression-output-too-large`。
    [[nodiscard]] virtual auto push(std::span<const char> src, const DecompressSink &sink) const -> Result<void> = 0;

    /// @brief 宣告输入结束，排出解码器内残留产物（如 gzip 尾块前的缓冲）。
    /// @param sink 解压产物消费回调；同步调用。
    /// @return 成功为 `Result<void>{}`；流未完整终结（缺尾块/校验和不符）落 `compression-data-corrupt`。
    [[nodiscard]] virtual auto finish(const DecompressSink &sink) const -> Result<void> = 0;

  protected:
    /// @brief 仅供实现方派生构造：多态基类不开放直接实例化。
    Decompressor() = default;
};

/// @brief 压缩编解码器抽象基类：声明实现名与它支持的方向，格式到实现的绑定发生在注册点。
///
/// 同一实现可注册到多个格式 ID（一份 zlib 绑定同服 gzip 与 zlib 两槽、内置 wuffs 一份实现服务
/// 三族）：`compress` / `create_decompressor` 的首参是注册表分派时**透传的槽位格式 ID**，实现据此
/// 产出对应流的编解码器，自身不持任何格式状态。全部方法 const、实现须线程安全（同 `ImageCodec`
/// 契约：注册表在锁外调用）；五法则禁拷贝移动，只经 `std::shared_ptr` 注入注册表，析构虚。
/// @note Thread: 实现方自行保证线程安全（注册表锁外调用）
/// @note Side-effects: 由实现决定
class CompressionCodec {
  public:
    /// @brief 析构。
    virtual ~CompressionCodec() = default;

    CompressionCodec(const CompressionCodec &) = delete;
    auto operator=(const CompressionCodec &) -> CompressionCodec & = delete;
    CompressionCodec(CompressionCodec &&) = delete;
    auto operator=(CompressionCodec &&) -> CompressionCodec & = delete;

    /// @brief 实现方的稳定标识，诊断与 `registered()` 列表用。
    /// @return 实现名（建议全小写短名，如 `"wuffs"` / `"my-zstd"`）。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 本实现是否支持压缩方向。
    /// @return 支持为 true；默认 false（只实现解压）。注册表在压缩分派前预检本位。
    [[nodiscard]] virtual auto can_compress() const -> bool { return false; }

    /// @brief 一次性压缩。未覆写即不支持压缩方向。
    /// @param format 注册表分派时透传的槽位格式 ID（单格式实现可忽略）。
    /// @param input 待压缩字节。
    /// @param options 压缩选项（level 语义由实现定义，框架只透传）。
    /// @return 成功携压缩产物；默认落 `general-not-supported`（detail 带 name()）。
    [[nodiscard]] virtual auto compress(std::string_view format, std::span<const char> input,
                                        const CompressOptions &options) const -> Result<std::vector<char>> {
        (void)format;
        (void)input;
        (void)options;
        return make_error(ErrorCode::GeneralNotSupported, std::string(name()) + ": compress not supported");
    }

    /// @brief 创建流式解压器。未覆写即本实现不支持解压方向。
    /// @param format 注册表分派时透传的槽位格式 ID（实现据此产出对应流的解码器）。
    /// @param options 解压选项（`max_output_bytes` 即产出上限，实现应自行止步）。
    /// @return 成功携就绪实例；默认落 `compression-unavailable`（流式是可选能力，不实现即没有）。
    [[nodiscard]] virtual auto create_decompressor(std::string_view format, const DecompressOptions &options) const
        -> Result<std::unique_ptr<Decompressor>> {
        (void)format;
        (void)options;
        return make_error(ErrorCode::CompressionUnavailable, std::string(name()) + ": no streaming decompressor");
    }

  protected:
    /// @brief 仅供实现方派生构造：多态基类不开放直接实例化。
    CompressionCodec() = default;
};

/// @brief 注册表诊断条目：格式 ID 与当前生效实现名（覆盖优先）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct RegisteredCodec {
    std::string format;  ///< 格式 ID（注册表持存的拷贝）
    std::string name;  ///< 当前生效实现的 `name()`
};

/// @brief 压缩编解码器注册表（进程单例）：按格式 ID 分派、内置 + 覆盖两张表、锁外调用实现。
///
/// `instance()` 首次访问时按 `AURORA_ENABLE_COMPRESSION` 懒注册内置 wuffs 三族解码器（键为三枚
/// 预定义常量）。`register_codec` 为**覆盖语义**：同格式 ID 后注册者替换先注册者（「替换实现」正是
/// 本能力开放的核心场景）；`unregister_codec` 清除覆盖并回落内置（若有），无内置亦无覆盖时该格式
/// 不可用——测试隔离因此确定：注销即回默认态，不会把后续用例留在自定义实现上。注销不影响已创建的
/// 在途解压器（实例由调用方持有，与注册表槽位脱钩）。分派时锁内取 `shared_ptr` 副本、锁外调用实现
/// ——不在持锁期间运行实现方代码，与实现内重入注册也不会互锁。
/// @note Thread: 线程安全（互斥量护槽位；全部方法 const，内部可变状态）
/// @note Side-effects: 首次访问时构造进程级单例并按宏注册内置实现
class CompressionCodecRegistry {
  public:
    /// @brief 进程唯一注册表实例；首次访问时按 `AURORA_ENABLE_COMPRESSION` 懒注册内置实现。
    /// @return 全局单例的引用。
    static auto instance() -> CompressionCodecRegistry &;

    /// @brief 注册编解码器（覆盖语义）：同格式 ID 后注册者替换先注册者；空指针忽略。
    /// @param format 格式 ID（按字节精确匹配；内置三格式见预定义常量，自定义 ID 自选）。
    /// @param codec 实现实例（空 `shared_ptr` 忽略）。
    auto register_codec(std::string_view format, std::shared_ptr<CompressionCodec> codec) const -> void;

    /// @brief 注销该格式 ID 的覆盖注册并回落内置（若有）；无内置亦无覆盖时该格式不可用。
    /// @param format 格式 ID。
    auto unregister_codec(std::string_view format) const -> void;

    /// @brief 诊断列表：逐格式给出当前生效实现（覆盖优先）。
    /// @return 条目按格式 ID 字典序排列；无内置且无覆盖的构建为空表。
    [[nodiscard]] auto registered() const -> std::vector<RegisteredCodec>;

    /// @brief 一次性压缩：按格式 ID 分派到已注册实现（分派前预检 `can_compress()`）。
    /// @param format 格式 ID。
    /// @param input 待压缩字节。
    /// @param options 压缩选项（透传给实现）。
    /// @return 成功携压缩产物；无实现落 `compression-unavailable`（detail 点名格式 ID），有实现但不
    ///         支持压缩方向同样落该码（detail 点名实现名与缺失方向）；实现返回的错误原样透传。
    [[nodiscard]] auto compress(std::string_view format, std::span<const char> input,
                                const CompressOptions &options = {}) const -> Result<std::vector<char>>;

    /// @brief 一次性解压：按格式 ID 分派，内部由流式派生（create → push 全量 → finish 收集）。
    /// @param format 格式 ID。
    /// @param input 压缩字节（整条流）。
    /// @param options 解压选项（上限等）。
    /// @return 成功携解压产物；失败落 `compression-unavailable`（无实现）、`compression-data-corrupt`
    ///         （流损坏/截断）、`compression-output-too-large`（越上限）。
    [[nodiscard]] auto decompress(std::string_view format, std::span<const char> input,
                                  const DecompressOptions &options = {}) const -> Result<std::vector<char>>;

    /// @brief 创建流式解压器：按格式 ID 分派；产出的实例经计数装饰器包裹（越限兜底在框架层）。
    /// @param format 格式 ID。
    /// @param options 解压选项（上限等）。
    /// @return 成功携就绪实例；无实现落 `compression-unavailable`；实现返回的错误原样透传。
    [[nodiscard]] auto create_decompressor(std::string_view format, const DecompressOptions &options = {}) const
        -> Result<std::unique_ptr<Decompressor>>;

  private:
    CompressionCodecRegistry();
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// 高层便捷自由函数：每个都是一行转调 CompressionCodecRegistry::instance() 的同名方法，
// 供调用点省略单例样板（等价形态，见类内对应成员函数的完整契约说明）。

/// @brief 注册编解码器（转调注册表同名方法；覆盖语义、空指针忽略）。
/// @param format 格式 ID。
/// @param codec 实现实例。
auto register_codec(std::string_view format, std::shared_ptr<CompressionCodec> codec) -> void;

/// @brief 注销该格式 ID 的覆盖注册并回落内置（转调注册表同名方法）。
/// @param format 格式 ID。
auto unregister_codec(std::string_view format) -> void;

/// @brief 一次性压缩（转调注册表同名方法）。
/// @param format 格式 ID（显式声明，不嗅探）。
/// @param input 待压缩字节。
/// @param options 压缩选项（透传给实现）。
/// @return 成功携压缩产物；失败落 `compression-unavailable`（无实现 / 实现不支持压缩方向），
///         实现自身的错误原样透传。
[[nodiscard]] auto compress(std::string_view format, std::span<const char> input, const CompressOptions &options = {})
    -> Result<std::vector<char>>;

/// @brief 一次性解压（转调注册表同名方法；内部由流式派生，语义与流式逐字等价）。
/// @param format 格式 ID（显式声明，不嗅探）。
/// @param input 压缩字节（整条流）。
/// @param options 解压选项（上限等）。
/// @return 成功携解压产物；失败落 `compression-unavailable`（无实现）、`compression-data-corrupt`
///         （流损坏/截断）、`compression-output-too-large`（越上限）。
[[nodiscard]] auto decompress(std::string_view format, std::span<const char> input,
                              const DecompressOptions &options = {}) -> Result<std::vector<char>>;

/// @brief 创建流式解压器（转调注册表同名方法；产出实例经框架层护栏包裹）。
/// @param format 格式 ID（显式声明，不嗅探）。
/// @param options 解压选项（上限等）。
/// @return 成功携就绪实例；失败落 `compression-unavailable`（无实现），实现自身的错误原样透传。
[[nodiscard]] auto create_decompressor(std::string_view format, const DecompressOptions &options = {})
    -> Result<std::unique_ptr<Decompressor>>;

}  // namespace aurora
