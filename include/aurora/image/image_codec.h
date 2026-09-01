#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/image.h" // Image：解码结果的统一像素载体（RGBA8 紧密打包）
#include "aurora/core/result.h"
#include "aurora/core/types.h" // Size

namespace aurora::image {

/// @brief 图片容器格式（与文件编码一一对应；Unknown 表示嗅探失败/兜底）。
/// @note 仅描述“容器/编码”，不描述像素布局；解码产物统一为 `Image`（RGBA8）。
enum class ImageFormat : std::uint8_t {
    Unknown = 0,
    BMP,
    GIF,
    JPEG,
    PNG,
    WebP,
    SVG,
};

/// @brief 解码后像素内存布局（当前 Aurora 渲染管线仅消费 RGBA8；其余为预留/未来）。
enum class PixelFormat : std::uint8_t {
    RGBA8 = 0, ///< 渲染管线实际消费格式，解码统一归一到此
    BGRA8,
    RGB8,
    ARGB8,
    Gray8,
    GrayAlpha8,
};

/// @brief 返回格式的可读名称（用于日志/诊断）。
[[nodiscard]] auto format_name(ImageFormat f) -> std::string_view;

/// @brief 按魔数嗅探格式（只读前若干字节，不依赖扩展名）。
/// @note SVG 为文本格式，检测 `<svg` 子串；无法判定返回 Unknown。
[[nodiscard]] auto detect_format(std::span<const std::uint8_t> header) -> ImageFormat;

/// @brief 按文件扩展名推测格式（非权威，仅作兜底/提示，优先级低于嗅探）。
[[nodiscard]] auto format_from_path(std::filesystem::path const &p) -> ImageFormat;

/// @brief 解码输入源：文件路径或内存字节（流场景可先读入内存再解码）。
struct ImageSource {
    enum class Kind : std::uint8_t {
        File,
        Memory,
    };
    Kind kind = Kind::File;
    std::filesystem::path path{};
    std::vector<std::uint8_t> memory;

    [[nodiscard]] static auto from_file(std::filesystem::path p) -> ImageSource {
        ImageSource s;
        s.kind = Kind::File;
        s.path = std::move(p);
        return s;
    }
    [[nodiscard]] static auto from_memory(std::vector<std::uint8_t> data) -> ImageSource {
        ImageSource s;
        s.kind = Kind::Memory;
        s.memory = std::move(data);
        return s;
    }
};

/// @brief 解码选项。
struct DecodeOptions {
    PixelFormat desired_format = PixelFormat::RGBA8; ///< 目标像素格式（当前仅 RGBA8 受支持）
    Size max_size{};                ///< 限宽限高（任意一维为 0 表示不限制）；解码后等比缩放到不超过该尺寸
    bool preserve_aspect = true;    ///< max_size 生效时是否保持纵横比（true=等比缩放到 fit）
    bool premultiply_alpha = false; ///< 是否预乘 alpha（默认不预乘，与渲染管线既有约定一致）
};

/// @brief 编码选项。
struct EncodeOptions {
    ImageFormat format = ImageFormat::PNG; ///< 目标容器格式
    int quality = 90;                      ///< 有损格式质量 0–100（JPEG/WebP 有损）
    int compression_level = 6;             ///< 无损压缩级别 0–9（PNG zlib 级别）
    bool lossless = true;                  ///< WebP：true=无损 / false=有损(quality 生效)
    bool preserve_alpha = true;            ///< 目标格式不支持 alpha 时是否保留（否则填不透明）
};

/// @brief 动画单帧。
struct ImageFrame {
    std::shared_ptr<Image> image;            ///< 该帧完整画布（已合成，可直接绘制）
    std::chrono::milliseconds duration{ 0 }; ///< 该帧持续时间
    int blend = 0;                           ///< 合成方式（0=SRC 覆盖, 1=OVER 叠加）
    int dispose = 0;                         ///< 帧后处理（0=无, 1=清为透明背景, 2=恢复上一帧）
};

/// @brief 动图（GIF / 动图 WebP / APNG 的多帧序列）。
struct AnimatedImage {
    std::vector<ImageFrame> frames;
    int loop_count = 0; ///< 0 表示无限循环
    int width = 0;
    int height = 0;
};

/// @brief 编解码器抽象：每种格式/库一个实现，由注册表统一调度。
/// @note 所有方法均为 const 纯函数（无状态），可多线程并发调用；实现须保证线程安全。
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，仅经 shared_ptr 使用，按值拷贝/移动无意义
class ImageCodec {
  public:
    virtual ~ImageCodec() = default;

    /// @brief 编解码器名称（如 "stb" / "libjpeg-turbo" / "libwebp" / "wuffs"）。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 该编解码器主负责的格式（StbCodec 等通用解码器返回 Unknown，靠 sniff 区分）。
    [[nodiscard]] virtual auto format() const -> ImageFormat { return ImageFormat::Unknown; }

    [[nodiscard]] virtual auto can_decode() const -> bool { return false; }
    [[nodiscard]] virtual auto can_encode() const -> bool { return false; }
    [[nodiscard]] virtual auto can_decode_animated() const -> bool { return false; }

    /// @brief 嗅探：给定文件头字节，本编解码器能否处理。注册表据此在多个候选中择优。
    [[nodiscard]] virtual auto sniff(std::span<const std::uint8_t> header) const -> bool {
        (void)header;
        return false;
    }

    /// @brief 解码（data 为完整文件字节）。失败返回结构化错误。
    [[nodiscard]] virtual auto decode(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
        -> Result<Image> {
        (void)data;
        (void)opt;
        return make_error(ErrorCode::GeneralNotSupported, std::string(name()) + ": decode not supported");
    }

    /// @brief 动图解码（data 为完整文件字节）。默认降级为单帧。
    [[nodiscard]] virtual auto decode_animated(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
        -> Result<AnimatedImage> {
        auto img = decode(data, opt);
        if (!img) {
            return img.error();
        }
        AnimatedImage anim;
        anim.width = img.value().width;
        anim.height = img.value().height;
        anim.frames.emplace_back(ImageFrame{ .image = std::make_shared<Image>(std::move(img.value())),
                                             .duration = std::chrono::milliseconds(0),
                                             .blend = 0,
                                             .dispose = 0 });
        return anim;
    }

    /// @brief 编码（img 为 RGBA8 像素），返回编码后字节。
    [[nodiscard]] virtual auto encode(const Image &img, const EncodeOptions &opt) const
        -> Result<std::vector<std::uint8_t>> {
        (void)img;
        (void)opt;
        return make_error(ErrorCode::GeneralNotSupported, std::string(name()) + ": encode not supported");
    }
};

/// @brief 编解码器注册表（进程内单例）：负责嗅探+调度+兜底。
class ImageCodecRegistry {
  public:
    /// @brief 进程唯一注册表实例。首次访问时自动注册内置编解码器。
    static auto instance() -> ImageCodecRegistry &;

    /// @brief 注册一个编解码器（可重复注册；先注册者优先被嗅探命中）。
    void register_codec(std::shared_ptr<ImageCodec> codec) const;

    /// @brief 解码：自动按嗅探选择解码器，失败依次尝试其余匹配者。
    [[nodiscard]] auto decode(const ImageSource &src, const DecodeOptions &opt = {}) const -> Result<Image>;
    [[nodiscard]] auto decode_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) const
        -> Result<Image>;
    [[nodiscard]] auto decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) const
        -> Result<Image>;

    /// @brief 动图解码（GIF/动图 WebP 返回多帧；静态格式返回单帧）。
    [[nodiscard]] auto decode_animated(const ImageSource &src, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;
    [[nodiscard]] auto decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;
    [[nodiscard]] auto decode_animated_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;

    /// @brief 编码为字节。
    [[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) const -> Result<std::vector<std::uint8_t>>;

    /// @brief 编码并写入文件（按 opt.format 或扩展名决定格式）。
    [[nodiscard]] auto save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt = {}) const
        -> Result<bool>;

    /// @brief 异步解码：后台线程执行，返回 future（结果仍为 Result<Image>）。
    [[nodiscard]] auto decode_async(const ImageSource &src, const DecodeOptions &opt = {}) const
        -> std::future<Result<Image>>;

    /// @brief 列出当前已注册编解码器（诊断用）。
    [[nodiscard]] auto registered() const -> std::vector<std::string>;

  private:
    ImageCodecRegistry();
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

/// @name 高层便捷自由函数（等价调用注册表单例）
/// @{
[[nodiscard]] auto decode_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) -> Result<Image>;
[[nodiscard]] auto decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) -> Result<Image>;
[[nodiscard]] auto decode(const ImageSource &src, const DecodeOptions &opt = {}) -> Result<Image>;
[[nodiscard]] auto decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt = {})
    -> Result<AnimatedImage>;
[[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) -> Result<std::vector<std::uint8_t>>;
[[nodiscard]] auto save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt = {})
    -> Result<bool>;
[[nodiscard]] auto decode_async(const ImageSource &src, const DecodeOptions &opt = {}) -> std::future<Result<Image>>;
/// @}

} // namespace aurora::image
