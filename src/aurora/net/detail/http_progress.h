// http_progress.h —— 发起侧进度回调的合流闸（CHG-010 §12「进度节流」；仅库实现可见，不属公共 API）。
//
// 为何单列：合流规则本身是一个有独立判据的状态机（「槽值 + 闸号 + 脏位」三者如何决定发射），而它一旦写进
// `Client::send` 的协程体，就只能靠真连接、真对端切块才能测——那既测不出「同帧至多一次」，也测不出末值
// 有没有丢。落在这里，零 socket、零时钟即可逐值断言发射次序；发起侧只负责在两个位置各叫一声：字节到了记
// 一笔、可发射的机会到了冲一次。
//
// 四条口径（规范性，落码与文档回写以此为准）：
// - **写入与发射解耦**：`observe()` 只更新槽值与脏位，**永不**调用回调；发射只发生在 `flush()`（可发射的
//   机会）与 `finish()`（收尾）两处。本库是响应式框架，回调直连字节到达会把「对端切得碎」放大成重绘风暴。
// - **闸有两型，缺省是回合闸**：装了帧号源就以帧号为闸（同一帧号内每方向至多一发），未装则以宿主回合为闸
//   （`flush()` 每调一次算一个回合）。两型都是「至多一次」，区别只在闸号由谁推进——宿主有帧循环时用帧号，
//   headless 与单测由回合数决定，判据因此可精确断言而不必依赖挂钟。
// - **每方向的末值恒发一次**：上传与下载各占一枚槽，`finish()` 无视闸号把仍脏的槽发出去。否则调用方无法
//   靠回调判定「收了摊」，只能去比字节数——那是把协议事实塞进调用方的算术里。
// - **空回调即整个闸不工作**：未挂回调的请求一发都不发，写路径也保持「一次写完」的原形态，进度这件观测口
//   因此对无观测需求的调用方零成本、零行为改动。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "aurora/net/http.h"

namespace aurora::net::detail {

/// @brief 进度合流闸：按方向各持一枚最新值槽，在可发射的机会上至多各发一次。
///
/// 生命周期随一条发起侧协程：`send` / `open_stream` 在帧内持有一个实例，字节到达处 `observe()`、每次续体
/// 恢复处 `flush()`、收尾处 `finish()`。非线程安全（发起侧协程恒在主线程续体上跑）。
/// @note Thread: single-threaded（随一条协程帧串行使用）
/// @note Side-effects: flush / finish 调用调用方回调
class ProgressGate {
  public:
    /// @brief 以发射口与可选帧号源构造。
    /// @param handler 调用方回调（可为空 = 本闸不工作）。
    /// @param frame_source 帧号源；空 = 用内部回合计数作闸（回合闸）。
    explicit ProgressGate(aurora::net::http::ProgressHandler handler, std::function<std::uint64_t()> frame_source = {});

    /// @brief 本闸是否有发射口（无回调时所有入口都应为廉价空操作）。
    /// @return 挂了回调为 true。
    [[nodiscard]] auto armed() const -> bool { return static_cast<bool>(handler_); }

    /// @brief 记录某方向的最新进度事实（不发射）。
    /// @param progress 进度值；按其 `phase` 落到对应槽。
    auto observe(const aurora::net::http::Progress &progress) -> void;

    /// @brief 一次可发射机会：闸号变了就把脏槽各发一次。
    ///
    /// 未装帧号源时本调用自增回合号（即每次调用都是一个新回合）；装了则闸号由帧号源给出，同一帧内多次调用
    /// 只有第一次能发。
    auto flush() -> void;

    /// @brief 收尾：把仍脏的槽各发一次，不看闸号（每方向的末值恒发是硬要求）。
    auto finish() -> void;

    /// @brief 当前闸号（观测口，供测试与日志判「这一发落在哪一回合/哪一帧」）。
    /// @return 装了帧号源为其返回值；否则为内部回合号。
    [[nodiscard]] auto gate() const -> std::uint64_t;

  private:
    /// @brief 每个方向一枚槽：最新值 + 脏位 + 上次发射时的闸号。
    struct Slot {
        std::optional<aurora::net::http::Progress> value;  ///< 最新进度（尚未发射时为非空脏值）
        std::uint64_t emitted_gate{0};  ///< 上次发射时的闸号；与 `gate()` 相同即本机会不发
        bool emitted_any{false};  ///< 是否发过（未发过时闸号无意义）
    };

    /// @brief 取某方向的槽下标（枚举值即下标，越界由调用方的枚举保证）。
    /// @param phase 进度方向。
    /// @return 槽下标。
    [[nodiscard]] static auto slot_index(aurora::net::http::ProgressPhase phase) -> std::size_t;

    aurora::net::http::ProgressHandler handler_;  ///< 发射口
    std::function<std::uint64_t()> frame_source_;  ///< 帧号源（空 = 回合闸）
    std::uint64_t turn_{0};  ///< 回合闸的回合号（无帧号源时才推进）
    std::array<Slot, 2> slots_{};  ///< 上传 / 下载各一枚
};

}  // namespace aurora::net::detail
