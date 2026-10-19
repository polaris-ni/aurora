#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora {

/// @brief 剪贴板抽象（specification/06-app-platform.md §8.2）。
///
/// - 文本：`set_text`/`get_text`，各平台实现：
/// - Windows：`SetClipboardData(CF_UNICODETEXT)` / `GetClipboardData(CF_UNICODETEXT)`
/// - Linux：`xclip -selection clipboard` / `xsel --clipboard`（需安装 xclip 或 xsel）
/// - macOS：`pbcopy` / `pbpaste`
/// - 图像：`set_image`/`get_image`，仅 Windows 经 `SetClipboardData(CF_DIB)` 实现，
/// 其它平台报 `GeneralNotSupported`。
/// - 富格式（自定义 MIME / 文件清单 / HTML）：`set_payload` / `get_payload` 及其便利封装。
/// 格式名一律取自 `app/mime_types.h` 的常量表。
///
/// **失败口径**（一律机器可见，不再以空串 / 静默 no-op 冒充成功）：
///
/// | 情形 | 结果 |
/// |:---|:---|
/// | 剪贴板打不开、被别的线程/进程占用、外壳工具（xclip/xsel/pbcopy）缺失、跨进程载荷非法 | `ClipboardAccessFailed` |
/// | 已打开但载荷提交失败（`GlobalAlloc` / `GlobalLock` / `SetClipboardData`）、文本不是合法 UTF-8 |
/// `ClipboardWriteFailed` | | 本平台无剪贴板实现 | `GeneralNotSupported` | |
/// 图像尺寸超限或像素缓冲与维度不一致（调用方参数错） | `GeneralInvalidArgument` | | 可访问但当前没有文本 / 图像 |
/// `Ok("")` / `Ok(空 Image)`——**空内容不是失败** |
/// | 富格式：本平台没有该 MIME 的读写机制 | `ClipboardFormatUnsupported`（category platform、
/// `retryable = false`——机制缺失是稳态，重试无益） |
/// | 富格式：格式未写入 / 载荷里没有可落成文件的条目 | `Ok(空值)`——空内容同样不是失败 |
///
/// 空文本与空图像是契约内的 no-op：不触碰剪贴板、保留既有内容，返回 `Ok`。富格式同口径：
/// `set_payload` 写空字节、`set_files` 写空清单、`set_html` 写空串都为 no-op，保留既有内容。
/// `ClipboardAccessFailed` 标 `retryable = true`（占用是暂时态），调用方据此可退避重试。
/// Windows 的文本写入先转码后开剪贴板：非法 UTF-8 在 `EmptyClipboard` 之前就被拒，
/// 失败的那一次不会把用户原本复制的内容清空。
///
/// **立骨期边界**：富格式 API 只在 memory 测试后端下承载数据；非测试路径上各平台一律返回
/// `ClipboardFormatUnsupported`（平台机制由后续落地，本轮不触碰 Win32 既有 text / image 实现）。
/// 故 `available_formats` 在平台路径上如实只列 `text/plain`（`image/png` 仅 Win32 有图像机制），
/// 富格式在机制落地前不入列——不因「新 API 已声明」就谎报可用格式。`text/plain` 的读写在平台
/// 路径上仍走 `set_text` / `get_text`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none (accesses system clipboard)
/// @note Rebuildable: no
class Clipboard {
  public:
    /// @brief 把文本写入系统剪贴板（UTF-8 入参，平台按需转码）。空串为 no-op，保留既有内容。
    /// @param text 待写入的 UTF-8 文本；空串时不触碰剪贴板、直接返回成功。
    /// @return 成功为 `Ok(true)`（空串 no-op 同样 `Ok(true)`）；失败为结构化 `Error`，按原因取
    /// `ClipboardAccessFailed` / `ClipboardWriteFailed` / `GeneralNotSupported` / `GeneralInvalidArgument`。
    static auto set_text(const std::string &text) -> Result<bool>;

    /// @brief 从系统剪贴板读取文本（UTF-8 返回）；可读但无文本时为空串。
    /// @return `Ok(文本)`——剪贴板可访问但当前没有文本时为 `Ok("")`（空内容不是失败）；
    /// 访问/解析失败为结构化 `Error`。
    [[nodiscard]] static auto get_text() -> Result<std::string>;

    /// @brief 把 RGBA8 图像写入系统剪贴板（经 CF_DIB）。空图像直接返回（不清除已有内容）。
    /// @param img RGBA8 图像；`width == 0`（空图像）时为 no-op，保留既有内容。
    /// @return 成功为 `Ok(true)`；失败为结构化 `Error`（非 Windows 平台为 `GeneralNotSupported`，
    /// 尺寸超限或缓冲与维度不符为 `GeneralInvalidArgument`）。
    static auto set_image(const Image &img) -> Result<bool>;

    /// @brief 从系统剪贴板读取 RGBA8 图像；无图像（格式不存在）返回空 `Image`（width==0）。
    /// @return `Ok(图像)`——剪贴板可访问但当前没有图像载荷时为 `Ok(空 Image)`（空内容不是失败）；
    /// 访问/载荷非法为结构化 `Error`。
    [[nodiscard]] static auto get_image() -> Result<Image>;

    /// @brief 列出当前剪贴板持有的 MIME 清单。
    ///
    /// 如实反映「此刻能读到什么」：平台路径上是各平台既有机制所覆盖的格式（文本三平台恒在列、
    /// 图像仅 Windows 在列），富格式待平台机制落地后才入列；memory 测试后端下则反映实际写入的
    /// 格式。调用方据此决定走哪条读取通道，而不是凭「API 已声明」假设格式可用。
    /// @return `Ok(MIME 清单)`——一个格式都没有时为 `Ok(空清单)`（空内容不是失败）；
    /// 本平台无剪贴板实现时为 `GeneralNotSupported`。
    [[nodiscard]] static auto available_formats() -> Result<std::vector<std::string>>;

    /// @brief 按 MIME 写入任意字节载荷（自定义格式的通用通道）。
    /// @param mime 目标 MIME，取自 `app/mime_types.h` 的常量表或调用方自定义键；空串视为参数错。
    /// @param bytes 待写入的原始字节；空字节为 no-op，保留既有内容。
    /// @return 成功为 `Ok(true)`（空字节 no-op 同样 `Ok(true)`）；`mime` 为空为
    /// `GeneralInvalidArgument`；本平台无该 MIME 的写入机制为 `ClipboardFormatUnsupported`。
    static auto set_payload(const std::string &mime, const std::vector<std::uint8_t> &bytes) -> Result<bool>;

    /// @brief 按 MIME 读取任意字节载荷。
    /// @param mime 待读取的 MIME。
    /// @return `Ok(字节)`——该格式未写入时为 `Ok(空字节)`（空内容不是失败）；本平台无该 MIME 的
    /// 读取机制为 `ClipboardFormatUnsupported`。
    [[nodiscard]] static auto get_payload(const std::string &mime) -> Result<std::vector<std::uint8_t>>;

    /// @brief 把本地路径清单写入剪贴板（编码为 `AURORA_MIME_URI_LIST`）。
    ///
    /// 入参**只接受本地路径**：URL 类内容不是本地路径，应改走 `set_payload` 直接给原始字节。
    /// 完整原始 uri-list 文本经 `get_payload(AURORA_MIME_URI_LIST)` 取得。
    /// @param paths 本地路径清单；空清单为 no-op，保留既有内容。
    /// @return 成功为 `Ok(true)`（空清单 no-op 同样 `Ok(true)`）；本平台无该格式的写入机制为
    /// `ClipboardFormatUnsupported`。
    static auto set_files(const std::vector<std::string> &paths) -> Result<bool>;

    /// @brief 读取剪贴板里的本地路径清单（解码 `AURORA_MIME_URI_LIST`）。
    ///
    /// 只返回可映射为本地路径的条目：非 `file` scheme 的行（`http://` 等）静默跳过；全部条目都
    /// 被跳过时为 `Ok(空清单)`——空内容不是失败。完整原始行经 `get_payload` 取得。
    /// @return `Ok(路径清单)`；本平台无该格式的读取机制为 `ClipboardFormatUnsupported`。
    [[nodiscard]] static auto get_files() -> Result<std::vector<std::string>>;

    /// @brief 把 HTML 片段写入剪贴板（映射 `AURORA_MIME_TEXT_HTML`）。
    /// @param html HTML 片段文本；空串为 no-op，保留既有内容。
    /// @return 成功为 `Ok(true)`（空串 no-op 同样 `Ok(true)`）；本平台无该格式的写入机制为
    /// `ClipboardFormatUnsupported`。
    static auto set_html(const std::string &html) -> Result<bool>;

    /// @brief 读取剪贴板里的 HTML 片段。
    /// @return `Ok(HTML 文本)`——该格式未写入时为 `Ok("")`（空内容不是失败）；本平台无该格式的
    /// 读取机制为 `ClipboardFormatUnsupported`。
    [[nodiscard]] static auto get_html() -> Result<std::string>;

    // ---- 测试注入点（test-only，见 specification/06-app-platform.md §8.2）----
    // 仓库私有测试设施（tests/）用于并行隔离的最小注入面：声明常驻（消费端调用始终可编译），
    // 实现体按 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 双宏裁切，
    // 任一关闭（含 Release 下 DEBUG 自动关闭）时返回 `false` / no-op，平台行为不变。

    /// @brief 安装进程内 memory 后端：此后 set/get 全部走内存，不触碰系统剪贴板。
    /// @return 注入是否生效（双宏未齐备时为 `false`）。
    [[nodiscard]] static auto install_test_backend() -> bool;

    /// @brief 清空 memory 后端内容（后端未安装时为 no-op）。
    static auto reset_test_backend() -> void;

    /// @brief 卸载 memory 后端，恢复平台实现。
    /// @return 是否确有后端被卸载（双宏未齐备时为 `false`）。
    [[nodiscard]] static auto remove_test_backend() -> bool;
};

}  // namespace aurora
