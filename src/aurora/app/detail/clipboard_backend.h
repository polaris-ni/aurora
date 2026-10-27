#pragma once

// 剪贴板平台后端单槽（进程级）：**内部头**（与 `file_picker.h` / `tray_events.h` 同列于 src/）。
//
// 为何要有这道缝：富格式（文件清单 / HTML / 自定义 MIME / uri-list）与惰性提供的平台机制全部
// 落在**平台头**上（Win32 要 `<windows.h>` 与 `CF_HDROP` / `CF_HTML` / `RegisterClipboardFormat`，
// macOS 要 `NSPasteboard`，Linux 两腿要 X11 / Wayland 的 selection 语义），而门面
// `src/aurora/app/clipboard.cpp` 一旦把这些平台头逐条塞进去，同一个函数体里就会同时出现三套
// 平台分支与三套错误口径。本头把平台侧收成**一张函数表**：门面只认这张表，表未登记即「本进程
// 没有富格式机制」，沿用立骨期的 `clipboard-format-unsupported`（不谎报）。
//
// 与 CHG-038 的同源关系：最近文档的 OS 投影走的是同一道缝
// （`window/detail/recent_documents_translate.h` 的 `ProjectionBackend`）。两处形态刻意一致——
// 「公共入口进程级、平台桥可注入、单测可用伪造后端走同一条产品代码路径」，后者是「被测的就是
// 产品代码本身而不是它的复刻」这条判据的落点。
//
// **未登记的后果是有意的**：无 `Win32Host` 存活即无剪贴板 owner 窗口，而 Win32 的延迟渲染
// （`WM_RENDERFORMAT`）与变更监听（`AddClipboardFormatListener`）都以 owner 窗口为投递目标，
// 无窗口时它们在平台层就不成立。故「无窗口 ⇒ 富格式与监听报不支持」是平台事实，不是降级。
//
// 门控：无。本头在**所有**平台编译（含 wasm），不引任何平台头。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aurora/app/clipboard.h"
#include "aurora/core/result.h"

namespace aurora::clipboard::detail {

/// @brief 平台桥登记到门面的**富格式通路**（进程级单槽，整体替换）。
///
/// 每个字段都可以缺席（为空）：缺席即「该子能力本平台没有」，门面对该入口如实报
/// `ClipboardFormatUnsupported`——与未登记整表时的口径逐字相同，调用方无需区分两种缺位。
struct PlatformBackend {
    /// @brief 按 MIME 写入原始字节（空字节的 no-op 语义由门面先行判定，桥只处理非空载荷）。
    std::function<Result<bool>(const std::string &mime, const std::vector<std::uint8_t> &bytes)> set_payload;
    /// @brief 按 MIME 读原始字节；格式不存在为 `Ok(空字节)`（空内容不是失败）。
    std::function<Result<std::vector<std::uint8_t>>(const std::string &mime)> get_payload;
    /// @brief 写本地路径清单（平台原生文件清单形态）。
    std::function<Result<bool>(const std::vector<std::string> &paths)> set_files;
    /// @brief 读本地路径清单；无该格式为 `Ok(空清单)`。
    std::function<Result<std::vector<std::string>>()> get_files;
    /// @brief 写 HTML 片段（平台侧按自己的组帧规则封装）。
    std::function<Result<bool>(const std::string &html)> set_html;
    /// @brief 读 HTML 片段；无该格式为 `Ok("")`。
    std::function<Result<std::string>()> get_html;
    /// @brief 注册惰性提供要约（延迟渲染）；整表替换旧要约。
    std::function<Result<void>(const ClipboardOffer &offer)> set_offer;
    /// @brief 列出**当前剪贴板持有的全部** MIME（含平台既有机制覆盖的文本 / 图像两档）。
    ///
    /// 交全量而非只交富格式：清单的「如实反映此刻能读到什么」是一条整体口径，拆成两段由不同层
    /// 各自拼就会有第二个真源（谁是文本档的权威立刻含糊）。
    std::function<Result<std::vector<std::string>>()> available_formats;
    /// @brief 注册 / 注销剪贴板变更监听（空回调即注销）；本平台无监听机制时返回错误。
    std::function<Result<void>(std::function<void()> callback)> set_change_listener;
};

/// @brief 登记平台后端（整体替换旧表；多宿主场景由桥侧自行引用计数）。
/// @param backend 平台后端函数表（可部分缺席）。
auto install_backend(PlatformBackend backend) -> void;

/// @brief 注销平台后端，回到「本进程无富格式机制」的立骨期口径。
auto remove_backend() -> void;

/// @brief 当前生效的后端（未登记为 `nullptr`）。
[[nodiscard]] auto active_backend() -> const PlatformBackend *;

/// @brief 触发一次「所有权被夺」回调（供平台桥在收到平台通知时调用，与测试入口 `emit_*` 同源）。
///
/// 锁外触发：应用回调可能再次进入 `Clipboard` 入口，持锁调用会自死锁。
auto fire_ownership_lost() -> void;

/// @brief 触发一次「内容变化」回调（同上，供平台桥与测试入口共用）。
auto fire_change() -> void;

/// @brief 当前后端是否提供剪贴板变更监听机制（供 `install_backend` 同步 `listen_supported`）。
[[nodiscard]] auto backend_has_listener() -> bool;

}  // namespace aurora::clipboard::detail
