#pragma once

/// @brief Win32/GDI 原生窗口后端：零三方依赖（仅 user32/gdi32），仅 `AURORA_PLATFORM_WINDOWS` 下编译。
/// 窗口宿主（创建/消息泵/事件翻译/DPI/同步重渲染）抽取到共享 `win32_host.h`，
/// 本类仅负责 GDI 上屏（SetDIBitsToDevice 把软件 Painter 帧缓冲拷到窗口）。
/// 重构后须保证行为与抽取前逐位等价：WM_SIZE/WM_PAINT 触发同步重渲染、白闪修复刷不变。
#ifdef AURORA_BACKEND_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
/// @brief Windows SDK 精简包含宏：阻断 Winsock/DDE 等旧子系统头文件，缩减 windows.h 包含面。
/// @param naming 非真实形参：门禁解析把本宏行尾行内豁免标记中的括号误当参数表；宏本身无参数。
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/environment/media_query.h"
#include "aurora/render/painter.h"
#include "aurora/window/surface.h"
#include "aurora/window/win32_host.h"

namespace aurora {

/// @brief Win32/GDI 后端：软件 Painter 帧缓冲经常驻 BGRA DIB section + `BitBlt` 上屏。
///
/// 窗口宿主（`Win32Host`）负责创建/消息/事件/DPI/同步重渲染；本类只做 GDI blit，
/// 与 `D3D11Surface` 共用宿主但后端不同。上屏路径：RGBA 帧缓冲 CPU swizzle 到
/// BGRA（GDI 原生序）DIB section 后 `BitBlt`——非原生 RGBA 掩码会迫使 GDI 逐像素
/// 慢速转换（5760×3132px 实测 87ms），swizzle+BitBlt 仅 ~9ms（bench_win32_present ④）。
class Win32Surface final : public Surface {
  public:
    /// @brief 构造：经共享 `Win32Host` 创建窗口并接入消息泵；三参重载以默认窗口样式转发到五参重载。
    /// @param w 窗口初始宽度（逻辑像素）。
    /// @param h 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    Win32Surface(int w, int h, const std::string &title) : Win32Surface(w, h, title, WindowStyleOptions{}) {}
    /// @brief 完整构造：样式与可见性一并传给共享 `Win32Host`（后者实际创建窗口）。
    /// @param w 窗口初始宽度（逻辑像素）。
    /// @param h 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    /// @param style 窗口样式选项（标题栏/边框等）。
    /// @param visibility 初始可见性（Normal/Hidden 等）。
    Win32Surface(int w, int h, const std::string &title, const WindowStyleOptions &style,
                 WindowVisibility visibility = WindowVisibility::Normal)
        : win_(std::make_unique<Win32Host>(w, h, title, style, visibility)) {}
    /// @brief 析构：释放常驻 DIB section 与内存 DC（窗口本体由宿主析构销毁）。
    ~Win32Surface() override { release_dib(); }

    /// @brief 禁用拷贝构造：本类持有唯一 OS 窗口/DIB 资源，拷贝会产生双重所有者。
    Win32Surface(const Win32Surface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一 OS 资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(const Win32Surface &) -> Win32Surface & = delete;
    /// @brief 禁用移动构造：宿主回调持有本对象地址，移动会使消息路由失效。
    Win32Surface(Win32Surface &&) = delete;
    /// @brief 禁用移动赋值：同移动构造，对象地址必须稳定。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(Win32Surface &&) -> Win32Surface & = delete;

    /// @brief 事件处理器：Win32 消息翻译为 aurora `Event` 后上抛，由 Application 统一派发。
    /// @param h 事件回调，接收宿主翻译后的归一化事件。
    auto set_event_handler(const EventHandler &h) -> void override { win_->set_event_handler(h); }
    /// @brief 注册窗口可见性状态上报句柄（最小化/被遮挡/前台激活）。
    /// @param h 可见性状态回调，参数为计算后的窗口可见态。
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        win_->set_window_state_handler(std::move(h));
    }
    /// @brief 注册窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen）。
    /// @param h 几何态回调，参数为计算后的窗口模式。
    auto set_window_mode_handler(WindowModeHandler h) -> void override { win_->set_window_mode_handler(std::move(h)); }
    /// @brief 同步重渲染请求（由 Window 注入 present_root）：WM_SIZE/WM_PAINT 触发。
    /// @param h 重渲染请求回调，宿主在系统几何变化时同步调用。
    auto set_present_request(PresentRequest h) -> void override { win_->set_present_request(std::move(h)); }
    /// @brief 运行时更新窗口标题（转发给共享宿主）。
    /// @param title 新窗口标题（UTF-8）。
    auto set_title(const std::string &title) -> void override { win_->set_title(title); }

    /// @brief 运行时更新悬停光标形状：`SetCursor` + 系统预置光标 `LoadCursor(nullptr, IDC_*)`。
    /// 映射与实现抽到 `src/aurora/window/win32_cursor.h` 的 `detail::set_win32_cursor`，与
    /// `D3D11Surface` 共用同一份（两者共用 `Win32Host` 宿主模型，映射不应重复实现）。
    /// 系统预置光标由 OS 拥有，无需释放（无泄漏）；泛型宏随 `UNICODE` 解析 A/W 变体。
    /// @param shape 光标语义形状，经共享映射表转为系统预置光标。
    /// @note 未编译验证：须 Windows + `AURORA_BACKEND_WIN32=ON` 构建后复查（本仓库的无头
    /// Linux 构建不含 Win32 后端）。
    auto set_cursor(CursorShape shape) -> void override;

    /// @brief 控件发起窗口拖拽移动（Win32：伪装 NC 拖拽 HTCAPTION）。
    auto begin_window_move() -> void override {
        PostMessageW(static_cast<HWND>(win_->hwnd()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    /// @brief 控件发起窗口边缘缩放（Win32：伪装 NC 拖拽对应 HT 边缘码）。
    /// @param edge 拖拽的窗口边缘，按枚举值序映射到 HT 码；None 越界值忽略。
    auto begin_window_resize(WindowResizeEdge edge) -> void override {
        // 序对应 WindowResizeEdge 枚举值序：None/Top/Bottom/Left/Right/TopLeft/TopRight/BottomLeft/BottomRight。
        static constexpr std::array<int, 9> HT = {HTNOWHERE, HTTOP,      HTBOTTOM,     HTLEFT,       HTRIGHT,
                                                  HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT};
        const auto idx = static_cast<std::size_t>(edge);
        if (idx != 0 && idx < HT.size()) {
            PostMessageW(static_cast<HWND>(win_->hwnd()), WM_NCLBUTTONDOWN, static_cast<WPARAM>(HT.at(idx)), 0);
        }
    }

    /// @brief 增量上屏脏区（设备坐标）：非空时 present() 仅 blit 脏矩形并界，而非整窗——
    /// 大窗口下整窗 SetDIBitsToDevice 是拖选帧的绝对大头（5760×3132px 实测 ~130ms，
    /// 占帧成本 93%）。脏区一次性消费（present 后清空），未设置则全量 blit（首帧/尺寸变化/
    /// WM_PAINT 兜底路径行为不变）。
    /// @param device_rects 本帧脏矩形列表（设备坐标），空表示全量 blit。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override { present_dirty_ = device_rects; }

    /// @brief 类背景擦除刷（测试/自检用）：非空表示已消除最大化黑屏。
    [[nodiscard]] static auto background_brush() -> void * { return Win32Host::background_brush(); }
    /// @brief 原生窗口句柄（测试/自检用）：可向该句柄发送 WM_PAINT/WM_SIZE 验证黑屏修复。
    [[nodiscard]] auto hwnd() const -> void * { return win_->hwnd(); }
    /// @brief 表层统一原生句柄：与 `hwnd()` 同源，返回窗口 HWND（以 `void *` 承载）。
    [[nodiscard]] auto native_handle() const -> void * override { return win_->hwnd(); }
    /// @brief 本窗口的无障碍桥：转发共享宿主 `Win32Host` 持有的唯一实例，
    /// 使 GDI 与 GPU 两路上屏共用同一份 id→Widget* 映射，不产生分裂。
    /// @return 宿主持有的桥指针（宿主拥有生命周期，调用方不得释放）。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override {
        return win_->accessibility_provider();
    }

    /// @brief 注入语义树根（转发共享宿主；桥未构造时由宿主记下）。
    /// @param root 语义树根控件指针。
    auto set_accessibility_root(Widget *root) -> void override { win_->set_accessibility_root(root); }
    /// @brief 注入 IME 候选窗定位查询（转发共享宿主，GDI/GPU 两路同一桥）。
    /// @param provider 返回光标屏幕矩形（设备坐标）的回调，供候选窗贴附定位。
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override {
        win_->set_composition_caret_provider(std::move(provider));
    }
    /// @brief 宿主同步重渲染次数（测试/自检用）：验证 WM_SIZE/WM_PAINT 触发了同步重渲染。
    /// 与 `frame_count()` 是两回事——本计数只在系统几何变化触发的同步重渲分支自增，帧循环上屏不计。
    /// @return 宿主同步重渲染的累计次数。
    [[nodiscard]] auto present_count() const -> int { return win_->present_count(); }

    /// @brief 开始一帧：清空上帧残留脏区，按真实客户区物理尺寸对齐 Painter 缓冲并预铺浅色底色。
    /// @param width 窗口逻辑宽（dp；×scale 得物理宽，非正时回落客户区实测）。
    /// @param height 窗口逻辑高（dp）。
    /// @return 恒返回 true（缓冲分配失败路径不存在，尺寸非法时按 1px 兜底）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 取软件绘制器：Painter 持有本帧 RGBA 缓冲，供上层组件 CPU 绘制。
    /// @return 内部 Painter 的引用（生命周期与本对象一致）。
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }
    /// @brief 上屏当前帧：RGBA→BGRA swizzle 进常驻 DIB section 后 BitBlt；脏区非空时增量 blit。
    /// @return 恒返回 true（DIB 资源未就绪时静默跳过本次上屏，不报错）。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前帧像素（设备像素缓冲，RGBA）：供 `save_snapshot` 抓帧。
    /// DEBUG 下覆写返回 Painter 缓冲；Release（未开 `AURORA_ENABLE_DEBUG`）回落基类默认值 nullptr，
    /// 使 `save_snapshot` 在 Release 返回 disabled 错误（零截图代码）。
    /// @return 当前帧 RGBA 像素首指针；Release 构建返回 nullptr。
    [[nodiscard]] auto data() const -> const std::uint8_t * override {
#ifdef AURORA_ENABLE_DEBUG
        return painter_.data();
#else
        return nullptr;
#endif
    }
    /// @brief 帧缓冲物理像素尺寸：Win32 painter 按 DPI 物理分辨率分配，故返回 painter 缓冲像素尺寸，
    /// 而非逻辑 `size()`（缩放比≠1 时二者不同，避免 `save_snapshot` 写出 PNG 宽高与像素数据错位）。
    /// @return Painter 帧缓冲的物理像素尺寸。
    [[nodiscard]] auto framebuffer_size() const -> Size override {
        return Size{.width = static_cast<float>(painter_.width()), .height = static_cast<float>(painter_.height())};
    }
    /// @brief 真实窗口截图（含非客户区/标题栏/边框）：经 `PrintWindow` 抓取真实屏幕画面为 PNG。
    /// 覆写基类默认（unsupported）；DEBUG 下调用共享 `detail::capture_window_by_hwnd`，
    /// Release（未开 `AURORA_ENABLE_DEBUG`）回落 unsupported 错误（零截图代码）。
    /// @param path 输出 PNG 文件路径。
    /// @return 抓取并写盘成功返回 true；失败或 Release 下返回错误 Result。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;
    /// @brief 已呈现帧数：每次 `present()` 真正上屏自增（与其他后端同一口径，供 `surface_state` 暴露）。
    /// @return 迄今真正上屏的帧数。
    [[nodiscard]] auto frame_count() const -> int override { return presented_frames_; }
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    /// @return 底色 RGBA（245,245,247,255，与 macOS/通用默认一致）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }
    /// @brief 窗口当前逻辑尺寸（转发共享宿主）。
    /// @return 窗口尺寸（设备无关像素）。
    [[nodiscard]] auto size() const -> Size override { return win_->size(); }
    /// @brief 窗口当前 DPI 缩放因子（转发共享宿主）。
    /// @return 缩放因子，1.0 表示无缩放。
    [[nodiscard]] auto scale_factor() const -> float override { return win_->scale_factor(); }
    /// @brief 是否已收到关闭窗口请求（转发共享宿主；WM_CLOSE/WM_DESTROY 置位）。
    /// @return 收到关闭请求为 true，主循环据此退出。
    [[nodiscard]] auto should_close() const -> bool override { return win_->should_close(); }
    /// @brief 抽取并派发本线程消息队列中的窗口消息（转发共享宿主的 PeekMessage 泵）。
    auto poll_platform_events() -> void override { win_->poll_platform_events(); }
    /// @brief 阻塞等待消息或超时（转发共享宿主）。
    /// @param timeout_ms 最长等待毫秒数；负值表示无限等待，0 表示立即返回。
    auto wait_events(double timeout_ms) -> void override { win_->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发共享宿主；PostMessage 线程安全）。
    auto request_wake() -> void override { win_->request_wake(); }
    /// @brief Win32 消息泵是线程级共享队列：一次 `poll_platform_events()` 即抽干本线程全部
    /// 窗口消息（`PeekMessageA(nullptr,…)` 无 hwnd 过滤）并经 `DispatchMessage` 按 HWND 路由，
    /// 故多窗口帧循环每帧只需 pump 一次。
    /// @return 恒为 true：Win32 消息泵一次即抽干线程级队列。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return true; }
    /// @brief Win32 等待是线程级：`MsgWaitForMultipleObjectsEx(QS_ALLINPUT)` 对任意窗口的
    /// 消息到达均返回，多窗口下不存在「只等某一个窗口」的饥饿问题。
    /// @return 恒为 true：Win32 等待对线程任意窗口消息均唤醒。
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return true; }
    /// @brief 建立 OS 层 owner 关系（转发共享宿主；`native_handle()` 取对方 HWND）。
    /// @param owner 父窗口 Surface；nullptr 表示解除 owner 关系。
    auto set_owner(const Surface *owner) -> void override {
        win_->set_owner(owner != nullptr ? owner->native_handle() : nullptr);
    }
    /// @brief 启用/禁用窗口输入（转发共享宿主；模态窗口屏蔽 owner）。
    /// @param on true 恢复输入，false 禁用输入。
    auto set_enabled(bool on) -> void override { win_->set_enabled(on); }
    /// @brief 提升 z 序（转发共享宿主）。
    auto raise() -> void override { win_->raise(); }
    /// @brief 激活窗口（转发共享宿主）。
    auto focus_window() -> void override { win_->focus_window(); }
    /// @brief 所在显示器 id（转发共享宿主；与 `app::Display::id` 同源）。
    /// @return 窗口所在显示器的稳定标识。
    [[nodiscard]] auto display_id() const -> int override { return win_->display_id(); }
    /// @brief 窗口屏幕位置（转发共享宿主；物理像素）。
    /// @return 窗口左上角的屏幕物理像素坐标。
    [[nodiscard]] auto position() const -> Point override { return win_->position(); }
    /// @brief 程序化移动窗口（转发共享宿主）。
    /// @param p 目标左上角屏幕坐标（物理像素）。
    auto set_position(Point p) -> void override { win_->set_position(p); }
    /// @brief 程序化设置外框尺寸（转发共享宿主）。
    /// @param s 目标外框尺寸（物理像素）。
    auto set_size(Size s) -> void override { win_->set_size(s); }
    /// @brief DPI 缩放变化回调（转发共享宿主）。
    /// @param h 缩放变化回调，参数为变化后的缩放因子。
    auto set_scale_change_handler(ScaleChangeHandler h) -> void override {
        win_->set_scale_change_handler(std::move(h));
    }

  private:
    /// @brief 释放常驻 DIB section 与内存 DC（析构/尺寸变化重建时）。
    auto release_dib() -> void;
    /// @brief 确保常驻 BGRA DIB section 与帧缓冲同尺寸（不同则重建）；失败返回 false。
    [[nodiscard]] auto ensure_dib(int w, int h) -> bool;
    /// @brief 全量上屏：整幅 swizzle + 整窗 BitBlt。
    auto present_full(HDC hdc, int w, int h) const -> void;
    /// @brief 增量上屏：逐脏矩形 swizzle + BitBlt。
    auto present_dirty(HDC hdc, int w, int h) const -> void;

    std::unique_ptr<Win32Host> win_;
    Painter painter_;
    int presented_frames_ = 0;  ///< 已上屏帧数（见 frame_count()）。
    std::vector<Rect> present_dirty_;  ///< 本帧增量上屏脏区（设备坐标；空=全量 blit）。
    // 常驻上屏资源：BGRA（GDI 原生序）DIB section，present 时 swizzle+BitBlt。
    HDC mem_dc_ = nullptr;  ///< 内存 DC（DIB 选入其中，BitBlt 源）。
    HBITMAP dib_ = nullptr;  ///< 常驻 BGRA DIB section。
    HGDIOBJ dib_old_ = nullptr;  ///< 选入前的旧位图（释放时换回）。
    std::uint32_t *dib_bits_ = nullptr;  ///< DIB 像素内存（BGRA，top-down，行跨 = 宽×4）。
    int dib_w_ = 0;  ///< DIB 当前宽（物理像素）。
    int dib_h_ = 0;  ///< DIB 当前高（物理像素）。
};

/// @brief 从 Win32 Surface 构造 `MediaQuery`（含系统 DPI/屏幕/减弱动效）。
/// 内联定义：由 `media_query.cpp` 在 `AURORA_BACKEND_WIN32` 下调用。
/// @param s 提供缩放因子与逻辑尺寸的 Win32 Surface（基类引用）。
/// @return 组装好的 MediaQuery：屏幕尺寸由 `GetSystemMetrics` 物理分辨率按缩放折算，
///         减弱动效取自 `SPI_GETCLIENTAREAANIMATION`。
inline auto win32_media_query(const Surface &s) -> MediaQuery {
    MediaQuery mq;
    const float scale = s.scale_factor();
    mq.scale_factor = scale;
    mq.size = s.size();  // 逻辑尺寸（设备无关像素）
    const int phys_w = GetSystemMetrics(SM_CXSCREEN);
    const int phys_h = GetSystemMetrics(SM_CYSCREEN);
    mq.screen_size = Size{
        .width = scale > 0.0F ? static_cast<float>(phys_w) / scale : static_cast<float>(phys_w),
        .height = scale > 0.0F ? static_cast<float>(phys_h) / scale : static_cast<float>(phys_h),
    };
    mq.orientation =
        (mq.screen_size.width >= mq.screen_size.height) ? ScreenOrientation::Landscape : ScreenOrientation::Portrait;
    mq.platform = PlatformKind::Windows;
    mq.device = DeviceKind::Desktop;
    BOOL animations_enabled = FALSE;
    if (SystemParametersInfoA(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0) != 0) {
        mq.prefer_reduced_motion = (animations_enabled == FALSE);
    }
    return mq;
}

}  // namespace aurora

#endif  // AURORA_BACKEND_WIN32
