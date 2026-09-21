#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"

namespace aurora {

class Painter;  // 前置声明（完整定义见 render/painter.h），避免 rhi/ ↔ painter.h 头耦合

namespace rhi {

/// @brief 软件 RHI 后端：把 `DisplayList` 命令**逐条转发**回 `Painter` 的对应原语。
///
/// 它是 RHI 抽象的首个实现（当前另有 GPU 实现 `GpuGlRhi`，见 §8.7），语义上等价于 D 轨抽取前的
/// `DisplayList::replay(Painter&)`：每条命令映射到同一个 `Painter` 原语调用、参数逐字段一致，
/// 因此 **DC 像素输出逐位不变**（golden 回归红线）。存在的意义是给 `DisplayList` 一个
/// 「后端无关的回放目标」，使 GPU 后端成为第二个平级消费者，而不必改动 `DisplayList`。
///
/// 绑定目标绘制器由调用方负责：`Surface::rhi()` 默认绑定 `Surface::painter()`；
/// 未绑定时 `submit` 为 no-op（不崩溃，便于测试构造空后端）。
///
/// 层命令（BeginLayer/EndLayer/DrawLayer）仿真：捕获 → 离屏定稿 → 层位图存储。
/// 默认落**进程级全局存储**（GPU 录制的 DL 回放至软件时逐帧构造临时实例——回退帧与
/// 离屏渲染共用一份存储，干净帧 DrawLayer 才能跨帧命中）；容量上限 64 条，超限清空
/// 并 bump 层代际（下帧全量重录，正确性不变）；DrawLayer 未命中同样 bump（单帧缺口自愈）。
class SoftwareRhi final : public RhiBackend {
  public:
    SoftwareRhi() = default;
    explicit SoftwareRhi(Painter &painter) : painter_(&painter) {}
    /// @brief 离屏回放构造：与父实例**共享层存储**（嵌套层经内层 DrawLayer 命中外层刚定稿的内容）。
    SoftwareRhi(Painter &painter, std::unordered_map<std::uint64_t, Image> *shared_store)
        : painter_(&painter), layer_store_(shared_store) {}

    /// @brief 绑定目标绘制器。可重复调用（`Surface::rhi()` 每次取用都重绑，保证与当前
    ///        `painter()` 一致）；`Painter` 生命周期与所属 `Surface` 一致。
    auto bind(Painter &painter) -> void { painter_ = &painter; }

    /// @brief 当前绑定的绘制器（未绑定时为 `nullptr`）。
    [[nodiscard]] auto painter() const -> Painter * { return painter_; }

    [[nodiscard]] auto name() const -> std::string_view override { return "software"; }

    /// @brief 执行一条命令（未绑定绘制器时为 no-op）。
    auto submit(const DrawCmd &cmd, const CmdData &data) -> void override;

  private:
    /// @brief 层捕获帧：BeginLayer 起缓冲命令，EndLayer 时离屏重放定稿层位图。
    struct LayerCapture {
        std::uint64_t key = 0;
        int width = 0;   ///< 层逻辑宽（dp）
        int height = 0;  ///< 层逻辑高（dp）
        std::vector<std::pair<DrawCmd, CmdData>> cmds;
    };

    Painter *painter_ = nullptr;
    /// @brief 层位图存储覆盖（键 → 直色 RGBA 位图）。空 = 用进程级全局层存储（默认）；
    /// 显式共享指针供离屏子实例与测试隔离场景（嵌套层经内层 DrawLayer 命中外层刚定稿的内容）。
    std::unordered_map<std::uint64_t, Image> *layer_store_ = nullptr;
    /// @brief DrawLayer 未命中告警去重（键级一次性，避免逐帧刷屏）。
    std::unordered_set<std::uint64_t> layer_miss_warned_;
    std::vector<LayerCapture> layer_captures_;  ///< 嵌套层捕获栈（空 = 不在层内）

    /// @brief 定稿捕获栈顶层：离屏重放捕获命令为层位图并入存储（嵌套层内层先定稿）。
    auto finalize_layer_capture() -> void;
};

}  // namespace rhi
}  // namespace aurora
