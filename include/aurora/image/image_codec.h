#pragma once

/// @brief
/// 图片编解码子系统公共接口：容器/像素格式枚举与魔数嗅探、编解码器抽象、注册表调度，以及等价转调单例的高层便捷函数。
/// @file image_codec.h

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"

namespace aurora::image {

/// @brief 图片容器格式（与文件编码一一对应；Unknown 表示嗅探失败/兜底）。
/// @note 仅描述“容器/编码”，不描述像素布局；解码产物统一为 `Image`（RGBA8）。
enum class ImageFormat : std::uint8_t {
    Unknown = 0,  ///< 嗅探/扩展名均未命中时的兜底值，非某种真实编码。
    BMP,  ///< Windows 位图（BI_HEADER），无压缩或 RLE。
    GIF,  ///< GIF 静图与动图（含帧延时/透明索引）。
    JPEG,  ///< JPEG/JPG 有损静图。
    PNG,  ///< PNG 无损静图（wuffs/stb 解码，自研 zlib 编码）。
    WebP,  ///< WebP 静图与动图（可无损或有损）。
    SVG,  ///< 可缩放矢量图，文本格式、无魔数，仅能按扩展名判定。
};

/// @brief 解码后像素内存布局（当前 Aurora 渲染管线仅消费 RGBA8；其余为预留/未来）。
enum class PixelFormat : std::uint8_t {
    RGBA8 = 0,  ///< 渲染管线实际消费格式，解码统一归一到此
    BGRA8,  ///< 蓝绿红 alpha 交错：部分平台纹理上传的原生布局。
    RGB8,  ///< 无 alpha 通道的三字节 packed 布局。
    ARGB8,  ///< alpha 在前的 32 位布局。
    Gray8,  ///< 单通道灰度（每像素 1 字节）。
    GrayAlpha8,  ///< 灰度 + 单通道 alpha（每像素 2 字节）。
};

/// @brief 返回格式的可读名称（用于日志/诊断）。
/// @param f 待命名的图片容器格式。
/// @return 与 f 对应的小写名称（如 "png"/"jpeg"/"webp"）；Unknown 或未覆盖时返回 "unknown"。
[[nodiscard]] auto format_name(ImageFormat f) -> std::string_view;

/// @brief 按魔数嗅探格式（只读前若干字节，不依赖扩展名）。
/// @param header 文件头部字节序列（仅需最前若干字节即可判定）。
/// @return 命中的容器格式；未匹配任何魔数时返回 Unknown。
/// @note SVG 为文本格式、**无魔数**，故嗅探恒返回 Unknown，须经扩展名（见 `format_from_path`）判定；
/// 无法判定返回 Unknown。
[[nodiscard]] auto detect_format(std::span<const std::uint8_t> header) -> ImageFormat;

/// @brief 按文件扩展名推测格式（非权威，仅作兜底/提示，优先级低于嗅探）。
/// @param p 待判定的文件路径（只取扩展名，大小写不敏感）。
/// @return 扩展名命中的容器格式；无匹配扩展名时返回 Unknown。
[[nodiscard]] auto format_from_path(std::filesystem::path const &p) -> ImageFormat;

/// @brief 解码输入源：文件路径或内存字节（流场景可先读入内存再解码）。
struct ImageSource {
    /// @brief 输入源判别：数据来自磁盘文件还是调用方提供的内存字节。
    enum class Kind : std::uint8_t {
        File,  ///< 从 path 指向的文件读取（读盘时才校验存在性）。
        Memory,  ///< 直接使用 memory 中的完整文件字节。
    };
    Kind kind = Kind::File;  ///< 当前源的类型；默认按文件路径解释。
    std::filesystem::path path;  ///< 默认构造即空路径，无需再写 `{}`（冗余成员初始化）。
    std::vector<std::uint8_t> memory;  ///< kind==Memory 时的完整文件字节（容器编码原样）。

    /// @brief 构造文件路径输入源。
    /// @param p 图片文件路径。
    /// @return kind==File、path==p 的 ImageSource。
    [[nodiscard]] static auto from_file(std::filesystem::path p) -> ImageSource {
        ImageSource s;
        s.kind = Kind::File;
        s.path = std::move(p);
        return s;
    }
    /// @brief 构造内存字节输入源。
    /// @param data 完整文件字节（移入后由源持有）。
    /// @return kind==Memory、memory==data 的 ImageSource。
    [[nodiscard]] static auto from_memory(std::vector<std::uint8_t> data) -> ImageSource {
        ImageSource s;
        s.kind = Kind::Memory;
        s.memory = std::move(data);
        return s;
    }
};

/// @brief 解码选项。
struct DecodeOptions {
    PixelFormat desired_format = PixelFormat::RGBA8;  ///< 目标像素格式（当前仅 RGBA8 受支持）
    Size max_size{};  ///< 限宽限高（任意一维为 0 表示不限制）；解码后等比缩放到不超过该尺寸
    bool preserve_aspect = true;  ///< max_size 生效时是否保持纵横比（true=等比缩放到 fit）
    bool premultiply_alpha = false;  ///< 是否预乘 alpha（默认不预乘，与渲染管线既有约定一致）
};

/// @brief 编码选项。
struct EncodeOptions {
    ImageFormat format = ImageFormat::PNG;  ///< 目标容器格式
    int quality = 90;  ///< 有损格式质量 0–100（JPEG/WebP 有损）
    int compression_level = 6;  ///< 无损压缩级别 0–9（PNG zlib 级别）
    bool lossless = true;  ///< WebP：true=无损 / false=有损(quality 生效)
    bool preserve_alpha = true;  ///< 目标格式不支持 alpha 时是否保留（否则填不透明）
};

/// @brief 动画单帧。
struct ImageFrame {
    std::shared_ptr<Image> image;  ///< 该帧完整画布（已合成，可直接绘制）
    std::chrono::milliseconds duration{0};  ///< 该帧持续时间
    int blend = 0;  ///< 合成方式（0=SRC 覆盖, 1=OVER 叠加）
    int dispose = 0;  ///< 帧后处理（0=无, 1=清为透明背景, 2=恢复上一帧）
};

/// @brief 动图（GIF / 动图 WebP / APNG 的多帧序列）。
struct AnimatedImage {
    std::vector<ImageFrame> frames;  ///< 按播放顺序排列的帧序列（静态格式解码为单帧）。
    int loop_count = 0;  ///< 0 表示无限循环
    int width = 0;  ///< 画布宽度（像素），各帧合成坐标系的基准。
    int height = 0;  ///< 画布高度（像素），各帧合成坐标系的基准。
};

/// @brief 编解码器抽象：每种格式/库一个实现，由注册表统一调度。
/// @note 所有方法均为 const 纯函数（无状态），可多线程并发调用；实现须保证线程安全。
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，仅经 shared_ptr 使用，按值拷贝/移动无意义
class ImageCodec {
  public:
    virtual ~ImageCodec() = default;

    /// @brief 编解码器名称（如 "stb" / "libjpeg-turbo" / "libwebp" / "wuffs"）。
    /// @return 实现方提供的稳定标识串。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 该编解码器主负责的格式（StbCodec 等通用解码器返回 Unknown，靠 sniff 区分）。
    /// @return 主责容器格式；通用/多格式解码器为 Unknown。
    [[nodiscard]] virtual auto format() const -> ImageFormat { return ImageFormat::Unknown; }

    /// @brief 是否具备静态图解码能力。
    /// @return 支持 decode 时为 true；默认 false。
    [[nodiscard]] virtual auto can_decode() const -> bool { return false; }
    /// @brief 是否具备编码能力。
    /// @return 支持 encode 时为 true；默认 false。
    [[nodiscard]] virtual auto can_encode() const -> bool { return false; }
    /// @brief 是否具备动图（多帧）解码能力。
    /// @return 支持 decode_animated 时为 true；默认 false。
    [[nodiscard]] virtual auto can_decode_animated() const -> bool { return false; }

    /// @brief 嗅探：给定文件头字节，本编解码器能否处理。注册表据此在多个候选中择优。
    /// @param header 文件头部字节序列。
    /// @return 能识别该魔数/格式时为 true；默认 false。
    [[nodiscard]] virtual auto sniff(std::span<const std::uint8_t> header) const -> bool {
        (void)header;
        return false;
    }

    /// @brief 解码（data 为完整文件字节）。失败返回结构化错误。
    /// @param data 完整文件字节。
    /// @param opt 解码选项（目标格式、缩放上限、预乘等）。
    /// @return 解码得到的 RGBA8 图像；未实现为 GeneralNotSupported。
    [[nodiscard]] virtual auto decode(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
        -> Result<Image> {
        (void)data;
        (void)opt;
        return make_error(ErrorCode::GeneralNotSupported, std::string(name()) + ": decode not supported");
    }

    /// @brief 动图解码（data 为完整文件字节）。默认降级为单帧。
    /// @param data 完整文件字节。
    /// @param opt 解码选项。
    /// @return 多帧 AnimatedImage；默认实现把 decode 的单帧结果包装为一帧（duration=0）。
    [[nodiscard]] virtual auto decode_animated(std::span<const std::uint8_t> data, const DecodeOptions &opt) const
        -> Result<AnimatedImage> {
        auto img = decode(data, opt);
        if (!img) {
            return img.error();
        }
        AnimatedImage anim;
        anim.width = img.value().width;
        anim.height = img.value().height;
        anim.frames.emplace_back(ImageFrame{.image = std::make_shared<Image>(std::move(img.value())),
                                            .duration = std::chrono::milliseconds(0),
                                            .blend = 0,
                                            .dispose = 0});
        return anim;
    }

    /// @brief 编码（img 为 RGBA8 像素），返回编码后字节。
    /// @param img 待编码的 RGBA8 图像。
    /// @param opt 编码选项（目标格式、质量、压缩级别等）。
    /// @return 编码后的容器字节；未实现为 GeneralNotSupported。
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
    /// @return 全局单例的引用。
    static auto instance() -> ImageCodecRegistry &;

    /// @brief 注册一个编解码器（可重复注册；先注册者优先被嗅探命中）。
    /// @param codec 待注册的编解码器（空指针将被忽略）。
    void register_codec(std::shared_ptr<ImageCodec> codec) const;

    /// @brief 解码：自动按嗅探选择解码器，失败依次尝试其余匹配者。
    /// @param src 输入源（文件路径或内存字节）。
    /// @param opt 解码选项。
    /// @return 解码得到的 RGBA8 图像；无解码器命中为 IOImageDecodeFailed。
    [[nodiscard]] auto decode(const ImageSource &src, const DecodeOptions &opt = {}) const -> Result<Image>;
    /// @brief 从文件路径解码（先读盘再走嗅探调度）。
    /// @param p 图片文件路径。
    /// @param opt 解码选项。
    /// @return 解码结果；打开/读取失败为 IOFileNotFound。
    [[nodiscard]] auto decode_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) const
        -> Result<Image>;
    /// @brief 从内存字节解码。
    /// @param data 完整文件字节。
    /// @param opt 解码选项。
    /// @return 解码得到的 RGBA8 图像；无解码器命中为 IOImageDecodeFailed。
    [[nodiscard]] auto decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) const
        -> Result<Image>;

    /// @brief 动图解码（GIF/动图 WebP 返回多帧；静态格式返回单帧）。
    /// @param src 输入源（文件路径或内存字节）。
    /// @param opt 解码选项。
    /// @return 多帧 AnimatedImage；失败为结构化错误。
    [[nodiscard]] auto decode_animated(const ImageSource &src, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;
    /// @brief 从文件路径解码动图。
    /// @param p 图片文件路径。
    /// @param opt 解码选项。
    /// @return 多帧 AnimatedImage；打开/读取失败为 IOFileNotFound。
    [[nodiscard]] auto decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;
    /// @brief 从内存字节解码动图。
    /// @param data 完整文件字节。
    /// @param opt 解码选项。
    /// @return 多帧 AnimatedImage；无动图解码器时回退为单帧静态图。
    [[nodiscard]] auto decode_animated_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) const
        -> Result<AnimatedImage>;

    /// @brief 编码为字节。
    /// @param img 待编码的 RGBA8 图像。
    /// @param opt 编码选项（目标格式等；Unknown 时回退到任一可用编码器）。
    /// @return 编码后的容器字节；无可用编码器为 GeneralNotSupported。
    [[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) const -> Result<std::vector<std::uint8_t>>;

    /// @brief 编码并写入文件（按 opt.format 或扩展名决定格式）。
    /// @param img 待保存的 RGBA8 图像。
    /// @param p 目标文件路径（opt.format 为 Unknown 时按扩展名推断，仍无法判定则 PNG）。
    /// @param opt 编码选项。
    /// @return 写入成功为 true；打开/编码/写入不完整为结构化错误。
    [[nodiscard]] auto save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt = {}) const
        -> Result<bool>;

    /// @brief 异步解码：后台线程执行，返回 future（结果仍为 Result<Image>）。
    /// @param src 输入源（文件路径或内存字节）。
    /// @param opt 解码选项。
    /// @return 承载 Result<Image> 的 std::future；有线程能力时为 async 任务，否则为惰性 future。
    /// @note 无线程能力构建（Emscripten 未开 `-pthread`，见 `AURORA_CAP_THREADS`）下没有后台线程，
    ///       返回**惰性** future：提交零开销、不抛异常，首次 `get()`/`wait()` 在调用线程就地解码。
    ///       调用方若靠 `wait_for(0)` 轮询实现非阻塞，需知此平台下首次轮询即为同步解码。
    [[nodiscard]] auto decode_async(const ImageSource &src, const DecodeOptions &opt = {}) const
        -> std::future<Result<Image>>;

    /// @brief 列出当前已注册编解码器（诊断用）。
    /// @return 各编解码器 name() 的副本列表（按注册顺序）。
    [[nodiscard]] auto registered() const -> std::vector<std::string>;

  private:
    ImageCodecRegistry();
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// 高层便捷自由函数：每个都是一行转调 ImageCodecRegistry::instance() 的同名方法，
// 供调用点省略单例样板（等价形态，见类内对应成员函数的完整契约说明）。

/// @brief 从文件路径解码图片（转调注册表同名方法）。
/// @param p 图片文件路径。
/// @param opt 解码选项。
/// @return 解码得到的 RGBA8 图像；打开/读取/无解码器命中时为对应错误。
[[nodiscard]] auto decode_file(const std::filesystem::path &p, const DecodeOptions &opt = {}) -> Result<Image>;

/// @brief 从内存字节解码图片（转调注册表同名方法）。
/// @param data 完整文件字节。
/// @param opt 解码选项。
/// @return 解码得到的 RGBA8 图像；无解码器命中为 IOImageDecodeFailed。
[[nodiscard]] auto decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt = {}) -> Result<Image>;

/// @brief 解码输入源（按 src.kind 选择文件/内存路径，转调注册表同名方法）。
/// @param src 输入源（文件路径或内存字节）。
/// @param opt 解码选项。
/// @return 解码得到的 RGBA8 图像；失败为结构化错误。
[[nodiscard]] auto decode(const ImageSource &src, const DecodeOptions &opt = {}) -> Result<Image>;

/// @brief 从文件路径解码动图（转调注册表同名方法）。
/// @param p 图片文件路径。
/// @param opt 解码选项。
/// @return 多帧 AnimatedImage（静态格式返回单帧）；失败为结构化错误。
[[nodiscard]] auto decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt = {})
    -> Result<AnimatedImage>;

/// @brief 编码为字节（转调注册表同名方法）。
/// @param img 待编码的 RGBA8 图像。
/// @param opt 编码选项（目标格式、质量等）。
/// @return 编码后的容器字节；无可用编码器为 GeneralNotSupported。
[[nodiscard]] auto encode(const Image &img, const EncodeOptions &opt) -> Result<std::vector<std::uint8_t>>;

/// @brief 编码并写入文件（转调注册表同名方法）。
/// @param img 待保存的 RGBA8 图像。
/// @param p 目标文件路径（opt.format 为 Unknown 时按扩展名推断）。
/// @param opt 编码选项。
/// @return 写入成功为 true；打开/编码/写入失败为结构化错误。
[[nodiscard]] auto save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt = {})
    -> Result<bool>;

/// @brief 异步解码（转调注册表同名方法）。
/// @param src 输入源（文件路径或内存字节）。
/// @param opt 解码选项。
/// @return 承载 Result<Image> 的 std::future。
[[nodiscard]] auto decode_async(const ImageSource &src, const DecodeOptions &opt = {}) -> std::future<Result<Image>>;

}  // namespace aurora::image
