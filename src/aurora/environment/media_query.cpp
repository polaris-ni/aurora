#include "aurora/environment/media_query.h"

#include "aurora/environment/environment.h"
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
    return mq;
#endif
}

auto MediaQuery::of(const BuildContext &ctx) -> const MediaQuery & {
    const MediaQuery *p = media_query_of(ctx);
    if (p != nullptr) {
        return *p;
    }
    // 无 Provider 注入：返回进程级默认实例（调用方应据此降级渲染/布局）。
    static constexpr MediaQuery default_instance{};
    return default_instance;
}

auto media_query_of(const BuildContext &ctx) -> const MediaQuery * { return ctx.environment<MediaQuery>(); }

} // namespace aurora
