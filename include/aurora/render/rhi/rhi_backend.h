#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/transform.h"
#include "aurora/render/display_list.h"

namespace aurora::rhi {

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
};

}  // namespace aurora::rhi
