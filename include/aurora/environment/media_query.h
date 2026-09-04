#pragma once

#include <cstdint>

#include "aurora/core/types.h"

namespace aurora {

class Surface;  ///< 前向声明：from_surface 仅按 const 引用取尺寸/缩放因子。
class BuildContext;  ///< 前向声明：of(ctx) / media_query_of(ctx) 仅按 const 引用读取环境链。

/**
 * @brief 屏幕方向（屏幕逻辑尺寸派生）。
 *
 * 与 `divider.h` 的 `Orientation{Horizontal,Vertical}`（分隔线方向）语义不同，独立枚举避免误用。
 */
enum class ScreenOrientation : std::uint8_t {
    Portrait,  ///< 竖屏：高 ≥ 宽
    Landscape,  ///< 横屏：宽 > 高
};

/**
 * @brief 运行平台（编译期常量为主）。
 *
 * Win32 后端下为 `Windows`；Headless/GLFW 等返回 `Unknown`。本期不做运行时 OS 探测。
 */
enum class PlatformKind : std::uint8_t {
    Unknown,  ///< 未知 / 非 Windows 后端（Headless/GLFW）
    Windows,  ///< Win32/GDI 后端
    MacOs,
    Linux,
    Web,
};

/**
 * @brief 设备形态（编译期常量为主）。
 *
 * Win32 后端下为 `Desktop`；其余返回 `Unknown`。
 */
enum class DeviceKind : std::uint8_t {
    Unknown,  ///< 未知 / 非桌面后端
    Desktop,  ///< 桌面（Win32）
    Mobile,
    Tablet,
};

/**
 * @brief 媒体查询：当前子树可见的响应式环境上下文（specification/07-environment-modifier.md §3.1 MediaQuery）。
 *
 * 经 `Provider<MediaQuery>` 沿「Environment 注入链」向下传播，子树经
 * `media_query_of(ctx)` / `MediaQuery::of(ctx)` 读取「最近祖先 Provider」生效的值。
 * 新增字段均保留默认值（向后兼容）；未注入 Provider 时读回默认实例。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct MediaQuery {
    Size size{};  ///< 当前窗口/子树可用逻辑尺寸（dp）。
    float scale_factor = 1.0F;  ///< 设备像素比（dp → device px）。
    float text_scale_factor = 1.0F;  ///< 系统字体缩放（辅助功能）。
    ScreenOrientation orientation = ScreenOrientation::Portrait;  ///< 由 `screen_size` 派生。
    Size screen_size{};  ///< 物理屏幕的逻辑尺寸（dp）；无 Provider 时为 0。
    PlatformKind platform = PlatformKind::Unknown;  ///< 编译期常量，非 Windows 为 Unknown。
    DeviceKind device = DeviceKind::Unknown;  ///< 编译期常量，非桌面为 Unknown。
    EdgeInsets padding{};  ///< 安全区（刘海/状态栏）内边距（dp）。
    bool prefer_reduced_motion = false;  ///< 系统「减弱动效」偏好。

    /// @brief 便捷构造：仅给定缩放因子（保留其余默认），用于轻量注入。
    [[nodiscard]] static auto of(float scale) -> MediaQuery {
        MediaQuery mq;
        mq.scale_factor = scale;
        return mq;
    }

    /// @brief 从 `Surface` 成型：读取尺寸与缩放因子；Win32 下经 `win32_media_query` 取真实屏幕/减弱动效。
    [[nodiscard]] static auto from_surface(const Surface &s) -> MediaQuery;

    /// @brief 读取最近祖先 Provider 注入的 `MediaQuery`；无则诊断并返回进程级默认实例。
    [[nodiscard]] static auto of(const BuildContext &ctx) -> const MediaQuery &;
};

/// @brief 读取最近祖先 Provider 注入的 `MediaQuery`；无 Provider 时返回 `nullptr`（调用方按需降级）。
[[nodiscard]] auto media_query_of(const BuildContext &ctx) -> const MediaQuery *;

}  // namespace aurora
