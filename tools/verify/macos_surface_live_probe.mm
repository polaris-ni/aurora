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
//   ✅ `os_color_scheme()` 对 NSApp 外观切换的实时跟随（Dark / Aqua 双向）；
//   ✅ `capture_window` 的内容视图缓存截图：PNG 可解码、像素尺寸等于帧缓冲、含已绘像素。
//   ✅ 装饰档位映射：`Borderless` 保住原生边缘缩放——`isResizable` 为真、内容区铺满窗框（顶边
//      内缩 0）、三枚标准窗钮隐藏、经 AppKit 队列投递的右下角拖拽真的改了尺寸，且改后尺寸能从
//      `Surface::size()` 读回；`resizable=false` 与 `Frameless` 两型实测均无缩放能力。
//
//   ❌ 本探针不验收鼠标/键盘事件翻译、输入法、系统拖放、光标形状：前两者需真实人机
//      输入（见 macos_cursor_live_probe / macos_drag_live_probe），后两者各有专用探针。
//      装饰档位段投递的拖拽只驱动**原生窗框**的缩放，不经过控件派发链，故不构成事件翻译证据。
//   ❌ 不经 WindowServer 抓屏证明「屏幕上真的显示了」：AppKit 的 `displayIfNeeded` 已同步
//      提交到 WindowServer，但背景会话下 `screencapture` 不可用；`capture_window` 取的是视图
//      自身的缓存绘制结果，本探针证明的是提交前的帧缓冲内容与提交调用成功。
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

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
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

auto fmt_nsize(NSSize s) -> std::string {
    return fmt_float(static_cast<float>(s.width)) + "x" + fmt_float(static_cast<float>(s.height));
}

/// @brief 抽干本进程 AppKit 事件队列若干轮（与 OS 输入探针同一条队列腿）。
auto pump_queue(int rounds) -> void {
    for (int i = 0; i < rounds; ++i) {
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event != nil) {
            [NSApp sendEvent:event];
        }
    }
}

/// @brief 向窗口右下角投递一次原生边缘拖拽（按下 + steps 段拖动 + 抬起），用于驱动窗框缩放。
///
/// 全部事件**先入队、后泵送**：可缩放窗框的 live resize 会在 `sendEvent:` 内进入 AppKit 窗框
/// 视图的嵌套跟踪循环，逐条「投递—泵送」让主线程卡在第一次 `sendEvent:` 里等后续事件（实测挂死）。
/// @param window 目标窗口。
/// @param steps 拖拽段数。
/// @param step_pt 每段位移（AppKit point，作用于窗口自身坐标系）。
auto post_corner_drag(NSWindow *window, int steps, double step_pt) -> void {
    // 前提一：窗口在序且队列已泵空——不这样做时同一份投递一次改尺都不发生。
    [window makeKeyAndOrderFront:nil];
    pump_queue(60);
    // 前提二：起笔前先向窗框几何中心（本探针的窗口无控件内容，落点中性）补投一次「按下—抬起」作 priming。
    // 不加这层 priming 时本腿偶发转红（同一份二进制三连跑里两轮 `300x200 -> 300x200`），加上之后稳定改尺。
    // key 位不是这里的机制：后台图形会话里 `isKeyWindow` 恒为 0 而改尺照常发生，primed 到位的轮次同样读 0。
    // 与实测形态最不矛盾的解释是 AppKit「点击获焦」的一次性代价——非 key 窗口欠着的第一个 `mouseDown:`
    // 被用作获焦而不转给窗框视图（与 OS 输入探针的 priming 同族语义），于是拖拽起笔被吃掉；priming 把这笔
    // 代价提前吃掉，起笔才落到窗框上。该解释未取证，故判据只锚「priming 之后必须改尺」，不锚任何 key /
    // 焦点读数，也不做「按 key 位重试」（后台会话取不到 key 位，那种重试判据恒不成立）。轮数取实测下界：
    // 单轮即稳定（本机连跑六轮、格式化后复跑一轮同为 82/82）。
    constexpr int k_priming_rounds = 1;
    for (int round = 0; round < k_priming_rounds; ++round) {
        const NSRect self_frame = [window frame];
        const NSPoint center =
            NSMakePoint(NSMidX(self_frame) - self_frame.origin.x, NSMidY(self_frame) - self_frame.origin.y);
        const NSTimeInterval prime_base = [NSProcessInfo processInfo].systemUptime;
        const auto post_prime = [&](NSEventType type, NSInteger number) {
            NSEvent *event = [NSEvent mouseEventWithType:type
                                                location:center
                                           modifierFlags:0
                                               timestamp:prime_base + (0.01 * static_cast<double>(number))
                                            windowNumber:[window windowNumber]
                                                 context:nil
                                             eventNumber:number
                                              clickCount:1
                                                pressure:1.0F];
            if (event != nil) {
                [NSApp postEvent:event atStart:NO];
            }
        };
        post_prime(NSEventTypeLeftMouseDown, 1);
        post_prime(NSEventTypeLeftMouseUp, 2);
        pump_queue(60);
    }
    const NSRect frame = [window frame];
    // 起笔取屏幕坐标的窗框右下角（与本库 `position()` 的左上原点口径无关，这里是 AppKit 的
    // 左下原点屏幕系），再换算进窗口自身坐标系——直接按约定手算会把拖拽点静默投偏。
    const NSPoint screen_corner = NSMakePoint(frame.origin.x + frame.size.width - 3.0, frame.origin.y + 2.0);
    const NSRect local = [window convertRectFromScreen:NSMakeRect(screen_corner.x, screen_corner.y, 1.0, 1.0)];
    const NSPoint start = NSMakePoint(NSMidX(local), NSMidY(local));
    AURORA_LOG_RAW("verify", "drag geometry: frame=", fmt_nsize(frame.size), "@", fmt_float(frame.origin.x), ",",
                   fmt_float(frame.origin.y), " screen_corner=(", fmt_float(screen_corner.x), ",",
                   fmt_float(screen_corner.y), ") local=(", fmt_float(start.x), ",", fmt_float(start.y),
                   ") visible=", [window isVisible] != 0 ? "1" : "0", " key=", [window isKeyWindow] != 0 ? "1" : "0",
                   " primed=", k_priming_rounds, " mouse_loc=", [NSEvent mouseLocation].x, ",",
                   [NSEvent mouseLocation].y, "\n");
    // 时间戳基准取系统开机秒（AppKit 事件时间戳的同源时基）而非 `[NSApp currentEvent]`：队列此刻
    // 可能为空，currentEvent 为 nil 会把整串拖拽压到 0.0 附近，AppKit 的窗框跟踪逻辑按时间戳判序，
    // 实测那种形态下一个事件都不生效。
    const NSTimeInterval base = [NSProcessInfo processInfo].systemUptime;

    const auto post = [&](NSEventType type, NSPoint at, NSInteger number, NSTimeInterval when, float pressure) {
        NSEvent *event = [NSEvent mouseEventWithType:type
                                            location:at
                                       modifierFlags:0
                                           timestamp:when
                                        windowNumber:[window windowNumber]
                                             context:nil
                                         eventNumber:number
                                          clickCount:1
                                            pressure:pressure];
        if (event != nil) {
            [NSApp postEvent:event atStart:NO];
        }
    };
    post(NSEventTypeLeftMouseDown, start, 1, base + 0.01, 1.0F);
    for (int i = 1; i <= steps; ++i) {
        const double moved = static_cast<double>(i) * step_pt;
        post(NSEventTypeLeftMouseDragged, NSMakePoint(start.x + moved, start.y - moved), static_cast<NSInteger>(i) + 10,
             base + (0.02 * static_cast<double>(i)), 1.0F);
    }
    const double total = static_cast<double>(steps) * step_pt;
    post(NSEventTypeLeftMouseUp, NSMakePoint(start.x + total, start.y - total), 99,
         base + (0.02 * static_cast<double>(steps + 1)), 0.0F);
    pump_queue(400);
}

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

        // ---- 截图：内容视图缓存位图 → PNG（DEBUG 门控，不需录屏授权）----
#ifdef AURORA_ENABLE_DEBUG
        NSString *capture_ns =
            [NSTemporaryDirectory() stringByAppendingPathComponent:@"aurora_macos_surface_capture.png"];
        const std::string capture_path = [capture_ns UTF8String] != nullptr ? std::string([capture_ns UTF8String]) : "";
        auto capture = surface.capture_window(capture_path);
        check(capture.ok() && capture.value(), "capture_window succeeds", capture ? "" : capture.error().message);
        NSFileManager *fm = [NSFileManager defaultManager];
        const bool capture_exists = [fm fileExistsAtPath:capture_ns] == YES;
        check(capture_exists, "captured PNG exists");
        unsigned long long capture_bytes = 0;
        if (capture_exists) {
            NSDictionary<NSFileAttributeKey, id> *attrs = [fm attributesOfItemAtPath:capture_ns error:nil];
            if (attrs != nil) {
                capture_bytes = [attrs fileSize];
            }
        }
        check(capture_bytes > 0, "captured PNG non-empty", aurora_verify::format_uint(capture_bytes));
        NSBitmapImageRep *decoded = nil;
        if (capture_exists) {
            NSData *png = [NSData dataWithContentsOfFile:capture_ns];
            decoded = (png != nil) ? [[NSBitmapImageRep alloc] initWithData:png] : nil;
        }
        check(decoded != nil, "captured PNG decodes");
        if (decoded != nil) {
            const NSInteger dw = [decoded pixelsWide];
            const NSInteger dh = [decoded pixelsHigh];
            const bool size_ok = dw == static_cast<NSInteger>(std::lround(static_cast<double>(fb1.width))) &&
                                 dh == static_cast<NSInteger>(std::lround(static_cast<double>(fb1.height)));
            check(size_ok, "captured PNG size == framebuffer",
                  aurora_verify::format_int(static_cast<long long>(dw)) + "x" +
                      aurora_verify::format_int(static_cast<long long>(dh)) + " vs " + fmt_size(fb1));
            // 红色矩形是逻辑 16×16、位于视图左上角；缓存位图的 y 轴朝向由 AppKit 决定，故在
            // 四角与中心各取一点，任一点命中即证明缓存的是真实绘制结果而非空白。
            // 判据取「红主导」而非绝对 rgb：缓存位图带显示器的广色域标签，转回 deviceRGB 后纯红
            // 会落在 (240,74,45) 一类的偏移值上，色域适配不该让真实的绘制结果判负。
            // 取样点用 std::array 承载而非 C 数组：C 数组的非常量下标即门禁里的 unchecked
            // container access 命中面，range-for + pair 读起来也更贴「点集」这一语义。
            const std::array<std::pair<NSInteger, NSInteger>, 5> samples{
                {{8, 8}, {8, dh - 9}, {dw - 9, 8}, {dw - 9, dh - 9}, {dw / 2, dh / 2}}};
            bool red_seen = false;
            for (const auto &[sample_x, sample_y] : samples) {
                if (sample_x < 0 || sample_y < 0 || sample_x >= dw || sample_y >= dh) {
                    continue;
                }
                NSColor *c = [[decoded colorAtX:sample_x
                                              y:sample_y] colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];
                if (c != nil && [c redComponent] > 0.7F && [c redComponent] - [c greenComponent] > 0.4F &&
                    [c redComponent] - [c blueComponent] > 0.4F) {
                    red_seen = true;
                    break;
                }
            }
            check(red_seen, "captured PNG contains the drawn red rect");
        }
        if (capture_exists) {
            (void)[fm removeItemAtPath:capture_ns error:nil];
        }
#else
        AURORA_LOG_RAW("verify", "SKIP  capture_window (AURORA_ENABLE_DEBUG off)\n");
#endif

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

        // ---- 装饰档位映射：Borderless 保留原生边缘缩放、Frameless 完全无装饰 ----
        // 判据分两路：`isResizable` 证「掩码映射对了」，真投递的拖拽证「原生窗框视图确实把这次
        // 拖拽变成了改尺」，改尺后还要能从 `Surface::size()` 读回（框架的尺寸链跟得上）。
        aurora::WindowOptions bl_opts;
        bl_opts.size = aurora::Size{.width = 300.0F, .height = 200.0F};
        bl_opts.title = "Aurora borderless resize probe";
        bl_opts.style.decoration = aurora::DecorationPolicy::Borderless;
        auto bl_result = aurora::create_window(aurora::MacOSOptions{bl_opts});
        check(bl_result.ok(), "borderless window created", bl_result.ok() ? "" : bl_result.error().message);
        if (bl_result.ok()) {
            auto bl_win = std::move(bl_result.value());
            aurora::Surface &bl = bl_win->surface();
            auto *bl_window = (__bridge NSWindow *)bl.native_handle();
            check(bl_window != nil, "borderless window bridges to NSWindow");
            check([bl_window isResizable] != 0, "borderless keeps the native resize capability");
            const NSRect bl_frame = [bl_window frame];
            const NSRect bl_content = [bl_window contentRectForFrameRect:bl_frame];
            check(nearly(static_cast<float>(bl_content.size.width), static_cast<float>(bl_frame.size.width)) &&
                      nearly(static_cast<float>(bl_content.size.height), static_cast<float>(bl_frame.size.height)),
                  "borderless content rect fills the frame",
                  fmt_nsize(bl_frame.size) + " vs " + fmt_nsize(bl_content.size));
            for (const auto &slot : {std::pair<NSWindowButton, const char *>{NSWindowCloseButton, "close"},
                                     std::pair<NSWindowButton, const char *>{NSWindowMiniaturizeButton, "miniaturize"},
                                     std::pair<NSWindowButton, const char *>{NSWindowZoomButton, "zoom"}}) {
                NSButton *button = [bl_window standardWindowButton:slot.first];
                check(button == nil || [button isHidden] != 0,
                      std::string{"borderless hides the "} + slot.second + " button");
            }

            const NSSize size_before = [bl_window frame].size;
            const aurora::Size logical_before = bl.size();
            post_corner_drag(bl_window, 12, 8.0);
            bl.poll_platform_events();
            const NSSize size_after = [bl_window frame].size;
            check(size_after.width != size_before.width || size_after.height != size_before.height,
                  "borderless corner drag really resizes the window",
                  fmt_nsize(size_before) + " -> " + fmt_nsize(size_after));
            const aurora::Size logical_after = bl.size();
            check(logical_after.width != logical_before.width || logical_after.height != logical_before.height,
                  "Surface::size() follows the native resize",
                  fmt_size(logical_before) + " -> " + fmt_size(logical_after));

            // 固定尺寸的 Borderless 不得因本档获得缩放能力：缩放来源只有 `style.resizable`。
            aurora::WindowOptions bl_fixed_opts = bl_opts;
            bl_fixed_opts.style.resizable = false;
            bl_fixed_opts.title = "Aurora borderless fixed probe";
            auto bl_fixed = aurora::create_window(aurora::MacOSOptions{bl_fixed_opts});
            check(bl_fixed.ok(), "borderless fixed-size window created", bl_fixed.ok() ? "" : bl_fixed.error().message);
            if (bl_fixed.ok()) {
                auto fixed_win = std::move(bl_fixed.value());
                auto *fixed_window = (__bridge NSWindow *)fixed_win->surface().native_handle();
                check(fixed_window != nil && [fixed_window isResizable] == 0,
                      "borderless with resizable=false stays non-resizable");
            }
        }

        aurora::WindowOptions fl_opts;
        fl_opts.size = aurora::Size{.width = 300.0F, .height = 200.0F};
        fl_opts.title = "Aurora frameless probe";
        fl_opts.style.decoration = aurora::DecorationPolicy::Frameless;
        auto fl_result = aurora::create_window(aurora::MacOSOptions{fl_opts});
        check(fl_result.ok(), "frameless window created", fl_result.ok() ? "" : fl_result.error().message);
        if (fl_result.ok()) {
            auto fl_win = std::move(fl_result.value());
            auto *fl_window = (__bridge NSWindow *)fl_win->surface().native_handle();
            check(fl_window != nil, "frameless window bridges to NSWindow");
            // 契约口径：`Frameless` = 完全无装饰、无缩放边框，故不得因本提案放宽。
            check([fl_window isResizable] == 0, "frameless keeps no resize capability");
            const NSSize before = [fl_window frame].size;
            post_corner_drag(fl_window, 12, 8.0);
            const NSSize after = [fl_window frame].size;
            check(after.width == before.width && after.height == before.height, "frameless corner drag changes nothing",
                  fmt_nsize(before) + " -> " + fmt_nsize(after));
        }

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
