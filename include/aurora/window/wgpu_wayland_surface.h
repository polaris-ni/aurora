#pragma once
#include "aurora/core/platform.h"  // 门控依赖 AURORA_PLATFORM_LINUX/ANDROID，须在守卫求值前可见

/// @brief wgpu_wayland_surface.h — Wayland 宿主 + WgpuRhi GPU 栅格上屏后端
/// （仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_WAYLAND（Linux）定义时编译）。
/// ------------------------------------------------------------
/// 与 Win32 的 WgpuWin32Surface、Linux/X11 的 WgpuX11Surface 同族、同帧调度契约：
/// `Window::present_root` 经 `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，
/// GPU 端光栅化并经 Wayland surface（`WGPUSurfaceSourceWaylandSurface`，wl_display*
/// 与 wl_surface* 同源于 `WaylandSurface::native_display()/native_handle()`）present 上屏。
/// 宿主复用：组合内嵌 `WaylandSurface`（窗口壳/事件泵/xkb 输入/CSD/几何全走它），
/// 本类只做「GPU 帧路径 + 软件回退分流」，不复制 wayland-client 逻辑，公共头零
/// wayland 依赖。
/// 软件回退：WgpuRhi 不可用（构造期）或运行期 `begin_frame` 失败（永久回退）时，
/// present 委托内嵌 WaylandSurface 的 wl_shm 路径。
/// 与 X11 版差异（如实申报）：
/// - 无 `capture_window`（Wayland 协议无抓屏原语，内嵌宿主同样未覆写，基类默认
///   报 disabled 即最终行为）；
/// - 自绘 CSD 装饰经命令通道合成进 GPU 帧：swapchain 独占整块 wl_surface，内嵌宿主画进
///   Painter 的标题栏不会随帧缓冲上屏，故每帧在 `Sink::end_frame` 里把装饰录制成
///   DisplayList 追加回放在 app 帧之后（绘制实现与软件路径同源 `csd::paint_title_bar`，
///   见 `WaylandSurface::record_client_decoration`）。合成器提供 xdg-decoration SSD 时
///   内嵌宿主不绘装饰，录制返回 false → 零额外开销。
/// @return 无返回值：本块为文件级说明（紧随的伪声明是 `#if` 续行折叠产物，非真实函数）。

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_WAYLAND)

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"  // Sink 的装饰录制缓冲成员需完整类型
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/wayland_surface.h"

namespace aurora {

/// @brief Wayland + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）与 Win32 `WgpuWin32Surface`、X11 `WgpuX11Surface` 完全一致：
/// `Window::present_gpu_frame` 以**逻辑 dp** 尺寸调 `sink.begin_frame`，本类内置适配器
/// 按内嵌宿主 scale 折算设备像素。
///
/// 失败分层（对齐兄弟宿主）：
/// - 构造期：Wayland 连接/窗口壳或 adapter/device/surface 任一失败 → `is_available()` false；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 永久回退，
///   本类 present() 委托 `WaylandSurface::present()`（wl_shm 软件上屏）。
class WgpuWaylandSurface final : public Surface {
  public:
    /// @brief 构造：创建内嵌 `WaylandSurface` 宿主，再初始化 wgpu adapter/device/Wayland surface（失败不抛异常）。
    /// @param width 窗口初始宽度（逻辑像素）。
    /// @param height 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    /// @param style 窗口样式选项（CSD/SSD 装饰等）。
    /// @param vsync 是否启用 FIFO（vsync）呈现（swapchain 配置期定，见 `set_vsync`）。
    /// @param visibility 初始可见性（Normal/Hidden 等）。
    WgpuWaylandSurface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                       bool vsync = true, WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 析构：释放 wgpu 资源，Wayland 连接与窗口壳由内嵌宿主自行销毁。
    ~WgpuWaylandSurface() override;

    /// @brief 禁用拷贝构造：持有唯一 wgpu device 与内嵌宿主，拷贝会产生双重所有者。
    WgpuWaylandSurface(const WgpuWaylandSurface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一 GPU/宿主资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(const WgpuWaylandSurface &) -> WgpuWaylandSurface & = delete;
    /// @brief 禁用移动构造：sink 回指本对象地址，移动会使帧路由失效。
    WgpuWaylandSurface(WgpuWaylandSurface &&) = delete;
    /// @brief 禁用移动赋值：同移动构造，对象地址必须稳定。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(WgpuWaylandSurface &&) -> WgpuWaylandSurface & = delete;

    /// @brief 内嵌 Wayland 宿主与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    /// @return 宿主可用且 adapter/device/surface 全部就绪为 true。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效（回退观测点，语义同 Win32 WgpuWin32Surface::gpu_active）。
    /// @return GPU 栅格路径生效为 true。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief 经软件路径（内嵌宿主 wl_shm）上屏的帧数——**GPU 生效期间应为 0**。
    /// 非零即「app 帧未走 GPU 通道」：软件回退，或系统要求的重绘绕过了 GPU 帧路径
    /// （后者是白闪缺陷的签名：`Window` 直接调 `present()`，而 GPU 模式下 Painter 帧缓冲
    /// 从不清绘，上屏只剩底色）。真机探针据此断言「无白闪帧」。
    /// @return 软件路径累计上屏帧数。
    [[nodiscard]] auto software_present_count() const -> int { return software_present_; }

    /// @brief 把自绘 CSD 装饰回放进 GPU 帧的帧数（观测点）。合成器提供 SSD 时内嵌宿主不绘
    /// 装饰，本计数恒 0；CSD 兜底合成器（GNOME 等）下应逐帧递增 = GPU 呈现帧数。
    /// @return 装饰回放合成进 GPU 帧的帧数。
    [[nodiscard]] auto decoration_replay_count() const -> int { return deco_replays_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    /// @return 内部 `Sink` 适配器指针（本对象拥有，调用方不得释放）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    /// @brief 开始一帧：底色缓冲与软件上屏准备全委托内嵌宿主（其 begin_frame 已含底色 fill 与尺寸对齐）。
    /// @param width 窗口逻辑宽（dp）。
    /// @param height 窗口逻辑高（dp）。
    /// @return 转发宿主结果（宿主无失败路径；GPU 帧失败经 sink 反馈并触发永久回退）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 取软件绘制器：转发内嵌宿主的 Painter（软件回退帧的绘制目标）。
    /// @return 宿主内部 Painter 的引用。
    [[nodiscard]] auto painter() -> Painter & override { return host_->painter(); }
    /// @brief 结束并上屏当前帧：GPU 帧已由 `WgpuRhi::end_frame` 内 present+commit 完成（空操作计数）；
    /// 软件回退帧委托内嵌宿主的 wl_shm 路径。
    /// @return GPU 帧恒 true；软件帧转发宿主结果（可能携带 wl_shm 上屏错误）。
    [[nodiscard]] auto present() -> Result<bool> override;

    /// @brief 窗口当前逻辑尺寸（转发内嵌宿主）。
    /// @return 窗口尺寸（设备无关像素）。
    [[nodiscard]] auto size() const -> Size override { return host_->size(); }
    /// @brief 窗口当前 DPI 缩放因子（转发内嵌宿主）。
    /// @return 缩放因子，1.0 表示无缩放。
    [[nodiscard]] auto scale_factor() const -> float override { return host_->scale_factor(); }
    /// @brief 是否已收到关闭窗口请求（转发内嵌宿主；xdg_toplevel close 置位）。
    /// @return 收到关闭请求为 true，主循环据此退出。
    [[nodiscard]] auto should_close() const -> bool override { return host_->should_close(); }
    /// @brief 非阻塞抽取并派发 Wayland 事件（转发内嵌宿主）。
    auto poll_platform_events() -> void override { host_->poll_platform_events(); }
    /// @brief 阻塞等待 Wayland 事件/唤醒/超时（转发内嵌宿主的 poll(2) 路径）。
    /// @param timeout_ms 最长等待毫秒数；负值表示无限等待，0 表示立即返回。
    auto wait_events(double timeout_ms) -> void override { host_->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发内嵌宿主；自唤醒管道线程安全）。
    auto request_wake() -> void override { host_->request_wake(); }
    /// @brief 事件处理器（转发内嵌宿主；Wayland 事件翻译在其事件泵内完成）。
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
    /// @brief 运行时更新悬停光标形状（转发内嵌宿主；光标资源在其 shm/主题路径管理）。
    /// @param shape 光标语义形状，按 WaylandSurface 映射转为主题光标。
    auto set_cursor(CursorShape shape) -> void override { host_->set_cursor(shape); }
    /// @brief 增量上屏脏区（设备坐标；转发内嵌宿主，仅软件回退帧生效）。
    /// @param device_rects 本帧脏矩形列表，空表示全量上屏。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override {
        host_->set_present_dirty(device_rects);
    }
    /// @brief IME 桥 provider 转发（判据与候选窗定位在内嵌宿主；刷新点见其 poll_platform_events）。
    /// @param provider 返回插入点客户区矩形（物理像素）的回调，供候选窗贴附定位。
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override {
        host_->set_composition_caret_provider(std::move(provider));
    }

    /// @brief vsync 开启且 GPU 路径可用时，FIFO present 阻塞到 vblank 自带帧节拍。
    /// @return 生效为 true（帧调度据此跳过 CPU 端 sleep 节流）。
    [[nodiscard]] auto paces_frames() const -> bool override { return gpu_ != nullptr && vsync_; }
    /// @brief 启用/关闭垂直同步（口径同 Win32/X11 版：swapchain 重配置时生效）。
    /// @param on true 启用 FIFO/vsync，false 关闭。
    auto set_vsync(bool on) -> void;
    /// @brief 当前 vsync 选项。
    /// @return 开启为 true。
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与内嵌 WaylandSurface 同色）。
    /// @return 底色 RGBA（245,245,247,255）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接：GPU 模式返回 nullptr（save_snapshot 报 unsupported）；
    /// 软件回退帧委托内嵌 WaylandSurface（DEBUG 下返回 Painter 缓冲）。
    /// @return GPU 模式或未开 DEBUG 时为 nullptr；软件回退帧为宿主 Painter RGBA 缓冲首指针。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 已呈现帧数（GPU 帧与软件回退帧均计数）。
    /// @return 迄今上屏（GPU 或软件路径）的总帧数。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    /// @brief 原生窗口句柄：内嵌 WaylandSurface 的 `wl_surface*`。
    [[nodiscard]] auto native_handle() const -> void * override { return host_->native_handle(); }

    /// @brief 标题栏配色注入（转发内嵌宿主；CSD 自绘路径生效，GPU 帧经装饰回放同步换色）。
    /// @param style 标题栏配色/样式。
    auto set_title_bar_style(const TitleBarStyle &style) -> void override { host_->set_title_bar_style(style); }
    /// @brief 标题栏图标注入（转发内嵌宿主；仅 CSD 自绘装饰时参与回放合成）。
    /// @param icon 图标图像共享指针，nullptr 表示无图标。
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void override { host_->set_title_bar_icon(icon); }
    /// @brief 控件发起窗口拖拽移动（转发内嵌宿主；xdg_toplevel move 请求）。
    auto begin_window_move() -> void override { host_->begin_window_move(); }
    /// @brief 控件发起窗口边缘缩放（转发内嵌宿主；xdg_toplevel resize 对应边缘）。
    /// @param edge 拖拽的窗口边缘。
    auto begin_window_resize(WindowResizeEdge edge) -> void override { host_->begin_window_resize(edge); }
    /// @brief 客户区相对窗口外框的内缩（转发内嵌宿主；CSD 装饰占位）。
    /// @return 四边内缩（SSD 或无装饰时全 0）。
    [[nodiscard]] auto content_inset() const -> EdgeInsets override { return host_->content_inset(); }
    /// @brief 请求关闭窗口（转发内嵌宿主；xdg_toplevel close）。
    auto close() -> void override { host_->close(); }
    /// @brief 最小化窗口（转发内嵌宿主）。
    auto minimize() -> void override { host_->minimize(); }
    /// @brief 最大化/还原切换（转发内嵌宿主）。
    auto toggle_maximize() -> void override { host_->toggle_maximize(); }
    /// @brief 进入/退出全屏（转发内嵌宿主；xdg_toplevel fullscreen）。
    /// @param on true 全屏，false 还原。
    auto set_fullscreen(bool on) -> void override { host_->set_fullscreen(on); }

    /// @brief 无障碍桥：落在内嵌宿主（AT-SPI2 桥由 WaylandSurface 承接，GPU/软件路径共用）。
    /// @return 宿主持有的桥指针（未建桥/降级时为 nullptr；宿主拥有生命周期）。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override {
        return host_->accessibility_provider();
    }
    /// @brief 注入语义树根（转发内嵌宿主；首帧注入触发建桥）。
    /// @param root 语义树根控件指针。
    auto set_accessibility_root(Widget *root) -> void override { host_->set_accessibility_root(root); }

  private:
    /// @brief 帧 sink 适配器：逻辑 dp × 宿主 scale → 设备像素转发 `WgpuRhi`，
    /// 并记录本帧是否走 GPU 路径（present 据此分流）。同 Win32/X11 版内置 Sink。
    class Sink final : public rhi::RhiFrameSink {
      public:
        /// @brief 构造：绑定 wgpu 后端与所属 Surface（两者生命周期由 Surface 保证）。
        /// @param rhi wgpu RHI 实例引用。
        /// @param owner 所属 WgpuWaylandSurface 引用（记录本帧 GPU 状态）。
        Sink(rhi::WgpuRhi &rhi, WgpuWaylandSurface &owner) : rhi_(&rhi), owner_(&owner) {}

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
        /// @brief 收口本帧：先把自绘 CSD 装饰回放在 app 帧之上（swapchain 独占 wl_surface，
        /// 装饰只能走命令通道），再交 `WgpuRhi::end_frame` 提交 + present。见 `.cpp`。
        auto end_frame() -> void override;

      private:
        rhi::WgpuRhi *rhi_;
        WgpuWaylandSurface *owner_;
        DisplayList deco_dl_;  ///< 装饰录制缓冲（逐帧复用，避免每帧分配）
    };

    std::unique_ptr<WaylandSurface> host_;  ///< 内嵌 Wayland 宿主（窗口壳/事件/软件回退上屏）
    std::unique_ptr<rhi::WgpuRhi> gpu_;  ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;  ///< 帧 sink 适配器（与 gpu_ 同生命周期）

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;  ///< 运行期 GPU 失效（永久软件回退）
    int frame_ = 0;  ///< 已呈现帧计数
    int software_present_ = 0;  ///< 软件路径上屏帧数（见 software_present_count()）
    int deco_replays_ = 0;  ///< 装饰回放进 GPU 帧的帧数（见 decoration_replay_count()）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WAYLAND (Linux)
