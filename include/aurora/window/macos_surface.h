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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Painter painter_;
    Size size_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
