/// 测试类型: unit
/// 目标单元: include/aurora/widget/tab_bar.h
/// 测试说明: 覆盖 TabBar 的运行期追加标签挂载时机——控件自身已布局后 `add_tab` 追加的内容子树
/// 必须在**下一次布局**由父侧 ctx 挂上（`TabBar` 的补挂机制），调用方不自带 `BuildContext`、
/// 也不自行 `mount`。修复前 `add_tab` 只做 `push_back` + `mark_needs_layout`，而 `on_mount` 是
/// 「挂载时遍历 tabs_」，故运行期追加的 tab 内容永不挂载：内容里的控件主题不跟、定时档不启动，
/// 且全程无日志——症状与「代码没写」同形。
///
/// 判据：追加当刻未挂载（父侧尚无 ctx 可用），一次布局后恰好挂载 1 次、订阅条数为固定值、
/// 挂到的是宿主 ctx；同宿主重复布局不重复挂载。

#include <cstddef>
#include <memory>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/tab_bar.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_tab_bar {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 挂载观测控件：记录挂载 / 卸载次数、挂载时看到的主题主色与宿主身份、订阅条数。
class MountProbe final : public Widget {
  public:
    int mounts = 0;  ///< `on_mount` 触发次数
    int unmounts = 0;  ///< `on_unmount` 触发次数
    bool theme_seen_at_mount = false;  ///< 挂载时是否读到了宿主注入的主题
    Color primary_at_mount;  ///< 挂载时读到的主题主色

    [[nodiscard]] auto type_name() const -> const char * override { return "MountProbe"; }
    /// @brief 额外登记一个自有信号，使订阅条数 = 自有 1 + `modifier` + `show` = 3（固定值可硬断言）。
    /// @param out 信号视图累加表（本控件追加自身信号）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&tick_state_); }
    [[nodiscard]] auto effect_count() const -> std::size_t { return effects_.size(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        ++mounts;
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

AURORA_TEST_CASE(runtime_added_tab_content_is_mounted_on_next_layout) {
    Theme theme;
    theme.primary = Color::from_rgba(77, 88, 99);
    auto bar = std::make_shared<TabBar>();
    bar->add_tab(Tab{.label = "first"});
    Provider<Theme> provider{theme, Node{bar}};
    LayoutEngine::layout(provider, bounded(320.0F, 240.0F));

    auto probe = std::make_shared<MountProbe>();
    bar->add_tab(Tab{.label = "second", .content = Node{probe}});
    AURORA_TEST_CHECK_EQ(bar->tab_count(), 2U);
    // 追加那一刻父侧未必有 ctx 可用，故此刻**不得**已挂载。
    AURORA_TEST_CHECK_EQ(probe->mounts, 0);

    LayoutEngine::layout(provider, bounded(320.0F, 240.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->unmounts, 0);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
    // 挂到的是宿主的 ctx：主题注入读得到，不是无环境的裸 ctx。
    AURORA_TEST_CHECK_TRUE(probe->theme_seen_at_mount);
    AURORA_TEST_CHECK_TRUE(probe->primary_at_mount == theme.primary);

    // 同宿主重复布局不重复挂载（既有幂等保护不得被补挂机制绕过）。
    LayoutEngine::layout(provider, bounded(320.0F, 240.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
}

AURORA_TEST_CASE(all_tabs_added_before_mount_need_no_deferred_pass) {
    // 建树期一次性追加的标签仍走 `on_mount` 那条路径（补挂位在首次布局时被消费掉，不重复触发）。
    auto bar = std::make_shared<TabBar>();
    auto probe = std::make_shared<MountProbe>();
    bar->add_tab(Tab{.label = "only", .content = Node{probe}});
    const BuildContext ctx;
    bar->mount(ctx);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    LayoutEngine::layout(*bar, bounded(320.0F, 240.0F));
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
}

}  // namespace aurora::test_cases::utest_tab_bar
