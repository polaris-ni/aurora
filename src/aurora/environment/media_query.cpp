#include "aurora/environment/media_query.h"

#include "aurora/app/display.h"
#include "aurora/environment/build_context.h"
#include "aurora/window/surface.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif

namespace aurora {

auto MediaQuery::from_surface(const Surface &s) -> MediaQuery {
#ifdef AURORA_BACKEND_WIN32
    return win32_media_query(s);
#else
    MediaQuery mq;
    mq.size = s.size();
    mq.scale_factor = s.scale_factor();
    mq.screen_size = mq.size;
    mq.orientation = (mq.size.width >= mq.size.height) ? ScreenOrientation::Landscape : ScreenOrientation::Portrait;
    mq.platform = PlatformKind::Unknown;
    mq.device = DeviceKind::Unknown;
    mq.padding = EdgeInsets{};
    // 并入客户端装饰（CSD）预留的安全区：标题栏高度（顶）与边框厚度（四周）。
    // 子树据 `MediaQuery.padding` 将内容下沉，避开自绘标题栏/边框（对齐 Flutter SafeArea）。
    const EdgeInsets ins = s.content_inset();
    mq.padding.top += ins.top;
    mq.padding.left += ins.left;
    mq.padding.right += ins.right;
    mq.padding.bottom += ins.bottom;
    mq.prefer_reduced_motion = false;
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    // macOS 腿：屏幕尺寸与方向改取**真实主显示器**，不再拿窗口尺寸冒充屏幕尺寸——后者使
    // `orientation` 在窗口被拉成竖长时误判为竖屏，且 `screen_size` 恒等于窗口宽，响应式断点据此
    // 分级就会错位。显示器枚举在本后端是真实 `NSScreen` 投影（`app/display_macos.cpp`），
    // `Display::bounds` 为物理像素，按本 Surface 的 dp 比折算成逻辑 dp（与 Win32 支的
    // `GetSystemMetrics(SM_CXSCREEN) / scale` 同一口径与「取主屏」的同一粒度）。
    const float scale = (mq.scale_factor > 0.0F) ? mq.scale_factor : 1.0F;
    const Display primary = app::primary_display();
    mq.screen_size = Size{.width = static_cast<float>(primary.bounds.size.width) / scale,
                          .height = static_cast<float>(primary.bounds.size.height) / scale};
    mq.orientation =
        (mq.screen_size.width >= mq.screen_size.height) ? ScreenOrientation::Landscape : ScreenOrientation::Portrait;
    // `platform` / `device` 两栏本批**不动**：它们的公共注释是「后端矩阵」陈述（只在 Win32 填），
    // 且 `platform` 是 `Theme::default_style` 的风格判据来源——改动它属跨后端一致性契约变更，
    // 与 `prefer_reduced_motion` 的 OS 接线一并先立变更提案定口径，再落码。
#endif
    return mq;
#endif
}

auto MediaQuery::of(const BuildContext &ctx) -> const MediaQuery & {
    const MediaQuery *p = media_query_of(ctx);
    if (p != nullptr) {
        return *p;
    }
    // 无 Provider 注入：返回进程级默认实例（调用方应据此降级渲染/布局）。
    static constexpr MediaQuery DEFAULT_INSTANCE{};
    return DEFAULT_INSTANCE;
}

auto media_query_of(const BuildContext &ctx) -> const MediaQuery * { return ctx.environment<MediaQuery>(); }

}  // namespace aurora
