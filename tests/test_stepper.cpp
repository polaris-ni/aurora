#include "aurora/widget/stepper.h"

#include "test_harness.h"

using aurora::Json;
using aurora::Stepper;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_stepper ===\n");

    // --- 构造 / 空状态 ---
    {
        const Stepper st;
        AURORA_TEST_CHECK(st.type_name() == std::string("Stepper"));
        AURORA_TEST_CHECK(st.steps().empty());
        AURORA_TEST_CHECK(st.current() == 0);
    }

    // --- 带 steps 构造 ---
    {
        const Stepper st({ { .label = "Step 1" }, { .label = "Step 2" }, { .label = "Step 3" } }, 0);
        AURORA_TEST_CHECK(st.steps().size() == 3);
        AURORA_TEST_CHECK(st.current() == 0);
        AURORA_TEST_CHECK(!st.is_last_step());
    }

    // --- next / prev ---
    {
        Stepper st({ { .label = "A" }, { .label = "B" }, { .label = "C" } }, 0);
        AURORA_TEST_CHECK(st.next()); // 0 -> 1
        AURORA_TEST_CHECK(st.current() == 1);
        AURORA_TEST_CHECK(st.next()); // 1 -> 2
        AURORA_TEST_CHECK(st.current() == 2);
        AURORA_TEST_CHECK(st.is_last_step());
        AURORA_TEST_CHECK(!st.next()); // 最后一步，触发 on_complete，返回 false
    }

    // --- prev 边界 ---
    {
        Stepper st({ { .label = "A" }, { .label = "B" } }, 0);
        AURORA_TEST_CHECK(!st.prev()); // 已在第 0 步
        AURORA_TEST_CHECK(st.current() == 0);
        st.next();
        AURORA_TEST_CHECK(st.prev());
        AURORA_TEST_CHECK(st.current() == 0);
    }

    // --- validate 阻止前进 ---
    {
        bool can_proceed = false;
        Stepper st({ { .label = "Step 1", .validate = [&]() -> bool { return can_proceed; } } }, 0);
        AURORA_TEST_CHECK(!st.next()); // validate 返回 false
        AURORA_TEST_CHECK(st.current() == 0);
        can_proceed = true;
        // 单步骤，validate 通过后触发 on_complete
        bool completed = false;
        st.set_on_complete([&]() -> void { completed = true; });
        AURORA_TEST_CHECK(!st.next()); // 最后一步，触发 on_complete
        AURORA_TEST_CHECK(completed);
    }

    // --- on_cancel 回调 ---
    {
        Stepper st({ { .label = "A" }, { .label = "B" } });
        bool cancelled = false;
        st.set_on_cancel([&]() -> void { cancelled = true; });
        // on_cancel 由 UI 点击触发，这里只验证 setter
        AURORA_TEST_CHECK(!cancelled);
    }

    // --- describe_static ---
    {
        const auto desc = Stepper::describe_static();
        AURORA_TEST_CHECK(desc.name == "Stepper");
        bool has_current = false;
        for (const auto &p : desc.properties) {
            if (p.name == "current") {
                has_current = true;
            }
        }
        AURORA_TEST_CHECK(has_current);
        AURORA_TEST_CHECK(desc.children_policy == "none");
    }

    // --- 序列化往返 ---
    {
        const Stepper st({ { .label = "A" }, { .label = "B" }, { .label = "C" } }, 2);
        Json props = Json::object();
        st.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("current"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["current"].get<int>() == 2);
        AURORA_TEST_CHECK(props.contains("step_count"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(props["step_count"].get<int>() == 3);

        Stepper st2({ { .label = "X" }, { .label = "Y" }, { .label = "Z" } });
        st2.deserialize_props(props);
        AURORA_TEST_CHECK(st2.current() == 2);
    }
}