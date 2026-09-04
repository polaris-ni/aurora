#include <algorithm>
#include <cmath>

#include "aurora/app/display.h"
#include "aurora/core/platform.h"
#include "aurora/window/window.h"

#ifdef AURORA_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace aurora::app {

namespace {
auto to_display(HMONITOR hmon, HDC hdc) -> Display {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(MONITORINFOEXW);
    GetMonitorInfoW(hmon, &mi);

    Display d;
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    d.id = static_cast<int>(reinterpret_cast<std::intptr_t>(hmon));  // HMONITOR 句柄作稳定 id
    // 名称（UTF-16 → UTF-8）。
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string s(static_cast<std::size_t>(n) - 1U, '\0');
            WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, s.data(), n, nullptr, nullptr);
            d.name = std::move(s);
        }
    }
    const RECT &r = mi.rcMonitor;
    d.bounds = Rect{
        .origin = Point{.x = static_cast<float>(r.left), .y = static_cast<float>(r.top)},
        .size = Size{.width = static_cast<float>(r.right - r.left), .height = static_cast<float>(r.bottom - r.top)}};
    const RECT &w = mi.rcWork;
    d.work_area = Rect{
        .origin = Point{.x = static_cast<float>(w.left), .y = static_cast<float>(w.top)},
        .size = Size{.width = static_cast<float>(w.right - w.left), .height = static_cast<float>(w.bottom - w.top)}};
    const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    d.scale_factor = dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
    d.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return d;
}
}  // namespace

auto list_displays() -> std::vector<Display> {
    std::vector<Display> out;
    // EnumDisplayMonitors 回调中 hdcMonitor 为该显示器 DC，可读取其 DPI。
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h_mon, HDC hdc, LPRECT, LPARAM lp) -> BOOL {
            // NOLINTNEXTLINE(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
            auto *displays = reinterpret_cast<std::vector<Display> *>(lp);
            displays->push_back(to_display(h_mon, hdc));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));  // NOLINT(*-pro-type-reinterpret-cast)
    if (out.empty()) {
        out.push_back(primary_display());
    }
    return out;
}

auto primary_display() -> Display {
    Display d;
    d.id = -1;
    d.name = "default";
    d.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 1920.0F, .height = 1080.0F}};
    d.work_area = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 1920.0F, .height = 1080.0F}};
    d.scale_factor = 1.0F;
    d.is_primary = true;
    return d;
}

auto move_window_to_display(Window &win, int display_id) -> void {
    const std::vector<Display> ds = list_displays();
    const Display *target = nullptr;
    for (const auto &d : ds) {
        if (d.id == display_id) {
            target = &d;
            break;
        }
    }
    Display fallback;
    if (target == nullptr) {
        fallback = primary_display();
        target = &fallback;
    }
    auto *const hwnd = static_cast<HWND>(win.surface().native_handle());
    if (hwnd == nullptr) {
        return;  // 无头/未知后端：no-op
    }
    // 以目标显示器中心（物理像素）定位其 HMONITOR。
    const float cx = target->bounds.origin.x + (target->bounds.size.width * 0.5F);
    const float cy = target->bounds.origin.y + (target->bounds.size.height * 0.5F);
    const POINT phys{.x = static_cast<LONG>(std::lround(cx)), .y = static_cast<LONG>(std::lround(cy))};
    const HMONITOR hmon = MonitorFromPoint(phys, MONITOR_DEFAULTTONEAREST);
    if (hmon == nullptr) {
        return;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hmon, &mi)) {
        return;
    }
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    const RECT &wa = mi.rcWork;  // 目标显示器工作区（物理像素）
    const int nx = wa.left + static_cast<int>(std::max(0L, (wa.right - wa.left - w) / 2L));
    const int ny = wa.top + static_cast<int>(std::max(0L, (wa.bottom - wa.top - h) / 2L));
    SetWindowPos(hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

auto display_containing(Point p) -> Display {
    for (const auto &d : list_displays()) {
        const Rect &b = d.bounds;
        if (p.x >= b.origin.x && p.x < b.right() && p.y >= b.origin.y && p.y < b.bottom()) {
            return d;
        }
    }
    return primary_display();
}

}  // namespace aurora::app
   // namespace aurora::app

#else

namespace aurora {
namespace app {

auto list_displays() -> std::vector<Display> { return {primary_display()}; }

auto primary_display() -> Display {
    Display d;
    d.id = -1;
    d.name = "default";
    d.bounds = Rect{Point{0.0F, 0.0F}, Size{1920.0F, 1080.0F}};
    d.work_area = Rect{Point{0.0F, 0.0F}, Size{1920.0F, 1080.0F}};
    d.scale_factor = 1.0F;
    d.is_primary = true;
    return d;
}

auto move_window_to_display(Window &, int) -> void {}

auto display_containing(Point) -> Display { return primary_display(); }

}  // namespace app
}  // namespace aurora

#endif
