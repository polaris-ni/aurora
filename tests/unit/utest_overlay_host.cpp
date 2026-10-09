/// 测试类型: unit
/// 目标单元: include/aurora/widget/popup.h（OverlayHost / Popup）
/// 测试说明: 覆盖浮层登记的序号契约——`add_overlay` 与 `remove_overlay` 对「0 = 基础内容、
/// 不可移除」必须口径一致。修复前 `add_overlay` 在宿主尚无基础内容时返回 0，而 `remove_overlay`
/// 拒收 0 ⇒ 调用方拿到一个永远删不掉的序号（浮层泄漏）。修复后该情形返回 `std::nullopt`，
/// 由类型本身表达「本次追加未产生可移除浮层」。
///
/// 判据：有基础内容时序号自 1 起且可移除；空宿主时返回 nullopt、节点虽已入树但不可移除。
///
/// 另覆盖运行期追加子树的挂载时机：宿主已布局后 `add_overlay` 的浮层必须在**下一次布局**被挂上
/// （`Container` 的补挂机制，父侧 ctx），调用方不自带 `BuildContext`；以及负向契约——
/// `remove_overlay` **不代调** `unmount`（摘除时无从判断子件是否仍在容器之外被持有）。
///
/// 末段覆盖 `Popup` 内容的窗口盒（`Widget::window_bounds()`）：`anchor_` 是**全局**坐标，
/// 只在 `on_paint` / `on_hit_test_chain` 施加，基类沿 `layout_parent_` 的递推看不到它 ⇒ 挂在
/// `Popup` 里的控件读数少一个锚点平移。症状是消费侧按该读数取点点不到控件（`type()` 的字符
/// 被基类 `Widget::on_text_input` 缺省 `is_handled = true` 静默吞掉）。两形态判据：
/// ①`Popup` 内容控件；②未挂 `Popup` 的普通子树（守缺省路径零变化）。
///
/// 基准纪律：**不用 `window_bounds()` 自己的输出当基准**，也不用 `HitNode.origin`（此前的教训：
/// 它在滚动容器内本身不是窗口坐标）。改为对真实命中链密集取点，实测「命中该控件」的点集外接框
/// 作为可达框基准——这条通路与 `window_bounds()` 完全独立，两者相符才说明读数与派发同源。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/environment/environment.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/provider.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_overlay_host {

using aurora::testing::require_value;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 一棵基础内容树（Column 内一个按钮）。
/// @return 基础内容节点。
auto base_content() -> Node { return Node{Button(ButtonProps{.label = "Base"})}; }

/// @brief 挂载观测控件：记录挂载 / 卸载次数、挂载时看到的宿主身份与主题主色、布局时读到的主题主色。
///
/// 另暴露 `effect_count()`：`effects_` 是 `Widget` 的 protected 成员，测试子类可读——它是
/// 「净订阅数不增长」唯一可判的量（宿主自报的挂载次数区分不出「退订了」与「退订前就只有一个」）。
class MountProbe final : public Widget {
  public:
    int mounts = 0;  ///< `on_mount` 触发次数
    int unmounts = 0;  ///< `on_unmount` 触发次数
    std::uint64_t host_at_mount = 0;  ///< 挂载时 ctx 的宿主身份
    bool theme_seen_at_mount = false;  ///< 挂载时是否读到了宿主注入的主题
    Color primary_at_mount;  ///< 挂载时读到的主题主色
    Color primary_at_layout;  ///< 最近一次布局读到的主题主色

    [[nodiscard]] auto type_name() const -> const char * override { return "MountProbe"; }
    /// @brief 额外登记一个自有信号，使订阅条数 = 自有 1 + `modifier` + `show` = 3（固定值可硬断言）。
    /// @param out 信号视图累加表（本控件追加自身信号）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&tick_state_); }
    [[nodiscard]] auto effect_count() const -> std::size_t { return effects_.size(); }

  protected:
    /// @brief 本控件的布局读环境（记主题主色），故**刻意不可缓存**——否则约束不变时 `on_layout`
    /// 整个被跳过，观测点就成了「上一次的残留值」，判据会假绿。
    /// @return 恒 false（布局依赖 ctx 环境，不参与布局缓存）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (const auto *th = ctx.environment<Theme>()) {
            primary_at_layout = th->primary;
        }
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        ++mounts;
        host_at_mount = ctx.host_id;
        if (const auto *th = ctx.environment<Theme>()) {
            theme_seen_at_mount = true;
            primary_at_mount = th->primary;
        }
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        ++unmounts;
    }

  private:
    Reactive<int> tick_state_;
};

}  // namespace

AURORA_TEST_CASE(overlay_index_starts_at_one_and_is_removable) {
    // 有基础内容：序号自 1 起，`remove_overlay` 收得下（与返回的序号同一口径）。
    auto host = std::make_shared<OverlayHost>(base_content());
    // 「检查 + 取值」走框架的 `require_value`：宏展开对 clang-tidy 的路径分析不透明，
    // 直接 `*first` 会被误报 unchecked-optional-access。
    const std::size_t first = require_value(host->add_overlay(Node{Button(ButtonProps{.label = "Overlay"})}));
    AURORA_TEST_CHECK_EQ(first, 1U);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 1U);

    host->remove_overlay(first);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);
}

AURORA_TEST_CASE(add_overlay_on_empty_host_is_not_removable) {
    // 空宿主：新节点落在序号 0（基础内容槽位）⇒ 返回 nullopt；且该节点确实不可移除，
    // 与 `remove_overlay` 的口径一致（修复前返回 0，调用方据此去删会被静默忽略）。
    auto host = std::make_shared<OverlayHost>();
    LayoutEngine::layout(*host, bounded(240.0F, 200.0F));
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);

    const std::optional<std::size_t> index = host->add_overlay(Node{Button(ButtonProps{.label = "Orphan"})});
    AURORA_TEST_CHECK_FALSE(index.has_value());

    // 节点已入树（追加本身生效），但不计入浮层、也删不掉——这正是返回值要如实表达的情形。
    host->remove_overlay(0);
    AURORA_TEST_CHECK_EQ(host->overlay_count(), 0U);
    AURORA_TEST_CHECK_EQ(host->child_count(), 1U);
}

AURORA_TEST_CASE(runtime_added_overlay_is_mounted_on_next_layout) {
    // 「根长期存活 + 运行期加子树」这一常见形态：浮层必须在下一次布局由父侧 ctx 挂上，
    // 症状此前是「浮层里的控件收不到挂载」——主题不跟、定时档不启动，且全程无日志。
    Theme theme_a;
    theme_a.primary = Color::from_rgba(11, 22, 33);
    Theme theme_b;
    theme_b.primary = Color::from_rgba(44, 55, 66);
    // ctx.env 必须非空（真实窗口里指向 `root_env_`）：`Provider::rebuild_env` 在有父环境时走
    // `Environment::with()` 每次重建一份新环境，换肤才生效；`env == nullptr` 的退化路径走
    // `set_local`，其 `map_.emplace` 对已存在的键不覆盖——那是独立于本任务的既有缺陷，不在此顺带修。
    Environment root_env;
    auto host = std::make_shared<OverlayHost>(base_content());
    Provider<Theme> provider{theme_a, Node{host}};
    BuildContext ctx;
    ctx.env = &root_env;
    // 先挂载：`Provider` 靠 mount 订阅自己持有的 `value_`，不挂载则 `set_value` 不会标脏、
    // 布局缓存不会失效——那正是「换肤不跟」的另一条独立故障路径，与本次判据无关。
    provider.mount(ctx);
    LayoutEngine::layout(provider, bounded(240.0F, 200.0F), ctx);

    auto probe = std::make_shared<MountProbe>();
    const std::size_t index = require_value(host->add_overlay(Node{probe}));
    AURORA_TEST_CHECK_EQ(index, 1U);
    // 追加那一刻父侧未必有 ctx 可用，故此刻**不得**已挂载。
    AURORA_TEST_CHECK_EQ(probe->mounts, 0);

    LayoutEngine::layout(provider, bounded(240.0F, 200.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->unmounts, 0);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
    // 挂到的是**宿主**的 ctx：主题注入读得到，不是无环境的裸 ctx。
    AURORA_TEST_CHECK_TRUE(probe->theme_seen_at_mount);
    AURORA_TEST_CHECK_TRUE(probe->primary_at_mount == theme_a.primary);

    // 换肤后仍读到新值：挂载补齐不是「挂上就冻结」。
    // 第二遍**换一组约束**：`State` 变更只向上失效，缓存命中的祖先会整段跳过子树
    // （既有布局缓存语义，不在本次范围），换约束才能让一次完整布局真的走到 probe。
    provider.set_value(theme_b);
    LayoutEngine::layout(provider, bounded(260.0F, 220.0F), ctx);
    AURORA_TEST_CHECK_TRUE(probe->primary_at_layout == theme_b.primary);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);  // 同宿主不重复挂载
}

AURORA_TEST_CASE(remove_overlay_does_not_unmount_detached_subtree) {
    // 负向契约：容器在移除子项时代调 unmount 即属越界——摘除时刻它无从判断这只子件是否
    // 「活在容器之外仍被持有」，按引用计数自动退订会误伤仍存活的子树。
    auto host = std::make_shared<OverlayHost>(base_content());
    LayoutEngine::layout(*host, bounded(240.0F, 200.0F));

    auto probe = std::make_shared<MountProbe>();
    const std::size_t index = require_value(host->add_overlay(Node{probe}));
    LayoutEngine::layout(*host, bounded(240.0F, 200.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);

    host->remove_overlay(index);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->unmounts, 0);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);

    // 摘除后再布局也不补挂、不重挂：已挂载的子树状态由持有者自己处置。
    LayoutEngine::layout(*host, bounded(240.0F, 200.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
}

// ---- Popup 内容的窗口盒（Widget::window_bounds 补 anchor_ 平移） ----
//
// 机制坐标：`Popup::on_layout` 把内容子盒 origin 钉在 {0,0}（浮层不参与父布局），`anchor_`
// 只在 `on_paint`（`content_box{origin = anchor_}`）与 `on_hit_test_chain`
// （`content_box.contains(global)`）两处各自施加。`Widget::window_bounds()` 沿 `layout_parent_`
// 递推时逐层加 `cb.origin + 滚动修正 + Modifier 平移`，三者都不含 `anchor_` ⇒ 挂在 Popup 里的
// 控件读数少一个锚点。
//
// 修法不是加一项而是**替换基准并终止上溯**：`anchor_` 是全局坐标，`Popup` 的两条路径全程不
// 参与 `Popup` 自身在树中的位置（命中链用 `bounds.origin + local` 起算后与 `anchor_` 盒比较），
// 故其上祖先链一律不参与。承载它的钩子是 `Widget::child_content_origin`（替换性重映射），
// 与 `scroll_content_offset`（加性滚动修正）语义不同、不可代偿。

/// @brief 树中 `OverlayHost` 之前的占位行高：刻意取非零，使「替换基准」与「叠加锚点」两种
///        读法在两形态判据里必然分叉（后者会多出这一个 LEAD_IN）。
constexpr float AURORA_LEAD_IN = 40.0F;
/// @brief `Popup` 内容首行（占位）的高度：探针位于内容盒内 y = 6 处。
constexpr float AURORA_CONTENT_HEAD = 6.0F;
/// @brief 探针控件的固定尺寸。
constexpr float AURORA_PROBE_W = 120.0F;
constexpr float AURORA_PROBE_H = 20.0F;
/// @brief 可达框实测的取点步长（dp）：实测框至多比真框大一个步长，判据容差取它。
constexpr float AURORA_PROBE_STEP = 0.25F;
/// @brief 可达框实测的搜索半宽（dp）：远大于步长，保证真框必被覆盖（读数错一个锚点也够得到）。
constexpr float AURORA_PROBE_SPAN = 48.0F;
/// @brief 浮层锚点（窗口绝对坐标）。
///
/// ⚠️ 必须落在 `OverlayHost` 的盒内（此处 y = LEAD_IN + 20 ∈ [LEAD_IN, LEAD_IN + 200]）：
/// `OverlayHost` 之上仍有 `Container` 的下降闸（`cb.contains(local)`，见 `widget.h`），落在宿主
/// 盒外的探点会被那一层挡掉、命中链到不了 `Popup`。这是「锚点须在宿主可视范围内」这一既有
/// 性质，与本次修复无关，但决定了两形态判据的锚点不能取窗口原点。
constexpr Point AURORA_ANCHOR{.x = 12.0F, .y = 60.0F};

/// @brief 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class AnchorBox final : public Widget {
  public:
    AnchorBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "AnchorBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*r*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

/// @brief 事件探针：记录收到的 `position` / `local_position`，并记一次被派发。
///
/// 必须挂 Clickable 修饰：`wants_click()` 缺省只看修饰链，不挂则基类不认它为命中目标、
/// `on_pointer_event` 永不触发（与 `utest_scroll` 的 LocalProbeRow 同因同解）。
class DispatchProbe final : public Widget {
  public:
    DispatchProbe(float w, float h) : w_(w), h_(h) {
        modifier.set(Modifier{}.clickable([]() -> void {}));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "DispatchProbe"; }
    [[nodiscard]] auto seen() const -> bool { return seen_; }
    [[nodiscard]] auto last_position() const -> Point { return position_; }
    [[nodiscard]] auto last_local() const -> Point { return local_; }
    /// @brief 清空记录，供同一控件在一条用例内做多次派发。
    ///
    /// 刻意**不叫** `reset`：`Widget` 派生对象常与 `shared_ptr` 并列出现在同一表达式里，
    /// `readability-ambiguous-smartptr-reset-call` 会把同名无参调用判为歧义。
    auto clear_records() -> void {
        seen_ = false;
        position_ = Point{.x = 0.0F, .y = 0.0F};
        local_ = Point{.x = 0.0F, .y = 0.0F};
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        seen_ = true;
        position_ = e.position;
        local_ = e.local_position;
        e.is_handled = true;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*r*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
    bool seen_ = false;
    Point position_{.x = 0.0F, .y = 0.0F};
    Point local_{.x = 0.0F, .y = 0.0F};
};

/// @brief 一棵「Column 内挂 OverlayHost（基础内容 + 浮层 Popup）」的树。
///
/// 宿主必须是 `OverlayHost` 而非裸 `Column`：`Popup` 在常规流中占**零尺寸**盒，故
/// `Container::on_hit_test_chain` 的下降闸（`cb.contains(local) || covers_extra_hit_box(...)`，
/// 见 `widget.h`）两者皆假——`cb.contains` 对零尺寸盒恒假，而 `Popup` 的
/// `covers_descendant_extra_hit_box` 按设计覆写为恒 false（它是「正面范式」，在**自己的**
/// 命中链入口里重映射下降）。`OverlayHost::on_hit_test_chain` 则**无条件**问每个子节点，
/// 这才是 `Popup` 的合法挂载形态（与 `demo_popup` 及集成侧一致）。
///
/// `OverlayHost` 摆在 y = LEAD_IN 处（非零），用来把「替换基准」与「叠加锚点」两种读法区分开。
struct PopupTree {
    std::shared_ptr<Column> root;
    std::shared_ptr<OverlayHost> host;
    std::shared_ptr<Popup> popup;
    std::shared_ptr<Column> content;
    std::shared_ptr<DispatchProbe> target;
};

/// @brief 搭「占位行 + OverlayHost（基础内容 + 打开的 Popup）」并完成布局。
/// @param anchor 浮层锚点（全局坐标）。
/// @return 已完成布局、且 `Popup` 处于打开态的 fixture。
auto make_popup_tree(const Point &anchor) -> PopupTree {
    PopupTree f;
    f.root = std::make_shared<Column>();
    f.root->add(Node{std::make_shared<AnchorBox>(300.0F, AURORA_LEAD_IN)});
    f.host = std::make_shared<OverlayHost>(Node{std::make_shared<AnchorBox>(300.0F, 10.0F)});
    f.target = std::make_shared<DispatchProbe>(AURORA_PROBE_W, AURORA_PROBE_H);
    auto inner = std::make_shared<Column>();
    // 内容首行：让探针不落在内容原点，窗口盒读数里才会出现「内容盒内偏移」这一项。
    inner->add(Node{std::make_shared<AnchorBox>(AURORA_PROBE_W, AURORA_CONTENT_HEAD)});
    inner->add(Node{f.target});
    f.content = inner;
    f.popup = std::make_shared<Popup>(Node{f.content});
    // 浮层走登记口：序号自 1 起（0 是基础内容槽位、不可移除）。
    LayoutEngine::layout(*f.host, bounded(300.0F, 200.0F));
    (void)require_value(f.host->add_overlay(Node{f.popup}));
    f.root->add(Node{f.host});
    LayoutEngine::layout(*f.root, bounded(320.0F, 240.0F));
    // 必须**先布局再打开**：关闭态下 `Popup` 不测量内容（`content_size_` 归零），
    // 打开后需再走一次布局才拿到内容尺寸。
    f.popup->open_at(anchor);
    LayoutEngine::layout(*f.root, bounded(320.0F, 240.0F));
    return f;
}

/// @brief 一棵不含 `Popup` 的普通子树：占位行 + 探针（两形态判据之②的树）。
struct PlainTree {
    std::shared_ptr<Column> root;
    std::shared_ptr<DispatchProbe> target;
};

/// @brief 搭「占位行 + 探针」的普通 `Column` 树并完成布局。
/// @return 已完成布局的 fixture。
auto make_plain_tree() -> PlainTree {
    PlainTree f;
    f.root = std::make_shared<Column>();
    f.root->add(Node{std::make_shared<AnchorBox>(300.0F, AURORA_LEAD_IN)});
    f.target = std::make_shared<DispatchProbe>(AURORA_PROBE_W, AURORA_PROBE_H);
    f.root->add(Node{f.target});
    LayoutEngine::layout(*f.root, bounded(320.0F, 240.0F));
    return f;
}

/// @brief 命中链实测：探测点 `p` 处的命中链里是否含指定控件。
/// @param root 命中测试的根。
/// @param p 探测点（窗口逻辑 dp）。
/// @param target 待判定控件（只做地址比对不解引用）。
/// @return 命中链含该控件为 true。
auto chain_hits(Widget &root, const Point &p, const Widget *target) -> bool {
    const Rect root_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = root.size()};
    const auto chain = root.hit_test_chain(p, root_box, BuildContext{});
    return std::ranges::any_of(chain, [target](const HitNode &n) -> bool { return n.ptr == target; });
}

/// @brief 命中链实测的**可达框**：以 `hint` 为搜索中心在其四周按固定步长密集取点，量出
///        「命中该控件」的点集外接框。
///
/// 这是判据的**独立基准**：走的是真实命中链，与 `window_bounds()` 的递推无任何共用产物，
/// 故两者相符才说明读数与派发同源。⚠️ 不用 `HitNode.origin` 当基准——此前的教训是它在滚动
/// 容器内本身不是窗口坐标；密集探针则无论坐标系如何都只反映「点得到与否」。
///
/// 命中判定走闭区间（`Rect::contains`），故实测框**至多比真框大一个步长**、不会偏小；
/// 判据的容差即取该步长。
///
/// @param root 命中测试的根。
/// @param target 待测控件。
/// @param hint 搜索窗中心（来自 `window_bounds()`，只用于定位搜索范围，不参与等式本身）。
/// @return 实测可达框；点集为空时返回 `std::nullopt`。
auto probe_reachable_box(Widget &root, const Widget *target, const Rect &hint) -> std::optional<Rect> {
    // 扫描用**整型**计数而非浮点累加：浮点循环计数器会被 `bugprone-float-loop-counter` 与
    // `clang-analyzer-security.FloatLoopCounter` 判红，且步进累积误差会让边界格点漂移。
    // 步长本身是 0.25（二进制可精确表示），故「格点序号 × 步长」与逐次加法逐位等价。
    const auto steps_x = static_cast<int>((hint.size.width + (2.0F * AURORA_PROBE_SPAN)) / AURORA_PROBE_STEP);
    const auto steps_y = static_cast<int>((hint.size.height + (2.0F * AURORA_PROBE_SPAN)) / AURORA_PROBE_STEP);
    const float x0 = hint.origin.x - AURORA_PROBE_SPAN;
    const float y0 = hint.origin.y - AURORA_PROBE_SPAN;
    bool any = false;
    float min_x = 0.0F;
    float min_y = 0.0F;
    float max_x = 0.0F;
    float max_y = 0.0F;
    for (int iy = 0; iy <= steps_y; ++iy) {
        const float y = y0 + (static_cast<float>(iy) * AURORA_PROBE_STEP);
        for (int ix = 0; ix <= steps_x; ++ix) {
            const float x = x0 + (static_cast<float>(ix) * AURORA_PROBE_STEP);
            if (!chain_hits(root, Point{.x = x, .y = y}, target)) {
                continue;
            }
            if (!any) {
                min_x = x;
                min_y = y;
                max_x = x;
                max_y = y;
                any = true;
                continue;
            }
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return Rect{.origin = Point{.x = min_x, .y = min_y}, .size = Size{.width = max_x - min_x, .height = max_y - min_y}};
}

/// @brief 派发一次「按下 + 抬起」，返回探针是否收到过按下。
///
/// `EventDispatcher` 是进程内持久单例且 Press 会建立指针捕获（见其 `@note`），故 Press 必配对
/// Release，否则捕获表会把下一次无关派发带偏（同批用例的既有纪律）。
///
/// @param root 派发起点。
/// @param probe 事件探针。
/// @param at 全局派发点（窗口逻辑 dp）。
/// @return 该探针收到过事件为 true。
auto press_at(Widget &root, DispatchProbe &probe, const Point &at) -> bool {
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = at;
    (void)EventDispatcher::dispatch(root, press, nullptr);

    MouseEvent release;
    release.action = MouseAction::Release;
    release.button = MouseButton::Left;
    release.position = at;
    (void)EventDispatcher::dispatch(root, release, nullptr);
    return probe.seen();
}

AURORA_TEST_CASE(popup_content_window_bounds_matches_probe_measured_reachable_box) {
    // 形态①：`Popup` 内容控件的窗口盒须与命中链实测可达框相符。
    // 树把 `OverlayHost` 摆在 y = LEAD_IN 的非零位置，故「叠加锚点」与「替换基准」两种读法
    // 在此必然分叉（前者 y 会多出一个 LEAD_IN），判据对修复机制本身敏感。
    PopupTree f = make_popup_tree(AURORA_ANCHOR);
    AURORA_TEST_REQUIRE(f.popup->is_open());
    // 缺口前提：`Popup` 在常规流中占零尺寸（否则内容子盒 origin 不会是 {0,0}）。
    AURORA_TEST_CHECK(f.popup->size().width <= 0.0F);
    AURORA_TEST_CHECK(f.popup->size().height <= 0.0F);

    const Rect wb = require_value(f.target->window_bounds());
    const Rect reachable = require_value(probe_reachable_box(*f.root, f.target.get(), wb));
    // 基准非空且确实是「一块有面积的框」——否则下面的等式会与空集恒真。
    AURORA_TEST_CHECK(reachable.size.width > 0.0F);
    AURORA_TEST_CHECK(reachable.size.height > 0.0F);
    // 与实测可达框相符，容差取探针步长（闭区间命中 ⇒ 实测框至多大一个步长）。
    AURORA_TEST_CHECK_NEAR(wb.origin.x, reachable.origin.x, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.origin.y, reachable.origin.y, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.size.width, reachable.size.width, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.size.height, reachable.size.height, AURORA_PROBE_STEP);

    // 独立复算真窗口位：anchor + 内容盒内偏移（探针上方 6dp 首行）。**不得**含 `OverlayHost`
    // 在树上的位置——那正是本次要钉死的语义：`anchor_` 是全局坐标，替换基准而非叠加。
    AURORA_TEST_CHECK_NEAR(wb.origin.x, AURORA_ANCHOR.x, 1e-3F);
    AURORA_TEST_CHECK_NEAR(wb.origin.y, AURORA_ANCHOR.y + AURORA_CONTENT_HEAD, 1e-3F);
    AURORA_TEST_CHECK_NEAR(wb.size.width, AURORA_PROBE_W, 1e-3F);
    AURORA_TEST_CHECK_NEAR(wb.size.height, AURORA_PROBE_H, 1e-3F);
    // 反向钉住「不是错值」：叠加读法（宿主树上位置 + anchor）比本读数恰多一个 LEAD_IN。
    AURORA_TEST_CHECK(wb.origin.y != AURORA_LEAD_IN + AURORA_ANCHOR.y + AURORA_CONTENT_HEAD);
    AURORA_TEST_CHECK_NEAR(wb.origin.y + AURORA_LEAD_IN,
                           AURORA_LEAD_IN + AURORA_ANCHOR.y + AURORA_CONTENT_HEAD, 1e-3F);
}

AURORA_TEST_CASE(plain_subtree_window_bounds_matches_probe_measured_reachable_box) {
    // 形态②：未挂 `Popup` 的普通子树，两者同样相符——守住缺省路径零变化。
    // 任何多减 / 少减一份平移（滚动修正、Modifier 内容平移、锚点）都会在这里现形。
    PlainTree f = make_plain_tree();
    const Rect wb = require_value(f.target->window_bounds());
    const Rect reachable = require_value(probe_reachable_box(*f.root, f.target.get(), wb));

    AURORA_TEST_CHECK(reachable.size.width > 0.0F);
    AURORA_TEST_CHECK(reachable.size.height > 0.0F);
    AURORA_TEST_CHECK_NEAR(wb.origin.x, reachable.origin.x, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.origin.y, reachable.origin.y, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.size.width, reachable.size.width, AURORA_PROBE_STEP);
    AURORA_TEST_CHECK_NEAR(wb.size.height, reachable.size.height, AURORA_PROBE_STEP);

    // 容差 0 的独立复算：占位行高，无任何修正项参与。
    AURORA_TEST_CHECK(wb.origin.x == 0.0F);
    AURORA_TEST_CHECK(wb.origin.y == AURORA_LEAD_IN);
    AURORA_TEST_CHECK(wb.size.width == AURORA_PROBE_W);
    AURORA_TEST_CHECK(wb.size.height == AURORA_PROBE_H);
}

AURORA_TEST_CASE(popup_paint_and_hit_semantics_unchanged) {
    // 判据③：钉住 `Popup` 现有绘制与命中语义，作为两形态用例的基线。
    // `anchor_` 的施加方式不止一种（绘制下传盒 / 命中链 contains 判定），两者都是既有语义，
    // 本次只补事后查询腿，不得改动。
    const Point anchor = AURORA_ANCHOR;
    PopupTree f = make_popup_tree(anchor);
    AURORA_TEST_REQUIRE(f.popup->is_open());

    // 绘制腿：内容盒原点仍为 `anchor_`（全局），不含 `OverlayHost` 在树上的位置。
    const Rect content_box = f.popup->content_bounds();
    AURORA_TEST_CHECK(content_box.origin.x == anchor.x);
    AURORA_TEST_CHECK(content_box.origin.y == anchor.y);
    AURORA_TEST_CHECK_NEAR(content_box.size.height, AURORA_CONTENT_HEAD + AURORA_PROBE_H, 1e-3F);

    // 命中腿：`anchor_` 盒内命中内容、盒外不命中（`content_box` 闭区间语义）。
    const Point inside{.x = anchor.x + 60.0F, .y = anchor.y + AURORA_CONTENT_HEAD + 10.0F};
    AURORA_TEST_CHECK(chain_hits(*f.root, inside, f.target.get()));
    AURORA_TEST_CHECK_FALSE(chain_hits(*f.root, Point{.x = anchor.x - 1.0F, .y = inside.y}, f.target.get()));
    AURORA_TEST_CHECK_FALSE(chain_hits(*f.root, Point{.x = inside.x, .y = anchor.y - 1.0F}, f.target.get()));

    // 关闭态：内容既不参与命中，窗口盒基准也不被替换（`child_content_origin` 返回 nullopt）。
    // 这里从**可观测后果**判：命中链为空，且内容控件窗口盒回到「宿主位置 + 内容内偏移」，
    // 而非 anchor —— 若关闭态仍返回 anchor，这条会转红。
    // ⚠️ 必须**换一组约束**：`Popup::close()` 只标脏 paint 不标脏 layout，约束不变的第二次
    // 布局会命中布局缓存、整段跳过 `on_layout`，`content_size_` 不会归零（同 `utest_overlay_host`
    // 里 provider 换约束的同款纪律）。
    f.popup->close();
    LayoutEngine::layout(*f.root, bounded(321.0F, 241.0F));
    AURORA_TEST_CHECK(f.popup->content_bounds().size.height <= 0.0F);
    AURORA_TEST_CHECK_FALSE(chain_hits(*f.root, inside, f.target.get()));
    const Rect closed_wb = require_value(f.target->window_bounds());
    AURORA_TEST_CHECK_NEAR(closed_wb.origin.x, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(closed_wb.origin.y, AURORA_LEAD_IN + AURORA_CONTENT_HEAD, 1e-3F);
}

AURORA_TEST_CASE(dispatch_by_window_bounds_lands_on_popup_content) {
    // 判据④：端到端正面形态——按 `window_bounds()` 的读数取点派发，内容控件须收到事件，
    // 且 `local_position` 等于该点在盒内的偏移。这是缺口原始症状（取点落不到控件 → 字符被基类
    // `Widget::on_text_input` 缺省 `is_handled = true` 静默吞掉）的正面闭合。
    PopupTree f = make_popup_tree(AURORA_ANCHOR);
    const Rect wb = require_value(f.target->window_bounds());
    const Point probe_point{.x = wb.origin.x + 60.0F, .y = wb.origin.y + 10.0F};

    f.target->clear_records();
    AURORA_TEST_REQUIRE(press_at(*f.root, *f.target, probe_point));
    AURORA_TEST_CHECK(f.target->seen());
    AURORA_TEST_CHECK(f.target->last_position().x == probe_point.x);
    AURORA_TEST_CHECK(f.target->last_position().y == probe_point.y);
    // 本地化坐标 = 派发点在盒内的偏移。这条同时钉住「origin 少减 / 多减一份平移」。
    AURORA_TEST_CHECK_NEAR(f.target->last_local().x, 60.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(f.target->last_local().y, 10.0F, 1e-3F);

    // 同一探针控件按**叠加读数**（宿主树上位置 + anchor，修复前的机制）取点落不到它——
    // 钉住缺口真实存在、非判据自证。
    f.target->clear_records();
    const Point unanchored{.x = 60.0F, .y = AURORA_LEAD_IN + AURORA_ANCHOR.y + AURORA_CONTENT_HEAD + 10.0F};
    AURORA_TEST_CHECK_FALSE(press_at(*f.root, *f.target, unanchored));
    AURORA_TEST_CHECK_FALSE(f.target->seen());
}

// ---- Popup 挂普通容器：绘制与命中均可达（坐标系重映射宿主的两侧放行） ----
//
// 缺口：`Popup` 在常规流中占**零尺寸**盒，而 `Container` 的三道闸都按「盒与区域有交集」判定——
// 绘制侧 `!global.intersects(clip)`（`Rect::intersects` 是严格比较）、命中侧 `cb.contains(local)`
// （闭区间但零尺寸恒假）⇒ 挂在裸容器下整棵被跳过，浮层**画不出也点不到，且不抛错不告警**。
// `Popup::covers_descendant_extra_hit_box` 恒 false 是「不申报追加盒」，并不能替代这两道闸的放行。
//
// 判据纪律：绘制侧用**像素级**判据（画布预填白、内容落墨即有深色像素），命中侧用命中链长度。
// 两者必须**同时**成立——只修命中会得到「点得到但看不见」，只修绘制会得到「看得见但点不到」，
// 都比两者都不改更难排查。

/// @brief 墨迹探针：在自身盒内铺满黑色，供像素级判据「内容真的画出来了」使用。
///
/// ⚠️ 用**不透明**黑：判据是「某像素 r < 阈值」，画布必须预填不透明白，否则半透明叠加会
/// 让空白区也偏暗、判据空转（同「像素判据不能在不透明底上判有墨」的纪律）。
class InkProbe final : public Widget {
  public:
    InkProbe(float w, float h) : w_(w), h_(h) {
        // 必须挂 Clickable：`wants_click()` 缺省只看修饰链，不挂则基类不认它为命中目标、
        // 命中链在自身处判假（本类无子节点）⇒ 盒内探点拿不到非空链。绘制不受此影响。
        modifier.set(Modifier{}.clickable([]() -> void {}));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "InkProbe"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{0, 0, 0, 255});
    }

  private:
    float w_;
    float h_;
};

/// @brief 负守卫探针：暴露两个新钩子的**缺省返回值**，并铺一块远大于自身盒的墨迹。
///
/// 单靠外部可观测行为（墨迹数 / 命中链长度）不足以钉住缺省值：绘制侧放行走
/// `paints_outside_layout_box()`、命中侧放行走 `covers_remapped_descendant()`，二者**各管一侧**，
/// 变异其一另一侧仍绿 ⇒ 外部行为可能看不出差别（实测：命中侧缺省改 true 时零尺寸兄弟照样被剔除，
/// 因它自身无子节点、无内容映射，闸放行后下降仍返回空链）。故本类把两个缺省值直接暴露出来断言。
class ZeroBoxGateProbe final : public Widget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "ZeroBoxGateProbe"; }
    /// @brief 绘制侧放行钩子的缺省返回值（本类未覆写 ⇒ 应为 false）。
    [[nodiscard]] auto paint_gate_default() const -> bool { return paints_outside_layout_box(); }
    /// @brief 命中侧放行钩子的缺省返回值（本类未覆写 ⇒ 应为 false）。
    [[nodiscard]] auto hit_gate_default() const -> bool {
        return covers_remapped_descendant(Point{.x = 0.0F, .y = 0.0F}, BuildContext{}, Point{.x = 0.0F, .y = 0.0F});
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }
    auto on_paint(Painter &p, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 60.0F, .height = 60.0F}},
                    Color{0, 0, 0, 255});
    }
};

/// @brief 像素级判据：绘制整棵树，数「深色像素」个数。
/// @param root 绘制起点。
/// @param w 画布宽（dp）。
/// @param h 画布高（dp）。
/// @return r 通道低于 128 的像素数（画布预填不透明白，故 0 = 什么都没画出来）。
auto count_ink_pixels(Widget &root, int w, int h) -> int {
    Painter p;
    p.begin(w, h);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                Color{255, 255, 255, 255});
    root.paint(p,
               Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                    .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
               BuildContext{});
    int inked = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (p.get_pixel(x, y).r < 128) {
                ++inked;
            }
        }
    }
    return inked;
}

/// @brief 搭「裸 `Column` + 打开的 `Popup`（内容 = 墨迹探针）」并完成布局。
/// @param anchor 浮层锚点（窗口绝对坐标）。
/// @return 已完成布局的 fixture（`root` 即绘制 / 命中起点）。
auto make_bare_column_popup(const Point &anchor) -> std::shared_ptr<Column> {
    auto root = std::make_shared<Column>();
    auto popup = std::make_shared<Popup>(Node{std::make_shared<InkProbe>(80.0F, 40.0F)});
    root->add(Node{popup});
    LayoutEngine::layout(*root, bounded(200.0F, 200.0F));
    // 先布局再打开：关闭态下 Popup 不测量内容，打开后需再走一次布局才拿到内容尺寸。
    popup->open_at(anchor);
    LayoutEngine::layout(*root, bounded(200.0F, 200.0F));
    return root;
}

AURORA_TEST_CASE(popup_in_plain_column_is_painted_and_hittable) {
    // 判据①：裸 `Column` 挂打开的 `Popup`，像素级判据 + 命中链判据**同时**成立。
    // 修复前实测：painted = 0、命中链 = 0（整棵被零尺寸闸跳过）。
    constexpr Point anchor{.x = 20.0F, .y = 20.0F};
    const std::shared_ptr<Column> root = make_bare_column_popup(anchor);

    // 绘制侧：内容盒（80×40，落在 anchor 处）真的落墨。
    const int inked = count_ink_pixels(*root, 200, 200);
    AURORA_TEST_CHECK(inked > 0);
    // 上界不做断言：字号/抗锯齿随平台变，只钉「有墨」这个方向（避免把判据绑到具体字形度量上）。

    // 命中侧：`anchor_` 盒内一点命中链**非空**、盒外一点命中链**为空**。
    const Rect root_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 200.0F}};
    const auto inside_chain =
        root->hit_test_chain(Point{.x = anchor.x + 40.0F, .y = anchor.y + 20.0F}, root_box, BuildContext{});
    const auto outside_chain =
        root->hit_test_chain(Point{.x = anchor.x - 5.0F, .y = anchor.y + 20.0F}, root_box, BuildContext{});
    AURORA_TEST_CHECK(!inside_chain.empty());
    AURORA_TEST_CHECK(outside_chain.empty());
}

AURORA_TEST_CASE(plain_container_gate_still_culls_zero_sized_siblings) {
    // 判据②：负守卫——普通零尺寸控件（**未**覆写新钩子）仍被绘制闸与命中闸跳过，
    // 且两个放行钩子的**缺省值都是 false**。
    //
    // 两段缺一不可：绘制侧放行走 `paints_outside_layout_box`、命中侧放行走
    // `covers_remapped_descendant`，二者各管一侧。把任一缺省改成 true，只会让**一侧**的外部
    // 行为变样，另一侧仍绿——只钉外部行为会漏过这种变异（实测：命中侧缺省改 true 时零尺寸
    // 兄弟照样被剔除，因它自身无子节点、无内容映射，闸放行后 `on_hit_test_chain` 仍返回空链）。
    auto gate = std::make_shared<ZeroBoxGateProbe>();
    auto root = std::make_shared<Column>();
    root->add(Node{gate});
    LayoutEngine::layout(*root, bounded(200.0F, 200.0F));

    // 缺省值段：两个钩子都未被覆写 ⇒ 都必须报 false。
    AURORA_TEST_CHECK_FALSE(gate->paint_gate_default());
    AURORA_TEST_CHECK_FALSE(gate->hit_gate_default());

    const Rect root_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 200.0F}};

    // 命中侧：零尺寸兄弟仍被命中闸跳过（cb.contains 闭区间对零尺寸盒恒假 + 两钩子缺省 false）。
    // 这条与 AURORA_ENABLE_OCCLUSION_CULLING 无关，恒成立——下降闸不依赖遮挡剔除宏。
    AURORA_TEST_CHECK(root->hit_test_chain(Point{.x = 10.0F, .y = 10.0F}, root_box, BuildContext{}).empty());

    // 绘制侧：零尺寸兄弟是否被绘制闸剔除，**取决于 AURORA_ENABLE_OCCLUSION_CULLING**。
    // 本用例要钉的是「两钩子缺省值恒为 false」（上面已钉），而**实际剔不剔除**由宏决定：
    // - ON：遮挡剔除闸按 global.intersects(clip) 严格判定，零尺寸盒恒假、且无 paints_outside
    //   放行 ⇒ 整棵被跳过，不应有墨迹（探点 (10,10) 落在它铺的 60×60 墨迹内）。
    // - OFF：无剔除闸，子节点无条件绘制，探针照常铺它的 60×60 墨迹 ⇒ 必有墨迹。
    // 反向不变量（宏关时零尺寸控件不被错误剔除）同样值得守住，故两侧各断言其正确极性。
#ifdef AURORA_ENABLE_OCCLUSION_CULLING
    AURORA_TEST_CHECK(count_ink_pixels(*root, 200, 200) == 0);
#else
    AURORA_TEST_CHECK(count_ink_pixels(*root, 200, 200) > 0);
#endif
}

AURORA_TEST_CASE(popup_draw_and_hit_gates_agree_in_plain_column) {
    // 判据④：三道闸同改的交叉验证——`anchor_` 盒内一点**既被绘制又被命中**，盒外一点**两者都不**。
    // 「绘制认、命中不认」与反向分叉都会让本条转红（只改一侧时）。
    constexpr Point anchor{.x = 20.0F, .y = 20.0F};
    const std::shared_ptr<Column> root = make_bare_column_popup(anchor);
    const Rect root_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 200.0F}};

    // 盒内：命中链非空（命中侧放行），且该点所在像素已落墨（绘制侧放行）。
    const Point inside{.x = anchor.x + 40.0F, .y = anchor.y + 20.0F};
    AURORA_TEST_CHECK(!root->hit_test_chain(inside, root_box, BuildContext{}).empty());
    Painter p;
    p.begin(200, 200);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 200.0F}},
                Color{255, 255, 255, 255});
    root->paint(p, root_box, BuildContext{});
    AURORA_TEST_CHECK(p.get_pixel(static_cast<int>(inside.x), static_cast<int>(inside.y)).r < 128);

    // 盒外：命中链为空，且该点仍是画布底色（未被画出来）。
    const Point outside{.x = anchor.x - 5.0F, .y = anchor.y + 20.0F};
    AURORA_TEST_CHECK(root->hit_test_chain(outside, root_box, BuildContext{}).empty());
    AURORA_TEST_CHECK(p.get_pixel(static_cast<int>(outside.x), static_cast<int>(outside.y)).r >= 128);
}

}  // namespace aurora::test_cases::utest_overlay_host
