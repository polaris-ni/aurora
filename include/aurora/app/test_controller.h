#pragma once

// ============================================================================
// test_controller.h — 无头 widget 测试驱动（对标 Flutter `WidgetTester`）。
// ----------------------------------------------------------------------------
// 给 App / 测试提供「渲染 → 交互 → 断言」闭环：内部以 `HeadlessSurface` + `Window`
// 驱动脏区间帧循环（`Window::present_root`，specification/06-app-platform.md §3.2），
// 交互复用 Inspector 的 simulate_* 合成事件（目标式语义），查找与断言走公共自描述
// 通道（`Widget::serialize_props` + 树前序遍历）。
//
// 需要无头后端：`AURORA_BACKEND_HEADLESS`（默认 ON）关闭时本头不声明该类，
// 消费者用 `#ifdef AURORA_BACKEND_HEADLESS` 分支处理（测试走 `AURORA_TEST_SKIP`）。
// ============================================================================

#ifdef AURORA_BACKEND_HEADLESS

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/widget/node.h"
#include "aurora/widget/props_io.h"

namespace aurora {

class Animator;
class Scheduler;
class Widget;
class Window;

/// @brief 测试驱动配置（构造时定形，运行期不可变以保证用例确定性）。
///
/// 定义在类外（而非嵌套）：嵌套类带默认成员初始化器时，其聚合体不能作为外层
/// 成员函数声明处的默认实参（"default member initializer ... required before the
/// end of its enclosing class"）。
struct TestControllerConfig {
    int width = 800;  ///< 视口宽（逻辑像素）
    int height = 600;  ///< 视口高（逻辑像素）
    double frame_seconds = 1.0 / 60.0;  ///< 每帧 dt（固定步长，不读真实时钟）
    std::string png_path;  ///< 非空时每次 present 写出 PNG（调试截图用）
};

/// @brief 无头 widget 测试驱动：持一棵 widget 树，按帧推进并完成交互与断言。
///
/// 典型用法：
/// @code
///   auto ok = Node{Button{"OK"}};
///   ok.set_id("ok");
///   TestController tc{Node{Column{ok, Text{"hello"}}}};
///   (void)tc.pump();                        // 挂载 + 布局 + 绘制首帧
///   const auto hits = tc.find_by_key("ok");
///   AURORA_TEST_REQUIRE_EQ(hits.size(), 1U);
///   (void)TestController::tap(hits.at(0));  // 点击触发 on_click
///   (void)tc.pump_and_settle();             // 收敛交互引发的重排/重绘/动画
///   (void)TestController::expect_prop(hits.at(0), "show", Json{true});
/// @endcode
///
/// @note Thread: main-thread only（与 `Window::present_root` 同约束）
/// @note Side-effects: `pump*` 会挂载/布局/绘制 widget 树；交互方法改变目标控件状态
/// @note Rebuildable: no（持有内部 Surface / tracing 会话状态）
class TestController {
  public:
    /// @brief 驱动配置：见 `TestControllerConfig`（历史名 `TestController::Config`）。
    using Config = TestControllerConfig;

    /// @brief 持有 root 并建立无头窗口（尺寸在首帧前即确立，避免整树被布局到 0×0 而白屏）。
    /// @param root 被测 widget 树的根（所有权转移给本控制器）。
    /// @param cfg  视口与帧参数；宽/高 <= 0 时回退到 800×600。
    explicit TestController(Node root, const TestControllerConfig &cfg = TestControllerConfig{});

    TestController(const TestController &) = delete;
    auto operator=(const TestController &) -> TestController & = delete;
    TestController(TestController &&other) noexcept;
    auto operator=(TestController &&other) noexcept -> TestController &;
    ~TestController();

    // ── 帧驱动 ──

    /// @brief 推进若干帧：每帧依次 tick 手势计时 → Animator → Scheduler → present。
    /// @param frames 帧数；<= 0 视为 1。
    /// @return present 失败返回带信息的 Error（如 Surface 失效）；否则空 Result。
    /// @note 帧序与 `Application::run` 一致（唯一差异：无跨线程回投排水的 drain_posted）。
    [[nodiscard]] auto pump(int frames = 1) -> Result<void>;

    /// @brief 连续推进直到「无事可做」——某帧为 idle 跳帧且无运行中动画。
    /// @param max_frames 上限帧数（防死循环）；达到上限即停。
    /// @return 实际推进的帧数；== max_frames 表示未在预算内收敛（调用方据此断言）。
    [[nodiscard]] auto pump_and_settle(int max_frames = 60) -> int;

    /// @brief 改变视口尺寸：改 Surface 尺寸并强制下一帧全量重排重绘。
    /// @note 宽/高 <= 0 时不改动，返回 `GeneralInvalidArgument`。
    [[nodiscard]] auto set_viewport(int width, int height) -> Result<void>;

    /// @brief 已呈现帧数（`HeadlessSurface::frame_count()`；idle 跳帧不计数）。
    [[nodiscard]] auto frame_count() const -> int;

    /// @brief 根节点（几何权威在 Node：`bounds()`）。
    [[nodiscard]] auto root_node() -> Node &;
    /// @brief 根 widget（事件派发根）。
    [[nodiscard]] auto root() -> Widget &;

    /// @brief 内部哑窗口（脏区帧语义；供需要 `Window` 的高级断言使用）。
    [[nodiscard]] auto window() -> Window &;
    /// @brief 内部动画管理器（用户注册的 `AnimationController` 经它每帧推进）。
    [[nodiscard]] auto animator() -> Animator &;
    /// @brief 内部定时任务调度器（`Timer` 控件 / `set_timeout` 按帧 dt 推进）。
    [[nodiscard]] auto scheduler() -> Scheduler &;

    // ── 查找（树前序遍历；返回全部命中，顺序＝先序）──

    /// @brief 按节点标识查找（`Node::set_id` 设置的 key）。
    [[nodiscard]] auto find_by_key(std::string_view id) const -> std::vector<Node>;
    /// @brief 按控件类型名查找（`Widget::type_name()`）。
    [[nodiscard]] auto find_by_type(std::string_view type) const -> std::vector<Node>;
    /// @brief 按文本内容查找：逐个比对文本类属性 `content|text|label|value|hint|placeholder`。
    /// @note 启发式匹配——各控件的文本属性名不统一；需精确语义时用 `expect_prop` 断言具体具名属性。
    [[nodiscard]] auto find_by_text(std::string_view text) const -> std::vector<Node>;

    // ── 交互（目标式语义：坐标相对目标控件自身，详见 Inspector::simulate_*）──
    //
    // 交互成功后会在内部窗口登记「下一帧全量重绘」，模拟真实平台每个输入事件都唤醒
    // 帧循环的行为——否则仅改内部 State 的交互（如合成文本输入）不产生窗口级脏标记，
    // 紧随其后的 pump 会被判为 idle 跳过、像素停留在交互前（实测结论，见 utest）。

    /// @brief 点击目标：Press + Release（中心）。
    [[nodiscard]] auto tap(Widget &w) -> Result<void>;
    /// @brief 点击目标节点（内部拷一份 `Node` 共享同一 widget，故可接受 const 实参）。
    [[nodiscard]] auto tap(const Node &n) -> Result<void>;
    /// @brief 拖拽目标：Press（中心）→ Move（中心 + delta）→ Release。
    [[nodiscard]] auto drag(Widget &w, const Point &delta) -> Result<void>;
    /// @brief 拖拽目标节点。
    [[nodiscard]] auto drag(const Node &n, const Point &delta) -> Result<void>;
    /// @brief 文本输入：置焦后向目标派发 `TextInputEvent`。
    [[nodiscard]] auto enter_text(Widget &w, std::string_view text) -> Result<void>;
    /// @brief 文本输入（节点重载）。
    [[nodiscard]] auto enter_text(const Node &n, std::string_view text) -> Result<void>;

    // ── 断言（失败返回带信息的 Error，便于调用方包装为测试失败原因）──

    /// @brief 断言节点可见：`show` 属性为真且已参与布局/绘制（非空几何）。
    /// @note 判定依据最近一次帧的几何，故须先 pump 至少一帧。
    [[nodiscard]] static auto expect_visible(const Node &n) -> Result<void>;

    /// @brief 断言属性值等于期望（值经 `Widget::serialize_props` 读出的 JSON 比对）。
    /// @note Json 字面量陷阱：`Json{"hello"}` 在 nlohmann 语义下是**数组** `["hello"]`，
    ///       字符串期望值须写成 `Json(std::string{"hello"})`（布尔用 `Json(true)`）。
    [[nodiscard]] static auto expect_prop(const Widget &w, std::string_view key, const Json &expected) -> Result<void>;
    /// @brief 断言属性值等于期望（节点重载）。
    [[nodiscard]] static auto expect_prop(const Node &n, std::string_view key, const Json &expected) -> Result<void>;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_HEADLESS
