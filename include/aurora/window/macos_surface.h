#pragma once
#include "aurora/core/platform.h"

/// @brief macOS AppKit/CoreGraphics Surface（ARCHITECTURE.md §8.4）：仅在 defined(AURORA_PLATFORM_MACOS) 时提供。
/// 其他平台降级为 HeadlessSurface。
/// @file macos_surface.h

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <memory>
#include <string>

#include "aurora/window/surface.h"

namespace aurora {

/// @brief macOS AppKit/CoreGraphics Surface 骨架：软件 Painter 帧缓冲路径已通，窗口壳/上屏/事件为 stub。
/// 窗口可见性策略（WindowVisibility）不在此落地：本后端无窗口壳。
class MacOSSurface : public Surface {
  public:
    /// @brief 构造骨架实例：记录初始尺寸并创建 Impl（create_window 当前为 stub，仅打日志）。
    /// @param w     初始窗口宽（像素）。
    /// @param h     初始窗口高（像素）。
    /// @param title 窗口标题（UTF-8；stub 阶段未真正设置到 NSWindow）。
    MacOSSurface(int w, int h, const std::string &title);
    /// @brief 析构：经 Impl::destroy_window 释放窗口壳（当前为 stub，无实际释放动作）。
    ~MacOSSurface() override;

    /// @brief 开启新一帧：重置 Painter 至目标尺寸、更新记录尺寸并以浅色底（#F5F5F7）整帧填充。
    /// @param w 帧宽（像素）。
    /// @param h 帧高（像素）。
    /// @return 软件帧缓冲路径恒成功；不代表像素已上屏（present 仍为 stub）。
    [[nodiscard]] auto begin_frame(int w, int h) -> Result<bool> override;
    /// @brief 取得整帧共享的软件绘制器。
    /// @return 内部 Painter 引用（生命周期随本 Surface，帧内容每次 begin_frame 重置）。
    [[nodiscard]] auto painter() -> Painter & override;
    /// @brief 提交当前帧：RGBA 帧缓冲 → CGImage → drawRect: 的上屏路径尚未实写（stub 仅打日志）。
    /// @return stub 恒报成功。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 最近一次约定（构造或 begin_frame）的帧尺寸。
    /// @return 尺寸（像素）。
    [[nodiscard]] auto size() const -> Size override;
    /// @brief 是否应关闭窗口（windowShouldClose: 翻译路径未实写，暂无人置位）。
    /// @return Impl 存活时返回其 should_close 标志（当前恒 false）；impl_ 已释放时返回 true。
    [[nodiscard]] auto should_close() const -> bool override;

    /// @brief 运行时更新悬停光标形状：`[[NSCursor …] set]`（AppKit 在 .cpp 内 `#import`）。
    /// 后端映射：Arrow→`arrowCursor`、IBeam→`IBeamCursor`、PointingHand→`pointingHandCursor`、
    /// ResizeNS→`resizeUpDownCursor`、ResizeEW→`resizeLeftRightCursor`、Move→`openHandCursor`、
    /// Crosshair→`crosshairCursor`、NotAllowed→`operationNotAllowedCursor`、Wait→`busyButClickableCursor`；
    /// ResizeNWSE/NESW→macOS 无公开对角缩放光标，回退 `arrowCursor`。
    /// @param shape 待应用的光标语义形状（对角缩放两取值按上述说明回退箭头）。
    /// @note 未编译验证：须 macOS + `AURORA_BACKEND_MACOS=ON` 构建后复查（本仓库的无头
    /// Linux 构建不含 Cocoa 后端）。
    auto set_cursor(CursorShape shape) -> void override;

  private:
    struct Impl;
    /// @brief Pimpl 状态（NSWindow/NSView 句柄、should_close 标志与事件回调，定义于 .cpp）。
    std::unique_ptr<Impl> impl_;
    /// @brief 整帧软件绘制器（begin_frame 重置、供 present 读取 RGBA 缓冲）。
    Painter painter_;
    /// @brief 最近一次约定（构造/begin_frame）的帧尺寸（像素）。
    Size size_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
