#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/spring.h"
#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/diagnostics.h"
#include "aurora/event/gesture.h"
#include "aurora/event/keycode.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 可拖拽重排列表（specification/04-widget.md §3.4）：全量实例化子项 + 内建垂直滚动 +
 *        拖拽换位（跟手 / 让位 / 落位动画）。
 *
 * 对标 Flutter `ReorderableListView`、Qt `QListView::InternalMove`。
 *
 * **数据契约（控件直接改写数据）**：构造注入 `State<std::vector<T>>` + `ItemBuilder`；松手落位后
 * 控件**自己**改写该 vector（`std::rotate` 语义）并 `set()` 触发重建，`on_reorder(old, new)` 回调
 * 供宿主持久化（在数据已改写之后触发）。这样「UI 动了数据没动」的错误不会发生。
 *
 * **与 `LazyList` 的分工**：本控件**不虚拟化**（全量实例化，适合 <500 项），换取可变行高与拖拽
 * 换位的直接几何；长列表请用 `LazyList`（虚拟化重排不做，见 `LAYOUT_PLAN`【裁决 15】）。
 *
 * 滚动位置可经 `set_restore_key()` 接入 `app::ScrollStorage`（与四个滚动控件同一契约）。
 *
 * **键盘替代路径（可访问性，specification/04-widget.md §3.4）**：指针拖拽不是唯一入口，本控件
 * 默认可用键盘完成同样的换位——获焦即把光标落在**首个可见项**（不改滚动位置），`↑`/`↓`
 * （及 `Home`/`End`）移动**光标**，`Space`/`Enter` 抓取 / 落位，`Esc` 取消并回到原位。两步式
 * 「先抓取后移动」与 ARIA 拖放网格的读屏操作习惯一致（Google Sheets / Gmail 的 `Alt+↑↓`
 * 换位同构），抓取期间光标项按抬升态绘制。每一步都经 `Widget::announce()` 向读屏播报位置与
 * 结果（「位置 3 / 共 8 项」等）。
 *
 * 方向键能到达控件内部，依赖 `Widget::wants_navigation_keys()`：派发器默认把方向键用作几何
 * 焦点导航，覆写该钩子后本控件先观察按键、未认领的按键仍回落焦点导航。整体开关是
 * `set_keyboard_reorder(false)`（关闭后方向键 / 空格完全交回默认语义）。
 *
 * 播报文案走 i18n：键为 `aurora.reorder.position` / `.grabbed` / `.dropped` /
 * `.dropped_in_place` / `.cancelled`（宿主可经 `default_string_table()` 登记译文模板，占位符
 * 为 `{0}` 起的位置数字）；未登记时回退英文字面量，不会播报空串。
 *
 * @tparam T 列表项类型（须可拷贝）。
 * @note Thread: main-thread only
 * @note Rebuildable: no（数据源为运行时 State，工厂注册仅收录自描述元数据）
 */
template <typename T>
class ReorderableList : public Container {
  public:
    using ItemBuilder = std::function<Node(const T &, int)>;

    ReorderableList() = default;

    ReorderableList(std::shared_ptr<State<std::vector<T>>> items, ItemBuilder builder, float gap = 0.0F)
        : items_(std::move(items)), builder_(std::move(builder)),
          gap_(gap < 0.0F ? (Diagnostics::degraded("layout", "ReorderableList gap 负值已降级为 0"), 0.0F) : gap) {
        // 落位动画与近边缘 auto-scroll 均由每帧 tick 驱动（同 Dismissible/ToastHost 模式）。
        needs_gesture_tick_ = true;
        rebuild_if_needed();
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ReorderableList"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ReorderableList",
            .properties =
                {
                    {.name = "gap",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "项间距(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "scroll_offset",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "当前滚动偏移(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "restore_key",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "滚动位置保存键（空=不参与恢复）",
                     .json_type = "string",
                     .enum_values = {},
                     .min_value = ""},
                    {.name = "drag_handle",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "是否限定右侧手柄区域起拖（false=整项可拖）",
                     .json_type = "boolean",
                     .enum_values = {},
                     .min_value = ""},
                    {.name = "auto_scroll_threshold",
                     .type = "float",
                     .default_value = "48.0",
                     .required = false,
                     .note = "拖拽近边缘自动滚动的触发带高(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "keyboard_reorder",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "键盘重排替代路径开关（方向键移光标 + 空格抓取/落位 + Esc 取消）",
                     .json_type = "boolean",
                     .enum_values = {},
                     .min_value = ""},
                },
            .events = {"on_reorder"},
            .children_policy = "multiple",
            .invariants = {"gap >= 0", "auto_scroll_threshold >= 0"},
            .examples = {"au::ReorderableList<std::string>{items, [](const std::string& s, int i){ return "
                         "au::Node{au::Text(s)}; }, 8.0F}"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        if (items_) {
            out.push_back(static_cast<SignalViewBase *>(items_.get()));
        }
    }

    /// @brief 运行时态不可序列化：与 `Repeater` 同一处理（数据源是 `State`，条目由宿主回填）。
    auto serialize_props(Json &props) const -> void override {
        Container::serialize_props(props);
        props["gap"] = gap_;
        props["scroll_offset"] = offset_;
        props["restore_key"] = restore_key_;
        props["drag_handle"] = drag_handle_;
        props["auto_scroll_threshold"] = auto_scroll_threshold_;
        props["keyboard_reorder"] = keyboard_reorder_;
        props["note"] = "ReorderableList items are runtime-state driven, not serialized";
    }

    // ---- 数据 ----

    /// @brief 项数（= 已实例化子项数，二者同源）。
    [[nodiscard]] auto item_count() const -> std::size_t { return children_.size(); }

    /// @brief 数据快照副本（观测 / 测试用；热路径请走 `item_count()`）。
    [[nodiscard]] auto data() const -> std::vector<T> { return items_ ? items_->get() : std::vector<T>{}; }

    /// @brief 当前数据源（可为空）。
    [[nodiscard]] auto items() const -> const std::shared_ptr<State<std::vector<T>>> & { return items_; }

    /// @brief 换数据源（标脏重建）。
    auto set_items(std::shared_ptr<State<std::vector<T>>> items) -> ReorderableList & {
        items_ = std::move(items);
        invalidate();
        return *this;
    }

    /// @brief 换条目构造器（标脏重建）。
    auto set_item_builder(ItemBuilder builder) -> ReorderableList & {
        builder_ = std::move(builder);
        invalidate();
        return *this;
    }

    /// @brief 项间距（dp，链式）。
    auto set_gap(float gap) -> ReorderableList & {
        gap_ = gap < 0.0F ? 0.0F : gap;
        mark_needs_layout();
        return *this;
    }
    [[nodiscard]] auto gap() const -> float { return gap_; }

    /// @brief 强制下次布局重建全部子项（数据**同长度**但内容 / 顺序变化时使用；长度变化自动检测）。
    auto invalidate() -> void {
        built_ = false;
        mark_needs_layout();
    }

    /// @brief 重排回调 `(old_index, new_index)`：在数据**已改写之后**触发（供宿主持久化）。
    auto set_on_reorder(std::function<void(int, int)> cb) -> ReorderableList & {
        on_reorder_ = std::move(cb);
        return *this;
    }

    /// @brief 程序化重排：把第 `from` 项移动到落位后的最终下标 `to`（0..count-1）——
    ///        直接改写数据源并重建（`std::rotate` 语义，`to == from` 即无变化）。
    ///
    /// 与拖拽共用同一插入位语义：`drop_slot()` / `slot_for_center()` 的返回值可直接传入。
    /// @return 数据是否实际变化。
    auto reorder(int from, int to) -> bool {
        if (!items_) {
            return false;
        }
        // 本检查把 `items_->get()` 读成「智能指针上多余的 get()」：此处的 `get()` 属于 `State`（值
        // 读取口，返回 `const std::vector<T> &`），不是 `shared_ptr::get()`——按建议删掉就成
        // `items_.size()`，编译不过。仅浏览器口径命中——native 遍同一份代码不报
        // （CODING_STANDARDS.md §5.2 的口径差异）。
        // NOLINTNEXTLINE(readability-redundant-smartptr-get)
        const int n = static_cast<int>(items_->get().size());
        if (from < 0 || from >= n) {
            return false;
        }
        const int slot = std::clamp(to, 0, n - 1);
        if (slot == from) {
            return false;
        }
        std::vector<T> v = items_->get();
        if (slot > from) {
            // 后移：受影响区间 [from, slot] 整体旋转（末端须含 slot —— 少一即变成「插到 slot 前」）。
            std::rotate(v.begin() + from, v.begin() + from + 1, v.begin() + slot + 1);
        } else {
            // 前移：受影响区间 [slot, from]。
            std::rotate(v.begin() + slot, v.begin() + from, v.begin() + from + 1);
        }
        items_->set(std::move(v));  // 数据为准：先改数据，再重建 UI
        invalidate();
        notify_accessibility_structure_changed(this);
        if (on_reorder_) {
            on_reorder_(from, slot);
        }
        return true;
    }

    /// @brief 数据长度变化时重建子项（`Repeater` 先例；顺序变化请走 `reorder` / `invalidate`）。
    auto rebuild_if_needed() -> void {
        if (!items_ || !builder_) {
            return;
        }
        const std::vector<T> &data = items_->get();
        if (built_ && data.size() == built_count_) {
            return;
        }
        std::vector<Node> kids;
        kids.reserve(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            Node item = builder_(data[i], static_cast<int>(i));
            if (item) {
                kids.push_back(std::move(item));
            }
        }
        children_ = std::move(kids);
        built_count_ = data.size();
        built_ = true;
        tops_.assign(children_.size(), 0.0F);
        heights_.assign(children_.size(), 0.0F);
        clamp_keyboard_state_to_items();
    }

    // ---- 滚动 ----

    /// @brief 当前滚动偏移（dp，向下为正）。
    [[nodiscard]] auto scroll_offset() const -> float { return offset_; }

    /// @brief 设置滚动偏移（夹取到内容范围；变化时标脏并按 `restore_key` 写回）。
    /// @return 偏移是否实际变化。
    auto set_scroll_offset(float offset) -> bool {
        const float clamped = std::clamp(offset, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return false;
        }
        offset_ = clamped;
        scroll_restored_ = true;  // 外部程序化设置 / 用户滚动：视为已就位，不再被键恢复覆盖
        write_back_offset();
        mark_needs_paint();
        return true;
    }

    /// @brief 内容总高（y 表末项底边）。
    [[nodiscard]] auto content_height() const -> float { return content_h_; }

    /// @brief 最大滚动偏移（内容高 - 视口高，不小于 0）。
    [[nodiscard]] auto max_scroll_offset() const -> float { return std::max(0.0F, content_h_ - viewport_h_); }

    /// @brief 滚动位置保存键（空 = 不参与恢复）。
    [[nodiscard]] auto restore_key() const -> const std::string & { return restore_key_; }
    auto set_restore_key(std::string key) -> ReorderableList & {
        restore_key_ = std::move(key);
        return *this;
    }

    /// @brief 滚轮滚动。
    auto on_scroll(ScrollEvent &e) -> void override {
        e.is_handled = true;
        // 拖拽期间吞掉滚轮（本控件内建滚动只由 auto-scroll 驱动，避免跟手位移与滚动叠加）。
        if (is_dragging()) {
            return;
        }
        const float before = offset_;
        const float target = ScrollViewport::clamp_offset(before, e.delta_y, step_, content_h_, viewport_h_);
        // 余量回传（嵌套滚动协调）：端点被夹掉的部分上冒给更浅可滚动祖先。
        e.remaining_y = ScrollViewport::remaining_offset(before, target, e.delta_y, step_);
        if (target == before) {
            return;
        }
        offset_ = target;
        scroll_restored_ = true;
        write_back_offset();
        mark_needs_paint();
    }

    /// @brief 真实滚动控件：滚轮派发时本控件是可滚动目标（最深优先）。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    // ---- 几何观测（验收锚点）----

    /// @brief 第 `index` 项的**内容坐标**顶端（未叠加滚动偏移）；越界返回 0。
    [[nodiscard]] auto item_top(int index) const -> float {
        return (index >= 0 && static_cast<std::size_t>(index) < tops_.size()) ? tops_[static_cast<std::size_t>(index)]
                                                                              : 0.0F;
    }
    /// @brief 第 `index` 项的实测高度；越界返回 0。
    [[nodiscard]] auto item_height(int index) const -> float {
        return (index >= 0 && static_cast<std::size_t>(index) < heights_.size())
                   ? heights_[static_cast<std::size_t>(index)]
                   : 0.0F;
    }
    /// @brief 当前视口可见项范围 `[first, last)`（不含预取缓冲；本控件不虚拟化，全部项均已实例化）。
    [[nodiscard]] auto visible_range() const -> std::pair<int, int> {
        const int n = static_cast<int>(children_.size());
        if (n == 0 || viewport_h_ <= 0.0F) {
            return {0, 0};
        }
        const int first = std::clamp(index_at_content_y(offset_), 0, n - 1);
        const int last = std::clamp(index_at_content_y(offset_ + viewport_h_) + 1, first + 1, n);
        return {first, last};
    }
    /// @brief 内容坐标 `y` 落在第几项（间隙 / 越界取最近项；空表返回 -1）。
    [[nodiscard]] auto index_at_content_y(float y) const -> int {
        if (tops_.empty()) {
            return -1;
        }
        const auto it = std::upper_bound(tops_.begin(), tops_.end(), y);
        const auto idx = static_cast<std::ptrdiff_t>(it - tops_.begin()) - 1;
        return static_cast<int>(std::clamp<std::ptrdiff_t>(idx, 0, static_cast<std::ptrdiff_t>(tops_.size()) - 1));
    }

    /// @brief 视口局部 y（相对本控件原点）落在第几项；越界返回 -1（间隙 / 已滚出）。
    [[nodiscard]] auto index_at_viewport_y(float local_y) const -> int { return index_at_content_y(local_y + offset_); }

    // ---- 拖拽（C2 / C3）----

    /// @brief 是否正在跟手拖动。
    [[nodiscard]] auto is_dragging() const -> bool { return drag_state_ == DragState::Dragging; }
    /// @brief 是否正在落位动画（松手后到数据提交前）。
    [[nodiscard]] auto is_settling() const -> bool { return drag_state_ == DragState::Settling; }
    /// @brief 被拖项 index（-1 = 无）。
    [[nodiscard]] auto drag_index() const -> int { return drag_index_; }
    /// @brief 目标插入位（0..count-1；-1 = 无拖拽）。语义同 `reorder` 的 `to`。
    [[nodiscard]] auto drop_slot() const -> int { return drop_slot_; }
    /// @brief 被拖项当前跟手位移（内容坐标 dp；落位动画期间为动画值）。测试观测点。
    [[nodiscard]] auto drag_follow() const -> float { return drag_follow_; }

    /// @brief 是否限定「右侧手柄区域」起拖（false = 整项可拖）。
    auto set_drag_handle(bool handle_only) -> ReorderableList & {
        drag_handle_ = handle_only;
        return *this;
    }
    [[nodiscard]] auto drag_handle() const -> bool { return drag_handle_; }

    /// @brief 起拖识别阈值（dp；默认 8，测试用注入）。
    auto set_drag_slop(double slop_dp) -> ReorderableList & {
        drag_.slop = slop_dp;
        return *this;
    }

    /// @brief 拖拽近边缘自动滚动的触发带高（dp，0 = 关闭）。
    auto set_auto_scroll_threshold(float px) -> ReorderableList & {
        auto_scroll_threshold_ = px < 0.0F ? 0.0F : px;
        return *this;
    }
    [[nodiscard]] auto auto_scroll_threshold() const -> float { return auto_scroll_threshold_; }

    /// @brief 落位动画的弹簧参数（默认 `SpringDescription{}`）。
    auto set_spring(SpringDescription spring) -> ReorderableList & {
        spring_ = spring;
        return *this;
    }

    /// @brief 键盘重排开启时认领方向键：派发器先把 ↑/↓/←/→ 投递给 `on_key_event`，
    ///        本控件未认领的按键仍回落几何焦点导航（见 `Widget::wants_navigation_keys()`）。
    [[nodiscard]] auto wants_navigation_keys() const -> bool override { return keyboard_reorder_; }

    /// @brief 键盘重排开启时认领 Space / Enter（抓取 / 落位）；未认领则回落 `activate()`。
    [[nodiscard]] auto wants_activation_keys() const -> bool override { return keyboard_reorder_; }

    // ---- 键盘重排（可访问性替代路径）----

    /// @brief 键盘重排路径总开关（默认开启）。关闭后方向键 / 空格 / Enter 交回派发器默认语义
    ///        （几何焦点导航与 `activate()`），光标与抓取态一并清除。
    auto set_keyboard_reorder(bool enabled) -> ReorderableList & {
        keyboard_reorder_ = enabled;
        if (!enabled) {
            keyboard_grabbed_ = false;
            keyboard_grab_from_ = -1;
            keyboard_index_ = -1;
            mark_needs_paint();
        }
        return *this;
    }
    [[nodiscard]] auto keyboard_reorder() const -> bool { return keyboard_reorder_; }

    /// @brief 键盘光标所在项 index（-1 = 尚未定位；首次按方向键即落到首 / 末项）。
    [[nodiscard]] auto keyboard_index() const -> int { return keyboard_index_; }
    /// @brief 光标项是否处于「已抓取、未落位」态。
    [[nodiscard]] auto is_keyboard_grabbed() const -> bool { return keyboard_grabbed_; }
    /// @brief 抓取时的原始 index（-1 = 未抓取）。取消抓取即回到该位。
    [[nodiscard]] auto keyboard_grab_index() const -> int { return keyboard_grab_from_; }

    /// @brief 程序化移动键盘光标到第 `index` 项（夹取到 `[0, count-1)`，滚入视口并播报位置）。
    ///        抓取态下即等价「把被拾起项挪到目标位」，提交仍由 `drop_keyboard_item()` 完成。
    /// @return 光标是否实际变化。
    auto set_keyboard_index(int index) -> bool {
        const int n = static_cast<int>(children_.size());
        if (n == 0) {
            keyboard_index_ = -1;
            return false;
        }
        const int clamped = std::clamp(index, 0, n - 1);
        if (clamped == keyboard_index_) {
            return false;
        }
        keyboard_index_ = clamped;
        ensure_index_visible(clamped);
        mark_needs_paint();
        announce_position();
        return true;
    }

    /// @brief 键盘抓取光标项（等价按下 Space / Enter）；已抓取或无光标时为 no-op。
    /// @return 抓取态是否发生变化。
    auto grab_keyboard_item() -> bool {
        if (keyboard_grabbed_ || keyboard_index_ < 0) {
            return false;
        }
        keyboard_grabbed_ = true;
        keyboard_grab_from_ = keyboard_index_;
        mark_needs_paint();
        announce(announce_text(
            "aurora.reorder.grabbed",
            {LocalizedString{std::to_string(keyboard_index_ + 1)}, LocalizedString{std::to_string(item_count())}},
            "Position " + std::to_string(keyboard_index_ + 1) + " of " + std::to_string(item_count()) +
                ". Item grabbed. Use arrow keys to move,"
                " space to drop, escape to cancel."));
        return true;
    }

    /// @brief 键盘落位：把抓取项移到光标位（`reorder` 的插入位语义）并提交数据；未抓取为 no-op。
    /// @return 数据是否实际变化（原位落位返回 false，但抓取态已清除）。
    auto drop_keyboard_item() -> bool {
        if (!keyboard_grabbed_) {
            return false;
        }
        const int from = keyboard_grab_from_;
        const int to = keyboard_index_;
        keyboard_grabbed_ = false;
        keyboard_grab_from_ = -1;
        mark_needs_paint();
        const bool moved = reorder(from, to);
        if (moved) {
            announce(announce_text("aurora.reorder.dropped",
                                   {LocalizedString{std::to_string(from + 1)}, LocalizedString{std::to_string(to + 1)},
                                    LocalizedString{std::to_string(item_count())}},
                                   "Item moved from position " + std::to_string(from + 1) + " to position " +
                                       std::to_string(to + 1) + " of " + std::to_string(item_count()) + "."));
        } else {
            announce(announce_text(
                "aurora.reorder.dropped_in_place",
                {LocalizedString{std::to_string(to + 1)}, LocalizedString{std::to_string(item_count())}},
                "Item dropped at position " + std::to_string(to + 1) + " of " + std::to_string(item_count()) + "."));
        }
        return moved;
    }

    /// @brief 键盘取消：清除抓取态并把光标移回抓取前的位置（数据不变）；未抓取为 no-op。
    /// @return 抓取态是否发生变化。
    auto cancel_keyboard_grab() -> bool {
        if (!keyboard_grabbed_) {
            return false;
        }
        const int home = keyboard_grab_from_;
        keyboard_grabbed_ = false;
        keyboard_grab_from_ = -1;
        keyboard_index_ = home;
        ensure_index_visible(home);
        mark_needs_paint();
        announce(
            announce_text("aurora.reorder.cancelled",
                          {LocalizedString{std::to_string(home + 1)}, LocalizedString{std::to_string(item_count())}},
                          "Reorder cancelled. Item returned to position " + std::to_string(home + 1) + " of " +
                              std::to_string(item_count()) + "."));
        return true;
    }

    /// @brief 纯逻辑：给定「被拖项中心的内容坐标 y」求插入位（含 ±2dp 滞回，不依赖手势状态）。
    ///
    /// 是拖拽换位与 auto-scroll 判定的同一份几何（`slot_from_center`），供几何单测与宿主预演。
    /// @param from_index   被拖项 index
    /// @param center_y     被拖项中心的内容坐标 y
    /// @param current_slot 当前插入位（< 0 = 从 `from_index` 起算）
    [[nodiscard]] auto slot_for_center(int from_index, float center_y, int current_slot = -1) const -> int {
        const CompactGeometry g = build_compact_geometry(from_index);
        const int start = current_slot >= 0 ? current_slot : std::min(from_index, static_cast<int>(g.mids.size()));
        return slot_from_center(g, start, center_y, AURORA_HYSTERESIS);
    }

    /// @brief 插入位 `slot` 对应的目标顶端（内容坐标）。测试 / 落位动画共用。
    [[nodiscard]] auto target_top_for_slot(int slot) const -> float {
        const int from = drag_index_ >= 0 ? drag_index_ : 0;
        const CompactGeometry g = build_compact_geometry(from);
        const auto s = static_cast<std::size_t>(std::clamp(slot, 0, static_cast<int>(g.mids.size())));
        return s < g.tops.size() ? g.tops[s] : g.tail_top;
    }

    /// @brief 自描述中登记的属性表（供测试 / Inspector 读取）。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const Node &child : children_) {
            fn(child.widget());
        }
    }

    /// @brief 本控件自绘滚动 + 拖拽位移（每帧可变），禁自身 DL 缓存（同 Scroll）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    /// @brief `on_layout` 依赖运行时数据（State 驱动重建）与拖拽落位，非纯函数 ⇒ 禁布局缓存。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        rebuild_if_needed();
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 480.0F};
        }
        viewport_w_ = self.width;
        viewport_h_ = self.height;

        // 逐项测量并建 y 累计表（可变行高）：表是换位几何 / 命中 / 可见范围的唯一输入。
        const std::size_t n = children_.size();
        tops_.assign(n, 0.0F);
        heights_.assign(n, 0.0F);
        const Constraints item_c{.min = Size{.width = self.width, .height = 0.0F},
                                 .max = Size{.width = self.width, .height = Size::infinity().height}};
        float y = 0.0F;
        for (std::size_t i = 0; i < n; ++i) {
            Node &child = children_[i];
            child.widget().set_layout_parent(this);
            const Size s = child.widget().layout(item_c, ctx);
            const float h = std::max(0.0F, s.height);
            tops_[i] = y;
            heights_[i] = h;
            // 子 bounds 记为**内容坐标**：滚动偏移在绘制/命中期叠加（与 LazyList 一致）。
            child.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = y}, .size = Size{.width = self.width, .height = h}});
            y += h;
            if (i + 1 < n) {
                y += gap_;
            }
        }
        content_h_ = y;

        // 滚动位置恢复（首次可滚动布局时生效；须在夹取之前，使恢复在本帧即生效）。
        maybe_restore_scroll();
        // 内容高变化后偏移可能越界：夹取并重绘（不标布局脏，避免与本次布局互相触发）。
        const float clamped = std::clamp(offset_, 0.0F, max_scroll_offset());
        if (clamped != offset_) {
            offset_ = clamped;
            mark_needs_paint();
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口：子项在内容坐标下的局部 y 可能为负或越界，软件 Painter 直写内存缓冲，
        // 溢出坐标会越界访问（同 LazyList / Scroll 的 push_clip 约定）。
        p.push_clip(bounds);
        const int dragged = is_dragging() || is_settling() ? drag_index_ : -1;
        const int lifted = keyboard_lifted_index();
        for (int i = 0; i < static_cast<int>(children_.size()); ++i) {
            if (i == dragged || i == lifted) {
                continue;  // 被拖项 / 键盘抓取项最后绘制（视觉顶层）
            }
            paint_item(p, bounds, ctx, i);
        }
        if (lifted >= 0) {
            paint_lifted_item(p, bounds, ctx, lifted);
        }
        if (dragged >= 0) {
            paint_dragged_item(p, bounds, ctx, dragged);
        }
        paint_keyboard_cursor(p, bounds, ctx);
        p.pop_clip();
    }

    // 滚轮命中：整视口优先返回自身（同 LazyList——若子项优先，落在 Text 上的滚轮会被叶子吞掉，
    // 列表永不滚动）。指针事件仍走 on_hit_test_chain（保留子项点击命中与拖拽起拖）。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        // 手柄带由列表**自留**（不下降给子项）：条目自带 Clickable/Button 时会消费 Press，
        // 事件冒不到列表 ⇒ 整项拖拽起不来。手柄带是列表自己的作用域，故 `set_drag_handle(true)`
        // 在「条目可点击」的常见场景下依然能起拖。整项可拖模式（默认）则要求条目不吃指针事件。
        if (drag_handle_ && local.x >= bounds.size.width - AURORA_HANDLE_BAND) {
            return {};
        }
        // 逆向遍历：与绘制顺序一致（拖拽中的项绘制在顶层，故最先命中）。
        const int dragged = is_dragging() || is_settling() ? drag_index_ : -1;
        if (dragged >= 0) {
            const Rect cb = item_rect(bounds, dragged);
            if (cb.contains(local)) {
                // 拖拽期间屏蔽被拖项子控件的指针事件（裁决 18）：命中链止于本控件，
                // 松手事件仍由指针捕获回到这里，跟手不断。
                return {};
            }
        }
        for (int i = static_cast<int>(children_.size()) - 1; i >= 0; --i) {
            const Rect cb = item_rect(bounds, i);
            if (!cb.contains(local)) {
                continue;
            }
            const Rect global{.origin = bounds.origin + cb.origin, .size = cb.size};
            std::vector<HitNode> r =
                children_[static_cast<std::size_t>(i)].widget().hit_test_chain(local - cb.origin, global, ctx);
            if (!r.empty()) {
                return r;
            }
        }
        return {};
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Container::tick_gestures(now);  // 修饰链 + 子项 tick
        on_tick(now);
    }

    /// @brief 键盘重排按键入口：`↑`/`↓` 移光标（抓取态下即挪目标位）、`Home`/`End` 跳首末、
    ///        `Space`/`Enter` 抓取 ⇄ 落位、`Esc` 取消。
    ///
    /// 与基类默认实现的关键差别：**未认领的按键不置 `is_handled`**，以便派发器把方向键回落给
    /// 几何焦点导航、把 Enter/Space 回落给 `activate()`、把「未抓取时的 Esc」留给页面级返回。
    /// 指针拖拽 / 落位动画期间整条键盘路径让路（两套换位通道不并存，避免数据被双写）。
    auto on_key_event(KeyEvent &e) -> void override {
        if (!keyboard_reorder_ || e.action != KeyAction::Down || children_.empty() || drag_state_ != DragState::Idle) {
            return;
        }
        const int n = static_cast<int>(children_.size());
        switch (static_cast<KeyCode>(e.key)) {
            case KeyCode::ArrowUp:
                move_keyboard_cursor(-1);
                e.is_handled = true;
                return;
            case KeyCode::ArrowDown:
                move_keyboard_cursor(1);
                e.is_handled = true;
                return;
            case KeyCode::Home:
                set_keyboard_index(0);
                e.is_handled = true;
                return;
            case KeyCode::End:
                set_keyboard_index(n - 1);
                e.is_handled = true;
                return;
            case KeyCode::Space:
            case KeyCode::Enter:
                if (keyboard_grabbed_) {
                    drop_keyboard_item();
                } else {
                    grab_keyboard_item();
                }
                e.is_handled = true;
                return;
            case KeyCode::Escape:
                if (cancel_keyboard_grab()) {
                    e.is_handled = true;
                }
                return;
            default:
                return;  // 其余按键交回派发器默认语义
        }
    }

    /// @brief 获焦：光标未定位时落到**首个可见项**（并播报位置，读屏一进列表即知当前项）；
    ///        失焦：撤销未落位的抓取（数据未变，光标回原位）。
    ///
    /// 故意不落到 index 0：获焦不是滚动请求。若在此 `ensure_index_visible(0)`，一个已由
    /// `restore_key` 恢复到中段的列表会在获焦瞬间被拽回顶部（`itest_reorder` 曾以此抓到回归）。
    auto on_focus_change(bool focused) -> void override {
        Container::on_focus_change(focused);
        if (!focused) {
            cancel_keyboard_grab();
            return;
        }
        if (!keyboard_reorder_ || keyboard_index_ >= 0 || children_.empty()) {
            return;
        }
        const int n = static_cast<int>(children_.size());
        keyboard_index_ = std::clamp(visible_range().first, 0, n - 1);
        mark_needs_paint();
        announce_position();
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        handle_pointer(e);
        // 拖拽 / 落位中消费事件（父级与子项都不再响应点击）。
        if (is_dragging() || is_settling()) {
            e.is_handled = true;
        } else {
            Container::on_pointer_event(e);  // 修饰链（Clickable 等）照常
        }
    }

    auto on_pointer_event(TouchEvent &e) -> void override {
        // 原始触点只走修饰链：拖拽由派发器按每个触点**合成的 MouseEvent** 驱动（与
        // Draggable / LongPress 的既有语义一致）。若在此再喂同一个识别器，两条通道会互相
        // 覆盖 delta/轴锁定状态（识别器是单指针状态机）。
        if (is_dragging() || is_settling()) {
            e.is_handled = true;
            return;
        }
        Container::on_pointer_event(e);
    }

  private:
    // ---- 绘制辅助 ----

    /// @brief 第 `index` 项在本控件局部坐标系下的矩形（含滚动偏移与让位 / 跟手位移）。
    [[nodiscard]] auto item_rect(const Rect &bounds, int index) const -> Rect {
        const auto i = static_cast<std::size_t>(index);
        const float top = tops_[i] - offset_ + shuffle_offset(index) + drag_follow_offset(index);
        return Rect{.origin = Point{.x = 0.0F, .y = top},
                    .size = Size{.width = bounds.size.width, .height = heights_[i]}};
    }

    auto paint_item(Painter &p, const Rect &bounds, const BuildContext &ctx, int index) -> void {
        const Rect local = item_rect(bounds, index);
        if (local.origin.y + local.size.height <= 0.0F || local.origin.y >= bounds.size.height) {
            return;  // 完全在视口外：跳过（边界用包含性判定，避免贴边行被误绘）
        }
        const Rect global{.origin = bounds.origin + local.origin, .size = local.size};
        children_[static_cast<std::size_t>(index)].widget().paint(p, global, ctx);
    }

    /// @brief 被拖项：抬升（自绘阴影）+ 跟手位移绘制在顶层（裁决 16）。
    auto paint_dragged_item(Painter &p, const Rect &bounds, const BuildContext &ctx, int index) -> void {
        const Rect local = item_rect(bounds, index);
        const Rect global{.origin = bounds.origin + local.origin, .size = local.size};
        paint_drag_shadow(p, global, local);
        children_[static_cast<std::size_t>(index)].widget().paint(p, global, ctx);
    }

    // ---- 滚动恢复 ----

    auto apply_restored_offset(float raw) -> void {
        const float clamped = std::clamp(raw, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        offset_ = clamped;
        mark_needs_paint();
    }

    auto maybe_restore_scroll() -> void {
        if (scroll_restored_ || restore_key_.empty() || max_scroll_offset() <= 0.0F) {
            return;
        }
        scroll_restored_ = true;
        ScrollStorage::instance().claim(restore_key_, this);
        if (const auto stored = ScrollStorage::instance().read(restore_key_); stored.has_value()) {
            apply_restored_offset(*stored);
        }
    }

    auto write_back_offset() -> void {
        if (restore_key_.empty()) {
            return;
        }
        ScrollStorage::instance().write(restore_key_, offset_);
    }

    // ---- 拖拽几何 ----

    /// @brief 压缩序几何（把被拖项从序列里拿掉后的顶端 / 中点表）：换位判定与让位位移的唯一输入。
    struct CompactGeometry {
        std::vector<float> tops;  ///< 压缩序各项顶端（内容坐标）
        std::vector<float> mids;  ///< 压缩序各项中点
        float tail_top = 0.0F;  ///< 末尾插入位（slot = count-1）对应的顶端
    };

    [[nodiscard]] auto build_compact_geometry(int excluded_index) const -> CompactGeometry {
        CompactGeometry g;
        const int n = static_cast<int>(heights_.size());
        if (n <= 1 || excluded_index < 0 || excluded_index >= n) {
            return g;
        }
        g.tops.reserve(static_cast<std::size_t>(n - 1));
        g.mids.reserve(static_cast<std::size_t>(n - 1));
        float y = 0.0F;
        int oi = 0;
        for (int i = 0; i < n; ++i) {
            if (i == excluded_index) {
                continue;
            }
            g.tops.push_back(y);
            g.mids.push_back(y + (heights_[static_cast<std::size_t>(i)] / 2.0F));
            y += heights_[static_cast<std::size_t>(i)];
            if (oi + 1 < n - 1) {
                y += gap_;  // 压缩序内相邻项之间才有间距
            }
            ++oi;
        }
        g.tail_top = y;
        return g;
    }

    /// @brief 由「被拖项中心 y」与压缩序中点表求插入位；跨中点须超出 ±eps 才切换（滞回防抖）。
    [[nodiscard]] static auto slot_from_center(const CompactGeometry &g, int current_slot, float center, float eps)
        -> int {
        const int last_slot = static_cast<int>(g.mids.size());  // == count-1（插到末尾）
        int slot = std::clamp(current_slot, 0, last_slot);
        while (slot < last_slot && center > g.mids[static_cast<std::size_t>(slot)] + eps) {
            ++slot;
        }
        while (slot > 0 && center < g.mids[static_cast<std::size_t>(slot) - 1] - eps) {
            --slot;
        }
        return slot;
    }

    /// @brief 让位位移（绘制期偏移，裁决 17）：其余项按「移除被拖项后的目标序」直接位移。
    [[nodiscard]] auto shuffle_offset(int index) const -> float {
        if (drag_index_ < 0 || index == drag_index_ || drag_state_ == DragState::Idle) {
            return 0.0F;
        }
        if (static_cast<std::size_t>(index) >= tops_.size() || drag_geom_.mids.empty()) {
            return 0.0F;
        }
        const int oi = index < drag_index_ ? index : index - 1;
        const float base = drag_geom_.tops[static_cast<std::size_t>(oi)];
        const float final_top =
            (oi < drop_slot_) ? base : base + heights_[static_cast<std::size_t>(drag_index_)] + gap_;
        return final_top - tops_[static_cast<std::size_t>(index)];
    }

    /// @brief 被拖项的跟手 / 落位位移（内容坐标）。
    [[nodiscard]] auto drag_follow_offset(int index) const -> float {
        return (index == drag_index_ && drag_state_ != DragState::Idle) ? drag_follow_ : 0.0F;
    }

    /// @brief 被拖项抬升（自绘阴影；不污染子项修饰链，裁决 16）。
    auto paint_drag_shadow(Painter &p, const Rect &global, const Rect & /*local*/) -> void {
        p.draw_shadow(global, 0.0F, 6.0F, 12.0F, Color(0, 0, 0, 70));
    }

    // ---- 键盘重排辅助 ----

    /// @brief 键盘抓取项（抬升态绘制）；无抓取返回 -1。
    [[nodiscard]] auto keyboard_lifted_index() const -> int {
        return (keyboard_reorder_ && keyboard_grabbed_) ? keyboard_index_ : -1;
    }

    /// @brief 键盘抓取项：与拖拽项同一「阴影 + 顶层」抬升表达，让「已拾起」可被看见。
    auto paint_lifted_item(Painter &p, const Rect &bounds, const BuildContext &ctx, int index) -> void {
        const Rect local = item_rect(bounds, index);
        const Rect global{.origin = bounds.origin + local.origin, .size = local.size};
        paint_drag_shadow(p, global, local);
        children_[static_cast<std::size_t>(index)].widget().paint(p, global, ctx);
    }

    /// @brief 键盘光标环：焦点在本控件（或仍持有抓取）时才画，主题主色描边。
    auto paint_keyboard_cursor(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        if (!keyboard_reorder_ || keyboard_index_ < 0 || (!is_focused() && !keyboard_grabbed_)) {
            return;
        }
        const Rect local = item_rect(bounds, keyboard_index_);
        if (local.origin.y + local.size.height <= 0.0F || local.origin.y >= bounds.size.height) {
            return;  // 光标项已滚出视口（正常路径下 `ensure_index_visible` 会先滚回来）
        }
        const Rect global{.origin = bounds.origin + local.origin, .size = local.size};
        p.draw_rounded_border(global, AURORA_CURSOR_RING_RADIUS,
                              keyboard_grabbed_ ? AURORA_CURSOR_GRABBED_THICKNESS : AURORA_CURSOR_THICKNESS,
                              inherit_theme(ctx).primary);
    }

    /// @brief 相对移动光标；未定位时 `+1` 落到首项、`-1` 落到末项（与列表类控件的键盘习惯一致）。
    auto move_keyboard_cursor(int delta) -> void {
        const int n = static_cast<int>(children_.size());
        if (n == 0) {
            return;
        }
        set_keyboard_index(keyboard_index_ < 0 ? (delta >= 0 ? 0 : n - 1) : keyboard_index_ + delta);
    }

    /// @brief 把第 `index` 项滚入视口（贴边时各留一点余量由 clamp 处理）。
    auto ensure_index_visible(int index) -> void {
        if (index < 0 || static_cast<std::size_t>(index) >= tops_.size() || viewport_h_ <= 0.0F) {
            return;
        }
        const float top = item_top(index);
        const float bottom = top + item_height(index);
        if (bottom > offset_ + viewport_h_) {
            set_scroll_offset(bottom - viewport_h_);
        } else if (top < offset_) {
            set_scroll_offset(top);
        }
    }

    /// @brief 播报光标位置（「位置 i / 共 n 项」）——读屏在纯浏览态也得到反馈。
    auto announce_position() -> void {
        if (keyboard_index_ < 0) {
            return;
        }
        announce(announce_text(
            "aurora.reorder.position",
            {LocalizedString{std::to_string(keyboard_index_ + 1)}, LocalizedString{std::to_string(item_count())}},
            "Position " + std::to_string(keyboard_index_ + 1) + " of " + std::to_string(item_count()) + "."));
    }

    /// @brief 播报文案解析：按 key 查 `default_string_table()`（宿主可登记译文模板），
    ///        未登记时回退 `fallback` 英文字面量——绝不播报空串。
    [[nodiscard]] static auto announce_text(const std::string &key, std::vector<LocalizedString> args,
                                            const std::string &fallback) -> std::string {
        LocalizedString s;
        s.key = key;
        s.args = std::move(args);
        s.localize = true;
        s.text = fallback;  // StringTable::resolve 查表失败即用 text 兜底
        return s.resolve(&default_string_table(), Locale{});
    }

    /// @brief 数据长度变化后把光标夹回有效范围（保留「未定位」的 -1，不虚设光标）。
    auto clamp_keyboard_state_to_items() -> void {
        const int n = static_cast<int>(children_.size());
        if (n == 0) {
            keyboard_index_ = -1;
            keyboard_grabbed_ = false;
            keyboard_grab_from_ = -1;
            return;
        }
        if (keyboard_index_ >= n) {
            keyboard_index_ = n - 1;
        }
        if (keyboard_grab_from_ >= n) {
            keyboard_grab_from_ = n - 1;
        }
    }

    // ---- 拖拽状态机 ----

    /// @brief 按下：记录候选被拖项（含手柄判定）与项内抓取偏移。
    auto hint_press(const MouseEvent &e) -> void {
        press_index_ = -1;
        const float local_y = e.local_position.y;
        const int idx = index_at_content_y(local_y + offset_);
        if (idx < 0) {
            return;
        }
        const float top = tops_[static_cast<std::size_t>(idx)] - offset_;
        const float h = heights_[static_cast<std::size_t>(idx)];
        if (local_y < top || local_y >= top + h) {
            return;  // 落在间距 / 内容之外：不起拖
        }
        if (drag_handle_ && e.local_position.x < viewport_w_ - AURORA_HANDLE_BAND) {
            return;  // 限定手柄区域：按在项内非手柄处不起拖
        }
        press_index_ = idx;
    }

    /// @brief 超过 slop 起拖：锁定被拖项、建压缩序几何、初始化插入位。
    auto begin_drag() -> void {
        if (press_index_ < 0 || std::cmp_greater_equal(press_index_, heights_.size())) {
            return;
        }
        if (drag_.axis() != DragAxis::Vertical) {
            return;  // 本控件只做垂直重排：横向拖动不接管
        }
        drag_index_ = press_index_;
        drag_geom_ = build_compact_geometry(drag_index_);
        drop_slot_ = std::min(drag_index_, static_cast<int>(drag_geom_.mids.size()));
        drag_follow_ = 0.0F;
        release_velocity_ = 0.0F;
        has_move_sample_ = false;
        drag_state_ = DragState::Dragging;
        mark_needs_paint();
    }

    /// @brief 跟手推进：位移 1:1、夹在内容范围内，并按中心 y 重算插入位。
    auto update_drag(float delta_y) -> void {
        const auto i = static_cast<std::size_t>(drag_index_);
        const float min_follow = -tops_[i];
        const float max_follow = std::max(min_follow, content_h_ - heights_[i] - tops_[i]);
        const float clamped = std::clamp(delta_y, min_follow, max_follow);
        // 初速度估计：**帧间**位移 / 固定 60Hz 采样（仿 DragToDismiss：识别器不持时钟，
        // 量级误差由 spring 阻尼自然吸收）。首次移动没有上一个样本，速度取 0——
        // 否则会把「起拖到首次移动的整段位移」误当成一帧的速度（虚高 60 倍，落位会冲过头）。
        release_velocity_ = has_move_sample_ ? (clamped - drag_follow_) * AURORA_ASSUMED_FPS : 0.0F;
        has_move_sample_ = true;
        drag_follow_ = clamped;
        const float center = tops_[i] + (heights_[i] / 2.0F) + drag_follow_;
        const int slot = slot_from_center(drag_geom_, drop_slot_, center, AURORA_HYSTERESIS);
        if (slot != drop_slot_) {
            drop_slot_ = slot;
        }
        mark_needs_paint();
    }

    /// @brief 松手落位：spring 动画滑到目标槽位，静止后提交数据（裁决 19：自驱动 tick，不碰 Animator）。
    auto end_drag() -> void {
        const auto i = static_cast<std::size_t>(drag_index_);
        const float target = target_top_for_slot(drop_slot_) - tops_[i];
        // 初速度按帧间估计并夹在上界内（防极端拖速把落位拉飞）。
        const float velocity = std::clamp(release_velocity_, -AURORA_MAX_RELEASE_VELOCITY, AURORA_MAX_RELEASE_VELOCITY);
        // 位移可忽略 / 无障碍要求减弱动态：直接落位，不逐帧动画。
        if (current_accessibility_settings().reduce_motion || std::abs(target - drag_follow_) < AURORA_SETTLE_EPSILON) {
            drag_follow_ = target;
            commit_drop();
            mark_needs_paint();
            return;
        }
        spring_active_ = SpringSimulation(spring_, static_cast<double>(drag_follow_), static_cast<double>(target),
                                          static_cast<double>(velocity));
        settle_t_ = 0.0;
        drag_state_ = DragState::Settling;
        mark_needs_paint();
    }

    /// @brief 落位动画推进：spring 值直接作为被拖项位移；静止即提交数据。
    ///        提交后该项的自然位置正好等于动画终点 ⇒ 无跳变（落位几何与换位几何同源）。
    auto tick_settle(double dt) -> void {
        if (!spring_active_.has_value()) {
            commit_drop();
            mark_needs_paint();
            return;
        }
        settle_t_ += dt;
        if (current_accessibility_settings().reduce_motion || spring_active_->is_settled(settle_t_)) {
            drag_follow_ = static_cast<float>(spring_active_->target());
            spring_active_.reset();
            commit_drop();
            mark_needs_paint();
            return;
        }
        drag_follow_ = static_cast<float>(spring_active_->value(settle_t_));
        mark_needs_paint();
    }

    /// @brief 近边缘 auto-scroll：被拖项进入视口上下边缘带内时按侵入深度比例持续滚动（dp/s）。
    ///
    /// 跟手位移以**内容坐标**计，故滚动 Δ 后须给 `drag_follow_` 补同样的 Δ，被拖项才会停在
    /// 手指下方（屏幕上不动、列表从下面穿过去）。滚动只走内部程序化路径；滚轮在同轴冲突下已被
    /// 吞掉（裁决 18）。
    auto tick_auto_scroll(double dt) -> void {
        if (auto_scroll_threshold_ <= 0.0F || max_scroll_offset() <= 0.0F || drag_index_ < 0) {
            return;
        }
        const auto i = static_cast<std::size_t>(drag_index_);
        const float local_top = tops_[i] - offset_ + drag_follow_;
        const float local_bottom = local_top + heights_[i];
        const float band = std::min(auto_scroll_threshold_, viewport_h_ / 2.0F);
        float speed = 0.0F;  // dp/s，正 = offset 增大（内容上移、向下滚）
        if (local_top < band) {
            speed = -AURORA_AUTO_SCROLL_MAX_SPEED * (1.0F - (std::max(0.0F, local_top) / band));
        } else if (local_bottom > viewport_h_ - band) {
            speed = AURORA_AUTO_SCROLL_MAX_SPEED * (1.0F - (std::max(0.0F, viewport_h_ - local_bottom) / band));
        }
        if (speed == 0.0F) {
            return;
        }
        const float old_offset = offset_;
        offset_ = std::clamp(offset_ + (speed * static_cast<float>(dt)), 0.0F, max_scroll_offset());
        if (offset_ == old_offset) {
            return;
        }
        drag_follow_ += offset_ - old_offset;  // 屏幕位置守恒（见上方注释）
        // 内容坐标变了：重算插入位（与拖拽共用同一份几何）。
        const float center = tops_[i] + (heights_[i] / 2.0F) + drag_follow_;
        drop_slot_ = slot_from_center(drag_geom_, drop_slot_, center, AURORA_HYSTERESIS);
        mark_needs_paint();
    }

    /// @brief 提交重排：把被拖项移到插入位（数据为准），随后清空拖拽态。
    auto commit_drop() -> void {
        const int from = drag_index_;
        const int slot = drop_slot_;
        clear_drag_state();
        reorder(from, slot);
        mark_needs_paint();
    }

    auto clear_drag_state() -> void {
        drag_state_ = DragState::Idle;
        drag_index_ = -1;
        drop_slot_ = -1;
        drag_follow_ = 0.0F;
        release_velocity_ = 0.0F;
        has_move_sample_ = false;
        press_index_ = -1;
        drag_geom_ = CompactGeometry{};
        spring_active_.reset();
        settle_t_ = 0.0;
    }

    auto handle_pointer(MouseEvent &e) -> void {
        if (e.action == MouseAction::Press) {
            if (drag_state_ == DragState::Settling) {
                return;  // 落位动画未结束：先让它落定，避免数据与 UI 打架
            }
            hint_press(e);
            drag_.on_mouse(e);
            return;
        }
        if (e.action == MouseAction::Move) {
            drag_.on_mouse(e);
            if (drag_state_ == DragState::Idle && drag_.is_dragging()) {
                begin_drag();
            }
            if (drag_state_ == DragState::Dragging) {
                update_drag(drag_.delta().y);
            }
            return;
        }
        if (e.action == MouseAction::Release) {
            drag_.on_mouse(e);
            const bool was_dragging = drag_state_ == DragState::Dragging;
            if (was_dragging) {
                end_drag();
            }
            drag_.reset();
            press_index_ = -1;
        }
    }

    /// @brief 每帧推进：拖拽中走 auto-scroll、落位中走 spring；空闲零开销。
    auto on_tick(std::chrono::steady_clock::time_point now) -> void {
        const double dt =
            last_tick_.has_value() ? std::chrono::duration<double>(now - *last_tick_).count() : (1.0 / 60.0);
        last_tick_ = now;
        if (drag_state_ == DragState::Settling) {
            tick_settle(dt);
        } else if (drag_state_ == DragState::Dragging) {
            tick_auto_scroll(dt);
        }
    }

    enum class DragState : std::uint8_t { Idle, Dragging, Settling };

    std::shared_ptr<State<std::vector<T>>> items_;
    ItemBuilder builder_;
    std::function<void(int, int)> on_reorder_;
    float gap_ = 0.0F;
    std::size_t built_count_ = 0;
    bool built_ = false;

    std::vector<float> tops_;  ///< 内容坐标：每项顶端 y（布局期建立）
    std::vector<float> heights_;  ///< 每项实测高度
    float content_h_ = 0.0F;
    float viewport_w_ = 0.0F;
    float viewport_h_ = 0.0F;
    float offset_ = 0.0F;
    float step_ = 40.0F;  ///< 每单位滚轮增量对应的 dp
    std::string restore_key_;
    bool scroll_restored_ = false;
    bool drag_handle_ = false;
    float auto_scroll_threshold_ = 48.0F;

    // ---- 拖拽 / 落位态 ----
    DragState drag_state_ = DragState::Idle;
    DragRecognizer drag_;
    int drag_index_ = -1;  ///< 被拖项 index（-1 = 无）
    int drop_slot_ = -1;  ///< 目标插入位（0..count-1）
    int press_index_ = -1;  ///< 按下点命中的候选被拖项（-1 = 未命中项）
    float drag_follow_ = 0.0F;  ///< 被拖项跟手 / 落位位移（内容坐标 dp）
    float release_velocity_ = 0.0F;  ///< 松手初速度估计（dp/s，帧间差分）
    bool has_move_sample_ = false;  ///< 是否已有帧间速度样本（首次移动不算）
    CompactGeometry drag_geom_;  ///< 起拖时定稿的压缩序几何
    SpringDescription spring_{};  ///< 落位弹簧
    std::optional<SpringSimulation> spring_active_;  ///< 落位动画（Settling 期有效）
    double settle_t_ = 0.0;  ///< 落位动画已推进时间（秒）
    std::optional<std::chrono::steady_clock::time_point> last_tick_;  ///< 上一帧时间（求 dt）

    // ---- 键盘重排态 ----
    bool keyboard_reorder_ = true;  ///< 键盘替代路径开关（默认开启）
    int keyboard_index_ = -1;  ///< 键盘光标项 index（-1 = 未定位）
    bool keyboard_grabbed_ = false;  ///< 光标项是否已被键盘拾起
    int keyboard_grab_from_ = -1;  ///< 拾起时的原始 index（取消即回位）

    static constexpr float AURORA_HYSTERESIS = 2.0F;  ///< 换位滞回（dp，跨中点 ±2dp 内不切换）
    static constexpr float AURORA_HANDLE_BAND = 48.0F;  ///< 手柄区域宽度（dp，`drag_handle` 模式）
    static constexpr float AURORA_AUTO_SCROLL_MAX_SPEED = 600.0F;  ///< auto-scroll 最大速度（dp/s）
    static constexpr float AURORA_SETTLE_EPSILON = 0.5F;  ///< 位移小于该值直接落位（不做动画）
    static constexpr float AURORA_ASSUMED_FPS = 60.0F;  ///< 帧间速度估计的采样率假设（Hz）
    static constexpr float AURORA_MAX_RELEASE_VELOCITY = 3000.0F;  ///< 松手初速度上界（dp/s）
    static constexpr float AURORA_CURSOR_RING_RADIUS = 4.0F;  ///< 键盘光标环圆角（dp）
    static constexpr float AURORA_CURSOR_THICKNESS = 2.0F;  ///< 键盘光标环线宽（dp）
    static constexpr float AURORA_CURSOR_GRABBED_THICKNESS = 3.0F;  ///< 抓取态光标环线宽（dp，加粗以区分）
};

}  // namespace aurora
