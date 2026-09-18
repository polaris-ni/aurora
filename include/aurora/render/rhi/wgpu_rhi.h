#pragma once

// ============================================================
// wgpu_rhi.h — GPU 栅格后端：DisplayList 的 wgpu（WebGPU C API）消费者
// ------------------------------------------------------------
// 跨平台 GPU 主力路径（策略 B）：同一套 WGSL 管线覆盖 Vulkan / D3D12 / Metal / GLES，
// 经 third_party/wgpu-native（Rust 源码静态库，AURORA_BACKEND_GPU_WGPU）交付。
// 与 `GpuGlRhi` 平级：同为 `RhiBackend` + `RhiFrameSink` 实现，命令语义、批切分与
// 裁剪/混合口径以 GL 路径与软件路径为同源参照（specification/03-layout-render.md §8.7）。
//
// 本类仅在 `AURORA_BACKEND_GPU_WGPU=ON` 时存在（区别于 GpuGlRhi 的恒编译口径）：
// 实现 TU 链接 Rust 静态库，特性关闭时库内无该目标文件，声明亦不导出。
// wgpu 头（webgpu.h / wgpu.h）不外泄：全部资源持有关于 `Impl`（pimpl）。
// ============================================================

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/rhi_frame_sink.h"

#ifdef AURORA_BACKEND_GPU_WGPU

namespace aurora::rhi {

/// @brief wgpu 后端装配选项。
struct WgpuRhiOptions {
    /// @brief 底层图形 API 选择。Auto 按 wgpu 平台偏好（Windows: D3D12 → Vulkan → GLES；
    /// Linux: Vulkan → GLES；macOS: Metal）。
    enum class Backend : std::uint8_t {
        Auto,    ///< 平台默认优先级链
        Vulkan,  ///< 显式 Vulkan
        D3D12,   ///< 显式 D3D12（仅 Windows）
        Metal,   ///< 显式 Metal（仅 macOS/iOS）
        GLES,    ///< 显式 OpenGL ES（兼容兜底，能力受限：无 compute）
    };

    Backend backend = Backend::Auto;

    /// @brief 原生窗口句柄（`Surface::native_handle()` 口径：Win32 为 HWND，X11 为
    /// Display*，见 `native_display`）。`nullptr` = 离屏模式（渲染目标为内部纹理，
    /// 供 `read_pixels` 读回；无 swapchain，测试/探针通道）。
    void *native_window = nullptr;

    /// @brief X11 `Display*`（仅 `native_window` 为 Xlib Window 时使用；Win32 忽略）。
    void *native_display = nullptr;

    /// @brief 离屏模式初始尺寸（设备像素；非离屏模式忽略，`begin_frame` 可再重设）。
    int offscreen_width = 0;
    int offscreen_height = 0;

    /// @brief 垂直同步（FIFO）。离屏模式无意义。
    bool vsync = true;
};

/// @brief GPU 栅格后端：`DisplayList` 的 wgpu 消费者（`RhiBackend` + `RhiFrameSink`）。
///
/// 与 `GpuGlRhi` 同构的命令消费口径：`submit` 只做「命令 → 顶点/状态」翻译，GPU 调用
/// 集中在批提交与帧收尾；批切分保序不重排（管线/裁剪/纹理变化即断批）。裁剪统一
/// shader 内 SDF alpha（不用 scissor），与软件路径逐像素交叠语义同源。
///
/// 初始化失败（无可用 adapter / device 申请失败 / 着色器编译失败）→ `valid()` 为 false，
/// 调用方整体回退软件路径，不做逐命令混合（对齐 `RhiFrameSink::begin_frame` 契约）。
///
/// 能力位（`capabilities()`）：gpu=true；compute 随所选后端（Vulkan/D3D12/Metal=true，
/// GLES=false）；native_surface_import 本轮为 false——wgpu-native C API（v29）未暴露
/// 外部共享纹理导入入口，兑现路径随上游 C API 扩展（探测回退契约不变）。
///
/// @note Thread: main-thread only（wgpu device/surface 所属线程）
class WgpuRhi final : public RhiBackend, public RhiFrameSink {
  public:
    /// @brief 帧级诊断计数（性能观测 / 测试断言；与 GpuGlRhi::FrameStats 同意形）。
    struct FrameStats {
        std::uint32_t draw_calls = 0;     ///< 批提交次数（= 渲染 pass 内 draw 数）
        std::uint32_t vertices = 0;       ///< 本帧提交顶点数
        std::uint32_t skipped_cmds = 0;   ///< 遇到未实现命令而跳过的条数
    };

    /// @brief 默认构造：不初始化 wgpu（`valid()` 为 false），供占位与测试桩场景。
    WgpuRhi();
    /// @brief 装载构造：创建 instance/adapter/device（+ 离屏目标或 swapchain surface）。
    /// 失败时 `valid()` 为 false，不抛异常。
    explicit WgpuRhi(const WgpuRhiOptions &options);
    ~WgpuRhi() override;
    WgpuRhi(const WgpuRhi &) = delete;
    auto operator=(const WgpuRhi &) -> WgpuRhi & = delete;
    WgpuRhi(WgpuRhi &&) = delete;
    auto operator=(WgpuRhi &&) -> WgpuRhi & = delete;

    /// @brief wgpu 资源是否就绪（构造成功且未发生设备丢失级失败）。
    [[nodiscard]] auto valid() const -> bool;

    [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }

    /// @brief 命令消费面视图（`DisplayList::replay` 入口）；帧调度与命令消费同对象。
    [[nodiscard]] auto backend() -> RhiBackend & override { return *this; }

    /// @brief 消费一条命令（翻译进当前批；提交延迟到批切换/帧尾）。
    auto submit(const DrawCmd &cmd, const CmdData &data) -> void override;

    // ---- RhiFrameSink ----
    [[nodiscard]] auto begin_frame(int device_width, int device_height, float scale) -> bool override;
    auto end_frame() -> void override;

    /// @brief 本帧（自 `begin_frame` 起）累计诊断计数。
    [[nodiscard]] auto stats() const -> FrameStats;

    /// @brief 字形图集页边长（默认 1024；须在 `begin_frame` 前设置，非正值忽略）。
    /// 常规消费者无须调用；测试用小页覆盖「满页开新页 / 页数封顶 LRU 淘汰」路径。
    auto set_glyph_page_size(int side) -> void;

    /// @brief 当前帧内容读回（RGBA8，行序自上而下）。仅供诊断/快照/容差 golden。
    /// 调用窗口：`end_frame` 之后、下一次 `begin_frame` 之前。返回 false = 不可用或失败。
    [[nodiscard]] auto read_pixels(std::vector<std::uint8_t> &out) -> bool;

    // ---- RhiBackend 能力位与流式纹理契约（specification/03 §8.7）----

    [[nodiscard]] auto capabilities() const -> RhiCapabilities override;

    /// @brief 取常驻流式纹理槽（键寻址，槽复用、尺寸变化就地重定义；与 `DrawImage`
    /// 流式分支共享同一存储）。@return 句柄（非零）；`0` = 后端不可用。
    [[nodiscard]] auto acquire_stream_image(std::uint64_t key, int width, int height) -> StreamImageId override;

    /// @brief 流式图像增量更新：`pixels` 为整图像素基址（RGBA8 直色非预乘），
    /// `stride_bytes` 行跨距字节数（`0` = 紧凑行），脏矩形 (x,y,w,h) sub-upload
    /// （wgpu `queueWriteTexture` + bytes_per_row/rows_per_image 布局参数）。
    auto update_stream_image(StreamImageId id, const std::uint8_t *pixels, std::size_t stride_bytes, int x, int y,
                             int w, int h) -> void override;

    /// @brief 释放流式图像槽（先落地待提交批再销毁；句柄此后无效，重复释放无害）。
    auto release_stream_image(StreamImageId id) -> void override;

    /// @brief 原生表面导入：wgpu-native v29 C API 无外部共享纹理导入入口 → 单次告警并
    /// 返回 `0`，调用方回退 CPU 上传路径（契约行为与 GL 后端一致，能力位恒 false）。
    [[nodiscard]] auto import_native_surface(const NativeSurfaceFrame &frame) -> StreamImageId override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora::rhi

#endif  // AURORA_BACKEND_GPU_WGPU
