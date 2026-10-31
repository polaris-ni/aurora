/* macOS Surface 真机验收探针（人工触发，不进 CTest）
// ============================================================================
// 验收范围：
//   ✅ 真实 AppKit 窗口创建（NSWindow + 内容视图）与 Close 语义；
//   ✅ `begin_frame` / `Painter` / `present` 的软件上屏路径：帧缓冲尺寸、像素落点、
//      清屏底色、`frame_count` 推进；
//   ✅ 逻辑尺寸与物理帧缓冲的换算（`backingScaleFactor`）、`set_size` 同步生效；
//   ✅ `set_position` / `position` 的物理像素往返（左上原点 / y 向下）；
//   ✅ `display_id` 与 `app::list_displays` / `primary_display` / `display_containing`
//      的同一 id 空间（CGDirectDisplayID）；
//   ✅ `os_color_scheme()` 对 NSApp 外观切换的实时跟随（Dark / Aqua 双向）。
//
//   ❌ 本探针不验收鼠标/键盘事件翻译、输入法、系统拖放、光标形状：前两者需真实人机
//      输入（见 macos_cursor_live_probe / macos_drag_live_probe），后两者各有专用探针。
//   ❌ 不经像素截图证明「屏幕上真的显示了」：AppKit 的 `displayIfNeeded` 已同步提交到
//      WindowServer，但背景会话下 `screencapture` 不可用；本探针证明的是提交前的帧缓冲
//      内容与提交调用成功。
//
// 构建：
//   cmake -S . -B build-macos -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-macos --target aurora_verify_macos_surface
//   ./build-macos/aurora_verify_macos_surface
//
// 退出码：
//   0  全部判据通过
//   1  至少一条判据失败（逐行 FAIL 指出）
//   2  环境不可用（无法创建窗口：无 WindowServer / 无图形会话）
//   3  判据不适用（例如本机无可用 NSScreen 记录）
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_MACOS
#error "aurora_verify_macos_surface 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#ifndef AURORA_BACKEND_MACOS
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/app/display.h"
#include "aurora/render/painter.h"
#include "aurora/window/macos_surface.h"
#include "aurora/window/platform.h"
#include "aurora/window/window.h"

#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "verify_print.h"

namespace {

using aurora::Color;

// 探针的通过 / 失败计数：匿名命名空间内的进程私有记账（无跨 TU 可见性、无初始化顺序
// 问题），就地豁免非 const 全局（同 x11_csd_live_probe 的探针记账口径）。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_checks = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int g_failures = 0;

auto check(bool ok, const std::string &name, const std::string &detail = {}) -> void {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    AURORA_LOG_RAW("verify", ok ? "PASS  " : "FAIL  ", aurora_verify::pad_right(name, 42), detail, "\n");
}

auto fmt_float(float v) -> std::string {
    return aurora_verify::format_int(static_cast<long long>(std::lround(static_cast<double>(v))));
}

auto fmt_size(aurora::Size s) -> std::string { return fmt_float(s.width) + "x" + fmt_float(s.height); }

auto fmt_point(aurora::Point p) -> std::string { return "(" + fmt_float(p.x) + "," + fmt_float(p.y) + ")"; }

auto nearly(float a, float b) -> bool { return std::fabs(a - b) <= 1.0F; }

/// @brief 取帧缓冲 (x, y) 处的 RGBA 并与 expected 比；坐标越界判不符，绝不越读。
/// @param frame 整帧字节（`Painter` 缓冲的视图，空视图 = 数据不可用）。
/// @param width 帧缓冲宽（像素）。
/// @param x 列坐标。
/// @param y 行坐标。
/// @param expected 期望颜色。
/// @return 四通道全等时为 true。
auto pixel_is(std::span<const std::uint8_t> frame, int width, int x, int y, Color expected) -> bool {
    if (frame.empty() || width <= 0 || x < 0 || y < 0) {
        return false;
    }
    const std::size_t offset =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)) * 4U;
    if (offset + 4U > frame.size()) {
        return false;
    }
    const std::span<const std::uint8_t> rgba = frame.subspan(offset, 4);
    return rgba[0] == expected.r && rgba[1] == expected.g && rgba[2] == expected.b && rgba[3] == expected.a;
}

/// 把 nsappearance 设为指定外观并返回原值（调用方负责恢复）。
auto set_app_appearance(NSAppearance *appearance) -> NSAppearance * {
    NSAppearance *saved = [NSApp appearance];
    [NSApp setAppearance:appearance];
    // NSApp.appearance 的生效是同步的；再读一次 `effectiveAppearance` 作为本探针的观测点。
    return saved;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与既有探针同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    @autoreleasepool {
        AURORA_LOG_RAW("verify", "aurora_verify_macos_surface\n");
        AURORA_LOG_RAW("verify", "----------------------------------------\n");

        aurora::WindowOptions base;
        base.size = aurora::Size{.width = 360.0F, .height = 240.0F};
        base.title = "Aurora macOS surface probe";
        aurora::MacOSOptions opts{base};

        auto win_result = aurora::create_window(opts);
        if (!win_result) {
            AURORA_LOG_RAW("verify", "ENV-UNAVAILABLE: create_window failed: ", win_result.error().message, "\n");
            return 2;
        }
        auto win = std::move(win_result.value());
        aurora::Surface &surface = win->surface();
        auto *mac = dynamic_cast<aurora::MacOSSurface *>(&surface);
        check(mac != nullptr, "backend is MacOSSurface");
        if (mac == nullptr) {
            return 1;
        }

        check(surface.native_handle() != nullptr, "native NSWindow handle",
              aurora_verify::format_handle(surface.native_handle()));
        auto *window = (__bridge NSWindow *)surface.native_handle();
        check(window != nil, "native_handle bridges to NSWindow");
        check([window isVisible] != 0, "window is visible after creation");
        check(nearly(surface.size().width, 360.0F) && nearly(surface.size().height, 240.0F), "initial logical size",
              fmt_size(surface.size()));

        surface.set_title("Aurora macOS surface probe / retitled");
        check(std::string([[window title] UTF8String]) == "Aurora macOS surface probe / retitled",
              "set_title reaches NSWindow", [[window title] UTF8String]);

        const float scale = surface.scale_factor();
        check(scale > 0.0F, "backing scale factor positive", fmt_float(scale));

        // ---- 上屏：整帧底色 + 红色矩形，提交后核对帧缓冲 ----
        auto begin1 = surface.begin_frame(360, 240);
        check(begin1.ok(), "begin_frame #1");
        if (begin1) {
            aurora::Painter &painter = surface.painter();
            painter.fill_rect(aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                                           .size = aurora::Size{.width = 16.0F, .height = 16.0F}},
                              Color::red());
            auto present1 = surface.present();
            check(present1.ok() && present1.value(), "present #1", present1 ? "" : present1.error().message);
        }
        const aurora::Size fb1 = surface.framebuffer_size();
        const aurora::Size expect1{.width = 360.0F * scale, .height = 240.0F * scale};
        check(nearly(fb1.width, expect1.width) && nearly(fb1.height, expect1.height), "framebuffer = logical x scale",
              fmt_size(fb1) + " vs " + fmt_size(expect1));
        check(surface.frame_count() == 1, "frame_count after first present",
              aurora_verify::format_int(surface.frame_count()));

        const std::uint8_t *pixels = surface.data();
        check(pixels != nullptr, "painter data available after begin_frame");
        const int fbw = static_cast<int>(std::lround(static_cast<double>(fb1.width)));
        const int fbh = static_cast<int>(std::lround(static_cast<double>(fb1.height)));
        // 「指针 + 字节数」的帧缓冲视图（数据不可见时长度取 0，判据自会判不符而不越读）。
        const std::size_t row_bytes = static_cast<std::size_t>(fbw) * 4U;
        const std::size_t frame_bytes = (pixels != nullptr) ? row_bytes * static_cast<std::size_t>(fbh) : 0U;
        const std::span<const std::uint8_t> frame(pixels, frame_bytes);
        const Color clear = surface.clear_color();
        check(pixel_is(frame, fbw, 0, 0, Color::red()), "red rect pixel at (0,0)");
        check(pixel_is(frame, fbw, fbw - 1, fbh - 1, clear), "clear color at bottom-right",
              "rgba(" + aurora_verify::format_int(clear.r) + "," + aurora_verify::format_int(clear.g) + "," +
                  aurora_verify::format_int(clear.b) + "," + aurora_verify::format_int(clear.a) + ")");

        // ---- 几何：set_size 同步生效，下一帧按新尺寸重建 ----
        surface.set_size(aurora::Size{.width = 420.0F, .height = 300.0F});
        check(nearly(surface.size().width, 420.0F) && nearly(surface.size().height, 300.0F),
              "set_size reflected by size()", fmt_size(surface.size()));
        auto begin2 = surface.begin_frame(420, 300);
        check(begin2.ok(), "begin_frame #2");
        auto present2 = surface.present();
        check(present2.ok() && present2.value(), "present #2");
        const aurora::Size fb2 = surface.framebuffer_size();
        check(nearly(fb2.width, 420.0F * scale) && nearly(fb2.height, 300.0F * scale), "framebuffer follows set_size",
              fmt_size(fb2));
        check(surface.frame_count() == 2, "frame_count after second present",
              aurora_verify::format_int(surface.frame_count()));

        // ---- 位置：物理像素往返（左上原点 / y 向下）----
        surface.set_position(aurora::Point{.x = 80.0F, .y = 100.0F});
        const aurora::Point pos = surface.position();
        check(nearly(pos.x, 80.0F) && nearly(pos.y, 100.0F), "set_position round-trip", fmt_point(pos));

        // ---- 显示器：同一 id 空间 ----
        const int wid = surface.display_id();
        const std::vector<aurora::Display> displays = aurora::app::list_displays();
        check(wid >= 0, "window display_id valid", aurora_verify::format_int(wid));
        check(!displays.empty(), "app::list_displays non-empty", aurora_verify::format_uint(displays.size()));
        const aurora::Display primary = aurora::app::primary_display();
        check(primary.is_primary, "primary_display is_primary", primary.name);
        bool id_seen = false;
        for (const auto &d : displays) {
            if (d.id == wid) {
                id_seen = true;
                const float cx = pos.x + (surface.size().width * scale * 0.5F);
                const float cy = pos.y + (surface.size().height * scale * 0.5F);
                const aurora::Display under = aurora::app::display_containing(aurora::Point{.x = cx, .y = cy});
                check(under.id == wid, "display_containing(window center) == display_id",
                      aurora_verify::format_int(under.id) + " vs " + aurora_verify::format_int(wid));
                check(d.scale_factor > 0.0F, "display scale factor positive", fmt_float(d.scale_factor));
                break;
            }
        }
        check(id_seen, "display_id present in list_displays", aurora_verify::format_int(wid));

        // ---- OS 明暗偏好：NSApp 外观切换后实时跟随 ----
        const aurora::ColorScheme initial = aurora::os_color_scheme();
        check(initial == aurora::ColorScheme::Dark || initial == aurora::ColorScheme::Light,
              "os_color_scheme returns Light/Dark", initial == aurora::ColorScheme::Dark ? "Dark" : "Light");
        NSAppearance *saved = set_app_appearance([NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]);
        check(aurora::os_color_scheme() == aurora::ColorScheme::Dark, "os_color_scheme follows DarkAqua");
        set_app_appearance([NSAppearance appearanceNamed:NSAppearanceNameAqua]);
        check(aurora::os_color_scheme() == aurora::ColorScheme::Light, "os_color_scheme follows Aqua");
        set_app_appearance(saved);

        // ---- 事件泵：队列级等待/唤醒不崩溃，语义位为 true ----
        check(surface.pumps_thread_queue(), "pumps_thread_queue");
        check(surface.waits_thread_queue(), "waits_thread_queue");
        surface.request_wake();
        surface.wait_events(1.0);
        surface.poll_platform_events();
        check(true, "request_wake / wait_events / poll_platform_events completed");

        // ---- 运行期显隐 / 置顶：原生态读回、幂等、不动几何、不抢焦点 ----
        check(aurora::platform().capabilities().runtime_visibility, "capabilities().runtime_visibility");
        check(aurora::platform().capabilities().runtime_always_on_top, "capabilities().runtime_always_on_top");
        aurora::WindowState observed = aurora::WindowState::Visible;
        surface.set_window_state_handler([&observed](aurora::WindowState s) { observed = s; });
        const NSRect frame_before = [window frame];
        const aurora::Point pos_before = surface.position();
        check(surface.is_visible(), "is_visible() true after creation");
        check(!surface.is_always_on_top(), "is_always_on_top() false at default style");
        check(surface.is_always_on_top() == ([window level] > NSNormalWindowLevel), "topmost readback == native level");

        surface.set_always_on_top(true);
        check(surface.is_always_on_top(), "set_always_on_top(true) readback");
        check([window level] == NSFloatingWindowLevel, "set_always_on_top(true) reaches NSWindow.level",
              aurora_verify::format_int(static_cast<long long>([window level])));
        check(NSEqualRects([window frame], frame_before) != NO, "topmost toggle keeps frame");
        check(surface.is_visible(), "topmost toggle keeps visibility");
        surface.set_always_on_top(true);  // 幂等：同值不再发 AppKit 调用
        check([window level] == NSFloatingWindowLevel, "set_always_on_top(true) idempotent");
        surface.set_always_on_top(false);
        check(!surface.is_always_on_top(), "set_always_on_top(false) readback");
        check([window level] == NSNormalWindowLevel, "set_always_on_top(false) back to normal level");

        surface.set_visible(false);
        check(!surface.is_visible(), "set_visible(false) readback");
        check([window isVisible] == 0, "set_visible(false) reaches NSWindow.isVisible");
        check(observed == aurora::WindowState::Hidden, "hidden window reports WindowState::Hidden",
              aurora_verify::format_int(static_cast<long long>(observed)));
        check(NSEqualRects([window frame], frame_before) != NO && nearly(surface.position().x, pos_before.x) &&
                  nearly(surface.position().y, pos_before.y),
              "hide keeps frame and position");
        // 隐藏后提层 / 聚焦都不得把窗口唤回（`Surface::raise` / `focus_window` 契约）。
        surface.raise();
        check(!surface.is_visible(), "raise() does not un-hide");
        surface.focus_window();
        check(!surface.is_visible(), "focus_window() does not un-hide");
        // 隐藏期间渲染与像素读回照常工作。
        auto begin_hidden = surface.begin_frame(420, 300);
        auto present_hidden = surface.present();
        check(begin_hidden.ok() && present_hidden.ok() && present_hidden.value(), "present while hidden");
        check(surface.data() != nullptr, "painter data available while hidden");

        surface.set_visible(true);
        check(surface.is_visible(), "set_visible(true) readback");
        check([window isVisible] != 0, "set_visible(true) reaches NSWindow.isVisible");
        check(observed != aurora::WindowState::Hidden, "shown window leaves WindowState::Hidden",
              aurora_verify::format_int(static_cast<long long>(observed)));
        check([window isKeyWindow] == 0, "show does not steal keyboard focus");
        check(NSEqualRects([window frame], frame_before) != NO, "show keeps frame");
        surface.set_visible(true);  // 幂等
        check(surface.is_visible(), "set_visible(true) idempotent");
        surface.set_window_state_handler({});

        // ---- Close 语义：close 置位、cancel_close 可撤销 ----
        surface.close();
        check(surface.should_close(), "close() sets should_close");
        surface.cancel_close();
        check(!surface.should_close(), "cancel_close() clears should_close");
        surface.close();
        check(surface.should_close(), "close() sets should_close again");

        AURORA_LOG_RAW("verify", "----------------------------------------\n");
        AURORA_LOG_RAW("verify", "summary: ", aurora_verify::format_int(g_checks - g_failures), "/",
                       aurora_verify::format_int(g_checks), " checks passed\n");
        return g_failures == 0 ? 0 : 1;
    }
}
