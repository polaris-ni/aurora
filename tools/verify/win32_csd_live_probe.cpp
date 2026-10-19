// Win32 客户端自绘装饰（CSD）—— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖：`Win32Host` 自绘装饰路径（`WindowStyleOptions::decoration` 五档策略的 Win32 语义）
// 端到端验收。与 X11 CSD 探针同定位，但判据全部落在**两条确定性通道**上，不依赖屏幕捕获：
//   ① `SendMessage(WM_NCHITTEST)` 直报热区：窗口过程同步执行，返回值即该点命中码，
//      与「屏幕上真实显示什么」解耦（远程/虚拟桌面下抓屏不可见的问题不影响本探针）；
//   ② DEBUG 构建的 `data()` 直读 Painter 帧缓冲：`present()` 内宿主已把装饰幂等覆绘进
//      帧缓冲，读回即「这一帧真的画上去的像素」。
//
// 逐段判据（与任务书验收条目对应）：
//   A) ClientSide（AdwaitaDark）：吞框后客户区 == 整窗（`WM_NCCALCSIZE` 生效）；安全区
//      只报栏高；标题栏带像素级绘出（`#303030`）且内容区零入侵；**命中逐点回归**——客户区
//      每个物理像素的 `WM_NCHITTEST` 返回值与共享层期望值（`classify_resize_zone` /
//      `title_bar_geometry` / `hit_button` 同一组合序）逐点相等；悬停分层；关闭钮**释放
//      触发**（同钮按下/松开 → 关窗成立）。
//   B) 热更与按压分层：运行期 `set_title_bar_style` 换预设 → 下一帧装饰带更新、安全区同步；
//      Windows 视觉语言下关闭钮 hover → press 底色分层（按压压过悬停）；按下后拖离再松手
//      **不触发**且无卡住的高亮。
//   C) Auto（SSD）：安全区恒零、装饰不入侵客户区、系统 caption 原样（`WM_NCCALCSIZE` 未吞框）。
//   D) Frameless：安全区恒零、不绘任何装饰、全客户区 `HTCLIENT`（无热区无标题带）。
//   E) Borderless：无标题栏（零 inset、零绘制）但保留八向热区；Alt 按住时任意处可移动
//      （`HTCAPTION`），松开即恢复。
//   F) 降级：`shadow_margin_dp` 显式 0 → 热区同步归零（边缘回 `HTCLIENT`），标题栏带与
//      安全区不受影响（投影撤除只影响热区带宽）。
//   G) 最大化触发与几何：关闭/最大/最小钮中的最大化钮释放触发 → 窗口态真的切换；最大化后
//      客户区 == 显示器工作区（经典 -8dp 边框问题的修正点）；安全区保留栏高；热区关闭、
//      标题带仍回报 `HTCAPTION`（系统承接拖拽还原）；再按一次还原。
//
// 为什么必须真机（无头 CI 证明不了的部分）：真实 HWND 与消息泵、`WM_NCCALCSIZE` 的吞框
// 几何、`WM_NCHITTEST` 的系统语义（`HTCAPTION` 白给的拖拽/双击/Aero Snap）、DWM composition
// 能力探测、以及最大化时 Windows 把窗口外扩到工作区之外的行为，都只在真实桌面上发生。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BACKEND_WIN32=ON
//         -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_csd
//   build-verify\aurora_verify_win32_csd.exe
// 运行：命令行直接跑，自动段无需人工干预（G 段会短暂最大化一个可见窗口再还原）。
// 退出码：0 全部通过；1 有判据失败；2 环境不可用（建窗失败 / 后端或 DEBUG 未开）。
//
// 已知边界（不代偿，如实登记）：
//   1. DWM composition **关闭**时的降级路径需要真的关掉桌面合成，属破坏性环境变更，
//      本探针只验 `shadow_margin_dp` 显式 0 这一路（同一 `frame_margin_dp` 折算出口）。
//   2. 自绘模式下 Aero Snap 布局面板与系统菜单（Alt+Space）不提供（`HTCAPTION` 自带的
//      拖拽/双击最大化保留）——见 BUILD_OPTIONS 平台差异注记。
//   3. `D3D11Surface` / `WgpuWin32Surface` 两条 GPU 上屏路径与 GDI 路径共用同一宿主三口
//      （`paint_decoration` / `record_client_decoration` / `content_inset`），本构建未开
//      GPU 后端，故只跑 GDI 一路；另两路的前转实现在同一提交内逐字对齐。
//   4. 像素判据依赖 `AURORA_ENABLE_DEBUG=ON`（`data()` 才暴露帧缓冲），构建条件已保证。

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_csd can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#ifndef AURORA_BACKEND_WIN32
#error "aurora_verify_win32_csd requires AURORA_BACKEND_WIN32"
#endif

#ifndef AURORA_ENABLE_DEBUG
#error "aurora_verify_win32_csd requires AURORA_ENABLE_DEBUG (data() exposes the frame buffer)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <dwmapi.h>  // DwmIsCompositionEnabled（WIN32_LEAN_AND_MEAN 不含 dwmapi；随 aurora PUBLIC 链接）
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// 库内部共享层头（src/）：期望值必须取共享层**同一份**函数（`frame_margin_dp` /
// `classify_resize_zone` / `hit_button`）——探针若自己写第二份判定式，验的只是两份实现
// 偶然一致，失去「独立基准」的意义。src 包含目录同 WASAPI/ALSA 探针先例。
#include "aurora/window/detail/csd_decoration.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/win32_surface.h"
#include "aurora/window/window_frame_style.h"
#include "verify_print.h"

namespace {

/// @brief 探针记账（通过/总计）：函数局部静态 + 访问器收敛，避免命名空间级可变全局量。
struct Tally {
    int checks = 0;  ///< 断言总数。
    int pass = 0;  ///< 通过数。
};

/// @brief 取进程私有记账单例。
/// @return 记账结构引用（调用方就地累加）。
[[nodiscard]] auto tally() -> Tally & {
    static Tally t;
    return t;
}

/// @brief 输出一行探针结论（经 `AURORA_LOG_RAW`：无前缀、不过级别过滤、恒写 stdout）。
/// @param text 一行文本（调用方不传行尾换行）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记录一条断言：打印 PASS/FAIL 行并累计失败数。
/// @param passed 断言是否成立。
/// @param label 断言描述（ASCII）。
auto check(bool passed, const std::string &label) -> void {
    ++tally().checks;
    if (passed) {
        ++tally().pass;
        emit(std::string("[PASS] ") + label);
    } else {
        emit(std::string("[FAIL] ") + label);
    }
}

/// @brief 打印一行测量记录（非断言，供人工核对与跨机比对）。
/// @param text 记录文本（ASCII）。
auto note(const std::string &text) -> void { emit(std::string("[NOTE] ") + text); }

/// @brief 独立读 DWM composition 能力（不经宿主缓存：宿主侧与探针侧各自调用同一 API，
/// 一致性本身即「单源」判据的一半）。
/// @return DWM composition 启用时 true。
[[nodiscard]] auto dwm_composition_enabled() -> bool {
    BOOL enabled = FALSE;
    return DwmIsCompositionEnabled(&enabled) == S_OK && enabled != FALSE;
}

/// @brief 抽干本线程消息队列（每步注入后调用，保证消息被同步处理完）。
auto pump() -> void {
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/// @brief 取表面原生窗口句柄。
/// @param surf 目标表面。
/// @return HWND（未建窗时为 nullptr）。
[[nodiscard]] auto hwnd_of(const aurora::Win32Surface &surf) -> HWND { return static_cast<HWND>(surf.hwnd()); }

/// @brief 读客户区矩形（物理像素；空矩形表示读取失败）。
/// @param hwnd 窗口句柄。
/// @return 客户区矩形。
[[nodiscard]] auto client_rect(HWND hwnd) -> RECT {
    RECT rc{};
    if (GetClientRect(hwnd, &rc) == 0) {
        return RECT{};
    }
    return rc;
}

/// @brief 读窗口外框矩形（屏幕坐标，物理像素）。
/// @param hwnd 窗口句柄。
/// @return 外框矩形。
[[nodiscard]] auto window_rect(HWND hwnd) -> RECT {
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) == 0) {
        return RECT{};
    }
    return rc;
}

/// @brief 读窗口所在显示器工作区（物理像素；最大化客户区的独立基准）。
/// @param hwnd 窗口句柄。
/// @return 工作区矩形。
[[nodiscard]] auto monitor_work_rect(HWND hwnd) -> RECT {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) == 0) {
        return RECT{};
    }
    return mi.rcWork;
}

/// @brief 客户区原点（屏幕坐标）：`WM_NCHITTEST` 的 lParam 是屏幕坐标，须先平移。
/// @param hwnd 窗口句柄。
/// @return 客户区左上角的屏幕坐标。
[[nodiscard]] auto client_origin(HWND hwnd) -> POINT {
    POINT pt{.x = 0, .y = 0};
    if (ClientToScreen(hwnd, &pt) == 0) {
        return POINT{.x = 0, .y = 0};
    }
    return pt;
}

/// @brief 矩形宽（物理像素）。
/// @param rc 矩形。
/// @return 宽（右减左）。
[[nodiscard]] auto rect_w(const RECT &rc) -> int { return rc.right - rc.left; }

/// @brief 矩形高（物理像素）。
/// @param rc 矩形。
/// @return 高（下减上）。
[[nodiscard]] auto rect_h(const RECT &rc) -> int { return rc.bottom - rc.top; }

/// @brief 一次 `WM_NCHITTEST` 采样：回报命中码与**往返后**的逻辑坐标。
struct HitSample {
    LRESULT code = HTCLIENT;  ///< `WM_NCHITTEST` 返回值。
    double lx = 0.0;  ///< 往返后的逻辑 x（物理 ÷ scale，与宿主 `to_logical` 同式）。
    double ly = 0.0;  ///< 往返后的逻辑 y。
};

/// @brief 向窗口同步投递 `WM_NCHITTEST` 并取回命中码。
///
/// 期望值按**往返后**的逻辑坐标计算（`ScreenToClient` 的逆运算），与宿主内部
/// `to_logical` 逐位同式，故浮点取整不会在边界点上制造假失配。
/// @param hwnd 窗口句柄。
/// @param origin 客户区原点（屏幕坐标）。
/// @param scale 当前缩放（逻辑 ↔ 物理）。
/// @param px 客户区物理 x。
/// @param py 客户区物理 y。
/// @return 命中码与往返逻辑坐标。
[[nodiscard]] auto sample_hit(HWND hwnd, POINT origin, float scale, int px, int py) -> HitSample {
    const LPARAM lp = MAKELPARAM(origin.x + px, origin.y + py);
    const LRESULT code = SendMessageA(hwnd, WM_NCHITTEST, 0, lp);
    return HitSample{.code = code,
                     .lx = static_cast<double>(px) / static_cast<double>(scale),
                     .ly = static_cast<double>(py) / static_cast<double>(scale)};
}

/// @brief 按**逻辑 dp** 坐标采样 `WM_NCHITTEST`（内部完成物理换算与客户区原点平移）。
///
/// 逐点网格走物理像素（覆盖全部取整形态），定点判据走逻辑坐标（与几何层同域，
/// 免得把逻辑盒位当物理像素用——最大化后盒位随客户区宽度重算，更须如此）。
/// @param surf 目标表面。
/// @param lx 逻辑 x（dp）。
/// @param ly 逻辑 y（dp）。
/// @return 命中码与往返逻辑坐标。
[[nodiscard]] auto sample_hit_logical(aurora::Win32Surface &surf, double lx, double ly) -> HitSample {
    const HWND hwnd = hwnd_of(surf);
    const float scale = surf.scale_factor();
    const POINT origin = client_origin(hwnd);
    const int px = static_cast<int>(std::lround(lx * scale));
    const int py = static_cast<int>(std::lround(ly * scale));
    return sample_hit(hwnd, origin, scale, px, py);
}

/// @brief 命中码 → 可读名（诊断输出用；与实现侧 `resize_zone_to_hit_test` 同表）。
/// @param code `WM_NCHITTEST` 返回值。
/// @return 可读名称；未知码回十六进制串。
[[nodiscard]] auto hit_code_name(LRESULT code) -> std::string {
    switch (code) {
        case HTCLIENT:
            return "HTCLIENT";
        case HTCAPTION:
            return "HTCAPTION";
        case HTLEFT:
            return "HTLEFT";
        case HTRIGHT:
            return "HTRIGHT";
        case HTTOP:
            return "HTTOP";
        case HTBOTTOM:
            return "HTBOTTOM";
        case HTTOPLEFT:
            return "HTTOPLEFT";
        case HTTOPRIGHT:
            return "HTTOPRIGHT";
        case HTBOTTOMLEFT:
            return "HTBOTTOMLEFT";
        case HTBOTTOMRIGHT:
            return "HTBOTTOMRIGHT";
        default:
            break;
    }
    return "HT(" + std::to_string(code) + ")";
}

/// @brief 共享层热区分类 → Win32 命中码（实现侧 `resize_zone_to_hit_test` 的同口径期望表）。
/// @param zone 共享层热区分类。
/// @return 对应 `HT*` 命中码；`None` → `HTCLIENT`。
[[nodiscard]] auto zone_to_hit_test(aurora::csd::CsdResizeZone zone) -> LRESULT {
    switch (zone) {
        case aurora::csd::CsdResizeZone::Left:
            return HTLEFT;
        case aurora::csd::CsdResizeZone::Right:
            return HTRIGHT;
        case aurora::csd::CsdResizeZone::Top:
            return HTTOP;
        case aurora::csd::CsdResizeZone::Bottom:
            return HTBOTTOM;
        case aurora::csd::CsdResizeZone::TopLeft:
            return HTTOPLEFT;
        case aurora::csd::CsdResizeZone::TopRight:
            return HTTOPRIGHT;
        case aurora::csd::CsdResizeZone::BottomLeft:
            return HTBOTTOMLEFT;
        case aurora::csd::CsdResizeZone::BottomRight:
            return HTBOTTOMRIGHT;
        case aurora::csd::CsdResizeZone::None:
            break;
    }
    return HTCLIENT;
}

/// @brief Normal 态下的期望命中码：与 `Win32Host::Impl::handle_nchittest` 同一组合序
/// （内缘八向 → 标题栏带按钮 → 标题栏空白 → 其余客户区），全部取共享层函数。
///
/// `zones_active` / `band_active` 两个开关对应策略解析结果：ClientSide 两者皆真；
/// Borderless 只开热区；Frameless 全关。热区带宽走 `frame_margin_dp` 三路归零，
/// 故显式 0 或 DWM 不可用时同一函数自然给出「边缘回 HTCLIENT」的期望。
/// @param tb 标题栏样式（栏高与按钮几何）。
/// @param frame 窗框样式（投影厚度即热区带宽）。
/// @param capable DWM composition 能力（探针独立读入）。
/// @param zones_active 本策略是否激活八向热区。
/// @param band_active 本策略是否激活标题栏带。
/// @param width 客户区逻辑宽（dp）。
/// @param height 客户区逻辑高（dp）。
/// @param lx 逻辑 x。
/// @param ly 逻辑 y。
/// @return 期望的 `WM_NCHITTEST` 返回值。
[[nodiscard]] auto expected_hit_code(const aurora::TitleBarStyle &tb, const aurora::WindowFrameStyle &frame,
                                     bool capable, bool zones_active, bool band_active, float width, float height,
                                     double lx, double ly) -> LRESULT {
    if (zones_active) {
        const int margin = aurora::csd::frame_margin_dp(frame, capable);
        if (margin > 0 && width > 2.0F * static_cast<float>(margin) && height > 2.0F * static_cast<float>(margin)) {
            const aurora::csd::CsdResizeZone zone = aurora::csd::classify_resize_zone(
                lx - static_cast<double>(margin), ly - static_cast<double>(margin),
                static_cast<double>(width) - (2.0 * static_cast<double>(margin)),
                static_cast<double>(height) - (2.0 * static_cast<double>(margin)), margin);
            if (zone != aurora::csd::CsdResizeZone::None) {
                return zone_to_hit_test(zone);
            }
        }
    }
    if (band_active && aurora::csd::point_in_title_bar(ly, tb.height)) {
        const aurora::TitleBarGeometry g = aurora::title_bar_geometry(width, tb, false, true);
        if (aurora::csd::hit_button(g, lx, ly) != aurora::csd::AURORA_CSD_BUTTON_NONE) {
            return HTCLIENT;  // 按钮交互留给自绘层（客户区消息）
        }
        return HTCAPTION;  // 空白带：系统白给拖拽/双击最大化/Aero Snap
    }
    return HTCLIENT;
}

/// @brief 命中逐点回归：客户区每个物理像素采样 `WM_NCHITTEST`，与共享层期望值逐点比对。
/// @param surf 被测表面（Normal 态、未最大化）。
/// @param tb 标题栏样式。
/// @param frame 窗框样式。
/// @param capable DWM 能力（独立读入）。
/// @param zones_active 本策略是否激活八向热区。
/// @param band_active 本策略是否激活标题栏带。
/// @param label 段标签（ASCII，进断言描述）。
auto check_hit_grid(aurora::Win32Surface &surf, const aurora::TitleBarStyle &tb, const aurora::WindowFrameStyle &frame,
                    bool capable, bool zones_active, bool band_active, const std::string &label) -> void {
    const HWND hwnd = hwnd_of(surf);
    const RECT rc = client_rect(hwnd);
    const int pw = rect_w(rc);
    const int ph = rect_h(rc);
    const float scale = surf.scale_factor();
    const aurora::Size size = surf.size();
    const POINT origin = client_origin(hwnd);
    int mismatches = 0;
    int samples = 0;
    std::string first_mismatch;
    for (int py = 0; py < ph; ++py) {
        for (int px = 0; px < pw; ++px) {
            const HitSample s = sample_hit(hwnd, origin, scale, px, py);
            const LRESULT want =
                expected_hit_code(tb, frame, capable, zones_active, band_active, size.width, size.height, s.lx, s.ly);
            ++samples;
            if (s.code != want) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = label + ": first mismatch at logical (" + std::to_string(s.lx) + ", " +
                                     std::to_string(s.ly) + "): got " + hit_code_name(s.code) + " want " +
                                     hit_code_name(want);
                }
            }
        }
    }
    note(label + ": sampled " + std::to_string(samples) + " client pixels, mismatches=" + std::to_string(mismatches));
    if (!first_mismatch.empty()) {
        emit(std::string("[FAIL] ") + first_mismatch);
    }
    check(mismatches == 0, label + ": WM_NCHITTEST matches the shared-layer expectation at every sampled point");
}

/// @brief 读帧缓冲一像素（打包为 `0xRRGGBB`；DEBUG 构建 `data()` 才暴露缓冲）。
///
/// 缓冲字节序是 R,G,B,A（`Painter` 契约定死），故高位拼 R——注意别与 GDI 的 BGRA
/// 上屏序混同：`present()` 里的 swizzle 只写 DIB，不回头改帧缓冲。
/// @param surf 已 present 过的表面。
/// @param lx 逻辑 x。
/// @param ly 逻辑 y。
/// @return 像素 RGB（忽略 alpha）；缓冲未就绪时回 0。
[[nodiscard]] auto pixel_at(aurora::Win32Surface &surf, double lx, double ly) -> std::uint32_t {
    aurora::Painter &p = surf.painter();
    const std::uint8_t *d = p.data();
    if (d == nullptr) {
        return 0U;
    }
    const float scale = surf.scale_factor();
    const int px = std::clamp(static_cast<int>(std::lround(lx * scale)), 0, std::max(p.width() - 1, 0));
    const int py = std::clamp(static_cast<int>(std::lround(ly * scale)), 0, std::max(p.height() - 1, 0));
    const std::size_t byte =
        ((static_cast<std::size_t>(py) * static_cast<std::size_t>(p.width())) + static_cast<std::size_t>(px)) * 4U;
    // data() 是帧缓冲裸数组（DEBUG 构建暴露，长度 = w×h×4，Win32Surface 契约定死），按偏移就地豁免。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return (static_cast<std::uint32_t>(d[byte]) << 16U) | (static_cast<std::uint32_t>(d[byte + 1U]) << 8U) |
           static_cast<std::uint32_t>(d[byte + 2U]);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

/// @brief 绘一帧「应用内容」：整幅填白后 present——宿主在 present 内幂等覆绘装饰带，
/// 与生产时序（app 内容 → 装饰覆盖 → 上屏）逐拍一致。
/// @param surf 目标表面。
auto paint_content(aurora::Win32Surface &surf) -> void {
    (void)surf.begin_frame(static_cast<int>(surf.size().width), static_cast<int>(surf.size().height));
    aurora::Painter &p = surf.painter();
    const float scale = surf.scale_factor();
    const float lw = static_cast<float>(p.width()) / scale;
    const float lh = static_cast<float>(p.height()) / scale;
    p.fill_rect(
        aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F}, .size = aurora::Size{.width = lw, .height = lh}},
        aurora::Color{255, 255, 255, 255});
    (void)surf.present();
}

/// @brief 注入一条客户区鼠标消息（坐标逻辑 dp → 物理像素）并抽干队列。
/// @param surf 目标表面。
/// @param msg 消息（`WM_MOUSEMOVE` / `WM_LBUTTONDOWN` / `WM_LBUTTONUP`）。
/// @param wp wParam（按下带 `MK_LBUTTON`，其余 0）。
/// @param lx 逻辑 x。
/// @param ly 逻辑 y。
auto send_mouse(aurora::Win32Surface &surf, UINT msg, WPARAM wp, double lx, double ly) -> void {
    const HWND hwnd = hwnd_of(surf);
    const float scale = surf.scale_factor();
    const int px = static_cast<int>(std::lround(lx * scale));
    const int py = static_cast<int>(std::lround(ly * scale));
    SendMessageA(hwnd, msg, wp, MAKELPARAM(px, py));
    pump();
}

/// @brief 注入 `WM_SYSKEYDOWN` / `WM_SYSKEYUP`（修饰键跟踪入口；`VK_MENU` 只推进态不派发）。
/// @param surf 目标表面。
/// @param down true = 按下，false = 抬起。
/// @param vk 虚拟键码。
auto send_syskey(aurora::Win32Surface &surf, bool down, int vk) -> void {
    SendMessageA(hwnd_of(surf), down ? WM_SYSKEYDOWN : WM_SYSKEYUP, static_cast<WPARAM>(vk), 0);
    pump();
}

/// @brief 逻辑坐标 → 矩形中心点（按钮几何用）。
/// @param r 矩形（逻辑 dp）。
/// @return 中心点。
[[nodiscard]] auto center_of(const aurora::Rect &r) -> aurora::Point {
    return aurora::Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
}

/// @brief 像素值 → `#RRGGBB` 串（诊断输出用）。
/// @param rgb 打包像素值。
/// @return 十六进制串。
[[nodiscard]] auto rgb_hex(std::uint32_t rgb) -> std::string {
    static constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";
    std::string s("#");
    for (int shift = 16; shift >= 0; shift -= 8) {
        const auto byte = static_cast<unsigned>((rgb >> shift) & 0xFFU);
        s.push_back(HEX_DIGITS[(byte >> 4U) & 0xFU]);
        s.push_back(HEX_DIGITS[byte & 0xFU]);
    }
    return s;
}

// ---- 段 A：ClientSide（AdwaitaDark）几何 / 安全区 / 像素 / 命中回归 / 悬停 / 释放触发 ----

/// @brief 段 A 判据（见文件头）。返回 false 表示环境不可用（建窗失败）。
/// @param capable DWM 能力（独立读入）。
/// @return 环境可用时 true。
auto segment_client_side(bool capable) -> bool {
    constexpr int probe_w = 320;
    constexpr int probe_h = 200;
    constexpr auto band_adwaita_dark = 0x303030U;
    constexpr auto close_hover_adwaita = 0xE01B24U;
    constexpr auto content_white = 0xFFFFFFU;
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::ClientSide;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    aurora::Win32Surface surf(probe_w, probe_h, "aurora-csd-probe-client", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the ClientSide probe window could not be created (exit 2)");
        return false;
    }
    pump();

    // ① 吞框几何：WM_NCCALCSIZE 吞掉系统 caption/边框后，客户区 == 整窗（无外延 margin 带）。
    const RECT client = client_rect(hwnd);
    const RECT frame = window_rect(hwnd);
    const float scale = surf.scale_factor();
    check(rect_w(client) == rect_w(frame) && rect_h(client) == rect_h(frame),
          "client rect equals window rect (WM_NCCALCSIZE swallowed the non-client frame)");
    const aurora::Size logical = surf.size();
    check(std::fabs(logical.width - static_cast<float>(probe_w)) < 0.01F &&
              std::fabs(logical.height - static_cast<float>(probe_h)) < 0.01F,
          "host logical size equals the requested dp (320x200)");
    // 物理口径（回归防线）：吞框档客户区 == 窗口矩形，若建窗期做了 AdjustWindowRect 外扩，
    // 客户区会比请求多出不可见边框带宽（150% DPI 下每边 10px），布局视口与真实客户区错位
    // （底部/右侧纯背景带）；而 logical 经构造尾部覆写仍会回读到请求 dp，单看逻辑值发现不了
    // ——必须把物理客户区钉在「请求 dp × scale」上。
    const int want_w = static_cast<int>(std::lround(static_cast<float>(probe_w) * scale));
    const int want_h = static_cast<int>(std::lround(static_cast<float>(probe_h) * scale));
    check(rect_w(client) == want_w && rect_h(client) == want_h,
          "client physical size equals requested dp x scale (no NC inflation under the swallowed frame)");
    note("scale=" + std::to_string(scale) + " client=" + std::to_string(rect_w(client)) + "x" +
         std::to_string(rect_h(client)) + " logical=" + std::to_string(logical.width) + "x" +
         std::to_string(logical.height));

    // ② 安全区：只报标题栏带高度（与 X11 逐值一致的口径）。
    const aurora::EdgeInsets inset = surf.content_inset();
    check(std::fabs(inset.top - 36.0F) < 0.01F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
          "content_inset reports the title bar height only (top=36, rest 0)");

    // ③ 像素级装饰带：AdwaitaDark 底色真绘出，内容区零入侵。
    paint_content(surf);
    check(
        pixel_at(surf, 10.0, 10.0) == band_adwaita_dark,
        "title bar band painted with the AdwaitaDark background (#303030), got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    check(pixel_at(surf, 10.0, 38.0) == content_white, "content just below the band is not invaded by the decoration");
    check(pixel_at(surf, 10.0, 100.0) == content_white, "app content area is not invaded by the decoration");

    // ④ 命中逐点回归（八向热区 + 标题带 + 按钮 → HTCLIENT 全映射）。
    check_hit_grid(surf, style.title_bar, style.frame, capable, /*zones_active=*/true, /*band_active=*/true,
                   "client-side hit test");

    // ⑤ 悬停分层：关闭钮内切圆浮出特征红，未悬停钮保持栏底色。
    const aurora::TitleBarGeometry g = aurora::title_bar_geometry(logical.width, style.title_bar, false, true);
    const aurora::Point close_center = center_of(g.close);
    const aurora::Point min_center = center_of(g.minimize);
    note("adwaita buttons: close=" + std::to_string(g.close.origin.x) + "," + std::to_string(g.close.origin.y) +
         " min=" + std::to_string(g.minimize.origin.x) + "," + std::to_string(g.minimize.origin.y));
    send_mouse(surf, WM_MOUSEMOVE, 0, close_center.x, close_center.y);
    paint_content(surf);
    check(pixel_at(surf, close_center.x, close_center.y - 6.0F) == close_hover_adwaita,
          "hovered close button shows the Adwaita close_hover fill (#E01B24), got " +
              rgb_hex(pixel_at(surf, close_center.x, close_center.y - 6.0F)));
    check(pixel_at(surf, min_center.x, min_center.y - 6.0F) == band_adwaita_dark,
          "non-hovered button keeps the band background, got " +
              rgb_hex(pixel_at(surf, min_center.x, min_center.y - 6.0F)));

    // ⑥ 释放触发：同钮按下/松开 → 关闭动作成立（WM_CLOSE → should_close）。
    send_mouse(surf, WM_LBUTTONDOWN, MK_LBUTTON, close_center.x, close_center.y);
    paint_content(surf);
    send_mouse(surf, WM_LBUTTONUP, 0, close_center.x, close_center.y);
    pump();
    check(surf.should_close(), "release on the same close button triggers the window close");
    return true;
}

// ---- 段 B：热更三口 + Windows 语言按压分层 + 拖离取消 ----

/// @brief 段 B 判据（见文件头）。返回 false 表示环境不可用。
/// @return 环境可用时 true。
auto segment_hot_reload_and_press_layers() -> bool {
    constexpr auto band_windows_dark = 0x202020U;
    constexpr auto close_hover_windows = 0xC42B1CU;
    constexpr auto close_pressed_windows = 0x991D11U;
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::ClientSide;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-hotreload", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the hot-reload probe window could not be created (exit 2)");
        return false;
    }
    pump();
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == 0x303030U, "initial band is the AdwaitaDark background (#303030)");

    // 热更：换预设 → 下一帧装饰带更新，安全区同步（两预设栏高同为 36）。
    const aurora::TitleBarStyle windows = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::WindowsDark);
    surf.set_title_bar_style(windows);
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == band_windows_dark,
          "hot-reloaded band shows the WindowsDark background (#202020), got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    const aurora::EdgeInsets inset = surf.content_inset();
    check(std::fabs(inset.top - 36.0F) < 0.01F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
          "content_inset stays in sync after the hot reload (top=36, rest 0)");

    // Windows 视觉语言：关闭钮为整高矩形，hover/press 底色可像素区分（按压压过悬停）。
    const aurora::Size size = surf.size();
    const aurora::TitleBarGeometry g = aurora::title_bar_geometry(size.width, windows, false, true);
    const aurora::Point close_center = center_of(g.close);
    // 取盒内、字形（X 两条对角线）之外的采样点：几何层保证字形只占盒宽的中三分之一。
    const aurora::Point close_off_glyph{.x = g.close.origin.x + 4.0F,
                                        .y = g.close.origin.y + (g.close.size.height * 0.5F)};
    send_mouse(surf, WM_MOUSEMOVE, 0, close_center.x, close_center.y);
    paint_content(surf);
    check(pixel_at(surf, close_off_glyph.x, close_off_glyph.y) == close_hover_windows,
          "hovered close button shows close_hover (#C42B1C), got " +
              rgb_hex(pixel_at(surf, close_off_glyph.x, close_off_glyph.y)));
    send_mouse(surf, WM_LBUTTONDOWN, MK_LBUTTON, close_center.x, close_center.y);
    paint_content(surf);
    check(pixel_at(surf, close_off_glyph.x, close_off_glyph.y) == close_pressed_windows,
          "pressed close button overrides hover with close_pressed (#991D11), got " +
              rgb_hex(pixel_at(surf, close_off_glyph.x, close_off_glyph.y)));

    // 取消：按下后拖离再松手 → 不触发，且无卡住的高亮。
    send_mouse(surf, WM_MOUSEMOVE, MK_LBUTTON, 100.0, 100.0);
    send_mouse(surf, WM_LBUTTONUP, 0, 100.0, 100.0);
    pump();
    check(!surf.should_close(), "drag away before release cancels the close (no action on release)");
    paint_content(surf);
    check(pixel_at(surf, close_off_glyph.x, close_off_glyph.y) == band_windows_dark,
          "no stuck highlight after the cancelled press, got " +
              rgb_hex(pixel_at(surf, close_off_glyph.x, close_off_glyph.y)));
    return true;
}

// ---- 段 C：Auto（SSD）零入侵 ----

/// @brief 段 C 判据（见文件头）。返回 false 表示环境不可用。
/// @return 环境可用时 true。
auto segment_ssd_zero_invasion() -> bool {
    constexpr auto content_white = 0xFFFFFFU;
    aurora::WindowStyleOptions style;  // decoration 缺省 = Auto：保留系统非客户区
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-auto", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the auto(ssd) probe window could not be created (exit 2)");
        return false;
    }
    pump();
    const aurora::EdgeInsets inset = surf.content_inset();
    check(inset.top == 0.0F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
          "auto(ssd) content_inset stays zero");
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == content_white,
          "auto(ssd) decoration draws nothing into the client area, got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    // 系统非客户区原样：窗框顶带必须回报系统非客户码（吞框后会变成自绘语义的
    // HTCLIENT/HTCAPTION），且顶带竖向扫描必须能找到系统 caption（HTCAPTION）。
    const RECT frame = window_rect(hwnd);
    const int caption_px = frame.left + (rect_w(frame) / 2);
    bool top_is_nonclient = false;
    bool saw_caption = false;
    for (int dy = 2; dy <= 40; ++dy) {
        const LRESULT code = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(caption_px, frame.top + dy));
        if (dy == 2 && code != HTCLIENT) {
            top_is_nonclient = true;
        }
        if (code == HTCAPTION) {
            saw_caption = true;
        }
    }
    check(top_is_nonclient, "auto(ssd) window frame top is a system non-client zone (frame not swallowed)");
    check(saw_caption, "auto(ssd) system caption is intact (HTCAPTION found in the top band)");
    const HitSample inside = sample_hit_logical(surf, 10.0, 10.0);
    check(inside.code == HTCLIENT, "auto(ssd) client interior reports HTCLIENT, got " + hit_code_name(inside.code));
    return true;
}

// ---- 段 D：Frameless 完全无装饰 ----

/// @brief 段 D 判据（见文件头）。返回 false 表示环境不可用。
/// @return 环境可用时 true。
auto segment_frameless_no_decoration() -> bool {
    constexpr auto content_white = 0xFFFFFFU;
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::Frameless;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-frameless", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the frameless probe window could not be created (exit 2)");
        return false;
    }
    pump();
    const aurora::EdgeInsets inset = surf.content_inset();
    check(inset.top == 0.0F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
          "frameless content_inset stays zero");
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == content_white,
          "frameless decoration draws nothing, got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    check_hit_grid(surf, style.title_bar, style.frame, dwm_composition_enabled(), /*zones_active=*/false,
                   /*band_active=*/false, "frameless hit test");
    return true;
}

// ---- 段 E：Borderless 热区 + 修饰键移动兜底 ----

/// @brief 段 E 判据（见文件头）。返回 false 表示环境不可用。
/// @param capable DWM 能力（独立读入）。
/// @return 环境可用时 true。
auto segment_borderless_zones(bool capable) -> bool {
    constexpr auto content_white = 0xFFFFFFU;
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::Borderless;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-borderless", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the borderless probe window could not be created (exit 2)");
        return false;
    }
    pump();
    const aurora::EdgeInsets inset = surf.content_inset();
    check(inset.top == 0.0F && inset.left == 0.0F && inset.right == 0.0F && inset.bottom == 0.0F,
          "borderless content_inset stays zero (no title bar to reserve)");
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == content_white,
          "borderless decoration draws nothing (no title bar), got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    check_hit_grid(surf, style.title_bar, style.frame, capable, /*zones_active=*/true, /*band_active=*/false,
                   "borderless hit test");

    // 修饰键移动兜底：Alt 按住时任意处可移动（HTCAPTION），松开即恢复客户区语义。
    const HitSample plain = sample_hit_logical(surf, 160.0, 100.0);
    check(plain.code == HTCLIENT,
          "borderless interior reports HTCLIENT without modifiers, got " + hit_code_name(plain.code));
    send_syskey(surf, true, VK_MENU);
    const HitSample with_alt = sample_hit_logical(surf, 160.0, 100.0);
    check(with_alt.code == HTCAPTION,
          "borderless interior reports HTCAPTION while Alt is held, got " + hit_code_name(with_alt.code));
    send_syskey(surf, false, VK_MENU);
    const HitSample after_release = sample_hit_logical(surf, 160.0, 100.0);
    check(after_release.code == HTCLIENT,
          "borderless interior returns to HTCLIENT after Alt is released, got " + hit_code_name(after_release.code));
    return true;
}

// ---- 段 F：降级归零（shadow_margin_dp 显式 0）----

/// @brief 段 F 判据（见文件头）。返回 false 表示环境不可用。
/// @param capable DWM 能力（独立读入）。
/// @return 环境可用时 true。
auto segment_degraded_margin_zero(bool capable) -> bool {
    constexpr auto band_adwaita_dark = 0x303030U;
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::ClientSide;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    style.frame.shadow_margin_dp = 0;  // 显式 0：与 DWM 不可用同一折算出口
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-degraded", style, aurora::WindowVisibility::Hidden);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the degraded probe window could not be created (exit 2)");
        return false;
    }
    pump();
    const aurora::EdgeInsets inset = surf.content_inset();
    check(std::fabs(inset.top - 36.0F) < 0.01F, "degraded margin keeps the title bar inset (top=36)");
    paint_content(surf);
    check(pixel_at(surf, 10.0, 10.0) == band_adwaita_dark,
          "degraded margin still paints the title bar band (#303030), got " + rgb_hex(pixel_at(surf, 10.0, 10.0)));
    // 热区归零：边缘/角落从 HT* 回落到标题带或客户区语义（margin=0 时八向判定恒 None，
    // 顶带 36dp 内仍归标题栏管——这与网格期望值同源，见下方全量回归）。
    const HitSample left_edge = sample_hit_logical(surf, 2.0, 100.0);
    const HitSample bottom_edge = sample_hit_logical(surf, 160.0, 198.0);
    const HitSample top_left = sample_hit_logical(surf, 2.0, 2.0);
    const HitSample top_right = sample_hit_logical(surf, 318.0, 2.0);
    const HitSample band_blank = sample_hit_logical(surf, 160.0, 10.0);
    const HitSample close_button = sample_hit_logical(surf, 299.0, 18.0);
    check(left_edge.code == HTCLIENT,
          "degraded left edge reports HTCLIENT (hot zone zeroed), got " + hit_code_name(left_edge.code));
    check(bottom_edge.code == HTCLIENT,
          "degraded bottom edge reports HTCLIENT (hot zone zeroed), got " + hit_code_name(bottom_edge.code));
    check(top_left.code == HTCAPTION,
          "degraded top-left falls back to the title band (HTCAPTION), got " + hit_code_name(top_left.code));
    check(top_right.code == HTCAPTION,
          "degraded top-right falls back to the title band (HTCAPTION), got " + hit_code_name(top_right.code));
    check(band_blank.code == HTCAPTION,
          "degraded title band blank still reports HTCAPTION, got " + hit_code_name(band_blank.code));
    check(close_button.code == HTCLIENT,
          "degraded close button still reports HTCLIENT, got " + hit_code_name(close_button.code));
    // 全量回归：期望函数在 margin=0 下自然给出「全 HTCLIENT/带内语义」的期望值。
    check_hit_grid(surf, style.title_bar, style.frame, capable, /*zones_active=*/true, /*band_active=*/true,
                   "degraded hit test");
    return true;
}

// ---- 段 G：最大化释放触发 + 最大化几何 ----

/// @brief 段 G 判据（见文件头）。返回 false 表示环境不可用。
/// @return 环境可用时 true。
auto segment_maximize_trigger_and_geometry() -> bool {
    aurora::WindowStyleOptions style;
    style.decoration = aurora::DecorationPolicy::ClientSide;
    style.title_bar = aurora::TitleBarStyle::from_preset(aurora::CsdPreset::AdwaitaDark);
    // 本段需要窗口态真的切换（SC_MAXIMIZE → WM_SIZE → 模式上报），用可见窗口跑：
    // 与 win32_dpi 探针同口径（真机探针人工触发，短暂闪现在可接受范围内）。
    aurora::Win32Surface surf(320, 200, "aurora-csd-probe-maximize", style);
    const HWND hwnd = hwnd_of(surf);
    if (hwnd == nullptr) {
        emit("result: the maximize probe window could not be created (exit 2)");
        return false;
    }
    pump();
    int maximize_seen = 0;
    int restore_seen = 0;
    surf.set_window_mode_handler([&maximize_seen, &restore_seen](aurora::WindowMode mode) {
        if (mode == aurora::WindowMode::Maximized) {
            ++maximize_seen;
        } else if (mode == aurora::WindowMode::Normal) {
            ++restore_seen;
        }
    });
    const aurora::Size normal_size = surf.size();
    const aurora::EdgeInsets normal_inset = surf.content_inset();
    check(std::fabs(normal_inset.top - 36.0F) < 0.01F, "normal-mode content_inset reports the bar height (top=36)");

    // 最大化钮释放触发（Adwaita 布局：盒居中于标题带右端）。
    const aurora::TitleBarGeometry g = aurora::title_bar_geometry(normal_size.width, style.title_bar, false, true);
    const aurora::Point max_center = center_of(g.maximize);
    send_mouse(surf, WM_LBUTTONDOWN, MK_LBUTTON, max_center.x, max_center.y);
    send_mouse(surf, WM_LBUTTONUP, 0, max_center.x, max_center.y);
    pump();
    check(maximize_seen >= 1, "release on the maximize button switches the window to Maximized");
    if (maximize_seen < 1) {
        return true;  // 未切换：后续几何判据失去前提，不再制造连带失败
    }

    // 最大化几何：客户区 == 显示器工作区（经典 -8dp 边框问题的修正点）。
    const RECT client = client_rect(hwnd);
    const RECT work = monitor_work_rect(hwnd);
    check(rect_w(client) == rect_w(work) && rect_h(client) == rect_h(work),
          "maximized client rect equals the monitor work area (no off-screen title bar)");
    const aurora::EdgeInsets max_inset = surf.content_inset();
    check(std::fabs(max_inset.top - 36.0F) < 0.01F, "maximized content_inset keeps the bar height (top=36)");
    const HitSample left_edge = sample_hit_logical(surf, 2.0, 100.0);
    const HitSample band_blank = sample_hit_logical(surf, 160.0, 10.0);
    check(left_edge.code == HTCLIENT,
          "maximized left edge reports HTCLIENT (resize zones off), got " + hit_code_name(left_edge.code));
    check(band_blank.code == HTCAPTION,
          "maximized title band blank reports HTCAPTION (drag-restore), got " + hit_code_name(band_blank.code));

    // 再按一次最大化钮 → 还原（释放触发在两种态间往返都成立）。⚠️ 盒位随客户区宽度
    // 重算：最大化后按钮贴到更宽客户区的右缘，必须按新几何取中心，不能沿用 Normal 态的坐标。
    const aurora::Size max_size = surf.size();
    const aurora::TitleBarGeometry g_max = aurora::title_bar_geometry(max_size.width, style.title_bar, true, true);
    const aurora::Point max_center_max = center_of(g_max.maximize);
    send_mouse(surf, WM_LBUTTONDOWN, MK_LBUTTON, max_center_max.x, max_center_max.y);
    send_mouse(surf, WM_LBUTTONUP, 0, max_center_max.x, max_center_max.y);
    pump();
    check(restore_seen >= 1, "release on the maximize button restores the window to Normal");
    return true;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与其它探针同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    aurora::init_console();  // Windows 控制台切 UTF-8，避免表格错行
    emit("== Aurora Win32 CSD live probe ==");

#ifndef AURORA_BACKEND_WIN32
    emit("result: Win32 backend is off in this build (exit 2)");
    return 2;
#else
    const bool capable = dwm_composition_enabled();
    emit("DWM composition enabled (probe's independent read) = " + std::to_string(static_cast<int>(capable)));
    if (!capable) {
        note("DWM composition is off: resize hot zones degrade to zero (frame_margin_dp), criteria adapt");
    }

    if (!segment_client_side(capable)) {
        return 2;
    }
    if (!segment_hot_reload_and_press_layers()) {
        return 2;
    }
    if (!segment_ssd_zero_invasion()) {
        return 2;
    }
    if (!segment_frameless_no_decoration()) {
        return 2;
    }
    if (!segment_borderless_zones(capable)) {
        return 2;
    }
    if (!segment_degraded_margin_zero(capable)) {
        return 2;
    }
    if (!segment_maximize_trigger_and_geometry()) {
        return 2;
    }

    emit("checks=" + std::to_string(tally().checks) + " pass=" + std::to_string(tally().pass));
    if (tally().pass != tally().checks) {
        emit("result: one or more criteria failed (exit 1)");
        return 1;
    }
    emit("PASS: Win32 CSD acceptance passed (all criteria exercised)");
    return 0;
#endif
}
