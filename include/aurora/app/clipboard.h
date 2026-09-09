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
