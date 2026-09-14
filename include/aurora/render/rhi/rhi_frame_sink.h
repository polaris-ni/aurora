#pragma once

#include <string_view>

namespace aurora::rhi {

class RhiBackend;  // 前置声明：命令消费面（backend() 返回其引用；完整定义见 rhi_backend.h）

/// @brief 帧调度契约：GPU 后端的帧生命周期挂点（与 `RhiBackend` 的命令消费正交）。
///
/// `RhiBackend::submit` 只定义「一条命令如何被消费」；本接口定义「一帧如何开始与结束」。
/// 两者分离使 `DisplayList::replay` 保持单一命令语义，而帧调度由 Surface 驱动：
/// `Window::present_root` 在 GPU 路径上按序调用 `begin_frame` → `replay(sink.backend())`
/// → `end_frame`，随后经 `Surface::present()` 完成 swap（GL 上下文与窗口所有权在 Surface，
/// present 不入本接口）。
///
/// 语义约定：
/// - GPU 路径恒全量重绘（GPU 帧每帧整帧重建，不做部分裁剪）：`begin_frame` 将帧缓冲
///   重置为**零基底**（透明黑）并复位裁剪/全局 alpha 状态，整帧内容完全由本次回放的
///   命令建立（与软件路径 `begin` 后零基底语义同源）。
/// - `end_frame` 后画面处于可呈现状态（管线 flush + MSAA resolve + blit 到默认帧缓冲）。
/// - `begin_frame` 返回 false 表示后端不可用（初始化失败 / 上下文丢失）；调用方本帧回退
///   软件路径，此后应视该后端为永久失效（实现方须保证 `valid()` 随之转 false）。
///
/// @note Thread: main-thread only（GL 上下文所属线程）
class RhiFrameSink {
  public:
    RhiFrameSink() = default;
    virtual ~RhiFrameSink() = default;
    RhiFrameSink(const RhiFrameSink &) = delete;
    auto operator=(const RhiFrameSink &) -> RhiFrameSink & = delete;
    RhiFrameSink(RhiFrameSink &&) = delete;
    auto operator=(RhiFrameSink &&) -> RhiFrameSink & = delete;

    /// @brief 后端标识（诊断与自检用；如 `"gpu-gl"`）。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 命令消费面：同一后端对象作为 `RhiBackend` 的视图（`DisplayList::replay` 入口）。
    /// 帧调度与命令消费同源——实现类同时继承两接口时通常返回 `*this`。
    [[nodiscard]] virtual auto backend() -> RhiBackend & = 0;

    /// @brief 开始一帧：确保帧缓冲/视口按目标尺寸就绪并重置零基底与状态（见类注释语义约定）。
    /// @param device_width / device_height 呈现目标的设备像素尺寸（framebuffer 级）。
    /// @param scale 设备像素 / 逻辑 dp 比例（命令坐标均为逻辑 dp，GPU 端按比例映射）。
    /// @return false = 后端不可用，调用方须回退软件路径。
    [[nodiscard]] virtual auto begin_frame(int device_width, int device_height, float scale) -> bool = 0;

    /// @brief 结束一帧：flush 全部已提交命令并输出到可呈现目标（见类注释语义约定）。
    virtual auto end_frame() -> void = 0;
};

}  // namespace aurora::rhi
