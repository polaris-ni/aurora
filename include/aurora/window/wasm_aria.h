#pragma once
#include "aurora/core/platform.h"  // NOLINT

// WASM/ARIA 无障碍桥（specification/06-app-platform.md §2.4 / ARCHITECTURE.md §8.4）：
// 仅在 defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM) 时提供。
//
// 形态：语义树快照 → 页面隐藏镜像容器（`<div id="aurora-a11y-<canvasId>">`，视觉隐藏但
// **保留在可访问性树中**）→ 读屏经浏览器原生 ARIA 支持消费。折算与序列化全部走中立层
// `aurora/window/detail/aria_protocol.h`（先纯后桥），本头只剩生命周期与反向动作回灌。
//
// 与原生桥的两处**刻意差异**（如实申报）：
//  - **无懒激活信号可用**（D14 例外）：浏览器没有 `WM_GETOBJECT`/D-Bus 查询那样的
//    「读屏来了」事件（navigator 无读屏探测 API），故首个语义树根注入即激活 —— 拉取式
//    （D9）仍成立：只有 dirty（结构/字段事件、换根）才重投影 + 发 ops，静止页面零 DOM  churn。
//  - **无几何面**：镜像元素不带画布坐标，读屏按 DOM 顺序导航（不支持「点按位置探测」）。
//
// 反向动作（读屏 → 控件）：JS 侧在镜像元素上挂 click/focus 监听，把 (id, 动作位) 写入
// 页面级队列；桥**自持 rAF 蹦床**每帧排水并回灌 `Widget::perform_accessibility_action`。
// 为何不由 `present()` 帧尾独扛：静止页面没有脏帧就没有 present，读屏动作/播报/增量同步
// 会饿死——rAF 自驱保证任何帧序下动作延迟 ≤1 拍。走**轮询队列**而非导出函数，是为了零
// 链接要求：消费者不必配 `-sEXPORTED_FUNCTIONS`，任何 Emscripten 设置下都成立（代价 =
// 动作下一拍生效，≤16ms，读屏交互无感）。标签页隐藏时 rAF 停摆（与帧循环同语义，如实申报）。
//
// @note Thread: main-thread only（浏览器单线程；与快照/帧循环同线程）

#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/a11y_provider.h"
#include "aurora/widget/a11y_diff.h"

namespace aurora {

class Widget;

/// @brief Wasm ARIA 桥：每窗口一实例（镜像容器按 canvas id 隔离，多窗口互不串树）。
class WasmAriaBridge final : public a11y::Provider {
  public:
    /// @param container_id 镜像容器 DOM id（惯例 `"aurora-a11y-" + canvas_id`，须全页唯一）
    explicit WasmAriaBridge(std::string container_id);
    ~WasmAriaBridge() override;

    /// @brief 激活：注册进桥广播表、置 `screen_reader_active`、请求首帧全量应用。
    ///
    /// WASM 无外部激活信号，本方法由**首个 `set_root`** 调用（见文件头 D14 例外申报）。
    auto activate() -> void override;
    /// @brief 去激活：注销广播表、清空并移除镜像容器（窗口销毁必经，防残留孤儿树）。
    auto deactivate() -> void override;
    /// @brief dirty 时重投影 + diff，产出全量/ops JSON 经 EM_JS 应用；并排水反向动作队列。
    auto sync_if_dirty() -> void override;
    auto mark_dirty() -> void override { dirty_ = true; }
    [[nodiscard]] auto is_active() const -> bool override { return active_; }

    /// @brief 注入语义树根（`Window::present_root` 每帧调用）。
    ///
    /// 幂等：同根重复注入仅在**换根**时置脏。首个非空根到达即激活（本桥的激活信号）。
    auto set_root(Widget *root) -> void override;
    /// @brief RTL 标志：随全量载荷写入容器 `dir` 属性；变化即强制下次全量。
    auto set_rtl(bool rtl) -> void override;

    /// @brief 动态播报（G4）→ 容器内 `aria-live="polite"` 区的文本替换。
    auto on_announcement(const std::string &text, const Widget *target) -> void override;
    /// @brief 根控件销毁：立即切断根并清空投影（子节点生死由下次重投影自然收敛）。
    auto on_widget_destroying(const Widget *w) -> void override;

    [[nodiscard]] auto name() const -> std::string override { return "wasm-aria"; }

    /// @brief 排水页面级反向动作队列：(id, 动作位) → 活快照查控件 → perform 回灌。
    ///
    /// 与 `sync_if_dirty()` 同由自驱 rAF 蹦床（`raf_tick`）每拍执行 —— 队列空时只是一次
    /// 廉价 JS 探测。动作只认**当前快照**在树的 id（已销毁者静默丢弃，读屏侧自然收敛）。
    auto pump_actions() -> void;

    /// @brief 存活桥表（跨窗口动作寻址：id → 所属桥 → 活控件快照）。构造入表、析构出表。
    ///        兼作 rAF 蹦床的悬垂守卫：旧拍触发时先查本表，不在表者即已析构，不触碰 userData。
    [[nodiscard]] static auto live_bridges() -> std::vector<WasmAriaBridge *> &;

  private:
    /// @brief 按控件指针在活快照中查 id（播报 target 解析用；未命中 0）。
    [[nodiscard]] auto id_of_widget(const Widget *w) const -> std::uint64_t;
    /// @brief 按 id 在活快照中查控件（动作回灌用；未命中 nullptr）。
    [[nodiscard]] auto widget_of_id(std::uint64_t id) const -> Widget *;
    /// @brief 重投影：构建新快照，首发全量、续发 ops，并替换活快照。
    auto rebuild_and_apply() -> void;
    /// @brief rAF 蹦床（自驱拍）：每拍 `sync_if_dirty` + `pump_actions` 后自我续订。
    ///        先以 `live_bridges()` 成员性判「实例尚活」再解引用 userData——析构后仍被
    ///        调度的旧拍正是靠这道守卫安全出局（`Application::raf_owner_` 同语义）。
    static auto raf_tick(double /*time*/, void *user_data) -> bool;

    std::string container_id_;
    Widget *root_ = nullptr;  ///< 非拥有裸根（生命周期由宿主 `present_root` 喂入/切断）
    a11y::TreeSnapshot snapshot_;  ///< 活快照（DOM 镜像与之同构；widget 指针仅本帧内有效）
    bool has_snapshot_ = false;  ///< false = 下次同步走全量载荷
    bool dirty_ = true;  ///< 拉取式脏位（D9）
    bool active_ = false;  ///< 生命周期闩（activate/deactivate 幂等）
    bool raf_pending_ = false;  ///< 已排一拍未落（防双链；落拍/出局时复位）
    bool rtl_ = false;
};

}  // namespace aurora

#endif  // AURORA_PLATFORM_WASM && AURORA_BACKEND_WASM
