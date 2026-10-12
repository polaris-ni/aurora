// 客户端自绘装饰（CSD）—— X11 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 用途：在**真实 X server** 上验证 X11Surface 的自绘装饰路径端到端可用：
//   A) DecorationPolicy::ClientSide → _MOTIF_WM_HINTS 撤 WM 口沿后，标题栏带
//      真正绘出（AdwaitaDark 预设底色像素级比对），content_inset 上报栏高；
//   B) Auto（SSD）→ content_inset 恒零、装饰不入侵应用内容（WM 口沿原样）；
//   C) Frameless → 同 B 的零 inset 口径。
// 判定全部离屏（DEBUG 构建的 data() 直读 Painter 缓冲，不依赖窗口映射）。
// 悬停 / 按压 / 拖拽 / 双击的手工目视段不进本探针（见 codespec/manual-test/10-window.md）。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_X11=ON -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_x11_csd
// 构建（方式 ② 手写命令，须先有一个 DEBUG+X11 构建目录 build-x11-dbg）：
//   g++ -std=c++20 -DAURORA_BACKEND_X11 -DAURORA_ENABLE_DEBUG -DAURORA_PLATFORM_LINUX \
//       -I include tools/verify/x11_csd_live_probe.cpp -o /tmp/x11_csd_live_probe \
//       build-x11-dbg/libaurora.a build-x11-dbg/third_party/harfbuzz/libharfbuzz.a \
//       build-x11-dbg/third_party/freetype/libfreetype.a -lX11 -lz -lpthread -ldl -lm
//
// 运行：DISPLAY=:0 ./aurora_verify_x11_csd        （退出码 0=全过 1=判据失败 2=环境不支持）
//
// 注意：像素判据依赖 AURORA_ENABLE_DEBUG=ON 构建（data() 才暴露帧缓冲）。

#include <cmath>
#include <cstdio>

#include "aurora/window/x11_surface.h"

using namespace aurora;

namespace {

int g_checks = 0;
int g_pass = 0;

void check(bool ok, const char *name) {
    ++g_checks;
    if (ok) {
        ++g_pass;
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
    }
}

std::uint32_t pixel_at(const X11Surface &surf, int x, int y) {
    const int w = static_cast<int>(surf.size().width * surf.scale_factor());
    const std::uint8_t *d = surf.data();
    const std::size_t off =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
    return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8U) |
           (static_cast<std::uint32_t>(d[off + 2]) << 16U);
}

/// @brief 绘一帧：整幅填白（应用内容），present 后由装饰层幂等覆绘标题栏带。
void paint_content(X11Surface &surf) {
    (void)surf.begin_frame(320, 200);
    Painter &p = surf.painter();
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 320.0F, .height = 200.0F}},
                Color{255, 255, 255, 255});
    (void)surf.present();
}

}  // namespace

int main() {
    // ---- A) ClientSide：CSD 标题栏带真绘出 + 安全区上报 ----------------------
    WindowStyleOptions client;
    client.decoration = DecorationPolicy::ClientSide;
    client.title_bar = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    {
        X11Surface surf(320, 200, "csd-probe-client", client, WindowVisibility::Hidden);
        if (!surf.is_available()) {
            std::printf("SKIP: no X display\n");
            return 2;
        }
        const EdgeInsets inset = surf.content_inset();
        std::printf("content_inset: top=%.1f left=%.1f right=%.1f bottom=%.1f\n", static_cast<double>(inset.top),
                    static_cast<double>(inset.left), static_cast<double>(inset.right),
                    static_cast<double>(inset.bottom));
        check(std::fabs(inset.top - 36.0F) < 0.01F && inset.left == 0.0F && inset.right == 0.0F,
              "client-side content_inset reports title bar height only");
        paint_content(surf);
        const std::uint32_t band = pixel_at(surf, 10, 10);
        const std::uint32_t below = pixel_at(surf, 10, 100);
        std::printf("band=%06X below=%06X (expect 303030 / FFFFFF)\n", static_cast<unsigned>(band),
                    static_cast<unsigned>(below));
        check(band == 0x303030U, "client-side title bar band painted with AdwaitaDark bg");
        check(below == 0xFFFFFFU, "client-side app content area not invaded by decoration");
    }

    // ---- B) Auto：SSD 口沿保留，装饰零入侵 ----------------------------------
    {
        WindowStyleOptions auto_ssd;
        auto_ssd.title_bar = client.title_bar;
        X11Surface surf(320, 200, "csd-probe-auto", auto_ssd, WindowVisibility::Hidden);
        if (!surf.is_available()) {
            std::printf("SKIP: no X display\n");
            return 2;
        }
        const EdgeInsets inset = surf.content_inset();
        check(inset.top == 0.0F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
              "auto(ssd) content_inset stays zero");
        paint_content(surf);
        check(pixel_at(surf, 10, 10) == 0xFFFFFFU, "auto(ssd) decoration draws nothing into content");
    }

    // ---- C) Frameless：无装饰无热区，口径同 SSD ------------------------------
    {
        WindowStyleOptions frameless;
        frameless.decoration = DecorationPolicy::Frameless;
        X11Surface surf(320, 200, "csd-probe-frameless", frameless, WindowVisibility::Hidden);
        if (!surf.is_available()) {
            std::printf("SKIP: no X display\n");
            return 2;
        }
        const EdgeInsets inset = surf.content_inset();
        check(inset.top == 0.0F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
              "frameless content_inset stays zero");
        paint_content(surf);
        check(pixel_at(surf, 10, 10) == 0xFFFFFFU, "frameless decoration draws nothing");
    }

    std::printf("checks=%d pass=%d\n", g_checks, g_pass);
    return (g_pass == g_checks) ? 0 : 1;
}
