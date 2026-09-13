#pragma once

#include <string_view>

#include "aurora/render/rhi/rhi_backend.h"

namespace aurora {

class Painter;  // 前置声明（完整定义见 render/painter.h），避免 rhi/ ↔ painter.h 头耦合

namespace rhi {

/// @brief 软件 RHI 后端：把 `DisplayList` 命令**逐条转发**回 `Painter` 的对应原语。
///
/// 它是 RHI 抽象的首个（也是当前唯一）实现，语义上等价于 D 轨抽取前的
/// `DisplayList::replay(Painter&)`：每条命令映射到同一个 `Painter` 原语调用、参数逐字段一致，
/// 因此 **DC 像素输出逐位不变**（golden 回归红线）。存在的意义是给 `DisplayList` 一个
/// 「后端无关的回放目标」，使 GPU 后端成为第二个平级消费者，而不必改动 `DisplayList`。
///
/// 绑定目标绘制器由调用方负责：`Surface::rhi()` 默认绑定 `Surface::painter()`；
/// 未绑定时 `submit` 为 no-op（不崩溃，便于测试构造空后端）。
class SoftwareRhi final : public RhiBackend {
  public:
    SoftwareRhi() = default;
    explicit SoftwareRhi(Painter &painter) : painter_(&painter) {}

    /// @brief 绑定目标绘制器。可重复调用（`Surface::rhi()` 每次取用都重绑，保证与当前
    ///        `painter()` 一致）；`Painter` 生命周期与所属 `Surface` 一致。
    auto bind(Painter &painter) -> void { painter_ = &painter; }

    /// @brief 当前绑定的绘制器（未绑定时为 `nullptr`）。
    [[nodiscard]] auto painter() const -> Painter * { return painter_; }

    [[nodiscard]] auto name() const -> std::string_view override { return "software"; }

    /// @brief 执行一条命令（未绑定绘制器时为 no-op）。
    auto submit(const DrawCmd &cmd, const CmdData &data) -> void override;

  private:
    Painter *painter_ = nullptr;
};

}  // namespace rhi
}  // namespace aurora
