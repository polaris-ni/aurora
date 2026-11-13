// file_dialog_macos.cpp — 文件对话框的 macOS 原生实现（NSOpenPanel / NSSavePanel）。
//
// 口径与 Win32 腿同构：钩子优先 → 非交互回空 → 原生模态面板 → 取消即 `Ok(空)`。
// 与 Win32 的差别只有「失败面」：AppKit 面板不存在可失败的构造步骤，故本腿不返回
// `Error`（真无 GUI 上下文时 WARN 后按取消回空——不静默说谎、不挂起）。
//
// 为何 macOS 默认走原生面板而不是自绘腿：macOS 用户对 NSOpenPanel / NSSavePanel 的
// 侧栏位置、iCloud / 网络卷可见性、沙盒授权（security-scoped URL）有明确预期；自绘
// 枚举走 `std::filesystem`，在沙盒进程里既拿不到授权目录也触达不到系统位置。自绘腿
// 仍保留给「macOS 平台但未启用 AppKit 后端」（GLFW / X11 / Headless）的构建。
//
// 门控：`AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS`（CMake 只把本文件按 ObjC++
// 语言登记进 macOS 后端）；其余组合由 `file_dialog_selfdrawn.cpp` 或公共头内联回退承接。
//
// API 下限：类型筛选走 `NSSavePanel.allowedContentTypes`（macOS 12 起），更老的系统没有
// 等价的无弃用投递面；本仓不设 `CMAKE_OSX_DEPLOYMENT_TARGET`（见 BUILD_OPTIONS.md），
// 故不做 11 及以下的兼容分叉——如实记在这里而不是留一段永远走不到的兜底。

#include "aurora/app/file_dialog.h"
#include "aurora/core/platform.h"  // NOLINT
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "aurora/app/detail/file_dialog_plan.h"
#include "aurora/core/log.h"

namespace fs = std::filesystem;

namespace aurora::file_dialog {
namespace {

/// @brief `std::string`（UTF-8）→ `NSString`；空串或非法 UTF-8 得 nil。
/// @param text UTF-8 文本。
/// @return 自动释放的 `NSString`；无效输入为 nil（调用方据此留 WARN 或跳过）。
auto ns_string(std::string_view text) -> NSString * {
    if (text.empty()) {
        return nil;
    }
    // `stringWithUTF8String:` 读 NUL 终止串；临时 `std::string` 的生命周期覆盖整条消息发送。
    return [NSString stringWithUTF8String:std::string(text).c_str()];
}

/// @brief 把筛选器模式（`*.png` 形态）折成裸扩展名（`png`）。
///
/// 公共面把 `Filter::extensions` 定义为通配模式，两条实实现腿都从同一形态出发：Win32 直接
/// 把整串交给 `SetFileTypes`，本腿要先取出 UTType 认得的扩展名。只做「取末段 → 去 `*` →
/// 去前导点」，其余原样（复合扩展名如 `tar.gz` 交 UTType 自行判定）。
/// @param pattern 通配模式（如 `*.png`）。
/// @return 裸扩展名；模式为空或只有通配符时为空串。
[[nodiscard]] auto pattern_extension(std::string_view pattern) -> std::string {
    std::string_view tail = file_dialog_plan::last_path_segment(pattern);
    if (!tail.empty() && tail.front() == '*') {
        tail.remove_prefix(1);
    }
    if (!tail.empty() && tail.front() == '.') {
        tail.remove_prefix(1);
    }
    return std::string(tail);
}

/// @brief 统一投递用户标题：写面板提示行（`message`），不覆盖系统窗口标题。
/// @param panel 目标面板（打开 / 保存 / 目录三种形态共有）。
/// @param title 用户标题（空串 = 不表达意图）。
auto apply_title(NSSavePanel *panel, const std::string &title) -> void {
    if (title.empty()) {
        return;
    }
    NSString *message = ns_string(title);
    if (message == nil) {
        AURORA_LOG_WARN("file_dialog", "title is not valid UTF-8, keeping the panel default: ", title);
        return;
    }
    [panel setMessage:message];
}

/// @brief 初始位置：目录存在才投递；不可解析 / 不是目录时留 WARN 并停在系统默认位置。
/// @param panel 目标面板。
/// @param initial_dir 用户给的初始目录（UTF-8；空串 = 不表达意图）。
auto apply_initial_dir(NSSavePanel *panel, const std::string &initial_dir) -> void {
    if (initial_dir.empty()) {
        return;
    }
    NSString *const directory = ns_string(initial_dir);
    if (directory == nil) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir is not valid UTF-8, keeping the system default location: ", initial_dir);
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(fs::path(initial_dir), ec)) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir is not an existing directory, keeping the system default location: ", initial_dir);
        return;
    }
    [panel setDirectoryURL:[NSURL fileURLWithPath:directory]];
}

/// @brief 把筛选器与缺省扩展名折成 UTType 列表。
///
/// 原生面板没有「命名筛选器」这一概念（`Filter::name` 无落点，文档已声明），能表达的是
/// 允许的内容类型集合；缺省扩展名排在最前，让 `NSSavePanel` 在用户漏打后缀时补的就是它。
/// @param filters 用户筛选器列表（空 = 不限制）。
/// @param default_extension 归一后的缺省扩展名（空 = 无偏好）。
/// @return 允许类型数组；一枚都折不出时为空数组（调用方据此保持「不限制」）。
[[nodiscard]] auto content_types(const std::vector<Filter> &filters, const std::string &default_extension)
    -> NSArray<UTType *> * {
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    const auto append = [types](const std::string &extension) -> void {
        if (extension.empty()) {
            return;
        }
        NSString *text = ns_string(extension);
        if (text == nil) {
            return;
        }
        UTType *type = [UTType typeWithFilenameExtension:text];
        if ((type != nil) && ![types containsObject:type]) {
            [types addObject:type];
        }
    };
    append(default_extension);
    for (const Filter &filter : filters) {
        for (const std::string &pattern : filter.extensions) {
            append(pattern_extension(pattern));
        }
    }
    return types;
}

/// @brief 投递类型限制；列表为空时保持面板默认（不限制任何类型）。
/// @param panel 目标面板。
/// @param filters 用户筛选器列表。
/// @param default_extension 归一后的缺省扩展名（仅保存形态非空）。
auto apply_filters(NSSavePanel *panel, const std::vector<Filter> &filters, const std::string &default_extension)
    -> void {
    NSArray<UTType *> *types = content_types(filters, default_extension);
    if ([types count] > 0) {
        [panel setAllowedContentTypes:types];
    }
}

/// @brief 保存形态的名字栏初值：只取末段（目录部分会显示成带斜杠的怪名字）。
/// @param panel 目标保存面板。
/// @param suggested_name 用户建议名（UTF-8；空串 / 只有目录部分 = 不表达意图）。
auto apply_save_name(NSSavePanel *panel, const std::string &suggested_name) -> void {
    const std::string name = file_dialog_plan::suggested_file_name(suggested_name);
    if (name.empty()) {
        return;
    }
    NSString *text = ns_string(name);
    if (text == nil) {
        AURORA_LOG_WARN("file_dialog", "suggested_name is not valid UTF-8, leaving the name field empty: ", name);
        return;
    }
    [panel setNameFieldStringValue:text];
}

/// @brief 面板 URL → UTF-8 路径（空串 = 无选中 / 路径不可表示）。
/// @param url 面板产出的文件 URL。
/// @return 文件系统路径；不可表示时为空串。
[[nodiscard]] auto url_path(NSURL *url) -> std::string {
    if (url == nil) {
        return {};
    }
    NSString *path = [url path];
    const char *utf8 = (path != nil) ? [path UTF8String] : nullptr;
    return (utf8 != nullptr) ? std::string(utf8) : std::string{};
}

/// @brief 原生模态前置判据：没有 NSApplication 就没有可挂的模态会话。
///
/// 刻意**不**调 `[NSApplication sharedApplication]` 创建 NSApp：那会把「无 GUI 上下文」
/// 变成「悄悄拉起 AppKit 再弹一个没人接管的模态窗」（无头 CI 上表现为挂起）。真 GUI
/// 进程在窗口创建时已由 `MacOSSurface` 初始化 NSApp，此判据恒真。
/// @return 有 GUI 上下文时为 true。
[[nodiscard]] auto has_app_context() -> bool {
    if (NSApp == nil) {
        AURORA_LOG_WARN("file_dialog", "no NSApplication context; returning empty result (equivalent to cancel)");
        return false;
    }
    return true;
}

}  // namespace

auto open_file(const Options &opts) -> Result<std::vector<std::string>> {
    headless_last_options = opts;
    if (!headless_open_result.empty()) {
        return headless_open_result;  // 钩子优先：不构造面板（与另两腿同序）
    }
    if (!interactive) {
        return std::vector<std::string>{};  // 非交互 = 取消（不弹 UI）
    }
    if (!has_app_context()) {
        return std::vector<std::string>{};
    }
    std::vector<std::string> picked;
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:YES];  // 与 Win32 的 FOS_ALLOWMULTISELECT 同口径
        [panel setResolvesAliases:YES];
        apply_title(panel, opts.title);
        apply_initial_dir(panel, opts.initial_dir);
        apply_filters(panel, opts.filters, {});
        if ([panel runModal] == NSModalResponseOK) {
            NSArray<NSURL *> *urls = [panel URLs];
            const NSUInteger count = (urls != nil) ? [urls count] : 0;
            for (NSUInteger i = 0; i < count; ++i) {
                std::string path = url_path([urls objectAtIndex:i]);
                if (!path.empty()) {
                    picked.push_back(std::move(path));
                }
            }
        }
    }
    return picked;
}

auto save_file(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    if (!interactive) {
        return std::string{};
    }
    if (!has_app_context()) {
        return std::string{};
    }
    std::string picked;
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];
        const std::string extension = file_dialog_plan::normalize_extension(opts.default_extension);
        apply_title(panel, opts.title);
        apply_initial_dir(panel, opts.initial_dir);
        apply_filters(panel, opts.filters, extension);
        apply_save_name(panel, opts.suggested_name);
        if ([panel runModal] == NSModalResponseOK) {
            picked = url_path([panel URL]);
        }
    }
    // 面板通常已按首选类型补齐后缀；补全口径仍走折算层兜底（名字最后一段没打点才补），
    // 与自绘腿逐字节同解，避免「同一条输入在两个 macOS 构建上落出不同文件名」。
    if (!picked.empty()) {
        picked = file_dialog_plan::with_default_extension(picked, opts.default_extension);
    }
    return picked;
}

auto open_folder(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    if (!interactive) {
        return std::string{};
    }
    if (!has_app_context()) {
        return std::string{};
    }
    std::string picked;
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:YES];  // 「新建文件夹」与系统面板一致
        [panel setResolvesAliases:YES];
        apply_title(panel, opts.title);
        apply_initial_dir(panel, opts.initial_dir);
        // 目录没有类型语义：filters 不投递（与自绘腿「忽略 filters」同口径）。
        if ([panel runModal] == NSModalResponseOK) {
            picked = url_path([panel URL]);
        }
    }
    return picked;
}

}  // namespace aurora::file_dialog

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
