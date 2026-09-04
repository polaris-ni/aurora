#pragma once

#include <string>

#include "aurora/core/image.h"

namespace aurora {

/**
 * @brief 剪贴板抽象（specification/06-app-platform.md §8.2）。
 *
 * - 文本：`set_text`/`get_text`，各平台实现：
 *   - Windows：`SetClipboardData(CF_UNICODETEXT)` / `GetClipboardData(CF_UNICODETEXT)`
 *   - Linux：`xclip -selection clipboard` / `xsel --clipboard`（需安装 xclip 或 xsel）
 *   - macOS：`pbcopy` / `pbpaste`
 * - 图像：`set_image`/`get_image`，仅 Windows 经 `SetClipboardData(CF_DIB)` 实现，
 *   其它平台 no-op（读取返回空 `Image`）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none (accesses system clipboard)
 * @note Rebuildable: no
 */
class Clipboard {
  public:
    /// @brief 把文本写入系统剪贴板（UTF-8 入参，平台按需转码）。
    static auto set_text(const std::string &text) -> void;

    /// @brief 从系统剪贴板读取文本（UTF-8 返回）。
    [[nodiscard]] static auto get_text() -> std::string;

    /// @brief 把 RGBA8 图像写入系统剪贴板（经 CF_DIB）。空图像直接返回（不清除已有内容）。
    static auto set_image(const Image &img) -> void;

    /// @brief 从系统剪贴板读取 RGBA8 图像；无图像（或不可用）返回空 `Image`（width==0）。
    [[nodiscard]] static auto get_image() -> Image;
};

}  // namespace aurora
