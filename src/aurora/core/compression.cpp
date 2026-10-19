#include "aurora/core/compression.h"

#include <cstddef>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef AURORA_ENABLE_COMPRESSION

// wuffs 官方单文件集成约定：整份 .c 当头包含（此处只取声明，WUFFS_IMPLEMENTATION 由
// cmake 的 aurora_wuffs OBJECT 库在唯一 TU 定义、链接进 aurora 提供实现）。就地豁免：
// 这是 wuffs 唯一支持的集成用法，非误包含 C 源文件（图片编解码同此约定）。
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "wuffs/release/c/wuffs-v0.3.c"

#endif

namespace aurora {

namespace {

/// @brief 解压产物暂存块字节数：wuffs io_buffer 的 dst 容量，兼顾系统调用次数与拷贝量。
constexpr std::size_t AURORA_OUTPUT_CHUNK_BYTES = std::size_t{64} * 1024;

auto corrupt_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::CompressionDataCorrupt, std::string(detail));
}

/// @brief 「格式无任何编解码器」的 detail：点名格式 ID；宏关闭的构建追加内置缺席成因字样
///        （HTTP 侧既有两例的 message 文本断言依赖 `AURORA_ENABLE_COMPRESSION` 字样）。
auto unavailable_detail(std::string_view format) -> std::string {
    std::string detail = "no codec registered for format '" + std::string(format) + "'";
#ifndef AURORA_ENABLE_COMPRESSION
    detail += " (builtin decoders not compiled in: AURORA_ENABLE_COMPRESSION=OFF)";
#endif
    return detail;
}

/// @brief 注册表交出的每个解压器都经本装饰器包裹：越 `max_output_bytes` 即止交并落
///        `compression-output-too-large`，不依赖实现是否自觉（框架层兜底护栏）。
///
/// 三条语义：越限之后的字节不再交调用方（wrapper 丢弃，不转发 Sink）；同一轮里越限标志优先于
/// 解码器错误；push-after-finish 与 finish 幂等在此统一收口，编解码器实现可免于自行维护流态。
/// 限额的单一事实源仍是 `DecompressOptions`——实现层（如内置 wuffs）自行止步省 CPU，装饰器只保证
/// 交付面。经 `std::unique_ptr` 持有，moved-from 即空指针（调用属调用方用法错误，库不设「已移动」态）。
/// @note Thread: 非线程安全（单实例单线程驱动，与被包裹实例同纪律）
class GuardedDecompressor final : public Decompressor {
  public:
    /// @brief 包裹一个实现方解压器。
    /// @param format 分派时的格式 ID（错误 detail 用）。
    /// @param inner 实现方实例（注册表已拒空，调用方须传非空）。
    /// @param max_output_bytes 解压产物字节上限（`std::nullopt` = 不设限）。
    GuardedDecompressor(std::string format, std::unique_ptr<Decompressor> inner,
                        std::optional<std::uint64_t> max_output_bytes)
        : format_(std::move(format)), inner_(std::move(inner)), max_output_bytes_(max_output_bytes) {}

    auto push(std::span<const char> src, const DecompressSink &sink) const -> Result<void> override {
        if (finished_) {
            return corrupt_error("push after finish (single-stream scope)");
        }
        std::optional<Error> over_limit;
        const auto guarded = [this, &sink, &over_limit](std::span<const char> chunk) -> void {
            if (over_limit.has_value()) {
                return;  // 越限之后的字节丢弃，绝不交调用方
            }
            if (max_output_bytes_.has_value() && produced_ + chunk.size() > *max_output_bytes_) {
                over_limit = make_error(ErrorCode::CompressionOutputTooLarge,
                                        "decompressed output exceeds max_output_bytes (" +
                                            std::to_string(*max_output_bytes_) + " bytes) for format " + format_);
                return;
            }
            produced_ += chunk.size();
            sink(chunk);
        };
        auto pushed = inner_->push(src, guarded);
        if (over_limit.has_value()) {
            return over_limit.value();  // 越限标志优先于解码器错误（与内置实现 emit_output 次序一致）
        }
        return pushed;
    }

    auto finish(const DecompressSink &sink) const -> Result<void> override {
        if (finished_) {
            return Result<void>{};  // 幂等：流已完整终结，无残留可排。
        }
        std::optional<Error> over_limit;
        const auto guarded = [this, &sink, &over_limit](std::span<const char> chunk) -> void {
            if (over_limit.has_value()) {
                return;
            }
            if (max_output_bytes_.has_value() && produced_ + chunk.size() > *max_output_bytes_) {
                over_limit = make_error(ErrorCode::CompressionOutputTooLarge,
                                        "decompressed output exceeds max_output_bytes (" +
                                            std::to_string(*max_output_bytes_) + " bytes) for format " + format_);
                return;
            }
            produced_ += chunk.size();
            sink(chunk);
        };
        auto flushed = inner_->finish(guarded);
        if (over_limit.has_value()) {
            return over_limit.value();
        }
        if (flushed) {
            finished_ = true;
        }
        return flushed;
    }

  private:
    std::string format_;  ///< 分派时的格式 ID（错误 detail 用）
    std::unique_ptr<Decompressor> inner_;  ///< 实现方实例（注册表保证非空）
    std::optional<std::uint64_t> max_output_bytes_;
    mutable std::uint64_t produced_{0};  ///< 已交付的解压产物累计（跨 push / finish 单调）
    mutable bool finished_{false};  ///< 流是否已完整终结（push-after-finish 与 finish 幂等在此收口）
};

#ifdef AURORA_ENABLE_COMPRESSION

/// @brief wuffs 家族枚举：注册表以字符串 ID 分派，进入实现侧收敛为三族之一。
enum class WuffsFamily : std::uint8_t { Gzip, Zlib, Deflate };

/// @brief 格式 ID → wuffs 家族（内置表只注册三枚预定义常量键，恒命中）。
auto wuffs_family_of(std::string_view format) -> WuffsFamily {
    if (format == AURORA_COMPRESS_FORMAT_GZIP) {
        return WuffsFamily::Gzip;
    }
    if (format == AURORA_COMPRESS_FORMAT_ZLIB) {
        return WuffsFamily::Zlib;
    }
    return WuffsFamily::Deflate;
}

/// @brief 三族解码器的统一创建：wuffs 每族一对入口，此处按家族分派。
auto wuffs_decoder_alloc(WuffsFamily family) -> void * {
    switch (family) {
        case WuffsFamily::Gzip:
            return wuffs_gzip__decoder__alloc();
        case WuffsFamily::Zlib:
            return wuffs_zlib__decoder__alloc();
        case WuffsFamily::Deflate:
            return wuffs_deflate__decoder__alloc();
    }
    return nullptr;
}

auto wuffs_decoder_workbuf_len(WuffsFamily family, void *decoder) -> std::size_t {
    const auto worst = [&]() -> std::uint64_t {
        switch (family) {
            case WuffsFamily::Gzip:
                return wuffs_gzip__decoder__workbuf_len(static_cast<wuffs_gzip__decoder *>(decoder)).max_incl;
            case WuffsFamily::Zlib:
                return wuffs_zlib__decoder__workbuf_len(static_cast<wuffs_zlib__decoder *>(decoder)).max_incl;
            case WuffsFamily::Deflate:
                return wuffs_deflate__decoder__workbuf_len(static_cast<wuffs_deflate__decoder *>(decoder)).max_incl;
        }
        return 0;
    }();
    return static_cast<std::size_t>(worst);
}

/// @brief 三族解码器的统一转换入口：从 src 消费压缩字节、向 dst 产出解压字节。
auto wuffs_decoder_transform(WuffsFamily family, void *decoder, wuffs_base__io_buffer *dst, wuffs_base__io_buffer *src,
                             wuffs_base__slice_u8 work) -> wuffs_base__status {
    switch (family) {
        case WuffsFamily::Gzip:
            return wuffs_gzip__decoder__transform_io(static_cast<wuffs_gzip__decoder *>(decoder), dst, src, work);
        case WuffsFamily::Zlib:
            return wuffs_zlib__decoder__transform_io(static_cast<wuffs_zlib__decoder *>(decoder), dst, src, work);
        case WuffsFamily::Deflate:
            return wuffs_deflate__decoder__transform_io(static_cast<wuffs_deflate__decoder *>(decoder), dst, src, work);
    }
    return wuffs_base__make_status("compression: unreachable family dispatch");
}

/// @brief wuffs 解码器的释放器：`wuffs_*__decoder__alloc()` 按 libc 堆分配（wuffs 自带 C++
/// 助手即以 `unique_ptr(alloc(), &free)` 配对），故经 unique_ptr 持有、析构即还。
struct WuffsDecoderDeleter {
    auto operator()(void *decoder) const noexcept -> void {
        std::free(decoder);  // NOLINT(*-no-malloc) wuffs alloc 与 libc free 配对，见类型注释
    }
};

/// @brief 内置 wuffs 流式解压器（库内部类型，不出公共头）：驱动口径与实现逐字沿用既有实现。
///
/// wuffs 驱动要点（本仓实测钉死，勿改）：io_transformer 是可恢复协程，每次调用从 `a_dst->meta.wi`
/// 重建续写指针、挂起点把新产产物快照进内部 32 KiB 窗口——驱动循环必须保留 dst 写位置、消费只推进
/// `meta.ri`、缓冲满时 `compact`；轮次间重置 `wi` 会令内部历史计数错位，表现为伪
/// `#deflate: bad distance`（大载荷跨轮必触发）。
class WuffsDecompressor final : public Decompressor {
  public:
    /// @brief 接管一个已分配的 wuffs 解码器。
    /// @param family 解码器家族（决定 transform 入口）。
    /// @param format_name 格式 ID 文本（错误 detail 用，即注册表的槽位键）。
    /// @param max_output_bytes 解压产物上限（实现层止步，越限即拒不吐后续字节）。
    /// @param decoder wuffs 解码器句柄（libc 堆分配，析构经 Deleter 归还）。
    /// @param work wuffs 工作缓冲（workbuf_len，三族均为个位数量级）。
    WuffsDecompressor(WuffsFamily family, std::string format_name, std::optional<std::uint64_t> max_output_bytes,
                      std::unique_ptr<void, WuffsDecoderDeleter> decoder, std::vector<std::uint8_t> work)
        : family_(family), format_name_(std::move(format_name)), max_output_bytes_(max_output_bytes),
          decoder_(std::move(decoder)), work_(std::move(work)) {
        staging_.resize(AURORA_OUTPUT_CHUNK_BYTES);
        out_.data.ptr = staging_.data();
        out_.data.len = staging_.size();
    }

    auto push(std::span<const char> src, const DecompressSink &sink) const -> Result<void> override {
        if (finished_) {
            return corrupt_error("push after finish (single-stream scope)");
        }
        if (src.empty()) {
            return Result<void>{};  // 空跨段合法：无事发生，等真正的输入。
        }
        wuffs_base__io_buffer in{};
        // wuffs io_buffer 只要可写指针（绝不写 src），char→uint8_t 为字节视图转换，就地豁免。
        // NOLINTNEXTLINE(*-pro-type-const-cast,*-pro-type-reinterpret-cast)
        in.data.ptr = const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(src.data()));
        in.data.len = src.size();
        in.meta.wi = src.size();
        in.meta.closed = false;
        return drive(&in, false, sink);
    }

    auto finish(const DecompressSink &sink) const -> Result<void> override {
        if (finished_) {
            return Result<void>{};  // 幂等：流已完整终结，无残留可排。
        }
        wuffs_base__io_buffer in{};  // 空输入且标记 closed：驱动残留产物排出并验证流完整终结。
        in.data.ptr = nullptr;
        in.data.len = 0;
        in.meta.wi = 0;
        in.meta.closed = true;
        return drive(&in, true, sink);
    }

  private:
    /// @brief 把一段解压产物 [begin, end) 经 Sink 交出，并累计限额计数（越限即止，不再吐出后续字节）。
    auto emit_output(std::size_t begin, std::size_t end, const DecompressSink &sink) const -> Result<void> {
        if (end <= begin) {
            return Result<void>{};
        }
        const auto len = end - begin;
        if (max_output_bytes_.has_value()) {
            const auto cap = *max_output_bytes_;
            if (produced_ + static_cast<std::uint64_t>(len) > cap) {
                return make_error(ErrorCode::CompressionOutputTooLarge,
                                  "decompressed output exceeds max_output_bytes (" + std::to_string(cap) +
                                      " bytes) for format " + format_name_);
            }
        }
        produced_ += static_cast<std::uint64_t>(len);
        // 解压产物按字节持存、Sink 契约为 char 视图：整窗一次转型后按 span 取子区间（不做指针算术）。
        // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
        const auto *staging_chars = reinterpret_cast<const char *>(staging_.data());
        sink(std::span<const char>(staging_chars, staging_.size()).subspan(begin, len));
        return Result<void>{};
    }

    /// @brief 驱动一轮解码循环：消费 in（ri→wi）向 out 产出，产物经 Sink 交出。
    ///
    /// 循环终止条件：输入耗尽（等更多切分）、流完整（end-of-data note）、出错。no-progress
    /// 守卫保证「既不消费输入也不产出输出且无状态返回」时不空转。
    auto drive(wuffs_base__io_buffer *in, bool input_closed, const DecompressSink &sink) const -> Result<void> {
        auto *out = &out_;
        const auto work = wuffs_base__make_slice_u8(work_.data(), work_.size());
        while (true) {
            const std::size_t src_before = in->meta.ri;
            const wuffs_base__status z = wuffs_decoder_transform(family_, decoder_.get(), out, in, work);
            // 交出本轮产物 [ri, wi) 并推进消费点。⚠️ 写位置 wi 必须原样留给解码器：wuffs 的
            // io_transformer 是可恢复协程，每次调用从 a_dst->meta.wi 重建续写指针，并在挂起点
            // 把 [调用前 wi, 现 wi) 快照进内部 32 KiB 窗口；轮次间把 wi 重置为 0 会令写位置回退、
            // 内部历史计数与缓冲区内容错位，表现为伪 `#deflate: bad distance`（本仓实测：小载荷
            // 单轮完成不触发，大载荷跨轮必触发）。
            if (auto emitted = emit_output(out->meta.ri, out->meta.wi, sink); !emitted) {
                return emitted;
            }
            out->meta.ri = out->meta.wi;
            if (z.is_error()) {
                return corrupt_error(std::string(z.repr != nullptr ? z.repr : "decoder error"));
            }
            if (z.is_complete()) {
                finished_ = true;
                if (in->meta.ri < in->meta.wi) {
                    return corrupt_error("trailing bytes after end of stream for format " + format_name_);
                }
                return Result<void>{};
            }
            if (out->meta.wi == out->data.len) {
                // 输出缓冲满：消费点已追平写点，compact 让出整窗（历史已由解码器内部快照，不依赖
                // dst 保留旧字节）。
                wuffs_base__io_buffer__compact(out);
                continue;
            }
            if (in->meta.ri == in->meta.wi) {
                if (input_closed) {
                    // 输入已 closed 且耗尽而流未完整终结：截断（上面那次调用已给了解码器排空机会）。
                    return corrupt_error("truncated input: stream not finished for format " + format_name_);
                }
                return Result<void>{};  // 等更多切分
            }
            if (in->meta.ri == src_before && out->meta.wi == out->meta.ri) {
                // 既不耗输入也不产输出且输出尚有空间：解码器需要更多输入才能继续。
                if (input_closed) {
                    return corrupt_error("truncated input: stream not finished for format " + format_name_);
                }
                return Result<void>{};
            }
        }
    }

    WuffsFamily family_;  ///< 解码器家族（transform 入口分派）
    std::string format_name_;  ///< 格式 ID 文本（错误 detail 用）
    std::optional<std::uint64_t> max_output_bytes_;  ///< 实现层止步上限（与装饰器同源 DecompressOptions）
    std::unique_ptr<void, WuffsDecoderDeleter> decoder_;  ///< wuffs 解码器句柄
    std::vector<std::uint8_t> staging_;  ///< 解压产物暂存（AURORA_OUTPUT_CHUNK_BYTES）
    mutable std::vector<std::uint8_t> work_;  ///< wuffs 工作缓冲（transform_io 经指针就地写，const 方法需穿透）
    mutable wuffs_base__io_buffer out_{};  ///< 指向 staging 的输出 io_buffer（meta 每轮推进）
    mutable std::uint64_t produced_{0};  ///< 已交付累计（const push / finish 内单调推进）
    mutable bool finished_{false};  ///< 流是否已完整终结
};

/// @brief 内置 wuffs 编解码器：只实现解压方向（wuffs 无任何编码器符号），一份实例服务三族槽位。
class WuffsCodec final : public CompressionCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "wuffs"; }

    [[nodiscard]] auto create_decompressor(std::string_view format, const DecompressOptions &options) const
        -> Result<std::unique_ptr<Decompressor>> override {
        const auto family = wuffs_family_of(format);
        auto decoder = std::unique_ptr<void, WuffsDecoderDeleter>(wuffs_decoder_alloc(family));
        if (decoder == nullptr) {
            return make_error(ErrorCode::CompressionUnavailable,
                              "decoder allocation failed for format " + std::string(format));
        }
        auto work = std::vector<std::uint8_t>(wuffs_decoder_workbuf_len(family, decoder.get()));
        std::unique_ptr<Decompressor> instance = std::make_unique<WuffsDecompressor>(
            family, std::string(format), options.max_output_bytes, std::move(decoder), std::move(work));
        return instance;
    }
};

#endif  // AURORA_ENABLE_COMPRESSION

}  // namespace

// ---------------------------------------------------------------------------
// CompressionCodecRegistry：内置 + 覆盖两张表、互斥量护槽位、锁外调用实现方代码。
// ---------------------------------------------------------------------------

struct CompressionCodecRegistry::Impl {
    /// 内置表：`instance()` 构造时按宏懒注册（宏关即空表，注入路径不受影响）。
    std::map<std::string, std::shared_ptr<CompressionCodec>, std::less<>> builtin;
    /// 覆盖表：应用经 register_codec 写入、unregister_codec 清除（覆盖语义）。
    std::map<std::string, std::shared_ptr<CompressionCodec>, std::less<>> overrides;
    std::mutex mutex;

    /// @brief 锁内查找并取 `shared_ptr` 副本：覆盖优先、回落内置。
    /// @param format 格式 ID。
    /// @return 命中的实现；无内置亦无覆盖为空。
    auto find(std::string_view format) -> std::shared_ptr<CompressionCodec> {
        const std::scoped_lock lock(mutex);
        if (const auto hit = overrides.find(format); hit != overrides.end()) {
            return hit->second;
        }
        if (const auto hit = builtin.find(format); hit != builtin.end()) {
            return hit->second;
        }
        return nullptr;
    }
};

auto CompressionCodecRegistry::instance() -> CompressionCodecRegistry & {
    // 函数内静态：首次访问即构造并按宏注册内置实现；进程单例（与 ImageCodecRegistry 同形态）。
    static CompressionCodecRegistry registry;
    return registry;
}

CompressionCodecRegistry::CompressionCodecRegistry() : impl_(std::make_shared<Impl>()) {
#ifdef AURORA_ENABLE_COMPRESSION
    // 内置 wuffs 编解码器：一份实现注册到三族格式槽，族间分派经槽位格式透传完成。
    const auto wuffs = std::make_shared<WuffsCodec>();
    impl_->builtin.emplace(std::string(AURORA_COMPRESS_FORMAT_GZIP), wuffs);
    impl_->builtin.emplace(std::string(AURORA_COMPRESS_FORMAT_ZLIB), wuffs);
    impl_->builtin.emplace(std::string(AURORA_COMPRESS_FORMAT_DEFLATE), wuffs);
#endif
}

auto CompressionCodecRegistry::register_codec(std::string_view format, std::shared_ptr<CompressionCodec> codec) const
    -> void {
    if (codec == nullptr) {
        return;  // 空指针忽略：无实现的注册是调用方笔误，不当条目收。
    }
    const std::scoped_lock lock(impl_->mutex);
    impl_->overrides.insert_or_assign(std::string(format), std::move(codec));
}

auto CompressionCodecRegistry::unregister_codec(std::string_view format) const -> void {
    const std::scoped_lock lock(impl_->mutex);
    impl_->overrides.erase(std::string(format));  // 只清覆盖；内置槽位恒在（宏开的构建），注销即回落
}

auto CompressionCodecRegistry::registered() const -> std::vector<RegisteredCodec> {
    // 锁内只取 (格式, shared_ptr) 副本，name() 在锁外调用——不在持锁期间运行实现方代码。
    std::vector<std::pair<std::string, std::shared_ptr<CompressionCodec>>> snapshot;
    {
        const std::scoped_lock lock(impl_->mutex);
        auto builtin_it = impl_->builtin.begin();
        auto override_it = impl_->overrides.begin();
        // 两张表按键归并：同键覆盖优先，输出按键字典序。
        while (builtin_it != impl_->builtin.end() || override_it != impl_->overrides.end()) {
            const bool builtin_first = builtin_it != impl_->builtin.end() && (override_it == impl_->overrides.end() ||
                                                                              builtin_it->first < override_it->first);
            if (builtin_first) {
                snapshot.emplace_back(builtin_it->first, builtin_it->second);
                ++builtin_it;
                continue;
            }
            const bool keys_equal = builtin_it != impl_->builtin.end() && override_it != impl_->overrides.end() &&
                                    builtin_it->first == override_it->first;
            snapshot.emplace_back(override_it->first, override_it->second);
            ++override_it;
            if (keys_equal) {
                ++builtin_it;  // 覆盖生效的槽位：内置那份不重复输出
            }
        }
    }
    std::vector<RegisteredCodec> out;
    out.reserve(snapshot.size());
    for (auto &[format, codec] : snapshot) {
        out.push_back(RegisteredCodec{.format = std::move(format), .name = std::string(codec->name())});
    }
    return out;
}

auto CompressionCodecRegistry::compress(std::string_view format, std::span<const char> input,
                                        const CompressOptions &options) const -> Result<std::vector<char>> {
    const auto codec = impl_->find(format);
    if (codec == nullptr) {
        return make_error(ErrorCode::CompressionUnavailable, unavailable_detail(format));
    }
    if (!codec->can_compress()) {
        // 「有编解码器但方向缺席」与「格式无实现」是两种 detail：这里点名实现名与缺失的方向。
        return make_error(ErrorCode::CompressionUnavailable, "codec '" + std::string(codec->name()) +
                                                                 "' registered for format '" + std::string(format) +
                                                                 "' does not support compression");
    }
    return codec->compress(format, input, options);  // 锁外调用；实现错误原样透传
}

auto CompressionCodecRegistry::decompress(std::string_view format, std::span<const char> input,
                                          const DecompressOptions &options) const -> Result<std::vector<char>> {
    // 一次性入口由流式派生（create → push 全量 → finish 收集），语义与流式逐字等价。
    auto created = create_decompressor(format, options);
    if (!created) {
        return created.error();
    }
    auto &decoder = created.value();
    std::vector<char> out;
    const auto collect = [&out](std::span<const char> chunk) { out.insert(out.end(), chunk.begin(), chunk.end()); };
    if (auto pushed = decoder->push(input, collect); !pushed) {
        return pushed.error();
    }
    if (auto finished = decoder->finish(collect); !finished) {
        return finished.error();
    }
    return out;
}

auto CompressionCodecRegistry::create_decompressor(std::string_view format, const DecompressOptions &options) const
    -> Result<std::unique_ptr<Decompressor>> {
    const auto codec = impl_->find(format);
    if (codec == nullptr) {
        return make_error(ErrorCode::CompressionUnavailable, unavailable_detail(format));
    }
    auto created = codec->create_decompressor(format, options);  // 锁外调用；实现错误原样透传
    if (!created) {
        return created.error();
    }
    if (created.value() == nullptr) {
        // 成功态携空指针是「说谎的实现」：如实报能力缺席，而不是让调用方拿到必错的实例。
        return make_error(ErrorCode::CompressionUnavailable, "codec '" + std::string(codec->name()) +
                                                                 "' returned a null decompressor for format '" +
                                                                 std::string(format) + "'");
    }
    // 框架层兜底护栏：计数装饰器包裹（限额单一事实源仍是 DecompressOptions）。
    return std::unique_ptr<Decompressor>(std::make_unique<GuardedDecompressor>(
        std::string(format), std::move(created.value()), options.max_output_bytes));
}

// ---------------------------------------------------------------------------
// 高层便捷自由函数：一行转调进程单例同名方法。
// ---------------------------------------------------------------------------

auto register_codec(std::string_view format, std::shared_ptr<CompressionCodec> codec) -> void {
    CompressionCodecRegistry::instance().register_codec(format, std::move(codec));
}

auto unregister_codec(std::string_view format) -> void {
    CompressionCodecRegistry::instance().unregister_codec(format);
}

auto compress(std::string_view format, std::span<const char> input, const CompressOptions &options)
    -> Result<std::vector<char>> {
    return CompressionCodecRegistry::instance().compress(format, input, options);
}

auto decompress(std::string_view format, std::span<const char> input, const DecompressOptions &options)
    -> Result<std::vector<char>> {
    return CompressionCodecRegistry::instance().decompress(format, input, options);
}

auto create_decompressor(std::string_view format, const DecompressOptions &options)
    -> Result<std::unique_ptr<Decompressor>> {
    return CompressionCodecRegistry::instance().create_decompressor(format, options);
}

}  // namespace aurora
