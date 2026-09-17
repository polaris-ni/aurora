#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/native_surface.h"
#include "aurora/core/transform.h"
#include "aurora/render/display_list.h"

namespace aurora::rhi {

/// @brief 流式图像句柄（后端不透明值；`0` 恒为无效句柄）。
///
/// 由 `acquire_stream_image` / `import_native_surface` 签发，指向后端内的常驻纹理槽
///（固定槽复用、不参与通用缓存淘汰）。生命周期由调用方管理：用毕 `release_stream_image`。
using StreamImageId = std::uint64_t;

/// @brief RHI 后端能力位（构造时确定，帧间不变；软件后端全 false）。
struct RhiCapabilities {
    bool gpu = false;                   ///< 硬件加速命令消费（GPU 后端为 true）
    bool native_surface_import = false; ///< 可导入平台原生 GPU 表面（`import_native_surface` 可用）
    bool compute = false;               ///< 支持 compute 内部加速（blur / mask / 重采样 / 层合成）
};

/// @brief 一条绘制命令所需的**变长数据**，由 `DisplayList` 在回放时把池下标解析为只读指针。
///
/// 空指针表示该命令不引用对应数据（如 `FillRect` 的 `text` 为 `nullptr`）。后端据此取
/// 文本 / 字体 / 渐变色标 / 图像 / 变换矩阵，**不需要也不应接触 `DisplayList` 的池下标语义**
/// ——下标合法性是录制方（`Painter::record`）的契约，解析集中在一处，避免每个后端各判一遍。
struct CmdData {
    const std::string *text = nullptr;           ///< DrawText 字符串
    const Font *font = nullptr;                  ///< DrawText 字体
    const std::vector<Color> *colors = nullptr;  ///< 渐变色标颜色数组
    const std::vector<float> *stops = nullptr;   ///< 渐变色标停靠数组（归一化 [0,1]）
    const Image *image = nullptr;                ///< DrawImage / Composite 的图像
    const Matrix2D *matrix = nullptr;            ///< Composite 的仿射变换矩阵
    const std::vector<Point> *points = nullptr;  ///< Polyline 的折线点集（逻辑 dp）
};

/// @brief RHI 后端：`DisplayList` 回放的**目标抽象**（command sink）。
///
/// 绘制指令的唯一来源是 `DisplayList`；`Painter` 与 GPU 后端是它的**平级消费者**：
/// `SoftwareRhi` 把命令逐条转发回 `Painter`（行为与历史逐位一致），GPU 后端（D 轨后续
/// 切片）实现同一接口，按管线状态批量提交。本接口只定义「命令消费者」这一件事，不含
/// 资源生命周期 / present —— 那些属于后端自身的实现细节（见 `RhiSwapchain` / `RhiPipeline`）。
///
/// 设计取舍：接口收成**单一 `submit`**（而不是把 18 个绘制原语各设一个虚函数）。理由是
/// 命令的几何/标量已全在 `DrawCmd` 里，单入口既让回放循环保持一行，也把「如何解释命令、
/// 如何合并成批次」留给后端 —— GPU 后端正是靠这一点做管线切换与批处理，而 18 个平铺虚函数
/// 会强迫它在原语之间重新推断管线状态。`Painter` 侧无需改动。
class RhiBackend {
  public:
    RhiBackend() = default;
    virtual ~RhiBackend() = default;
    RhiBackend(const RhiBackend &) = delete;
    auto operator=(const RhiBackend &) -> RhiBackend & = delete;
    RhiBackend(RhiBackend &&) = delete;
    auto operator=(RhiBackend &&) -> RhiBackend & = delete;

    /// @brief 后端标识（诊断与自检用；如 `"software"`）。
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// @brief 提交一条已录制的绘制命令。
    /// @param cmd  命令（几何 / 颜色 / 标量；变长部分见 `data`）
    /// @param data 该命令引用的变长数据（不引用的字段为 `nullptr`）
    virtual auto submit(const DrawCmd &cmd, const CmdData &data) -> void = 0;

    // ---- 资源扩展面（可选能力；默认实现 = 不支持，调用方按返回值回退常规路径） ----

    /// @brief 能力位查询（构造时确定）。
    [[nodiscard]] virtual auto capabilities() const -> RhiCapabilities { return {}; }

    /// @brief 取流式图像槽（固定纹理槽复用，不走 content_hash 缓存、不参与通用缓存淘汰）。
    /// 同 `key` 重复获取复用同槽；尺寸变化时后端就地重定义纹理存储。
    /// @return 句柄；`0` = 后端不支持（调用方回退常规 `DrawImage` 上传路径）。
    [[nodiscard]] virtual auto acquire_stream_image(std::uint64_t key, int width, int height) -> StreamImageId {
        (void)key;
        (void)width;
        (void)height;
        return 0;
    }

    /// @brief 流式图像增量更新：按行跨距 + 脏矩形 sub-upload（GL `glTexSubImage2D`）。
    /// @param pixels       像素基址（RGBA8 直色，非预乘；PMA 由后端在采样/上传阶段处理）
    /// @param stride_bytes 行跨距字节数；`0` = 紧凑行（width*4）
    /// @param x,y,w,h      脏矩形（像素坐标，左上原点；全图更新传整图范围）
    virtual auto update_stream_image(StreamImageId id, const std::uint8_t *pixels, std::size_t stride_bytes, int x,
                                     int y, int w, int h) -> void {
        (void)id;
        (void)pixels;
        (void)stride_bytes;
        (void)x;
        (void)y;
        (void)w;
        (void)h;
    }

    /// @brief 释放流式图像槽（句柄此后无效；重复释放无害）。
    virtual auto release_stream_image(StreamImageId id) -> void { (void)id; }

    /// @brief 导入平台原生 GPU 表面（dmabuf / IOSurface / D3D11 共享纹理 / AHardwareBuffer）。
    /// @return 流式图像句柄；`0` = 不支持或导入失败（调用方回退 CPU 上传路径，单帧警告不刷屏）。
    [[nodiscard]] virtual auto import_native_surface(const NativeSurfaceFrame &frame) -> StreamImageId {
        (void)frame;
        return 0;
    }
};

}  // namespace aurora::rhi
