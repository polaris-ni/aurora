#pragma once
#include "aurora/core/platform.h"

// macOS AppKit/CoreGraphics Surface（ARCHITECTURE.md §8.4）：仅在 defined(AURORA_PLATFORM_MACOS) 时提供。
// 其他平台降级为 HeadlessSurface。

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <memory>
#include <string>

#include "aurora/window/surface.h"

namespace aurora {

class MacOSSurface : public Surface {
  public:
    MacOSSurface(int w, int h, const std::string &title);
    ~MacOSSurface() override;

    [[nodiscard]] auto begin_frame(int w, int h) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override;
    [[nodiscard]] auto present() -> Result<bool> override;
    [[nodiscard]] auto size() const -> Size override;
    [[nodiscard]] auto should_close() const -> bool override;

    /// @brief 运行时更新悬停光标形状：`[[NSCursor …] set]`（AppKit 在 .cpp 内 `#import`）。
    /// 后端映射：Arrow→`arrowCursor`、IBeam→`IBeamCursor`、PointingHand→`pointingHandCursor`、
    /// ResizeNS→`resizeUpDownCursor`、ResizeEW→`resizeLeftRightCursor`、Move→`openHandCursor`、
    /// Crosshair→`crosshairCursor`、NotAllowed→`operationNotAllowedCursor`、Wait→`busyButClickableCursor`；
    /// ResizeNWSE/NESW→macOS 无公开对角缩放光标，回退 `arrowCursor`。
    /// @note 未编译验证：须 macOS + `AURORA_BACKEND_MACOS=ON` 构建后复查（本仓库的无头
    /// Linux 构建不含 Cocoa 后端）。
    auto set_cursor(CursorShape shape) -> void override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Painter painter_;
    Size size_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
