#pragma once
#include "aurora/core/platform.h"  // 门控依赖 AURORA_PLATFORM_LINUX/ANDROID，须在守卫求值前可见

/// @brief wgpu_x11_surface.h — X11 宿主 + WgpuRhi GPU 栅格上屏后端
/// （仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_X11（Linux）定义时编译）。
///
/// 与 Win32 的 WgpuWin32Surface 同族、同帧调度契约：`Window::present_root` 经
/// `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，GPU 端光栅化并经
/// Xlib surface（`WGPUSurfaceSourceXlibWindow`，Display* 与 XID 同源于
/// `X11Surface::native_display()/native_handle()`）present 上屏。
/// 宿主复用：组合内嵌 `X11Surface`（窗口创建/事件泵/光标/标题/几何全走它），
/// 本类只做「GPU 帧路径 + 软件回退分流」，不复制 Xlib 逻辑，公共头零 Xlib 依赖。
/// 软件回退：WgpuRhi 不可用（构造期）或运行期 `begin_frame` 失败（永久回退）时，
/// present 委托内嵌 X11Surface 的 XPutImage 路径。
/// 与 Win32 版差异（如实申报）：无 DPI per-monitor 变更回调、无 UIA/IMM32 桥
/// （X11 侧本就没有对应实现，X11Surface 也未覆写这些扩展点）。

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_X11)

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/x11_surface.h"

namespace aurora {

/// @brief X11 + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）与 Win32 `WgpuWin32Surface` 完全一致：`Window::present_gpu_frame`
/// 以**逻辑 dp** 尺寸调 `sink.begin_frame`，本类内置适配器按内嵌宿主 scale 折算设备像素。
///
/// 失败分层（对齐 Win32 版）：
/// - 构造期：X 连接/窗口或 adapter/device/surface 任一失败 → `is_available()` false；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 永久回退，
///   本类 present() 委托 `X11Surface::present()`（XPutImage 软件上屏）。
class WgpuX11Surface final : public Surface {
  public:
    /// @brief 构造：创建内嵌 `X11Surface` 宿主，再初始化 wgpu adapter/device/Xlib surface（失败不抛异常）。
    /// @param width 窗口初始宽度（逻辑像素）。
    /// @param height 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    /// @param style 窗口样式选项（置顶/无边框/尺寸限制等）。
    /// @param vsync 是否启用 FIFO（vsync）呈现（swapchain 配置期定，见 `set_vsync`）。
    /// @param visibility 初始可见性（Normal/Hidden 等）。
    WgpuX11Surface(int width, int height, const std::string &title, const WindowStyleOptions &style, bool vsync = true,
                   WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 析构：释放 wgpu 资源，窗口与 X 连接由内嵌宿主自行销毁。
    ~WgpuX11Surface() override;

    /// @brief 禁用拷贝构造：持有唯一 wgpu device 与内嵌宿主，拷贝会产生双重所有者。
    WgpuX11Surface(const WgpuX11Surface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一 GPU/宿主资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(const WgpuX11Surface &) -> WgpuX11Surface & = delete;
    /// @brief 禁用移动构造：sink 回指本对象地址，移动会使帧路由失效。
    WgpuX11Surface(WgpuX11Surface &&) = delete;
    /// @brief 禁用移动赋值：同移动构造，对象地址必须稳定。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(WgpuX11Surface &&) -> WgpuX11Surface & = delete;

    /// @brief 内嵌 X11 宿主与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    /// @return 宿主可用且 adapter/device/surface 全部就绪为 true。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效（回退观测点，语义同 Win32 WgpuWin32Surface::gpu_active）。
    /// @return GPU 栅格路径生效为 true。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief 经软件路径（内嵌宿主 XPutImage）上屏的帧数——**GPU 生效期间应为 0**
    /// （口径同 WgpuWaylandSurface，白闪缺陷的观测签名）。
    /// @return 软件路径累计上屏帧数。
    [[nodiscard]] auto software_present_count() const -> int { return software_present_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    /// @return 内部 `Sink` 适配器指针（本对象拥有，调用方不得释放）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    /// @brief 开始一帧：底色缓冲与软件上屏准备全委托内嵌宿主（其 begin_frame 已含 set_scale + 铺底色）。
    /// @param width 窗口逻辑宽（dp）。
    /// @param height 窗口逻辑高（dp）。
    /// @return 转发宿主结果（宿主恒返回 true；GPU 帧失败经 sink 反馈并触发永久回退）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 取软件绘制器：转发内嵌宿主的 Painter（软件回退帧的绘制目标）。
    /// @return 宿主内部 Painter 的引用。
    [[nodiscard]] auto painter() -> Painter & override { return host_->painter(); }
    /// @brief 结束并上屏当前帧：GPU 帧已由 `WgpuRhi::end_frame` 内 `wgpuSurfacePresent` 完成（空操作计数）；
    /// 软件回退帧委托内嵌宿主的 XPutImage 路径。
    /// @return 恒返回 true（内嵌宿主 present 无失败路径）。
    [[nodiscard]] auto present() -> Result<bool> override;

    /// @brief 窗口当前逻辑尺寸（转发内嵌宿主）。
    /// @return 窗口尺寸（设备无关像素）。
    [[nodiscard]] auto size() const -> Size override { return host_->size(); }
    /// @brief 窗口当前 DPI 缩放因子（转发内嵌宿主）。
    /// @return 缩放因子，1.0 表示无缩放。
    [[nodiscard]] auto scale_factor() const -> float override { return host_->scale_factor(); }
    /// @brief 是否已收到关闭窗口请求（转发内嵌宿主；WM_DELETE_WINDOW 置位）。
    /// @return 收到关闭请求为 true，主循环据此退出。
    [[nodiscard]] auto should_close() const -> bool override { return host_->should_close(); }
    /// @brief 非阻塞抽取并派发 X 事件（转发内嵌宿主）。
    auto poll_platform_events() -> void override { host_->poll_platform_events(); }
    /// @brief 阻塞等待 X 事件/唤醒/超时（转发内嵌宿主的 poll(2) 路径）。
    /// @param timeout_ms 最长等待毫秒数；负值表示无限等待，0 表示立即返回。
    auto wait_events(double timeout_ms) -> void override { host_->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发内嵌宿主；自唤醒管道线程安全）。
    auto request_wake() -> void override { host_->request_wake(); }
    /// @brief X11 泵只抽本连接队列（基类默认 false），显式转发保持与内嵌宿主同口径。
    /// @return 与内嵌宿主 `pumps_thread_queue()` 同值（X11 为 false）。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return host_->pumps_thread_queue(); }
    /// @brief X11 等待只覆盖本连接 fd，与宿主同口径转发。
    /// @return 与内嵌宿主 `waits_thread_queue()` 同值。
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return host_->waits_thread_queue(); }
    /// @brief 事件处理器（转发内嵌宿主；Xlib 事件翻译在其事件泵内完成）。
    /// @param h 事件回调，接收翻译后的归一化事件。
    auto set_event_handler(const EventHandler &h) -> void override { host_->set_event_handler(h); }
    /// @brief 注册窗口可见性状态上报句柄（转发内嵌宿主）。
    /// @param h 可见性状态回调，参数为计算后的窗口可见态。
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        host_->set_window_state_handler(std::move(h));
    }
    /// @brief 注册窗口几何态上报句柄（转发内嵌宿主）。
    /// @param h 几何态回调，参数为计算后的窗口模式。
    auto set_window_mode_handler(WindowModeHandler h) -> void override { host_->set_window_mode_handler(std::move(h)); }
    /// @brief 同步重渲染请求（由 Window 注入 present_root；转发内嵌宿主）。
    /// @param h 重渲染请求回调，宿主在系统几何变化时同步调用。
    auto set_present_request(PresentRequest h) -> void override { host_->set_present_request(std::move(h)); }
    /// @brief 运行时更新窗口标题（转发内嵌宿主）。
    /// @param title 新窗口标题（UTF-8）。
    auto set_title(const std::string &title) -> void override { host_->set_title(title); }
    /// @brief 运行时更新悬停光标形状（转发内嵌宿主；句柄缓存与释放均在宿主）。
    /// @param shape 光标语义形状，按 X11Surface 映射表转为 Xlib 游标字形。
    auto set_cursor(CursorShape shape) -> void override { host_->set_cursor(shape); }
    /// @brief 增量上屏脏区（设备坐标；转发内嵌宿主，仅软件回退帧生效）。
    /// @param device_rects 本帧脏矩形列表，空表示全量 blit。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override {
        host_->set_present_dirty(device_rects);
    }
    /// @brief IME 桥 provider 转发（XIM 锚点/回调全在内嵌宿主的事件泵里生效）。
    /// @param provider 返回插入点客户区矩形（物理像素）的回调，供 XIM XNSpotLocation。
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override {
        host_->set_composition_caret_provider(std::move(provider));
    }

    /// @brief vsync 开启且 GPU 路径可用时，FIFO present 阻塞到 vblank 自带帧节拍。
    /// @return 生效为 true（帧调度据此跳过 CPU 端 sleep 节流）。
    [[nodiscard]] auto paces_frames() const -> bool override { return gpu_ != nullptr && vsync_; }
    /// @brief 启用/关闭垂直同步（口径同 Win32 版：swapchain 重配置时生效）。
    /// @param on true 启用 FIFO/vsync，false 关闭。
    auto set_vsync(bool on) -> void;
    /// @brief 当前 vsync 选项。
    /// @return 开启为 true。
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与内嵌 X11Surface 同色）。
    /// @return 底色 RGBA（245,245,247,255）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接：GPU 模式返回 nullptr（save_snapshot 报 unsupported）；
    /// 软件回退帧委托内嵌 X11Surface（DEBUG 下返回 Painter 缓冲）。
    /// @return GPU 模式或未开 DEBUG 时为 nullptr；软件回退帧为宿主 Painter RGBA 缓冲首指针。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 真实窗口截图：委托内嵌 X11Surface 的 XGetImage 路径（合成器环境下含 GPU 上屏内容）。
    /// @param path 输出 PNG 文件路径。
    /// @return 转发宿主结果：抓取并写盘成功返回 true；Release 下返回 unsupported 错误。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

    /// @brief 已呈现帧数（GPU 帧与软件回退帧均计数）。
    /// @return 迄今上屏（GPU 或软件路径）的总帧数。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    /// @brief 原生窗口句柄：内嵌 X11Surface 的 XID（Window）。
    [[nodiscard]] auto native_handle() const -> void * override { return host_->native_handle(); }

    /// @brief 无障碍桥：落在内嵌宿主（AT-SPI2 桥由 X11Surface 承接，GPU/软件路径共用）。
    /// @return 宿主持有的桥指针（未建桥/降级时为 nullptr；宿主拥有生命周期）。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override {
        return host_->accessibility_provider();
    }
    /// @brief 注入语义树根（转发内嵌宿主；首帧注入触发建桥）。
    /// @param root 语义树根控件指针。
    auto set_accessibility_root(Widget *root) -> void override { host_->set_accessibility_root(root); }

  private:
    /// @brief 帧 sink 适配器：逻辑 dp × 宿主 scale → 设备像素转发 `WgpuRhi`，
    /// 并记录本帧是否走 GPU 路径（present 据此分流）。同 Win32 版 WgpuWin32Surface::Sink。
    class Sink final : public rhi::RhiFrameSink {
      public:
        /// @brief 构造：绑定 wgpu 后端与所属 Surface（两者生命周期由 Surface 保证）。
        /// @param rhi wgpu RHI 实例引用。
        /// @param owner 所属 WgpuX11Surface 引用（记录本帧 GPU 状态）。
        Sink(rhi::WgpuRhi &rhi, WgpuX11Surface &owner) : rhi_(&rhi), owner_(&owner) {}

        /// @brief 后端名字（诊断/日志标识）。
        /// @return 固定字符串视图 "gpu-wgpu"。
        [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }
        /// @brief 取底层 RHI 后端引用（供上层录制帧命令）。
        /// @return 绑定的 WgpuRhi 的基类引用。
        [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return *rhi_; }
        /// @brief 开始 GPU 帧：逻辑 dp 尺寸 ×scale 折算设备像素后转发 `WgpuRhi::begin_frame`。
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
        WgpuX11Surface *owner_;
    };

    std::unique_ptr<X11Surface> host_;  ///< 内嵌 X11 宿主（窗口/事件/软件回退上屏）
    std::unique_ptr<rhi::WgpuRhi> gpu_;  ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;  ///< 帧 sink 适配器（与 gpu_ 同生命周期）

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;  ///< 运行期 GPU 失效（永久软件回退）
    int frame_ = 0;  ///< 已呈现帧计数
    int software_present_ = 0;  ///< 软件路径上屏帧数（见 software_present_count()）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_X11 (Linux)
