#include "aurora/core/compression.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

/// @brief 格式的稳定文本名：仅用于错误 detail，不参与线上协商（线上令牌映射属 HTTP 层）。
auto format_name(CompressionFormat format) -> std::string_view {
    switch (format) {
        case CompressionFormat::Gzip:
            return "gzip";
        case CompressionFormat::Zlib:
            return "zlib";
        case CompressionFormat::Deflate:
            return "deflate";
    }
    return "unknown";
}

auto corrupt_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::CompressionDataCorrupt, std::string(detail));
}

#ifdef AURORA_ENABLE_COMPRESSION

/// @brief 三族解码器的统一创建：wuffs 每族一对入口，此处按格式分派。
auto wuffs_decoder_alloc(CompressionFormat format) -> void * {
    switch (format) {
        case CompressionFormat::Gzip:
            return wuffs_gzip__decoder__alloc();
        case CompressionFormat::Zlib:
            return wuffs_zlib__decoder__alloc();
        case CompressionFormat::Deflate:
            return wuffs_deflate__decoder__alloc();
    }
    return nullptr;
}

auto wuffs_decoder_workbuf_len(CompressionFormat format, void *decoder) -> std::size_t {
    const auto worst = [&]() -> std::uint64_t {
        switch (format) {
            case CompressionFormat::Gzip:
                return wuffs_gzip__decoder__workbuf_len(static_cast<wuffs_gzip__decoder *>(decoder)).max_incl;
            case CompressionFormat::Zlib:
                return wuffs_zlib__decoder__workbuf_len(static_cast<wuffs_zlib__decoder *>(decoder)).max_incl;
            case CompressionFormat::Deflate:
                return wuffs_deflate__decoder__workbuf_len(static_cast<wuffs_deflate__decoder *>(decoder)).max_incl;
        }
        return 0;
    }();
    return static_cast<std::size_t>(worst);
}

/// @brief 三族解码器的统一转换入口：从 src 消费压缩字节、向 dst 产出解压字节。
auto wuffs_decoder_transform(CompressionFormat format, void *decoder, wuffs_base__io_buffer *dst,
                             wuffs_base__io_buffer *src, wuffs_base__slice_u8 work) -> wuffs_base__status {
    switch (format) {
        case CompressionFormat::Gzip:
            return wuffs_gzip__decoder__transform_io(static_cast<wuffs_gzip__decoder *>(decoder), dst, src, work);
        case CompressionFormat::Zlib:
            return wuffs_zlib__decoder__transform_io(static_cast<wuffs_zlib__decoder *>(decoder), dst, src, work);
        case CompressionFormat::Deflate:
            return wuffs_deflate__decoder__transform_io(static_cast<wuffs_deflate__decoder *>(decoder), dst, src, work);
    }
    return wuffs_base__make_status("compression: unreachable format dispatch");
}

/// @brief wuffs 解码器的释放器：`wuffs_*__decoder__alloc()` 按 libc 堆分配（wuffs 自带 C++
/// 助手即以 `unique_ptr(alloc(), &free)` 配对），故经 unique_ptr 持有、析构即还。
struct WuffsDecoderDeleter {
    auto operator()(void *decoder) const noexcept -> void {
        std::free(decoder);  // NOLINT(*-no-malloc) wuffs alloc 与 libc free 配对，见类型注释
    }
};

#endif  // AURORA_ENABLE_COMPRESSION

}  // namespace

struct Decompressor::Impl {
    CompressionFormat format{CompressionFormat::Gzip};
    std::optional<std::uint64_t> max_output_bytes;
    std::uint64_t produced{0};
    bool finished{false};
#ifdef AURORA_ENABLE_COMPRESSION
    /// wuffs 解码器：三族入口各异，故以 void* 持有、用时按格式转具体类型；分配失败即空，
    /// 析构经 WuffsDecoderDeleter 交还 libc 堆。
    std::unique_ptr<void, WuffsDecoderDeleter> decoder;
    std::vector<std::uint8_t> work;  ///< wuffs 工作缓冲（workbuf_len，三族均为个位数量级）
    std::vector<std::uint8_t> staging;  ///< 解压产物暂存（AURORA_OUTPUT_CHUNK_BYTES）
    wuffs_base__io_buffer out{};  ///< 指向 staging 的输出 io_buffer（meta 每轮重置）

    /// @brief 把一段解压产物 [begin, end) 经 Sink 交出，并累计限额计数（越限即止，不再吐出后续字节）。
    static auto emit_output(Impl *impl, std::size_t begin, std::size_t end, const DecompressSink &sink) -> Result<void>;

    /// @brief 驱动一轮解码循环：消费 in（ri→wi）向 out 产出，产物经 Sink 交出。
    ///
    /// 循环终止条件：输入耗尽（等更多切分）、流完整（end-of-data note）、出错。no-progress
    /// 守卫保证「既不消费输入也不产出输出且无状态返回」时不空转。
    static auto drive(Impl *impl, wuffs_base__io_buffer *in, bool input_closed, const DecompressSink &sink)
        -> Result<void>;
#endif
};

Decompressor::Decompressor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Decompressor::~Decompressor() = default;

Decompressor::Decompressor(Decompressor &&other) noexcept = default;

auto Decompressor::operator=(Decompressor &&other) noexcept -> Decompressor & = default;

auto Decompressor::create(CompressionFormat format, const DecompressOptions &options) -> Result<Decompressor> {
#ifndef AURORA_ENABLE_COMPRESSION
    (void)format;
    (void)options;
    return make_error(ErrorCode::CompressionUnavailable,
                      "compression capability is not compiled in (AURORA_ENABLE_COMPRESSION=OFF)");
#else
    auto impl = std::make_unique<Impl>();
    impl->format = format;
    impl->max_output_bytes = options.max_output_bytes;
    impl->decoder.reset(wuffs_decoder_alloc(format));
    if (impl->decoder == nullptr) {
        return make_error(ErrorCode::CompressionUnavailable,
                          "decoder allocation failed for format " + std::string(format_name(format)));
    }
    impl->work.resize(wuffs_decoder_workbuf_len(format, impl->decoder.get()));
    impl->staging.resize(AURORA_OUTPUT_CHUNK_BYTES);
    impl->out.data.ptr = impl->staging.data();
    impl->out.data.len = impl->staging.size();
    return Decompressor(std::move(impl));
#endif
}

#ifdef AURORA_ENABLE_COMPRESSION

auto Decompressor::Impl::emit_output(Impl *impl, std::size_t begin, std::size_t end, const DecompressSink &sink)
    -> Result<void> {
    if (end <= begin) {
        return Result<void>{};
    }
    const auto len = end - begin;
    if (impl->max_output_bytes.has_value()) {
        const auto cap = *impl->max_output_bytes;
        if (impl->produced + static_cast<std::uint64_t>(len) > cap) {
            return make_error(ErrorCode::CompressionOutputTooLarge, "decompressed output exceeds max_output_bytes (" +
                                                                        std::to_string(cap) + " bytes) for format " +
                                                                        std::string(format_name(impl->format)));
        }
    }
    impl->produced += static_cast<std::uint64_t>(len);
    // 解压产物按字节持存、Sink 契约为 char 视图：整窗一次转型后按 span 取子区间（不做指针算术）。
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    const auto *staging_chars = reinterpret_cast<const char *>(impl->staging.data());
    sink(std::span<const char>(staging_chars, impl->staging.size()).subspan(begin, len));
    return Result<void>{};
}

auto Decompressor::Impl::drive(Impl *impl, wuffs_base__io_buffer *in, bool input_closed, const DecompressSink &sink)
    -> Result<void> {
    auto *out = &impl->out;
    const auto work = wuffs_base__make_slice_u8(impl->work.data(), impl->work.size());
    while (true) {
        const std::size_t src_before = in->meta.ri;
        const wuffs_base__status z = wuffs_decoder_transform(impl->format, impl->decoder.get(), out, in, work);
        // 交出本轮产物 [ri, wi) 并推进消费点。⚠️ 写位置 wi 必须原样留给解码器：wuffs 的
        // io_transformer 是可恢复协程，每次调用从 a_dst->meta.wi 重建续写指针，并在挂起点
        // 把 [调用前 wi, 现 wi) 快照进内部 32 KiB 窗口；轮次间把 wi 重置为 0 会令写位置回退、
        // 内部历史计数与缓冲区内容错位，表现为伪 `#deflate: bad distance`（本仓实测：小载荷
        // 单轮完成不触发，大载荷跨轮必触发）。
        if (auto emitted = emit_output(impl, out->meta.ri, out->meta.wi, sink); !emitted) {
            return emitted;
        }
        out->meta.ri = out->meta.wi;
        if (z.is_error()) {
            return corrupt_error(std::string(z.repr != nullptr ? z.repr : "decoder error"));
        }
        if (z.is_complete()) {
            impl->finished = true;
            if (in->meta.ri < in->meta.wi) {
                return corrupt_error("trailing bytes after end of stream for format " +
                                     std::string(format_name(impl->format)));
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
                return corrupt_error("truncated input: stream not finished for format " +
                                     std::string(format_name(impl->format)));
            }
            return Result<void>{};  // 等更多切分
        }
        if (in->meta.ri == src_before && out->meta.wi == out->meta.ri) {
            // 既不耗输入也不产输出且输出尚有空间：解码器需要更多输入才能继续。
            if (input_closed) {
                return corrupt_error("truncated input: stream not finished for format " +
                                     std::string(format_name(impl->format)));
            }
            return Result<void>{};
        }
    }
}

#endif  // AURORA_ENABLE_COMPRESSION

auto Decompressor::push(std::span<const char> src, const DecompressSink &sink) const -> Result<void> {
    if (impl_ == nullptr) {
        return corrupt_error("decompressor used after move");
    }
    if (impl_->finished) {
        return corrupt_error("push after finish (single-stream scope)");
    }
#ifndef AURORA_ENABLE_COMPRESSION
    (void)src;
    (void)sink;
    return make_error(ErrorCode::CompressionUnavailable,
                      "compression capability is not compiled in (AURORA_ENABLE_COMPRESSION=OFF)");
#else
    if (src.empty()) {
        return Result<void>{};
    }
    wuffs_base__io_buffer in{};
    // wuffs io_buffer 只要可写指针（绝不写 src），char→uint8_t 为字节视图转换，就地豁免。
    // NOLINTNEXTLINE(*-pro-type-const-cast,*-pro-type-reinterpret-cast)
    in.data.ptr = const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(src.data()));
    in.data.len = src.size();
    in.meta.wi = src.size();
    in.meta.closed = false;
    return Impl::drive(impl_.get(), &in, false, sink);
#endif
}

auto Decompressor::finish(const DecompressSink &sink) const -> Result<void> {
    if (impl_ == nullptr) {
        return corrupt_error("decompressor used after move");
    }
    if (impl_->finished) {
        return Result<void>{};  // 幂等：流已完整终结，无残留可排。
    }
#ifndef AURORA_ENABLE_COMPRESSION
    (void)sink;
    return make_error(ErrorCode::CompressionUnavailable,
                      "compression capability is not compiled in (AURORA_ENABLE_COMPRESSION=OFF)");
#else
    wuffs_base__io_buffer in{};  // 空输入且标记 closed：驱动残留产物排出并验证流完整终结。
    in.data.ptr = nullptr;
    in.data.len = 0;
    in.meta.wi = 0;
    in.meta.closed = true;
    return Impl::drive(impl_.get(), &in, true, sink);
#endif
}

auto decompress(CompressionFormat format, std::span<const char> input, const DecompressOptions &options)
    -> Result<std::vector<char>> {
#ifndef AURORA_ENABLE_COMPRESSION
    (void)format;
    (void)input;
    (void)options;
    return make_error(ErrorCode::CompressionUnavailable,
                      "compression capability is not compiled in (AURORA_ENABLE_COMPRESSION=OFF)");
#else
    auto created = Decompressor::create(format, options);
    if (!created) {
        return created.error();
    }
    auto &decoder = created.value();
    std::vector<char> out;
    const auto collect = [&out](std::span<const char> chunk) { out.insert(out.end(), chunk.begin(), chunk.end()); };
    if (auto pushed = decoder.push(input, collect); !pushed) {
        return pushed.error();
    }
    if (auto finished = decoder.finish(collect); !finished) {
        return finished.error();
    }
    return out;
#endif
}

auto compress(CompressionFormat format, std::span<const char> input) -> Result<std::vector<char>> {
    (void)input;
    return make_error(ErrorCode::CompressionUnavailable,
                      "compression is not implemented in this batch (decompression only); requested format " +
                          std::string(format_name(format)));
}

}  // namespace aurora
