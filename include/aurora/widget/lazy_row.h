#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/event/event.h"
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

/// @brief 横向虚拟列表控件：LazyRow 属性权威表与按需构建可见窗口内子项的叶式滚动控件。
namespace aurora {

/// @brief 横向虚拟列表属性（权威表）：一行一字段，键名、私有字段、脏档位与回落点只在行里声明一次。
///
/// 读件、写件、序列化与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 五行皆为**可空值件 + 常量档回落点**（虚拟列表没有随作用域而变的属性），故五枚读件一律无参；
/// 「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前 `item_count` / `item_extent` / `padding` / `cache_extent` / `restore_key` 是带声明处默认值
/// 的裸字段；现在五行统一为「可空 ⇔ 非必填 + 行上回落点」，声明处默认值逐个落成各行回落基线，
/// 生效值逐值不变。表序取「既有四键在前、`padding` 追加表尾」：`padding` 是本轮才进表的新键，
/// 摆回它的成员声明位置会让 `cache_extent` / `restore_key` 一并后移，放表尾只让基类键整体后移一位。
///
/// 两枚**不在表里**的公开事实：子项构造器 `item_builder` 是回调而非可序列化状态（同 `Button::on_click`），
/// 留在控件本体的公开槽位；运行时滚动偏移 `offset` 由交互产生、构造期无内容高度可夹取，仍是序列化面上的
/// 表外键（读写经 `set_scroll_offset()` / `scroll_offset()`，不经属性行）。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class LazyRowProps : public AbstractProps<LazyRowProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：子项总数。总数决定内容总宽 ⇒ 布局档 + 绘制档；回落基线 0（空列表）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto item_count_row() {
        return prop_row<&LazyRowProps::item_count_>("item_count")
            .layout()
            .paint()
            .fallback(&LazyRowProps::item_count_default)
            .note("Child count");
    }

    /// @brief 可空值件：每个子项的固定宽度（主轴 dp）。既决定内容总宽又决定行高 ⇒ 布局档 + 绘制档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto item_extent_row() {
        return prop_row<&LazyRowProps::item_extent_>("item_extent")
            .layout()
            .paint()
            .fallback(&LazyRowProps::item_extent_default)
            .note("Fixed child width (px)");
    }

    /// @brief 可空值件：视口外预构建缓冲（主轴 dp）。只改变 `on_paint` 现场算出的窗口边界、不改布局
    ///        ⇒ 绘制档；回落基线 0（不预取）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto cache_extent_row() {
        return prop_row<&LazyRowProps::cache_extent_>("cache_extent")
            .paint()
            .fallback(&LazyRowProps::cache_extent_default)
            .note("Prebuild buffer beyond the viewport (px)");
    }

    /// @brief 可空值件：滚动位置保存键（空 = 不参与恢复）。它只决定「偏移往哪个键写回」，
    ///        既不改几何也不改像素 ⇒ 无脏档；回落基线空串。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto restore_key_row() {
        return prop_row<&LazyRowProps::restore_key_>("restore_key")
            .fallback(&LazyRowProps::restore_key_default)
            .note("Scroll position save key (empty = excluded from restore)");
    }

    /// @brief 可空值件：四周内边距（dp）。计入内容总宽与行高 ⇒ 布局档 + 绘制档；回落基线四边各 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&LazyRowProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&LazyRowProps::padding_default)
            .note("Padding around items (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(item_count_row(), item_extent_row(), cache_extent_row(), restore_key_row(),
                              padding_row());
    }

    // ---- 读面：每行恰一件，五行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的子项总数。
    /// @return 生效总数；未表态时为 0。
    [[nodiscard]] auto item_count() const -> int { return stated(item_count_row()); }

    /// @brief 当前生效的子项固定宽度（dp）。
    /// @return 生效宽度；未表态时为 96。
    [[nodiscard]] auto item_extent() const -> float { return stated(item_extent_row()); }

    /// @brief 当前生效的视口外预构建缓冲（主轴 dp）。
    /// @return 生效缓冲长度；未表态时为 0（不预取）。
    [[nodiscard]] auto cache_extent() const -> float { return stated(cache_extent_row()); }

    /// @brief 当前生效的滚动位置保存键。
    /// @return 生效键名；未表态时为空串（不参与恢复）。
    [[nodiscard]] auto restore_key() const -> std::string { return stated(restore_key_row()); }

    /// @brief 当前生效的四周内边距（dp）。
    /// @return 生效内边距；未表态时四边各 0。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入子项总数；表态即脱离空列表基线。
    /// @param value 新的子项总数（负值照原样表态，绘制按窗口索引自然截断）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_item_count(int value) -> LazyRowProps & { return assign(item_count_row(), value); }

    /// @brief 装入子项固定宽度（dp）。
    /// @param value 新宽度。
    /// @return 引用本属性对象，便于链式续写。
    auto set_item_extent(float value) -> LazyRowProps & { return assign(item_extent_row(), value); }

    /// @brief 装入视口外预构建缓冲（主轴 dp）。
    /// @param value 新缓冲长度。
    /// @return 引用本属性对象，便于链式续写。
    auto set_cache_extent(float value) -> LazyRowProps & { return assign(cache_extent_row(), value); }

    /// @brief 装入滚动位置保存键。
    /// @param value 新键名；空串是「显式要一条不参与恢复的列表」，与「未表态」同值不同意。
    /// @return 引用本属性对象，便于链式续写。
    auto set_restore_key(const std::string &value) -> LazyRowProps & { return assign(restore_key_row(), value); }

    /// @brief 装入四周内边距（dp）。
    /// @param value 四边内边距。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(const EdgeInsets &value) -> LazyRowProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 子项总数回落基线。
    /// @return 0（空列表）。
    [[nodiscard]] static auto item_count_default() -> int { return 0; }

    /// @brief 子项宽度回落基线。
    /// @return 96.0F（dp）。
    [[nodiscard]] static auto item_extent_default() -> float { return 96.0F; }

    /// @brief 预构建缓冲回落基线。
    /// @return 0.0F（不预取）。
    [[nodiscard]] static auto cache_extent_default() -> float { return 0.0F; }

    /// @brief 保存键回落基线。
    /// @return 空串（不参与滚动位置恢复）。
    [[nodiscard]] static auto restore_key_default() -> std::string { return {}; }

    /// @brief 内边距回落基线。
    /// @return 四边各 0dp。
    [[nodiscard]] static auto padding_default() -> EdgeInsets { return EdgeInsets{}; }

  private:
    std::optional<int> item_count_;  ///< 子项总数表态位
    std::optional<float> item_extent_;  ///< 子项固定宽度表态位（dp）
    std::optional<float> cache_extent_;  ///< 视口外预构建缓冲表态位（主轴 dp）
    std::optional<std::string> restore_key_;  ///< 滚动位置保存键表态位
    std::optional<EdgeInsets> padding_;  ///< 四周内边距表态位（dp）

    friend class AbstractProps<LazyRowProps>;
};

/// @brief 横向虚拟列表（镜像 `LazyList`，主轴改为水平）。
///
/// 仅构建可见窗口（含 `cache_extent` 缓冲）内的子项，复杂度为 O(可见单元数)。
/// 横向滚轮（或拖拽）调整 `offset_`；`on_paint` 内 `push_clip` 防止父级圆角裁剪
/// 下的慢路径越界。命中测试返回自身（作为横向滚动叶），其内部子项点击通过
/// `on_item_click` 回调（按按下位置计算索引）上报，避免虚拟化子项不可作为稳定控件。
///
/// 属性一律经 `LazyRowProps` 的读写件访问（本类公有继承它，故 `row.set_item_extent(64.0F)` 与
/// `row.item_extent()` 都直接可用）；五行皆常量档，读件一律无参。子项构造器与点击回调是控件本体的
/// 槽位（回调不可序列化），运行时滚动偏移经 `set_scroll_offset()` / `scroll_offset()` 读写。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class LazyRow : public Widget, public LazyRowProps {
  public:
    /// @brief 子项构造器类型：按索引惰性产出子项 Node。
    using ItemBuilder = std::function<Node(int)>;

    /// @brief 默认构造：五行皆未表态（生效值即各行回落基线）；接线宿主并声明视口边界。
    LazyRow() {
        bind(*this);
        set_relayout_boundary(true);  // 视口尺寸由父约束决定、不依赖子节点（虚拟化）
    }

    /// @brief 以属性配置块构造（整份属性接管后接线宿主）。
    /// @param props 属性权威表对象，移动进本实例的 `LazyRowProps` 基类子对象。
    explicit LazyRow(LazyRowProps props) : LazyRowProps(std::move(props)) {
        bind(*this);
        set_relayout_boundary(true);
    }

    /// @brief 便捷构造：指定数量 + 构造器 + 统一子项宽度（转调属性面写件，单一存储）。
    /// @param count 子项总数（负值照原样表态，绘制按窗口索引自然截断）。
    /// @param builder 子项构造器（按索引惰性产出）。
    /// @param item_extent 每个子项的固定宽度（dp，默认即该行回落基线 96）。
    LazyRow(int count, ItemBuilder builder, float item_extent = 96.0F) : LazyRow() {
        item_builder = std::move(builder);
        set_item_count(count);
        set_item_extent(item_extent);
    }

    /// @brief 子项构造器槽位：按索引惰性产出子项，只在可见窗口内被调用。
    ///        非属性行——它是回调而非可序列化状态，故留在控件本体。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    ItemBuilder item_builder;  ///< 子项构造器（按需惰性调用）

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `LazyRowProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> LazyRowProps { return {}; }

    /// @brief 登记需订阅的信号视图：本表五行皆为值形态，没有可登记的来源。
    /// @param out 信号采集出口；本控件不向其写入任何信号。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 类型名字符串 "LazyRow"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override { return "LazyRow"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "LazyRow" 的描述表（item_count / item_extent / cache_extent / restore_key / padding
    ///         五行，事件 on_item_click，子节点策略 virtual）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("LazyRow");
        descriptor.events = {
            {"on_item_click", "void(int)", "Callback when a child is clicked (argument is the index)"}};
        descriptor.children_policy = "virtual";
        descriptor.examples = {"au::LazyRow{ 10, [](int i){ return au::Text(std::to_string(i)); } }"};
        return descriptor;
    }
    /// @brief 实例自描述：转发到 describe_static()。
    /// @return 本控件类型的 WidgetDescriptor（属性/事件/子策略元数据）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化标量属性到 `props`：必填行与已表态行入表，未表态的行不写键。
    /// @param props 目标 JSON 对象（基类属性先行，运行时 `offset` 最后追加）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性（width/height/show 等）
        props_serialize(props);
        props.set("offset", offset_);  // 运行时滚动位置（AI-first 可观测；表外键，非属性行）
    }
    /// @brief 从静态 JSON 回填标量属性；缺失键保持当前值。
    /// @note 运行时 `offset` 不是属性行：显式偏移优先于 `restore_key` 恢复，故记入 pending 并撤恢复标记。
    /// @param props 属性 JSON 对象（item_count/item_extent/cache_extent/restore_key/padding/offset）。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
        if (props.contains("offset")) {
            // 显式偏移优先于 restore_key 恢复：记入 pending，首次可滚动布局时应用。
            pending_offset_ = props.at("offset")->as_or<float>(0.0F);
            scroll_restored_ = false;
        }
    }

    /// @brief 设置子项总数（链式）：转调属性面写件，再回收已构建窗口。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的属性写件：缓存窗口是控件自身的事实、不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值与脏档仍由同一行给出）。
    /// @param c 新的子项总数。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_item_count(int c) -> LazyRow & {
        LazyRowProps::set_item_count(c);
        built_.clear();
        return *this;
    }
    /// @brief 设置子项构造器（链式；清空已构建窗口并标布局脏）。
    /// @param b 新的子项构造器（按索引惰性产出）。
    /// @return 引用本控件，便于链式续写。
    auto set_item_builder(ItemBuilder b) -> LazyRow & {
        item_builder = std::move(b);
        built_.clear();
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置子项点击回调（参数为子项索引）。
    /// @param cb 回调：松开未拖拽且命中有效子项时以该索引调用。
    /// @return 引用本控件，便于链式续写。
    auto set_on_item_click(std::function<void(int)> cb) -> LazyRow & {
        on_item_click_ = std::move(cb);
        return *this;
    }

    /// @brief 当前滚动偏移（dp，内容左移为正）。
    /// @return 内部偏移量 offset_。
    [[nodiscard]] auto scroll_offset() const -> float { return offset_; }

    /// @brief 最大滚动偏移（内容宽 - 视口宽，不小于 0）。
    /// @return 可滚动范围上限；内容不足以滚动时为 0。
    [[nodiscard]] auto max_scroll_offset() const -> float {
        // 视口宽度优先取最近一次布局的约束结果（`size()` 在 on_layout 返回后才更新，恢复路径依赖此值）。
        const EdgeInsets pad = padding();
        const float viewport_w = viewport_w_ > 0.0F ? viewport_w_ : std::max(1.0F, size().width - pad.left - pad.right);
        return std::max(0.0F, full_content_ - viewport_w);
    }

    /// @brief 程序化设置滚动偏移（夹取到 `[0, max_scroll_offset()]`；仅绘制脏，不标布局脏）。
    ///
    /// 与 `LazyList::set_scroll_offset` 的契约差异：本控件的可见窗口是在 `on_paint` 里按
    /// `offset_` 现场计算的（`on_layout` 只算内容总宽），故偏移变化**不需要**重布局；而
    /// `LazyList` 的子项在布局期排布，其 setter 须一并标布局脏。
    /// @param offset 期望偏移（dp）；越界部分被夹取，与当前值相等时为 no-op。
    auto set_scroll_offset(float offset) -> void {
        const float clamped = std::clamp(offset, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        offset_ = clamped;
        scroll_restored_ = true;  // 外部程序化设置：视为已就位，不再被键恢复覆盖
        write_back_offset();
        mark_needs_paint();
    }

    /// @brief 滚轮滚动：按 `delta_y × item_extent × 0.5` 换算为横向偏移并夹取，余量回传外层。
    /// @param e 滚动事件；置 `is_handled`，`remaining_y` 为端点夹掉后未消费的余量。
    auto on_scroll(ScrollEvent &e) -> void override {
        const EdgeInsets pad = padding();
        const float ext = item_extent();
        const float vw = std::max(1.0F, size().width - pad.left - pad.right);
        const float max_off = std::max(0.0F, full_content_ - vw);
        const float before = offset_;
        offset_ = std::max(0.0F, std::min(max_off, before + (e.delta_y * ext * 0.5F)));
        e.is_handled = true;
        scroll_restored_ = true;  // 用户主动滚动：放弃尚未生效的键恢复
        // 余量回传（嵌套滚动协调）：横向消费系数 item_extent×0.5/单位，端点夹掉的部分上冒。
        const float factor = ext * 0.5F;
        e.remaining_y = factor > 0.0F ? (e.delta_y - ((offset_ - before) / factor)) : e.delta_y;
        write_back_offset();
        mark_needs_paint();
    }

    /// @brief 真实滚动控件：滚轮派发时本控件是可滚动目标（最深优先）。
    /// @return 恒为 true（覆盖基类按 `overflow_` 的默认判定，使嵌套时滚轮归本控件）。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    /// @brief 无障碍滚动量：纯横向虚拟列表，纵向按「不可滚容器」申报（视口高 == 内容高 ⇒ 跨度 0）。
    /// 不暴露纵向 `IScrollProvider`；`get_VerticalViewSize` 由 `compute_vertical_view_size` 算得 100%。
    /// @note Side-effects: reads state
    /// @return `{min=0, max=0, position=0, viewport=content=viewport_h_}`。
    [[nodiscard]] auto accessibility_scroll() const -> std::optional<AccessibilityScrollRange> override {
        const auto vh = static_cast<double>(viewport_h_);
        return AccessibilityScrollRange{.min = 0.0, .max = 0.0, .position = 0.0, .viewport = vh, .content = vh};
    }

    /// @brief 指针事件入口：按下记录命中子项，横向拖拽推进滚动偏移，抬起未拖拽则回调子项点击。
    /// @param e 鼠标事件（就地读写：拖拽消费其 `local_position.x` 增量，消费后把 `is_handled` 置 true）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            pressed_ = true;
            dragging_ = false;
            press_local_ = e.local_position;
            press_index_ = index_at(e.local_position.x);
            last_drag_x_ = e.local_position.x;
        } else if (e.action == MouseAction::Move) {
            if (pressed_) {
                const float dx = e.local_position.x - last_drag_x_;
                if (std::fabs(e.local_position.x - press_local_.x) > 4.0F) {
                    dragging_ = true;
                }
                if (dragging_) {
                    const EdgeInsets pad = padding();
                    const float vw = std::max(1.0F, size().width - pad.left - pad.right);
                    const float max_off = std::max(0.0F, full_content_ - vw);
                    offset_ = std::max(0.0F, std::min(max_off, offset_ - dx));
                    scroll_restored_ = true;  // 拖拽即用户主动滚动
                    write_back_offset();
                    last_drag_x_ = e.local_position.x;
                    e.is_handled = true;
                    mark_needs_paint();
                }
            }
        } else if (e.action == MouseAction::Release) {
            if (pressed_ && !dragging_ && press_index_ >= 0 && press_index_ < item_count()) {
                if (on_item_click_) {
                    on_item_click_(press_index_);
                }
                e.is_handled = true;
            }
            pressed_ = false;
            dragging_ = false;
        }
    }

  protected:
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const EdgeInsets pad = padding();
        const float ext = item_extent();
        const float h_pad = pad.left + pad.right;
        const float v_pad = pad.top + pad.bottom;
        const float full = (static_cast<float>(item_count()) * ext) + h_pad;
        const float h = ext + v_pad;
        full_content_ = full;
        const Size constrained = c.constrain(Size{.width = full, .height = h});
        // 视口宽用**本次**约束结果（`size()` 在 on_layout 返回后才更新，恢复判定不能依赖它）。
        viewport_w_ = std::max(1.0F, constrained.width - h_pad);
        // 纵向视口高同样取自本次约束结果（纯横向列表纵向不可滚，视口高 == 内容高 ⇒ 跨度 0）。
        viewport_h_ = constrained.height;
        // 滚动位置恢复（首次可滚动布局时生效；显式反序列化的 offset 优先）。
        maybe_restore_scroll();
        return constrained;
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        // 每帧一次性读出各行生效值，循环体内不再重复解表态位。
        const EdgeInsets pad = padding();
        const float ext = item_extent();
        const float cache = cache_extent();
        const int count = item_count();
        const float v_pad = pad.top + pad.bottom;
        const float ch = std::max(1.0F, bounds.size.height - v_pad);
        const int first = std::max(0, static_cast<int>(std::floor((offset_ - cache) / ext)));
        const int last =
            std::min(count - 1, static_cast<int>(std::floor((offset_ + bounds.size.width - pad.left + cache) / ext)));
        // 标注：本帧不需要但仍在缓存中的子项回收
        for (size_t i = 0; i < built_.size(); ++i) {
            if (built_[i] && (std::cmp_less(i, first) || std::cmp_greater(i, last))) {
                built_[i] = Node{};
            }
        }
        if (built_.size() <= static_cast<size_t>(last)) {
            built_.resize(static_cast<size_t>(last) + 1);
        }

        for (int i = first; i <= last; ++i) {
            if (std::cmp_greater_equal(i, built_.size())) {
                built_.resize(static_cast<size_t>(i) + 1);
            }
            if (!built_[i]) {
                if (item_builder) {
                    built_[i] = Node{item_builder(i)};
                } else {
                    continue;
                }
            }
            const float x = pad.left + (static_cast<float>(i) * ext) - offset_;
            const Rect cb{.origin = Point{.x = x, .y = pad.top}, .size = Size{.width = ext, .height = ch}};
            built_[i].set_bounds(cb);
            built_[i].widget().layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = cb.size}, ctx);
            built_[i].widget().paint(p, Rect{.origin = bounds.origin + cb.origin, .size = cb.size}, ctx);
        }
        p.pop_clip();
    }

// clang 无 "-Wdangling-pointer" 告警组（实测报 -Wunknown-warning-option），故压制只在 GCC 下展开。
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)) {
            return {};
        }
        (void)ctx;
        // 虚拟化子项不以稳定控件形态参与命中链：横向列表自身作为点击/滚动叶。
        return std::vector{HitNode{this, weak_from_this(), bounds.origin}};
    }
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic pop
#endif

  private:
    /// @brief 恢复路径专用：夹取 → 赋值 → 标绘制脏（不写回、不改归属标记）。
    auto apply_restored_offset(float raw) -> void {
        const float clamped = std::clamp(raw, 0.0F, max_scroll_offset());
        if (clamped == offset_) {
            return;
        }
        offset_ = clamped;
        mark_needs_paint();
    }

    /// @brief 首次可滚动布局时恢复滚动位置：
    ///        ① 显式反序列化的 `offset` 优先；② 否则查 `ScrollStorage` 的 `restore_key`；
    ///        ③ 尚无内容可滚（`max_scroll_offset() == 0`）时保持等待，避免先夹到 0 再被内容吞掉。
    auto maybe_restore_scroll() -> void {
        if (scroll_restored_) {
            return;
        }
        if (pending_offset_.has_value()) {
            scroll_restored_ = true;
            const float explicit_offset = *pending_offset_;
            pending_offset_.reset();
            apply_restored_offset(explicit_offset);
            return;
        }
        const std::string key = restore_key();
        if (key.empty() || max_scroll_offset() <= 0.0F) {
            return;
        }
        scroll_restored_ = true;
        ScrollStorage::instance().claim(key, this);
        if (const auto stored = ScrollStorage::instance().read(key); stored.has_value()) {
            apply_restored_offset(*stored);
        }
    }

    /// @brief 位置变化时写回注册表（**仅内存**：落盘由 App 经 `ScrollStorage::sync` + `Preferences::flush` 决定）。
    auto write_back_offset() const -> void {
        const std::string key = restore_key();
        if (key.empty()) {
            return;
        }
        ScrollStorage::instance().write(key, offset_);
    }

    [[nodiscard]] auto index_at(float local_x) const -> int {
        const float idx = std::floor((local_x - padding().left + offset_) / item_extent());
        const int i = static_cast<int>(idx);
        if (i < 0 || i >= item_count()) {
            return -1;
        }
        return i;
    }

    float full_content_ = 0.0F;
    float offset_ = 0.0F;
    float viewport_w_ = 0.0F;  ///< 最近一次布局得到的视口宽（`size()` 在 on_layout 内尚是旧值）
    float viewport_h_ = 0.0F;  ///< 最近一次布局得到的视口高（纵向不可滚，仅供无障碍 ViewSize 申报）
    std::optional<float> pending_offset_;  ///< 显式反序列化的偏移（优先于 restore_key 恢复）
    bool scroll_restored_ = false;  ///< 是否已就位（恢复过一次 / 用户或外部程序化设置过）
    std::vector<Node> built_;

    std::function<void(int)> on_item_click_;
    bool pressed_ = false;
    bool dragging_ = false;
    Point press_local_{.x = 0.0F, .y = 0.0F};
    float last_drag_x_ = 0.0F;
    int press_index_ = -1;
};

}  // namespace aurora
