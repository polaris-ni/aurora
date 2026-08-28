#pragma once

#include <unordered_map>

#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 事件派发器：命中测试 + 同步派发（架构 §4.5 / §5.4）。
 *
 * - `hit_test`：在根树上找到最深可命中 widget（含 `Clickable` 修饰拦截），
 *   并返回「最深目标 → 根」的完整命中链（供冒泡派发复用）。
 * - `dispatch(MouseEvent)`：沿命中链自最深目标向根冒泡，逐级调用 `on_pointer_event`；
 *   任一控件设 `e.handled = true` 即终止冒泡；Press 时缓存命中链实现指针捕获。
 * - `dispatch(KeyEvent, FocusManager)`：Tab / Shift+Tab 触发焦点移动，否则只派发到焦点 widget（不冒泡）。
 * - `dispatch(ScrollEvent)`：派发到命中目标 widget 的 `onScroll`（不冒泡）。
 * - `dispatch(TextInputEvent, FocusManager)`：派发到当前焦点 widget 的 `onTextInput`（不冒泡）。
 * - `dispatch(FileDropEvent)`：派发到命中目标 widget（不冒泡）。
 *
 * 冒泡并不依赖 widget 父链指针：`hit_test_chain` 在命中时已把整条祖先链一并返回，
 * `deliver_chain` 直接沿该链自最深（链尾）向根（链头）派发并 stop-on-handled。
 * 触摸事件为「原始多点流全链广播 + 合成手势流冒泡」双路径。事件回调内写 `State` 触发 §5.1 定点刷新。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
class EventDispatcher {
  public:
    /// @brief 命中测试：返回根树下坐标 p 处最深可命中的 widget；无则 nullptr。
    /// @param root 命中测试的根节点；命中范围即以其 `size()` 构成的根矩形（原点 (0,0)）。
    /// @param p    待命中的坐标（根节点局部坐标系）。
    /// @return 最深可命中的 widget 指针；命中链为空（点到空白）时返回 nullptr。
    static auto hit_test(Widget &root, const Point &p) -> Widget * {
        return root.hit_test(p, Rect{ .origin = Point{}, .size = root.size() }, BuildContext{});
    }

    /// @brief 同步派发指针事件到命中目标；返回是否命中（handled 由 widget 写入 e）。
    /// @param root 派发起点（根 widget）；命中测试与命中链均局限于该子树。
    /// @param e    待派发的鼠标事件；沿命中链冒泡期间任一控件可写 `e.handled = true` 终止冒泡。
    /// @param fm 派发期间的当前焦点管理器（可选）；`request_focus()` 读取之，默认 nullptr 时焦点请求静默 no-op。
    /// @note 该静态入口委托进程内「持久」EventDispatcher 单例，因此同样保留跨事件指针捕获
    ///       （与 Application::m_mouse 行为一致）。这意味着即使调用方直接走静态 `dispatch`
    ///       （如 `run_demo` 调用 `EventDispatcher::dispatch`），按下后的拖选/拖拽在光标越过
    ///       目标边界或移出窗口时仍持续派发给按下时命中的目标，不会丢失选择。指针捕获表本身
    ///       仍以「实例成员」形式存于该持久单例（非全局静态），避免控件按值/栈管理时缓存悬空 Widget*。
    static auto dispatch(Widget &root, MouseEvent &e, FocusManager *fm = nullptr) -> bool;

    /// @brief 带指针捕获的鼠标派发（实体方法）：Press 命中后即缓存命中链，
    ///       后续 Move/Release 即使命中测试失败（光标移出根/窗口外、或落入重叠的兄弟控件）
    ///       也持续派发给同一目标，直到 Release 解除捕获。修复「拖选时光标移出窗口/重叠区后
    ///       释放事件丢失、选择卡住」以及「相邻控件重叠导致拖选被抢」的问题。
    /// @param root 派发起点（根 widget）；命中测试与指针捕获均局限于该子树。
    /// @param e    待派发的鼠标事件；冒泡期间任一控件可写 `e.handled = true` 终止冒泡。
    /// @param fm   派发期间的当前焦点管理器（可选）；Press 时据此转移/清除焦点，nullptr 则跳过焦点处理。
    /// @return 是否命中到任意控件（是否「消费」由 `e.handled` 表达）。
    auto dispatch_mouse(Widget &root, MouseEvent &e, FocusManager *fm = nullptr) -> bool;

    /// @brief 同步派发键盘事件；Tab/Shift+Tab 触发焦点移动，否则派发到焦点 widget。
    /// @param root 派发起点（根 widget）；键盘派发不经命中链，该形参未使用（仅为统一重载签名而保留）。
    /// @param e    待派发的键盘事件；命中快捷键或控件消费时写 `e.handled = true`。
    /// @param fm 焦点管理器（提供焦点 widget 与 Tab 序导航）；无则丢弃键盘事件。
    /// @return 事件是否被处理（焦点移动或焦点 widget 消费）。
    static auto dispatch(Widget &root, KeyEvent &e, FocusManager &fm) -> bool;

    /// @brief 同步派发滚轮事件到命中目标；返回是否命中。
    /// @param root 派发起点（根 widget）；在其子树内做命中测试确定滚动目标。
    /// @param e    待派发的滚轮事件（不冒泡，仅交给命中链最深叶节点）。
    /// @return 是否命中到可滚动目标。
    static auto dispatch(Widget &root, ScrollEvent &e) -> bool;

    /// @brief 同步派发文件拖放事件到命中目标；返回是否命中（handled 由 widget 写入 e）。
    /// @param root 派发起点（根 widget）；在其子树内按落点坐标做命中测试。
    /// @param e    待派发的文件拖放事件（不冒泡，仅交给命中目标）。
    /// @return 事件是否被目标控件消费（取 `e.handled`）。
    static auto dispatch(Widget &root, FileDropEvent &e) -> bool;

    /// @brief 同步派发文本输入事件到焦点 widget；无焦点则返回 false。
    /// @param root 派发起点（根 widget）；文本输入只路由到焦点控件，不经命中链。
    /// @param e    待派发的文本输入事件（不冒泡）。
    /// @param fm   焦点管理器；其当前焦点 widget 为唯一接收者，无焦点则直接返回 false。
    /// @return 事件是否被焦点控件消费（取 `e.handled`）。
    static auto dispatch(Widget &root, TextInputEvent &e, FocusManager &fm) -> bool;

  private:
    /// @brief 鼠标（pointer_id 缺省）捕获键：鼠标无 pointer id，用此哨兵键与触控 id 区分。
    static constexpr int AURORA_MOUSE_CAPTURE_KEY = -1;

    /// @brief 悬停追踪：无捕获 Move 时把新命中链与上次悬停链 diff，对离开/进入的控件
    ///       分别回调 `on_hover_change(false/true)`（Checkbox 等据此绘制 hover 反馈）。
    auto update_hover(const std::vector<HitNode> &chain) -> void;

    /// @brief 按指针 ID 缓存的命中链（指针捕获表）。Press 命中后写入，Release 清除；
    ///       活跃期 Move/Release 复用该链（即使光标移出根/窗口外或落入重叠兄弟控件），
    ///       保证拖选/拖拽连续。以实例形式存在（由 Application 持有一个），非全局静态。
    std::unordered_map<int, std::vector<HitNode>> m_pointer_capture;

    /// @brief 上次无捕获 Move 的悬停命中链。
    ///        HitNode.widget 为 `std::weak_ptr<Widget>`：虚拟列表（LazyList）等滚动回收子控件时，
    ///        悬停链中对应的弱引用失效，`update_hover` 经 `lock()` 检测后安全跳过，绝不解引用悬垂指针。
    std::vector<HitNode> m_hover_chain;
};

/// @brief 多点触控派发器（按指针 ID 做命中链捕获）。
///
/// 与 `EventDispatcher`（无状态、每次重新命中测试）不同，本类持有按 pointer id 缓存的命中链，
/// 以支持「单指持发 + 多指并发」：某指针按下时对其做命中测试并缓存整条命中链（指针捕获），
/// 活跃期所有该指针事件复用该链（即使手指移出初始控件仍路由到它），抬起即清除。
///
/// 该表以 **实例** 形式存在（由 `Application` 持有一个、测试每次构造一个），而非全局静态，
/// 避免跨 widget 子树 / 场景复用悬空 `Widget*`（控件按值 / 栈管理，无法用 `weak_ptr` 安全缓存）。
/// @note Thread: main-thread only
/// @note Side-effects: none
class TouchDispatcher {
  public:
    /// @brief 多点触控派发：按 `TouchPoint::id` 做指针捕获。
    ///        每个触点同时：① 把完整 `TouchEvent` 交给命中链（原始流，`touch()` 修饰器 / `PinchRecognizer` 消费）；
    ///        ② 合成对应 `MouseEvent`（携带 `pointer_id`）驱动 `Draggable`/`LongPress`/`Clickable`。
    /// @param root 派发起点（根 widget）；按其子树做命中测试并建立指针捕获。
    /// @param e    待派发的多点触控事件（`TouchEvent::points` 逐点处理，手势流冒泡可写 `handled`）。
    /// @param fm   派发期间的当前焦点管理器（可选）；触点首次按下时据此转移焦点，nullptr 则跳过。
    /// @return 是否有任意触点命中（handled 由各 widget 写入 e）。
    auto dispatch(Widget &root, TouchEvent &e, FocusManager *fm = nullptr) -> bool;

  private:
    /// @brief 按指针 ID 缓存的命中链（指针捕获表）。某指针 `id` 由活跃→非活跃（抬起）时清除对应链，避免悬空引用。
    std::unordered_map<int, std::vector<HitNode>> m_pointer_capture;
};

} // namespace aurora
