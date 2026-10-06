/// 测试类型: unit
/// 目标单元: include/aurora/widget/popup.h（OverlayHost）
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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/environment/environment.h"
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

}  // namespace aurora::test_cases::utest_overlay_host
