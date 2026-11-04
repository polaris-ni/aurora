#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/state/subscription.h"

namespace aurora {

class Widget;

/// @brief 剪贴板惰性提供要约：声明「我能给哪些格式、按 MIME 怎么取字节」，数据在真正被读取时才生成。
///
/// 与拖出侧 `DragOffer`（`event/event.h`）**字段形态同构**而各自独立：两条通道共享同一套 MIME
/// 词汇（`app/mime_types.h` 是单一真源），应用可把同一个 `provide` 回调同时交给两条通道；不引入
/// 跨 `app/` 与 `event/` 的统一数据对象——`event/` 不依赖 `app/` 的分层约束使合并无从落地。
/// @note Thread: n/a（纯值类型）
/// @note Side-effects: none
/// @note Rebuildable: no
struct ClipboardOffer {
    std::vector<std::string> formats;  ///< 可提供的 MIME 清单（顺序即优先级）
    /// @brief 按 MIME 提供字节的回调；返回 `Error` 视为该次提供失败（读取侧得到
    /// `ClipboardProviderFailed`，见 `Clipboard::set_offer`）。
    std::function<Result<std::vector<std::uint8_t>>(const std::string &mime)> provide;
};

/// @brief 剪贴板通知类别：位掩码，可组合订阅（见 `Clipboard::on_event`）。
enum class ClipboardEventKind : std::uint8_t {
    None = 0,  ///< 空掩码（不订阅任何类别；作为 `on_event` 的 `kinds` 入参是参数错）。
    ContentChanged = 1U << 0U,  ///< 系统剪贴板内容发生变化。
    OwnershipLost = 1U << 1U,  ///< 本进程的剪贴板所有权被其它进程夺走。
};

/// @brief 合并两类通知位。
/// @param lhs 左操作数
/// @param rhs 右操作数
/// @return 两类通知位的并集
[[nodiscard]] constexpr auto operator|(ClipboardEventKind lhs, ClipboardEventKind rhs) noexcept -> ClipboardEventKind {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
    return static_cast<ClipboardEventKind>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief 取两类通知位的交集。
/// @param lhs 左操作数
/// @param rhs 右操作数
/// @return 两类通知位的交集（不相交时为空掩码）
[[nodiscard]] constexpr auto operator&(ClipboardEventKind lhs, ClipboardEventKind rhs) noexcept -> ClipboardEventKind {
    return static_cast<ClipboardEventKind>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

/// @brief 就地合并通知位。
/// @param lhs 被修改的掩码
/// @param rhs 追加的通知位
/// @return `lhs` 的引用（合并后）
constexpr auto operator|=(ClipboardEventKind &lhs, ClipboardEventKind rhs) noexcept -> ClipboardEventKind & {
    lhs = lhs | rhs;
    return lhs;
}

/// @brief `flags` 是否包含 `kind` 位。
/// @param flags 待检查的掩码
/// @param kind 目标通知类别
/// @return 包含该位时为 true
[[nodiscard]] constexpr auto clipboard_event_has(ClipboardEventKind flags, ClipboardEventKind kind) noexcept -> bool {
    return static_cast<std::uint8_t>(flags & kind) != 0U;
}

/// @brief 剪贴板抽象（specification/06-app-platform.md §8.2）。
///
/// - 文本：`set_text`/`get_text`，各平台实现：
/// - Windows：`SetClipboardData(CF_UNICODETEXT)` / `GetClipboardData(CF_UNICODETEXT)`
/// - Linux：`xclip -selection clipboard` / `xsel --clipboard`（需安装 xclip 或 xsel）
/// - macOS：`pbcopy` / `pbpaste`（文本沿用外壳工具；富格式另走 `NSPasteboard`，见下）
/// - 图像：`set_image`/`get_image`，仅 Windows 经 `SetClipboardData(CF_DIB)` 实现，
/// 其它平台报 `GeneralNotSupported`（macOS 的 `image/png` 经 `set_payload` 走 `NSPasteboard`，
/// 不在图像通道）。
/// - 富格式（自定义 MIME / 文件清单 / HTML）：`set_payload` / `get_payload` 及其便利封装。
/// 格式名一律取自 `app/mime_types.h` 的常量表。Windows 由剪贴板 owner 窗口承载（`CF_HDROP` /
/// HTML Format / 自定义注册格式，惰性提供走 `WM_RENDERFORMAT`），macOS 由 `NSPasteboard`
/// 承载（文件 URL / HTML / 自定义 type，惰性提供走 `NSPasteboardItemDataProvider`）；Linux
/// 富格式机制尚未接线，读写如实报 `ClipboardFormatUnsupported`。
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
/// | 富格式：本平台没有该 MIME 的读写机制 | `ClipboardFormatUnsupported`（domain clipboard、kind
/// unsupported、`retryable = false`——机制缺失是稳态，重试无益） |
/// | 富格式：格式未写入 / 载荷里没有可落成文件的条目 | `Ok(空值)`——空内容同样不是失败 |
/// | 惰性要约不合法（`formats` 为空或 `provide` 为空回调） | `GeneralInvalidArgument` |
/// | 惰性提供：回调返回 `Error`，或回调为空而该格式被真正请求 | `ClipboardProviderFailed`（domain
/// clipboard、kind io、`retryable = false`——同一笔数据是否重试由应用决定，库不代做重试） |
/// | 变更监听：本平台无监听机制（注册即失败，**不谎报**） | `ClipboardListenUnsupported`（domain
/// clipboard、kind unsupported、`retryable = false`——机制缺失是稳态） |
///
/// 空文本与空图像是契约内的 no-op：不触碰剪贴板、保留既有内容，返回 `Ok`。富格式同口径：
/// `set_payload` 写空字节、`set_files` 写空清单、`set_html` 写空串都为 no-op，保留既有内容。
/// `ClipboardAccessFailed` 标 `retryable = true`（占用是暂时态），调用方据此可退避重试。
/// Windows 的文本写入先转码后开剪贴板：非法 UTF-8 在 `EmptyClipboard` 之前就被拒，
/// 失败的那一次不会把用户原本复制的内容清空。
///
/// **惰性提供与监听（本类同址扩充，不新开门面）**：`set_offer` 注册延迟渲染要约——各格式的数据在
/// 真正被读取时才经 `provide` 回调生成（对标 Win32 `SetClipboardData(format, NULL)` 的
/// `WM_RENDERFORMAT`、macOS pasteboard promise、GTK4 `GdkContentProvider`），与既有立即写通道
/// **混用合法且互不覆盖**；`on_event` 以位掩码承载所有权被夺与内容变化两类平台通知（纯多播、
/// 注册序派发），其中请求 `ContentChanged` 在无监听机制的平台上**注册即失败**
/// （`clipboard-listen-unsupported`）——与 `available_formats()` 「如实反映此刻能读到什么，不因
/// API 已声明就谎报可用」同款定调；
/// `clear()` 是**主动清空**，与空写入 no-op 的「保留既有内容」语义相反，二者并列写明以防误用。
/// 监听注册不是内容：`clear()` 清内容（要约即所有权声明，随内容一并清空），监听注册留存。
///
/// **平台接线现状**：富格式由平台后端函数表承载——Win32 后端随剪贴板 owner 窗口登记（无窗口
/// 时未登记，如实报 `ClipboardFormatUnsupported`），macOS 后端在首次问到后端时自举登记
/// （`NSPasteboard` 是应用级资源，无窗口同样可用），Linux 尚无富格式后端。后端登记时
/// `available_formats` 由其如实枚举此刻剪贴板实际持有的格式（含惰性要约声明的格式）；未登记时
/// 退回立骨期口径，只列既有机制覆盖的档位（`text/plain`，Win32 另加 `image/png`）。`text/plain`
/// 的读写在平台路径上仍走 `set_text` / `get_text`。惰性提供与变更监听按机制有无分部接线：
/// `set_offer` 在 Win32 / macOS 可用、Linux 报 `ClipboardFormatUnsupported`；`on_event` 请求
/// `ContentChanged` 时在 Win32 可用、macOS 因 `NSPasteboard` 无系统变更通知（库也不轮询）报
/// `ClipboardListenUnsupported`（只请求 `OwnershipLost` 恒成功）。`clear()` 按平台到位程度分部接线（Win32
/// `EmptyClipboard`、 Linux `xsel --clear`、macOS `pbcopy` 空输入重写 pasteboard；其余平台如实报
/// `GeneralNotSupported`）。
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
    /// 如实反映「此刻能读到什么」：平台后端登记时枚举该后端实际持有的格式（Win32 剪贴板格式、
    /// macOS pasteboard 类型，含惰性要约声明的格式）；未登记时退回既有机制覆盖的档位
    /// （`text/plain`，Win32 另加 `image/png`）；memory 测试后端下则反映实际写入的格式。调用方
    /// 据此决定走哪条读取通道，而不是凭「API 已声明」假设格式可用。
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

    /// @brief 注册惰性提供要约：注册后本进程持有剪贴板所有权，各格式的数据**在真正被读取时才经
    /// `provide` 回调生成**（延迟渲染）。
    ///
    /// 与立即写通道**混用合法且互不覆盖**：`set_text` / `set_payload` 等照旧当场落盘，惰性格式被
    /// 真正请求时才回调；同一 MIME 被随后的非空立即写通道写入时，该格式自要约改由立即写接管
    /// （与平台「后写者接管该格式」同款）。重复注册以新要约**整体替换**旧要约（含其已渲染缓存）。
    /// 渲染结果只生成一次并缓存（对标平台 `WM_RENDERFORMAT` 后由系统托管），回调返回 `Error`
    /// 不缓存（每次读取各自重试提供）。
    /// @param offer 惰性提供要约（格式清单 + 按 MIME 取字节的回调）；`formats` 为空或 `provide`
    /// 为空回调均为参数错。
    /// @return 成功为 `Ok`；要约不合法为 `GeneralInvalidArgument`；本平台无惰性提供机制为
    /// `ClipboardFormatUnsupported`（Win32 / macOS 已接线；Linux 尚未接线）。
    static auto set_offer(const ClipboardOffer &offer) -> Result<void>;

    /// @brief 订阅剪贴板通知（句柄形态）：`kinds` 位掩码指定要接收的类别。
    ///
    /// **纯多播**：同一回调注册几次就收几次，互不替换。派发按注册先后（FIFO）用注册快照逐条调用
    /// ——回调内新注册的条目**下一轮**才生效，回调内取消的条目**立即**停止接收（含本轮尚未轮到的
    /// 条目）。回调形参即命中类别；派发在通知到达的线程上串行执行，实现须轻量、不得在其中触发
    /// 重布局或再次读写剪贴板。
    ///
    /// **不谎报**：请求 `ContentChanged` 而本平台无变更监听机制时**注册即失败**、返回
    /// `ClipboardListenUnsupported`（注册了却收不到通知会让调用方逻辑静默失效，与
    /// `available_formats()` 的如实反映同款定调）；只请求 `OwnershipLost` 恒成功——所有权丢失
    /// 不依赖内容监听机制。返回的 `Subscription` 管理本次订阅：析构或显式 `reset()` 即注销，
    /// 注销不需要监听机制。
    /// @param kinds 订阅的通知类别（至少包含一类）。
    /// @param callback 收到通知时的回调（形参为命中类别）；空回调为参数错。
    /// @return 成功为 `Ok(Subscription)`；空回调或空 `kinds` 为 `GeneralInvalidArgument`；
    /// 请求 `ContentChanged` 而本平台无监听机制为 `ClipboardListenUnsupported`。
    [[nodiscard]] static auto on_event(ClipboardEventKind kinds, std::function<void(ClipboardEventKind)> callback)
        -> Result<Subscription>;

    /// @brief 订阅剪贴板通知（owner 守卫形态）：`owner` 销毁即自动注销。
    ///
    /// 守卫只影响**该条目的存活**，不改变纯多播与快照派发语义；owner 必须由 `std::shared_ptr`
    /// 托管（`weak_from_this()` 可用），否则弱守卫无从建立——静默退化为强引用反而会把控件的生命
    /// 周期拖到监听注册之后。
    /// @param owner 订阅归属控件（`Widget` 或其子类，须由 `std::shared_ptr` 托管）。
    /// @param kinds 订阅的通知类别（至少包含一类）。
    /// @param callback 收到通知时的回调（形参为命中类别）；空回调为参数错。
    /// @return 成功为 `Ok(Subscription)`；owner 非 `shared_ptr` 托管、空回调或空 `kinds` 为
    /// `GeneralInvalidArgument`；请求 `ContentChanged` 而本平台无监听机制为
    /// `ClipboardListenUnsupported`。
    [[nodiscard]] static auto on_event(Widget &owner, ClipboardEventKind kinds,
                                       std::function<void(ClipboardEventKind)> callback) -> Result<Subscription>;

    /// @brief 程序化清空：清空系统剪贴板全部格式（Win32 `EmptyClipboard`、Linux `xsel --clear`、
    /// macOS `pbcopy` 空输入重写 pasteboard 的平台锚）。
    ///
    /// 与空写入**语义相反**：空文本 / 空图像 / 空字节 / 空清单的 `set_*` 是契约内 no-op、**保留
    /// 既有内容**；本入口是**主动清空**。清空后 `available_formats()` 返回 `Ok(空清单)`；惰性要约
    /// 即所有权声明，随内容一并清空；监听注册不是内容，清空后仍然有效。
    /// @return 成功为 `Ok`；剪贴板打不开为 `ClipboardAccessFailed`、清空动作失败为
    /// `ClipboardWriteFailed`；本平台无清空机制为 `GeneralNotSupported`。
    static auto clear() -> Result<void>;
};

}  // namespace aurora
