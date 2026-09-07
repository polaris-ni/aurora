/// 测试类型: unit
/// 目标单元: include/aurora/navigation/transition_layer.h
/// 测试说明: 转场合成层（类型名、不可缓存 Display List、自描述 multiple、for_each_child 遍历新旧页、不订阅 progress）单元测试

#include <string>
#include <vector>

#include "aurora/navigation/transition_layer.h"
#include "aurora/state/state.h"
#include "aurora/widget/divider.h"
#include "aurora/widget/node.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_transition_layer {

AURORA_TEST() {
    State<double> progress{0.0};

    // ---- 1. 构造 + 类型名 / 不可缓存（内容逐帧变化） ----
    {
        Node old_node = Divider{};
        Node new_node = Divider{};
        TransitionLayer tl(std::move(old_node), std::move(new_node), &progress, TransitionKind::Fade);
        AURORA_TEST_CHECK_EQ(std::string(tl.type_name()), std::string("TransitionLayer"));
        AURORA_TEST_CHECK_FALSE(tl.can_cache_display_list());
    }

    // ---- 2. 自描述：children_policy 为 multiple ----
    {
        Node a = Divider{};
        Node b = Divider{};
        TransitionLayer tl(std::move(a), std::move(b), &progress, TransitionKind::Slide);
        const WidgetDescriptor d = tl.describe();
        AURORA_TEST_CHECK_EQ(std::string(d.name), std::string("TransitionLayer"));
        AURORA_TEST_CHECK_EQ(std::string(d.children_policy), std::string("multiple"));
    }

    // ---- 3. for_each_child：两棵子树各回调一次 ----
    {
        Node old_node = Divider{};
        Node new_node = Divider{};
        TransitionLayer tl(std::move(old_node), std::move(new_node), &progress, TransitionKind::Fade);
        int count = 0;
        tl.for_each_child([&count](const Widget &) -> void { ++count; });
        AURORA_TEST_CHECK_EQ(count, 2);
    }

    // ---- 4. for_each_child：空节点被跳过（operator bool 为假） ----
    {
        TransitionLayer tl(Node{}, Node{}, &progress, TransitionKind::Fade);
        int count = 0;
        tl.for_each_child([&count](const Widget &) -> void { ++count; });
        AURORA_TEST_CHECK_EQ(count, 0);
    }

    // ---- 5. collect_signals：不自行订阅 progress（宿主统一订阅）→ 不产出信号 ----
    {
        Node old_node = Divider{};
        Node new_node = Divider{};
        TransitionLayer tl(std::move(old_node), std::move(new_node), &progress, TransitionKind::Fade);
        std::vector<SignalViewBase *> out;
        tl.collect_signals(out);
        AURORA_TEST_CHECK(out.empty());
    }
}

}  // namespace aurora::test_cases::utest_transition_layer
