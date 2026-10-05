#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/diagnostics.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 虚拟滚动列表（specification/04-widget.md §3.4）：仅实例化可见区域 + 预取缓冲区的子项。
///
/// 与 `Repeater` 区分：`Repeater` 展开全部子项（适合 <100 项），`LazyList`
/// 仅构建可见窗口内的子项（适合 1000+ 项），滚出窗口的实例被回收。
///
/// 当前实现为固定行高模式（`item_extent`），可精确计算可见范围与总内容高度；
/// 可变行高模式作为后续增强。
///
/// 对标 Flutter `ListView.builder`、Qt `QListView`+delegate、WPF `VirtualizingStackPanel`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json（标量属性回填；条目须宿主经 `set_item_builder` 挂上）
/// 本行隐式生成的拷贝/移动构造逐成员复制 std::function 回调 builder_，而其拷贝与 operator()
/// 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
/// 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
/// 刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class LazyList : public Widget {
  public:
    /// @brief 条目构建器类型：按下标产出条目控件句柄。
    /// @param index 条目序号（0..count-1）。
    using ItemBuilder = std::function<Node(int index)>;

    /// @brief 默认构造：0 项、无 builder、默认行高 48。
    LazyList() = default;
    /// @brief 带参构造：固定行数与行高；非正行高降级为 48 并记 degraded。
    /// @param count 总项数（负值按 0 处理）。
    /// @param builder 条目构建器（移动接管）。
    /// @param item_extent 固定行高(dp)，默认 48。
    LazyList(int count, ItemBuilder builder, float item_extent = 48.0F)
        : count_(count < 0 ? 0 : count), builder_(std::move(builder)),
          item_extent_(
              item_extent > 0.0F
                  ? item_extent
                  : (Diagnostics::degraded("layout", "LazyList item_extent is not positive, degraded to 48"), 48.0F)) {
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    /// @brief 类型名 "LazyList"。
    /// @return 类型名常量串。
    [[nodiscard]] auto type_name() const -> const char * override { return "LazyList"; }

    /// @brief 静态描述符：count/item_extent/scroll_offset/cache_extent/restore_key 与吸附属性元数据。
    /// @return 本控件类型的 WidgetDescriptor。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "LazyList",
            .properties =
                {
                    {.name = "count",
                     .type = "int",
                     .default_value = "0",
                     .required = true,
                     .note = "Total item count",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "item_extent",
                     .type = "float",
                     .default_value = "48.0",
                     .required = false,
                     .note = "Fixed row height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "scroll_offset",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Current scroll offset (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "cache_extent",
                     .type = "float",
                     .default_value = "200.0",
                     .required = false,
                     .note = "Prefetch buffer beyond the viewport (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "restore_key",
                     .type = "string",
                     .default_value = "",
                     .required = false,
                     .note = "Scroll position save key (empty = excluded from restore)",
                     .json_type = "string",
                     .enum_values = {},
                     .min_value = ""},
                    {.name = "snap_extent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Snap period (dp); <=0 disables, ignored when snap_paging=true",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "snap_paging",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Paging mode: snap one viewport height per page",
                     .json_type = "boolean",
                     .enum_values = {},
                     .min_value = ""},
                    {.name = "snap_alignment",
                     .type = "ScrollSnapAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "Snap alignment (Start/Center/End)",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End"},
                     .min_value = ""},
                },
            .events = {},
            .children_policy = "none",
            .invariants = {"count >= 0", "item_extent > 0"},
            .examples = {"au::LazyList(10000, [](int i){ return au::Text(std::to_string(i)); }, 48.0F)"},
        };
    }
    /// @brief 实例描述符：转发 describe_static()。
    /// @return 本控件的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 信号收集：本控件无内建 SignalView，保持输出为空。
    /// @param out 信号视图累加表。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 总项数。
    /// @return 条目总数（构造/反序列化/`set_count` 后恒 ≥0）。
    [[nodiscard]] auto count() const -> int { return count_; }

    /// @brief 运行期改总项数（`count` 的可写入口）。
    ///
    /// 语义：
    /// - **越界**：负值按 0 处理（与带参构造、`deserialize_props` 同一口径，不分叉）。
    /// - **状态保持**：下标仍在新范围内的**存活条目不重建**——其内部状态（展开态、输入内容、
    ///   滚动位置、子控件身份）原样保留；只有滚出可见窗口或超出新范围的条目被回收，下次进入
    ///   窗口时由 `ItemBuilder` 重新构建。这正是「运行期改条目数」相对「重建整个列表」的
    ///   价值所在。
    /// - **回收口径**：新范围外的存活实例立即销毁（`live_` 中 `index >= count` 的条目）。
    ///   若当前焦点落在被回收的条目上，焦点随之失效（`FocusManager` 持弱引用，见
    ///   `focused()`）——宿主若需保留焦点应先把焦点移到仍在范围内的条目上。
    /// - **偏移夹取**：条目数减少后若当前偏移超出新的可滚范围，按新范围夹取并标脏，
    ///   否则列表会停在内容末尾之外的空白处。
    ///
    /// @param count 新的总项数（负值按 0 处理）。
    /// @note Side-effects: may recycle live items, clamps scroll offset, marks layout dirty
    auto set_count(int count) -> void {
        const int target = (count < 0) ? 0 : count;
        if (target == count_) {
            return;
        }
        count_ = target;
        // 立即回收越界实例（不等下一次布局）：否则 set_count 之后到重排之间，
        // live_ 里仍留着已不存在的条目，命中链会命中它们。
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first >= count_) {
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        // 条目减少后偏移可能越界：按新范围夹取（apply_offset 内部判重，值未变则不标脏）。
        apply_offset(offset_, /*cancel_glide=*/true);
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 当前滚动偏移（dp，向下为正）。
    /// @return 实时偏移，已钳制在 [0, max_scroll_offset()]。
    [[nodiscard]] auto scroll_offset() const -> float { return offset_; }

    /// @brief 设置滚动偏移（钳制到内容范围；外部跳转语义，会作废进行中的收位滑动）。
    /// @param offset 目标偏移(dp)，越界按内容范围夹取。
    auto set_scroll_offset(float offset) -> void { apply_offset(offset, /*cancel_glide=*/true); }

    /// @brief snap/paging 吸附（默认关闭，须显式配置）：每次滚轮收位后经短滑动吸附到条目对齐点。
    ///        `ScrollSnap::page()` 以视口高为一页；reduce-motion 下直落端点。
    /// @return 吸附配置（引用，宿主可只读检视）。
    [[nodiscard]] auto snap() const -> const ScrollSnap & { return snap_; }
    /// @brief 设置吸附配置（链式）。
    /// @param snap 吸附配置（拷贝接管）。
    /// @return *this（链式）。
    auto set_snap(ScrollSnap snap) -> LazyList & {
        snap_ = snap;
        return *this;
    }

    /// @brief 滚动位置保存键（空 = 不参与恢复；控件重建后据 `app::ScrollStorage` 恢复偏移）。
    /// @return 保存键常量引用。
    [[nodiscard]] auto restore_key() const -> const std::string & { return restore_key_; }
    /// @brief 设置滚动位置保存键（链式）。
    /// @param key 保存键（移动接管；空串关闭恢复）。
    /// @return *this（链式）。
    auto set_restore_key(std::string key) -> LazyList & {
        restore_key_ = std::move(key);
        return *this;
    }

    /// @brief 滚动到指定项（使其顶端对齐可视区顶端）。
    /// @param index 目标项下标（越界按 [0, count-1] 夹取）。
    /// @param animate true = 经收位滑动过渡（reduce-motion 下自动直落端点）；false = 立即就位。
    auto scroll_to_item(int index, bool animate = false) -> void {
        const float target = static_cast<float>(std::clamp(index, 0, std::max(0, count_ - 1))) * item_extent_;
        if (!animate || current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return;
        }
        scroll_restored_ = true;  // 同 set_scroll_offset：外部程序化设置视为已就位
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    /// @brief 程序化滚动到指定偏移（snap 对齐点由调用方决定；本接口只做夹取）。
    /// @param offset 目标偏移(dp)，夹取到 [0, max_scroll_offset()]。
    /// @param animate true = 经收位滑动过渡（reduce-motion 下直落）；false = 立即就位。
    /// @return 目标与当前偏移不同（即发生了移动或启动滑动）时为 true。
    auto scroll_to(float offset, bool animate = true) -> bool {
        const float target = std::clamp(offset, 0.0F, max_scroll_offset());
        if (target == offset_) {
            return false;
        }
        if (!animate || current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return true;
        }
        scroll_restored_ = true;  // 同 set_scroll_offset：外部程序化设置视为已就位
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
        return true;
    }

    /// @brief 是否正在收位滑动（测试/外部控制器观测点）。
    /// @return 滑动进行中为 true。
    [[nodiscard]] auto is_gliding() const -> bool { return glide_.active; }

    /// @brief 滚动偏移只读信号（滚动驱动动画原语）：宿主以纯函数派生视差/进度/淡入淡出。
    ///        懒创建；偏移每次变化（滚轮/滑动帧/程序化）写入。
    /// @note Side-effects: reads state (registers reactive dependency in Effect scope)
    /// @return 偏移信号视图引用（懒创建后恒有效）。
    [[nodiscard]] auto offset_signal() -> SignalView<float> & {
        if (!offset_state_) {
            offset_state_ = std::make_shared<State<float>>(offset_);
            published_offset_ = offset_;
        }
        return *offset_state_;
    }

    /// @brief 最大滚动偏移（内容高 - 视口高，不小于 0）。
    /// @return max(0, content_height() - 视口高)，单位 dp。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        return std::max(0.0F, content_height() - viewport_height_);
    }

    /// @brief 总内容高度。
    /// @return count × item_extent，单位 dp。
    [[nodiscard]] auto content_height() const -> float { return static_cast<float>(count_) * item_extent_; }

    /// @brief 当前可见范围 [first, last)（含 cache_extent 缓冲）。
    /// @return 半开区间；无可视项（空表/零行高/零视口）时为 (0, 0)。
    [[nodiscard]] auto visible_range() const -> std::pair<int, int> {
        if (count_ == 0 || item_extent_ <= 0.0F || viewport_height_ <= 0.0F) {
            return {0, 0};
        }
        const float lo = std::max(0.0F, offset_ - cache_extent_);
        const float hi = offset_ + viewport_height_ + cache_extent_;
        const int first = std::clamp(static_cast<int>(std::floor(lo / item_extent_)), 0, count_);
        const int last = std::clamp(static_cast<int>(std::ceil(hi / item_extent_)), 0, count_);
        return {first, last};
    }

    /// @brief 当前存活（已实例化）的子项数（测试观测点：应远小于 count）。
    /// @return live_ 表大小。
    [[nodiscard]] auto live_item_count() const -> std::size_t { return live_.size(); }

    /// @brief 设置预取缓冲区高度（链式）。
    /// @param extent 缓冲(dp)，负值按 0 处理。
    /// @return *this（链式）。
    auto set_cache_extent(float extent) -> LazyList & {
        cache_extent_ = extent < 0.0F ? 0.0F : extent;
        return *this;
    }

    /// @brief 挂上 / 换掉条目构建器（`LazyRow::set_item_builder` 同族）。
    ///
    /// `ItemBuilder` 是运行时回调、不参与序列化，故 `from_json` 重建出的列表「属性齐备而暂无条目」——
    /// 本接口就是那句「由宿主回填」的落点：宿主要么在此挂 builder，要么直接以带参构造建表。
    /// 赋值后标布局脏，下一帧按当前窗口构建条目。
    /// @param builder 新条目构建器（移动接管）。
    /// @return *this（链式）。
    auto set_item_builder(ItemBuilder builder) -> LazyList & {
        builder_ = std::move(builder);
        mark_needs_layout();
        return *this;
    }

    /// @brief 滚轮滚动。
    /// @param e 滚轮事件（读 delta_y，写 is_handled/remaining_y）。
    auto on_scroll(ScrollEvent &e) -> void override {
        const float before = offset_;
        set_scroll_offset(offset_ - (e.delta_y * AURORA_SCROLL_STEP));
        e.is_handled = true;
        // 余量回传（嵌套滚动协调）：端点被夹掉的部分上冒给更浅可滚动祖先。
        e.remaining_y = ScrollViewport::remaining_offset(before, offset_, e.delta_y, AURORA_SCROLL_STEP);
        // snap/paging：收位后向条目对齐点短滑动收束（reduce-motion 下直落端点）。
        if (snap_.enabled(viewport_height_)) {
            begin_snap_glide();
        }
    }

    /// @brief 真实滚动控件：滚轮派发时本控件是可滚动目标（最深优先）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    /// @brief 序列化标量属性（行数/行高/偏移/缓冲/保存键/吸附）。
    /// @param props 目标 JSON 对象（基类通用属性先写入）。
    auto serialize_props(Json &props) const -> void override {
        // 先链入基类通用属性。
        Widget::serialize_props(props);
        props.set("count", count_);
        props.set("item_extent", item_extent_);
        props.set("scroll_offset", offset_);
        props.set("cache_extent", cache_extent_);
        props.set("restore_key", restore_key_);
        props.set("snap_extent", snap_.extent);
        props.set("snap_paging", Json{snap_.paging});
        props.set("snap_alignment", snap_alignment_to_json(snap_.alignment));
    }

    /// @brief 从静态 JSON 回填标量属性。虚拟化条目仍持运行时 `ItemBuilder`（不可序列化），
    ///        故重建出的是「属性齐备、暂无条目」的列表——宿主挂上 builder 即照常工作。
    ///        非正值一律按 `Diagnostics::degraded` 降级，判据与构造器逐字一致（避免
    ///        「构造路径夹取、反序列化路径直写」的双标）。
    /// @param props 源 JSON 对象（仅回填存在的标量属性）。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("count")) {
            const int declared = props.at("count")->as_or<std::int32_t>(0);
            count_ = declared < 0 ? 0 : declared;
        }
        if (props.contains("item_extent")) {
            const auto declared = props.at("item_extent")->as_or<float>(0.0F);
            item_extent_ =
                declared > 0.0F
                    ? declared
                    : (Diagnostics::degraded("layout", "LazyList item_extent is not positive, degraded to 48"), 48.0F);
        }
        if (props.contains("cache_extent")) {
            set_cache_extent(props.at("cache_extent")->as_or<float>(0.0F));
        }
        if (props.contains("restore_key")) {
            set_restore_key(props.at("restore_key")->as_or<std::string>(""));
        }
        if (props.contains("snap_extent")) {
            snap_.extent = props.at("snap_extent")->as_or<float>(0.0F);
        }
        if (props.contains("snap_paging")) {
            snap_.paging = props.at("snap_paging")->as_or<bool>(false);
        }
        if (props.contains("snap_alignment")) {
            snap_.alignment = json_to_snap_alignment(*props.at("snap_alignment"));
        }
        if (props.contains("scroll_offset")) {
            // 显式偏移优先于 restore_key 恢复（见 maybe_restore_scroll）：记入 pending 待布局后应用。
            pending_offset_ = props.at("scroll_offset")->as_or<float>(0.0F);
            scroll_restored_ = false;
        }
    }

    /// @brief 遍历当前存活子项（滚出窗口的实例已回收，不在遍历范围）。
    /// @param fn 对每个存活子控件调用一次。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &val : live_ | std::views::values) {
            fn(val.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 480.0F};
        }
        viewport_height_ = self.height;
        // 滚动位置恢复（首次可滚动布局时生效）——须在计算可见窗口之前，使恢复在本帧即生效。
        maybe_restore_scroll();

        // 计算可见窗口并同步存活实例：新进入的构建、滚出的回收
        const auto [first, last] = visible_range();

        // 回收滚出窗口的实例
        for (auto it = live_.begin(); it != live_.end();) {
            if (it->first < first || it->first >= last) {
                it = live_.erase(it);
            } else {
                ++it;
            }
        }
        // 构建新进入窗口的实例
        if (builder_) {
            for (int i = first; i < last; ++i) {
                if (!live_.contains(i)) {
                    Node item = builder_(i);
                    if (item) {
                        item.widget().mount(ctx);
                        live_.emplace(i, std::move(item));
                    }
                }
            }
        }
        // 布局存活实例（固定行高，宽度撑满视口）
        Constraints item_c;
        item_c.min = Size{.width = self.width, .height = item_extent_};
        item_c.max = Size{.width = self.width, .height = item_extent_};
        for (auto &kv : live_) {
            kv.second.widget().set_layout_parent(this);
            kv.second.widget().layout(item_c, ctx);
            const float y = (static_cast<float>(kv.first) * item_extent_) - offset_;
            kv.second.set_bounds(
                Rect{.origin = Point{.x = 0.0F, .y = y}, .size = Size{.width = self.width, .height = item_extent_}});
        }
        return c.constrain(self);
    }

    /// @brief 吸顶覆盖层：把窗口内「已滚过头顶」的 sticky 项按 pin 位压顶绘制于视口顶部，
    ///        后到的头部把先前的向上顶出（与 Scroll 的覆盖层同一语义，虚拟化下只看窗口内项）。
    /// @param p 绘制器（转发给被钉驻项的 paint）。
    /// @param bounds 本控件绘制区域（钉驻矩形以此为基准偏移）。
    /// @param ctx 构建上下文（转发给被钉驻项）。
    /// @note 须在滚动位绘制**之后**调用，钉驻头部才能压在内容之上。
    auto paint_sticky_overlay(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
        std::vector<Node *> stickies;  // index 升序（live_ 为有序 map）
        for (auto &kv : live_ | std::views::values) {
            if (kv.widget().is_sticky_header()) {
                stickies.push_back(&kv);
            }
        }
        float stack = 0.0F;
        for (std::size_t i = 0; i < stickies.size(); ++i) {
            Node &node = *stickies[i];
            const float h = std::min(node.bounds().size.height, viewport_height_);
            if (h <= 0.0F || node.bounds().origin.y >= 0.0F) {
                continue;  // 尚未滚过头顶：随窗口正常滚动绘制
            }
            // 顶出规则同 Scroll：下一条 sticky 的视口顶部逼近时，本条被它向上顶出；最后一条到底。
            const float next_top =
                (i + 1 < stickies.size()) ? stickies[i + 1]->bounds().origin.y : viewport_height_ + h;
            const float pin_y = std::min(stack, next_top - h);
            if (pin_y + h <= 0.0F || pin_y >= viewport_height_) {
                continue;  // 已整条离开视口（其位置由顶出它的后继占据）
            }
            node.widget().paint(p,
                                Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + pin_y},
                                     .size = Size{.width = bounds.size.width, .height = node.bounds().size.height}},
                                ctx);
            stack = pin_y + h;
        }
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 裁剪到视口：软件 Painter 直接写内存缓冲，子项（含 cache 区，局部 y 可能为负或越界）
        // 若绘制坐标溢出缓冲会触发访问越界（0xC0000005）。必须裁剪到视口，与 scroll.h 的
        // ScrollingWidget 一致。
        p.push_clip(bounds);
        for (auto &kv : live_ | std::views::values) {
            const Rect cb = kv.bounds();
            // 完全在视口外的跳过绘制（缓冲区实例保留但不绘制）；用包含性边界判断，
            // 避免浮点边界处（cb 恰好贴边）的行被误绘到视口外。
            if (cb.origin.y + cb.size.height <= 0.0F || cb.origin.y >= bounds.size.height) {
                continue;
            }
            // 已滚过头顶的 sticky 项不在滚动位绘制（其可见性完全由下方覆盖层承载，
            // 跳过条件与 paint_sticky_overlay 的入口条件逐项一致，否则留空洞）。
            if (cb.origin.y < 0.0F && kv.widget().is_sticky_header()) {
                continue;
            }
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            kv.widget().paint(p, global, ctx);
        }
        // 覆盖层最后绘制：钉驻头部压在滚动内容之上（与 Scroll 的 blit-后-压顶同序）。
        paint_sticky_overlay(p, bounds, ctx);
        p.pop_clip();
    }

    // 滚轮命中：整视口优先返回自身（与 Scroll 一致）。若改回「子项优先」，
    // 光标落在子项（Text）时 hit_test 解析为叶控件，ScrollEvent 派发到叶子后 on_scroll
    // 为空操作，列表永不滚动。指针事件仍走 on_hit_test_chain（保留子项点击命中）。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        // 视口钳位与本控件 on_hit_test / on_paint 同口径：点在视口外一律不命中。虚拟化使 live_
        // 里留有视口外的缓存条目（cache_extent 预取），不加钳位时「条目已滚出视口、其覆盖绘制
        // 区却仍被祖先的闸认」⇒ 肉眼不可见的区域变得可点。
        const Rect viewport{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size};
        if (!viewport.contains(local)) {
            return {};
        }
        for (auto &kv : live_ | std::views::values) {
            const Rect cb = kv.bounds();
            // 闸并入条目的追加命中盒：条目内控件的覆盖绘制区（如条目里的展开面板）画在条目盒外，
            // 只按条目盒判定会拿不到点击（与 `Container::on_hit_test_chain` 同口径）。
            if (cb.contains(local) ||
                kv.widget().covers_extra_hit_box(local - cb.origin, ctx, bounds.origin + cb.origin)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                std::vector<HitNode> r = kv.widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    /// @brief 子树追加命中盒的聚合下降：遍历 live_ 条目（虚拟化子项不在 `child_nodes()` 里）。
    ///
    /// 视口钳位与本控件 `on_hit_test_chain` **逐字同构**——这是「闸认、自身不认」分叉的禁令来源：
    /// 祖先（`Column` 等）问本控件是否覆盖某点时走的正是本入口，此处不钳位则视口外缓存条目的
    /// 面板会被申报出去，而真实派发链上那一点会被 `on_hit_test_chain` 的钳位拒掉，两者分叉。
    /// @param local 待测点（本控件本地坐标）。
    /// @param ctx 构建上下文，原样透传给条目子树。
    /// @param ancestor_offset 本控件原点在视口坐标系中的 y（祖先下降时逐层累加；
    ///        缺省零表示调用方不知全局位置，覆写体须按纯本地几何判定）。
    /// @return 任一条目的子树申报覆盖此点为 true；点在视口外恒 false。
    /// @note Side-effects: pure
    [[nodiscard]] auto covers_descendant_extra_hit_box(const Point &local, const BuildContext &ctx,
                                                       const Point &ancestor_offset) const -> bool override {
        // 视口盒取自身尺寸（与 on_hit_test 的 `bounds.size` 同源：本控件在常规流中的盒即视口）。
        const Rect viewport{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = size()};
        if (!viewport.contains(local)) {
            return false;
        }
        // live_ 是 std::map：any_of 直接吃 associative_range，无需物化 values 视图
        // （Node 不可拷贝构造，物化那条路走不通）。
        return std::ranges::any_of(live_, [&local, &ctx, &ancestor_offset](const auto &kv) {
            const Rect cb = kv.second.bounds();
            return kv.second.widget().covers_extra_hit_box(local - cb.origin, ctx, ancestor_offset + cb.origin);
        });
    }

    /// @brief 收位滑动逐帧推进（自驱动 tick，不占 Animator；同 Scroll/Dismissible 模式）。
    ///        与 Scroll 的差异：本控件偏移参与子布局，每滑动帧经 apply_offset 标布局脏，
    ///        由虚拟化窗口重建保证开销仅与可见项数相关。
    /// @param now 本次 tick 的墙钟时刻（推进滑动步进与子控件节拍）。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &kv : live_ | std::views::values) {
            kv.widget().tick(now);
        }
        if (!glide_.active) {
            needs_gesture_tick_ = false;  // 静止后摘除每帧计时，回到空闲节流
            return;
        }
        const double dt =
            glide_last_.has_value() ? std::chrono::duration<double>(now - *glide_last_).count() : (1.0 / 60.0);
        glide_last_ = now;
        float v = glide_.tick(dt);
        // 布局可能在滑动中改变内容/视口尺寸：目标随动夹取，防滑出可滚范围。
        v = std::clamp(v, 0.0F, max_scroll_offset());
        if (!glide_.active) {  // 本帧抵达终点
            glide_last_.reset();
            needs_gesture_tick_ = false;
        }
        if (v != offset_) {
            apply_offset(v, /*cancel_glide=*/false);
            request_frame(false);  // 活跃帧信号：滑动期逐帧标脏，decide_wait 不误判空闲深睡
        }
    }

  private:
    /// @brief 启动向 snap 对齐点的收位滑动；reduce-motion 下直落端点
    ///        （对齐 `AnimationController::tick` 短路语义：状态与走完一致，只是无中间帧）。
    auto begin_snap_glide() -> void {
        const float target = ScrollViewport::snap_target(offset_, content_height(), viewport_height_, snap_);
        if (target == offset_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            set_scroll_offset(target);
            return;
        }
        glide_.start(offset_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    /// @brief 偏移落位的统一实现：夹取 → 赋值 → 写回/发布 → 标脏。
    /// @param offset 期望落位的滚动偏移（像素）；先夹取到 [0, max_scroll_offset()]，
    ///        夹取后与当前偏移相同则直接返回，不写回/发布/标脏。
    /// @param cancel_glide 收位滑动帧须传 `false`——公开入口的「外部程序化跳转作废滑动」语义
    ///        若作用于滑动自身，snap 收位只会推进一帧便冻结在中途。
    auto apply_offset(float offset, bool cancel_glide) -> void {
        const float clamped = std::clamp(offset, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        if (cancel_glide) {
            glide_.active = false;
        }
        offset_ = clamped;
        scroll_restored_ = true;  // 外部程序化设置 / 用户滚动：视为已就位，不再被键恢复覆盖
        write_back_offset();
        publish_offset();
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 偏移信号写入（仅当宿主索取过 offset_signal() 且值确实变化）。
    ///        用镜像值比较而非 `State::get()`——后者在 Effect 作用域会自订阅本控件。
    auto publish_offset() -> void {
        if (!offset_state_ || published_offset_ == offset_) {
            return;
        }
        published_offset_ = offset_;
        offset_state_->set(offset_);
    }

    /// @brief 恢复路径专用：夹取 → 赋值 → 标脏（不写回、不改归属标记）。
    auto apply_restored_offset(float raw) -> void {
        const float clamped = std::clamp(raw, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        offset_ = clamped;
        publish_offset();
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 首次可滚动布局时按 `restore_key` 恢复滚动位置；内容尚不可滚动则等待下一帧布局。
    ///        经 `deserialize_props` 显式声明的 `scroll_offset`（`pending_offset_`）**优先于**按键
    ///        恢复——前者是树里写明的状态，后者存在的意义正是「树没写时记住位置」（同 `Scroll` / `LazyRow`）。
    auto maybe_restore_scroll() -> void {
        if (scroll_restored_ || max_scroll_offset() <= 0.0F) {
            return;
        }
        if (pending_offset_.has_value()) {
            scroll_restored_ = true;
            const float explicit_offset = *pending_offset_;
            pending_offset_.reset();
            apply_restored_offset(explicit_offset);
            return;
        }
        if (restore_key_.empty()) {
            return;
        }
        scroll_restored_ = true;
        ScrollStorage::instance().claim(restore_key_, this);
        if (const auto stored = ScrollStorage::instance().read(restore_key_); stored.has_value()) {
            apply_restored_offset(*stored);
        }
    }

    /// @brief 位置变化时写回注册表（**仅内存**：落盘由 App 经 `ScrollStorage::sync` + `Preferences::flush` 决定）。
    auto write_back_offset() -> void {
        if (restore_key_.empty()) {
            return;
        }
        ScrollStorage::instance().write(restore_key_, offset_);
    }

    static constexpr float AURORA_SCROLL_STEP = 40.0F;  ///< 每单位滚轮增量对应的 dp

    int count_ = 0;
    ItemBuilder builder_;
    float item_extent_ = 48.0F;
    float offset_ = 0.0F;
    float cache_extent_ = 200.0F;
    float viewport_height_ = 0.0F;
    std::string restore_key_;  ///< 滚动位置保存键（空 = 不参与恢复）
    std::optional<float> pending_offset_;  ///< 显式反序列化的偏移（优先于 restore_key 恢复）
    bool scroll_restored_ = false;  ///< 是否已就位（恢复过一次 / 用户或外部程序化设置过）
    std::map<int, Node> live_;  ///< 存活实例：index -> Node（按序遍历便于绘制）
    ScrollSnap snap_;  ///< snap/paging 吸附配置（默认关闭）
    ScrollGlide glide_;  ///< snap 收位 / scroll_to 的短滑动时序
    std::optional<std::chrono::steady_clock::time_point> glide_last_;  ///< 上一滑动帧时刻（墙钟差 = dt）
    std::shared_ptr<State<float>> offset_state_;  ///< 懒创建的偏移信号（滚动驱动动画原语，经 offset_signal 暴露）
    float published_offset_ = 0.0F;  ///< 最近一次发布的偏移值（镜像，避免回读 get() 误订阅）
};

}  // namespace aurora
