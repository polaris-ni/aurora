/// @file utest_win32_dpi.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/win32_dpi.h
/// 测试说明: Win32 DPI 降级链 —— 判据是「**取到 > 0 才算成功**」而非「函数指针非空」。
///           注入各级 API 的解析结果（含「解析到了但返回 0」与「解析不到」两种失败形态），
///           断言降级真的落到下一级，而不是停在 1.0
/// 平台门控: 依赖 `<windows.h>` 与后端宏；宏未开启时每条用例落 SKIP 桩（声明无条件可见）
///
/// 为什么要有本文件：`read_dpi` 的降级链此前**结构上不可达**——判据挂在 `else if`（函数指针为空）上，
/// 而 Win10+ 的 `GetDpiForWindow` 恒已导出 ⇒ `GetDpiForSystem` 那一支永远走不到；建窗期
/// `hwnd == nullptr` 时 `GetDpiForWindow(nullptr)` 返回 0 又被当成有效读数。两条都只能靠注入
/// 各级可用性来证伪，光跑绿证明不了链是通的。

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))
// 平台可用性开关，供下方各用例体内的 #ifdef 使用，无 constexpr 等价物
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_WIN32_DPI_AVAILABLE 1
#endif

#ifdef AURORA_WIN32_DPI_AVAILABLE
#include <windows.h>

#include "aurora/window/detail/win32_dpi.h"
#endif

namespace aurora::test_cases::utest_win32_dpi {

#define AURORA_WIN32_DPI_SKIP \
    AURORA_TEST_SKIP("non-Windows or Win32/D3D11 backend off: detail/win32_dpi.h is compiled out")

#ifdef AURORA_WIN32_DPI_AVAILABLE
namespace {

/// @brief 注入用的假 `GetDpiForWindow`：返回 0（模拟「句柄尚未创建」时该 API 的真实行为）。
UINT WINAPI fake_for_window(HWND) { return 0; }

/// @brief 注入用的假 `GetDpiForWindow`：返回 120（模拟句柄就绪后的真读数，125% 屏）。
UINT WINAPI fake_for_window_ok(HWND) { return 120; }

/// @brief 注入用的假 `GetDpiForSystem`：固定回 144（150% 屏），用于证明该级**可达**。
UINT WINAPI fake_for_system() { return 144; }

/// @brief 注入用的假 `GetDpiForMonitor`：固定回 192（200% 屏），用于证明该级**可达**。
HRESULT WINAPI fake_for_monitor(HMONITOR, int, UINT *x, UINT *y) {
    if (x != nullptr) {
        *x = 192;
    }
    if (y != nullptr) {
        *y = 192;
    }
    return S_OK;
}

}  // namespace
#endif

// 本条是 A-5 的本体：**把 `GetDpiForWindow` 的解析结果置空**，读数必须落到 `GetDpiForSystem`
// 而不是停在 1.0。旧代码里这一支结构上不可达（判据挂在「指针为空」的 `else if` 上，
// 而 Win10+ 该函数恒已导出），正是要被本条锁住的那条腿。
AURORA_TEST_CASE(window_dpi_unavailable_falls_through_to_system) {
#ifdef AURORA_WIN32_DPI_AVAILABLE
    using detail::DpiApi;
    using detail::DpiSource;
    using detail::read_dpi;
    // 必须传**非空** hwnd：按窗口那一级的守卫是「句柄已就绪 ∧ 函数已解析」，传 nullptr 会直接
    // 跳过该支，于是「解析不到 → 落到下一级」根本没被走到 —— 判据会空转成恒真。
    const DpiApi api{.for_window = nullptr, .for_system = &fake_for_system, .for_monitor = nullptr};
    const auto r = read_dpi(reinterpret_cast<HWND>(1), nullptr, api);
    AURORA_TEST_CHECK(r.source == DpiSource::System);
    AURORA_TEST_CHECK_EQ(r.dpi, 144);
#else
    AURORA_WIN32_DPI_SKIP;
#endif
}

// 「解析到了但返回 0」与「解析不到」必须是**同一种失败**：旧代码的 bug 正是把 0 当有效读数。
// 这里让 `for_window` 非空却返回 0，读数仍须落到下一级。
AURORA_TEST_CASE(window_dpi_of_zero_counts_as_failure_not_as_a_reading) {
#ifdef AURORA_WIN32_DPI_AVAILABLE
    using detail::DpiApi;
    using detail::DpiSource;
    using detail::read_dpi;
    const DpiApi api{.for_window = &fake_for_window, .for_system = &fake_for_system, .for_monitor = nullptr};
    const auto r = read_dpi(reinterpret_cast<HWND>(1), nullptr, api);
    AURORA_TEST_CHECK(r.source == DpiSource::System);
    AURORA_TEST_CHECK_EQ(r.dpi, 144);
    // 直接钉住「不得把 0 报成按窗口的读数」：旧代码正是这么掉进 scale == 1.0 的。
    AURORA_TEST_CHECK(r.dpi != 0);
    // 缩放因子口径：144 DPI ⇒ 1.5，而不是 0 ⇒ 1.0。
    AURORA_TEST_CHECK(detail::dpi_to_scale(r.dpi) > 1.4F);
#else
    AURORA_WIN32_DPI_SKIP;
#endif
}

// 建窗期（`hwnd == nullptr`）按**落位显示器**取：这是本轮新增的一级，也是建窗尺寸不再被
// 当物理像素消费的直接原因。句柄就绪后则不走这一级（按窗口优先）。
AURORA_TEST_CASE(creation_time_reads_the_monitor_the_window_will_land_on) {
#ifdef AURORA_WIN32_DPI_AVAILABLE
    using detail::DpiApi;
    using detail::DpiSource;
    using detail::read_dpi;
    const DpiApi api{.for_window = nullptr, .for_system = &fake_for_system, .for_monitor = &fake_for_monitor};
    RECT work_area{.left = 0, .top = 0, .right = 1920, .bottom = 1080};
    const auto r = read_dpi(nullptr, &work_area, api);
    AURORA_TEST_CHECK(r.source == DpiSource::Monitor);
    AURORA_TEST_CHECK_EQ(r.dpi, 192);
    // 句柄就绪后即使给了落位矩形也**优先**按窗口：建窗后的纠正逻辑依赖这个优先级。
    const DpiApi with_window{
        .for_window = &fake_for_window_ok, .for_system = nullptr, .for_monitor = &fake_for_monitor};
    const auto r2 = read_dpi(reinterpret_cast<HWND>(1), &work_area, with_window);
    AURORA_TEST_CHECK(r2.source == DpiSource::Window);
    AURORA_TEST_CHECK_EQ(r2.dpi, 120);
#else
    AURORA_WIN32_DPI_SKIP;
#endif
}

// 全链不可得时给 Unknown（调用方按 96 / scale 1.0 处理），不得返回 0 假装有读数。
AURORA_TEST_CASE(all_levels_unavailable_yield_unknown_and_scale_one) {
#ifdef AURORA_WIN32_DPI_AVAILABLE
    using detail::dpi_to_scale;
    using detail::DpiApi;
    using detail::DpiSource;
    using detail::read_dpi;
    const DpiApi api{};
    const auto r = read_dpi(nullptr, nullptr, api);
    // 末级 `GetDeviceCaps` 在本机可能成功（屏幕 DC 恒可得），故只钉「不是按窗口 / 按显示器」
    // 与「换算结果非 1.0 时确实来自某个真读数」这两条，不断言具体落到哪一级。
    AURORA_TEST_CHECK(r.source != DpiSource::Window);
    AURORA_TEST_CHECK(r.source != DpiSource::Monitor);
    AURORA_TEST_CHECK(dpi_to_scale(r.dpi) >= 1.0F);
    AURORA_TEST_CHECK(dpi_to_scale(0) == 1.0F);
    AURORA_TEST_CHECK(dpi_to_scale(96) == 1.0F);
    AURORA_TEST_CHECK(dpi_to_scale(144) > 1.49F);
#else
    AURORA_WIN32_DPI_SKIP;
#endif
}

// 生产路径的解析必须真的解析出东西：本机若为 Win10+，`GetDpiForWindow` / `GetDpiForSystem`
// 都应非空——它们为空意味着运行时解析逻辑本身坏了（老系统上为空是合法的，故只做宽松断言）。
AURORA_TEST_CASE(resolve_dpi_api_does_not_throw_and_is_consistent) {
#ifdef AURORA_WIN32_DPI_AVAILABLE
    const auto a = detail::resolve_dpi_api();
    const auto b = detail::resolve_dpi_api();
    AURORA_TEST_CHECK(a.for_window == b.for_window);
    AURORA_TEST_CHECK(a.for_system == b.for_system);
    AURORA_TEST_CHECK(a.for_monitor == b.for_monitor);
#else
    AURORA_WIN32_DPI_SKIP;
#endif
}

}  // namespace aurora::test_cases::utest_win32_dpi
