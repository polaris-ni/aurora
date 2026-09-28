#pragma once

// ============================================================
// wgpu_win32_surface.h — Win32 宿主 + WgpuRhi GPU 栅格上屏后端
// ------------------------------------------------------------
// 仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_WIN32 定义时编译（Windows 宿主；
// X11 宿主见 wgpu_x11_surface.h 的 WgpuX11Surface）。
// 与 D3D11Surface 同族：共用 `Win32Host` 宿主，但本类是 **GPU 栅格路径**——
// `Window::present_root` 经 `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，
// 命令在 GPU 端光栅化并直接 present 到 HWND swapchain，消除每帧全屏像素上传。
// 软件回退：WgpuRhi 不可用（工厂层 is_available 闸）或运行期 `begin_frame` 失败
// （Window 永久回退）时，本类以 GDI `SetDIBitsToDevice` 上屏 CPU Painter 帧缓冲。
// ============================================================

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)

#include <memory>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/win32_host.h"

namespace aurora {

/// @brief Win32 + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）：`Window::present_gpu_frame` 以**逻辑 dp** 尺寸调
/// `sink.begin_frame`，故本类内置适配器把逻辑尺寸 × scale 折算为设备像素后转发
/// `WgpuRhi::begin_frame`（契约要求设备像素）。present 由 `WgpuRhi::end_frame` 内
/// `wgpuSurfacePresent` 完成，`Surface::present()` 在 GPU 帧上为空操作。
///
/// 失败分层（对齐 D3D11/Glfw 先例）：
/// - 构造期：adapter/device/surface 任一失败 → `is_available()` false，工厂按
///   `RendererPreference` 报错或回退其他 Surface；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 置永久软件
///   回退，本类 present() 走 GDI blit（Painter 帧缓冲仍由 begin_frame 维护）。
class WgpuWin32Surface final : public Surface {
  public:
    /// @brief 构造：经共享 `Win32Host` 创建窗口，再初始化 wgpu adapter/device/surface（失败不抛异常）。
    /// @param width 窗口初始宽度（逻辑像素）。
    /// @param height 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    /// @param style 窗口样式选项（标题栏/边框等）。
    /// @param vsync 是否启用 FIFO（vsync）呈现（swapchain 配置期定，见 `set_vsync`）。
    /// @param visibility 初始可见性（Normal/Hidden 等）。
    WgpuWin32Surface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                     bool vsync = true, WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 析构：释放 wgpu 资源与宿主窗口（宿主由 `Win32Host` 析构销毁）。
    ~WgpuWin32Surface() override;

    /// @brief 禁用拷贝构造：持有唯一 wgpu device/swapchain 资源，拷贝会产生双重所有者。
    WgpuWin32Surface(const WgpuWin32Surface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一 GPU 资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(const WgpuWin32Surface &) -> WgpuWin32Surface & = delete;
    /// @brief 禁用移动构造：宿主回调持有本对象地址，移动会使消息路由失效。
    WgpuWin32Surface(WgpuWin32Surface &&) = delete;
    /// @brief 禁用移动赋值：同移动构造，对象地址必须稳定。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(WgpuWin32Surface &&) -> WgpuWin32Surface & = delete;

    /// @brief 宿主窗口与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    /// @return 构造期 adapter/device/surface 全部成功为 true。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效：构造期成功且未发生运行期永久回退
    /// （`sink.begin_frame` 返回 false 后转 false，此后 present 走 GDI）。测试/自检用。
    /// @return GPU 栅格路径生效为 true。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief 经 GDI 软件路径上屏的帧数——**GPU 生效期间应为 0**（口径同 WgpuWaylandSurface）。
    /// 非零即「app 帧未走 GPU 通道」：软件回退，或系统要求的重绘绕过了 GPU 帧路径（GPU 模式下
    /// Painter 帧缓冲只维护底色，此类 present 上屏即白闪），故本计数是白闪缺陷的观测签名。
    /// @return GDI 软件路径累计上屏帧数。
    [[nodiscard]] auto software_present_count() const -> int { return software_present_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    /// @return 内部 `Sink` 适配器指针（本对象拥有，调用方不得释放）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    /// @brief 开始一帧：Painter 按逻辑尺寸 begin 并预铺底色（GPU 模式维护底色缓冲，软件回退为绘制目标）。
    /// @param width 窗口逻辑宽（dp；Painter 内部 ×scale 映射物理像素）。
    /// @param height 窗口逻辑高（dp）。
    /// @return 恒返回 true（无失败路径；GPU 帧的实际失败经 sink 反馈并触发永久回退）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 取软件绘制器：Painter 持有 CPU 缓冲（软件回退帧内容 / GPU 模式底色）。
    /// @return 内部 Painter 的引用（生命周期与本对象一致）。
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }
    /// @brief 结束并上屏当前帧：GPU 帧已由 `WgpuRhi::end_frame` 内 `wgpuSurfacePresent` 完成，本方法空操作；
    /// 软件回退帧走 GDI `SetDIBitsToDevice` blit。两路均计入 `frame_count()`。
    /// @return 恒返回 true。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 窗口当前逻辑尺寸（转发共享宿主）。
    /// @return 窗口尺寸（设备无关像素）。
    [[nodiscard]] auto size() const -> Size override { return win_->size(); }
    /// @brief 窗口当前 DPI 缩放因子（转发共享宿主）。
    /// @return 缩放因子，1.0 表示无缩放。
    [[nodiscard]] auto scale_factor() const -> float override { return win_->scale_factor(); }
    /// @brief 是否已收到关闭窗口请求（转发共享宿主；WM_CLOSE/WM_DESTROY 置位）。
    /// @return 收到关闭请求为 true，主循环据此退出。
    [[nodiscard]] auto should_close() const -> bool override { return win_->should_close(); }
    /// @brief 抽取并派发本线程消息队列中的窗口消息（转发共享宿主）。
    auto poll_platform_events() -> void override { win_->poll_platform_events(); }
    /// @brief 阻塞等待消息或超时（转发共享宿主）。
    /// @param timeout_ms 最长等待毫秒数；负值表示无限等待，0 表示立即返回。
    auto wait_events(double timeout_ms) -> void override { win_->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发共享宿主；PostMessage 线程安全）。
    auto request_wake() -> void override { win_->request_wake(); }
    /// @brief 与 Win32Surface 同源：宿主消息泵是线程级共享队列。
    /// @return 恒为 true：一次 pump 抽干本线程全部窗口消息。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return true; }
    /// @brief 与 Win32Surface 同源：等待经由线程级消息通道，覆盖本进程任意窗口。
    /// @return 恒为 true：线程任意窗口消息均唤醒等待。
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return true; }
    /// @brief 事件处理器：Win32 消息翻译为 aurora `Event` 后上抛（转发共享宿主）。
    /// @param h 事件回调，接收翻译后的归一化事件。
    auto set_event_handler(const EventHandler &h) -> void override { win_->set_event_handler(h); }
    /// @brief 注册窗口可见性状态上报句柄（最小化/被遮挡/前台激活；转发共享宿主）。
    /// @param h 可见性状态回调，参数为计算后的窗口可见态。
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        win_->set_window_state_handler(std::move(h));
    }
    /// @brief 注册窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen；转发共享宿主）。
    /// @param h 几何态回调，参数为计算后的窗口模式。
    auto set_window_mode_handler(WindowModeHandler h) -> void override { win_->set_window_mode_handler(std::move(h)); }
    /// @brief 同步重渲染请求（由 Window 注入 present_root）：WM_SIZE/WM_PAINT 触发（转发共享宿主）。
    /// @param h 重渲染请求回调，宿主在系统几何变化时同步调用。
    auto set_present_request(PresentRequest h) -> void override { win_->set_present_request(std::move(h)); }
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
    /// @brief 运行时更新窗口标题（转发给共享宿主）。
    /// @param title 新窗口标题（UTF-8）。
    auto set_title(const std::string &title) -> void override { win_->set_title(title); }
    /// @brief 运行时更新悬停光标形状：与 `Win32Surface` 共用 `detail::set_win32_cursor` 下发系统预置光标。
    /// @param shape 光标语义形状，经共享映射表转为系统预置光标。
    auto set_cursor(CursorShape shape) -> void override;
    /// @brief 控件发起窗口拖拽移动（Win32：伪装 NC 拖拽 HTCAPTION）。
    auto begin_window_move() -> void override;
    /// @brief 控件发起窗口边缘缩放（Win32：伪装 NC 拖拽对应 HT 边缘码，枚举值序映射）。
    /// @param edge 拖拽的窗口边缘；None/越界值忽略。
    auto begin_window_resize(WindowResizeEdge edge) -> void override;

    /// @brief vsync 开启且 GPU 路径可用时，FIFO present 阻塞到 vblank 自带帧节拍。
    /// @return 生效为 true（帧调度据此跳过 CPU 端 sleep 节流）。
    [[nodiscard]] auto paces_frames() const -> bool override { return gpu_ != nullptr && vsync_; }
    /// @brief 启用/关闭垂直同步（v1 口径：swapchain 配置后不改，运行期设置仅记录，
    /// 下次窗口尺寸变化触发重配置时生效）。
    /// @param on true 启用 FIFO/vsync，false 关闭。
    auto set_vsync(bool on) -> void;
    /// @brief 当前 vsync 选项。
    /// @return 开启为 true。
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与 Win32Surface 同色，脏区裁剪重绘重铺同源）。
    /// @return 底色 RGBA（245,245,247,255）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接（Painter 缓冲在 GPU 模式下不含帧内容）：返回
    /// nullptr 使 `save_snapshot` 明确报 unsupported；真实窗口截图走 `capture_window`。
    /// 软件回退帧返回 Painter 缓冲（DEBUG 下）。
    /// @return GPU 模式或未开 DEBUG 时为 nullptr；软件回退帧为 Painter RGBA 缓冲首指针。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 帧缓冲物理像素尺寸（painter 按 DPI 物理分辨率分配，同 Win32Surface 口径）。
    /// @return Painter 帧缓冲的物理像素尺寸。
    [[nodiscard]] auto framebuffer_size() const -> Size override {
        return Size{.width = static_cast<float>(painter_.width()), .height = static_cast<float>(painter_.height())};
    }
    /// @brief 真实窗口截图（含非客户区）：共享 `detail::capture_window_by_hwnd`
    /// （PrintWindow PW_RENDERFULLCONTENT，可抓 GPU swapchain 内容）。DEBUG 下生效。
    /// @param path 输出 PNG 文件路径。
    /// @return 抓取并写盘成功返回 true；Release（未开 `AURORA_ENABLE_DEBUG`）返回 unsupported 错误。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

    /// @brief 已呈现帧数（GPU 帧与 GDI 回退帧均计数）。
    /// @return 迄今上屏（GPU 或 GDI）的总帧数。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    /// @brief 宿主原生窗口句柄（测试/自检用；与 `Win32Surface::hwnd()` 同义、同宿主）。
    [[nodiscard]] auto hwnd() const -> void * { return win_->hwnd(); }
    /// @brief 表层统一原生句柄：与 `hwnd()` 同源，返回窗口 HWND（以 `void *` 承载）。
    [[nodiscard]] auto native_handle() const -> void * override { return win_->hwnd(); }
    /// @brief 本窗口的无障碍桥（D13）：转发共享宿主 `Win32Host` 持有的唯一实例。
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

  private:
    /// @brief 帧 sink 适配器：把 `Window::present_gpu_frame` 传入的逻辑 dp 尺寸折算为
    /// 设备像素（契约要求 `begin_frame` 收设备像素），再转发 `WgpuRhi`；并记录本帧是否
    /// 走了 GPU 路径（present 据此跳过 GDI blit）。
    class Sink final : public rhi::RhiFrameSink {
      public:
        /// @brief 构造：绑定 wgpu 后端与所属 Surface（两者生命周期由 Surface 保证）。
        /// @param rhi wgpu RHI 实例引用。
        /// @param owner 所属 WgpuWin32Surface 引用（记录本帧 GPU 状态）。
        Sink(rhi::WgpuRhi &rhi, WgpuWin32Surface &owner) : rhi_(&rhi), owner_(&owner) {}

        /// @brief 后端名字（诊断/日志标识）。
        /// @return 固定字符串视图 "gpu-wgpu"。
        [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }
        /// @brief 取底层 RHI 后端引用（供上层录制帧命令）。
        /// @return 绑定的 WgpuRhi 的基类引用。
        [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return *rhi_; }
        /// @brief 开始 GPU 帧：把逻辑 dp 尺寸 ×scale 折算为设备像素后转发 `WgpuRhi::begin_frame`。
        /// 失败置 owner 的永久回退标志（gpu_dead_）。
        /// @param width 帧逻辑宽（dp）。
        /// @param height 帧逻辑高（dp）。
        /// @param scale DPI 缩放因子（非正按 1.0 兜底）。
        /// @return GPU 帧开始成功为 true；失败为 false（本帧转软件回退）。
        [[nodiscard]] auto begin_frame(int width, int height, float scale) -> bool override;
        /// @brief 结束 GPU 帧：转发 `WgpuRhi::end_frame`（内部 `wgpuSurfacePresent` 上屏）。
        auto end_frame() -> void override { rhi_->end_frame(); }

      private:
        rhi::WgpuRhi *rhi_;
        WgpuWin32Surface *owner_;
    };

    /// @brief 软件回退上屏：RGBA Painter 缓冲 swizzle 到 BGRA 暂存后 `SetDIBitsToDevice`。
    auto present_gdi() -> void;

    std::unique_ptr<Win32Host> win_;  ///< 共享窗口宿主（同 D3D11Surface 模式）
    Painter painter_;  ///< CPU 帧缓冲：软件回退路径的绘制目标（GPU 模式维护底色缓冲）
    std::unique_ptr<rhi::WgpuRhi> gpu_;  ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;  ///< 帧 sink 适配器（与 gpu_ 同生命周期）
    std::vector<std::uint32_t> bgra_;  ///< 软件回退上屏的 BGRA swizzle 暂存

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;  ///< 运行期 GPU 失效（sink.begin_frame 返回 false，永久软件回退）
    int frame_ = 0;  ///< 已呈现帧计数
    int software_present_ = 0;  ///< GDI 软件路径上屏帧数（见 software_present_count()）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32
