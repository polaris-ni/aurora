/// 测试类型: unit
/// 目标单元: include/aurora/state/undo_stack.h
/// 测试说明: UndoStack 的空栈不变量、push 执行 redo 与描述查询、undo/redo 光标往返、push
/// 截断重做分支、深度上限丢弃最旧与 0 钳制、clear 清空，以及 macro 顺序重做/逆序撤销

#include <string>
#include <utility>
#include <vector>

#include "aurora/state/undo_stack.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_undo_stack {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）

AURORA_TEST_CASE(fresh_stack_reports_no_undo_redo) {
    // 空栈不变量：无历史、光标在 0、默认上限 100，undo/redo 均无事发生返回 false。
    const UndoStack stack;
    AURORA_TEST_CHECK_FALSE(stack.can_undo());
    AURORA_TEST_CHECK_FALSE(stack.can_redo());
    AURORA_TEST_CHECK_EQ(stack.count(), 0U);
    AURORA_TEST_CHECK_EQ(stack.index(), 0U);
    AURORA_TEST_CHECK_EQ(stack.limit(), 100U);
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{});
    AURORA_TEST_CHECK_EQ(stack.redo_description(), std::string{});

    UndoStack mutable_stack;
    AURORA_TEST_CHECK_FALSE(mutable_stack.undo());
    AURORA_TEST_CHECK_FALSE(mutable_stack.redo());
}

AURORA_TEST_CASE(push_executes_redo_and_updates_descriptions) {
    // push 即执行 redo 并推进光标；undo_description 指向它，redo_description 为空。
    UndoStack stack;
    int value = 1;
    stack.push(UndoCommand{
        .redo = [&]() -> void { value = 2; },
        .undo = [&]() -> void { value = 1; },
        .description = "set value to 2",
    });
    AURORA_TEST_CHECK_EQ(value, 2);
    AURORA_TEST_CHECK_EQ(stack.count(), 1U);
    AURORA_TEST_CHECK_EQ(stack.index(), 1U);
    AURORA_TEST_CHECK_TRUE(stack.can_undo());
    AURORA_TEST_CHECK_FALSE(stack.can_redo());
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"set value to 2"});
    AURORA_TEST_CHECK_EQ(stack.redo_description(), std::string{});
}

AURORA_TEST_CASE(undo_redo_roundtrip_follows_cursor) {
    // undo/redo 沿光标移动：执行对应命令并同步描述查询。
    UndoStack stack;
    int value = 1;
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "to 2"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 3; }, .undo = [&]() -> void { value = 2; }, .description = "to 3"});
    AURORA_TEST_CHECK_EQ(stack.count(), 2U);
    AURORA_TEST_CHECK_EQ(stack.index(), 2U);

    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 2);
    AURORA_TEST_CHECK_EQ(stack.index(), 1U);
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"to 2"});
    AURORA_TEST_CHECK_EQ(stack.redo_description(), std::string{"to 3"});

    AURORA_TEST_CHECK_TRUE(stack.redo());
    AURORA_TEST_CHECK_EQ(value, 3);
    AURORA_TEST_CHECK_EQ(stack.index(), 2U);
    AURORA_TEST_CHECK_FALSE(stack.can_redo());

    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 1);
    AURORA_TEST_CHECK_FALSE(stack.can_undo());
    AURORA_TEST_CHECK_FALSE(stack.undo());  // 栈底再撤无事发生
}

AURORA_TEST_CASE(push_truncates_redo_branch) {
    // undo 后 push 新命令：重做分支被永久截断，被截断命令的 redo/undo 不再触达。
    UndoStack stack;
    int value = 1;
    int b_redo_runs = 0;
    int b_undo_runs = 0;
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "A"});
    stack.push(UndoCommand{.redo = [&]() -> void {
                               ++b_redo_runs;
                               value = 3;
                           },
                           .undo = [&]() -> void {
                               ++b_undo_runs;
                               value = 2;
                           },
                           .description = "B"});
    AURORA_TEST_REQUIRE_TRUE(stack.undo());  // 撤销 B → 出现可重做分支
    AURORA_TEST_CHECK_EQ(b_undo_runs, 1);

    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 9; }, .undo = [&]() -> void { value = 2; }, .description = "C"});
    AURORA_TEST_CHECK_EQ(value, 9);  // 新命令立即执行
    AURORA_TEST_CHECK_EQ(stack.count(), 2U);  // 历史只剩 A、C
    AURORA_TEST_CHECK_EQ(stack.index(), 2U);
    AURORA_TEST_CHECK_FALSE(stack.can_redo());
    AURORA_TEST_CHECK_FALSE(stack.redo());
    AURORA_TEST_CHECK_EQ(b_redo_runs, 1);  // B 的 redo 不会被重放
    AURORA_TEST_CHECK_EQ(b_undo_runs, 1);  // 截断后 B 的 undo 也不会再触发
}

AURORA_TEST_CASE(limit_drops_oldest_commands_on_overflow) {
    // 深度上限：超限丢弃最旧命令，被丢弃者不参与撤销。
    UndoStack stack;
    stack.set_limit(2);
    AURORA_TEST_CHECK_EQ(stack.limit(), 2U);

    int a_undo_runs = 0;
    int value = 0;
    stack.push(UndoCommand{.redo = [&]() -> void { value = 1; },
                           .undo = [&]() -> void {
                               ++a_undo_runs;
                               value = 0;
                           },
                           .description = "A"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "B"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 3; }, .undo = [&]() -> void { value = 2; }, .description = "C"});
    AURORA_TEST_CHECK_EQ(stack.count(), 2U);  // 最旧的 A 被丢弃
    AURORA_TEST_CHECK_EQ(stack.index(), 2U);

    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 2);
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"B"});
    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 1);
    AURORA_TEST_CHECK_FALSE(stack.can_undo());
    AURORA_TEST_CHECK_EQ(a_undo_runs, 0);  // 被丢弃的命令不参与撤销
}

AURORA_TEST_CASE(set_limit_shrinks_history_and_clamps_zero) {
    // 收紧上限立即裁剪既有历史（保留最新）；0 视为 1（至少保留一步）。
    UndoStack stack;
    int value = 0;
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "A"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "B"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 3; }, .undo = [&]() -> void { value = 2; }, .description = "C"});

    stack.set_limit(1);
    AURORA_TEST_CHECK_EQ(stack.count(), 1U);
    AURORA_TEST_CHECK_EQ(stack.index(), 1U);
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"C"});
    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 2);

    stack.set_limit(0);
    AURORA_TEST_CHECK_EQ(stack.limit(), 1U);
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 9; }, .undo = [&]() -> void { value = 2; }, .description = "D"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 10; }, .undo = [&]() -> void { value = 9; }, .description = "E"});
    AURORA_TEST_CHECK_EQ(stack.count(), 1U);  // 超限即丢 D
    AURORA_TEST_CHECK_EQ(stack.index(), 1U);
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"E"});
    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 9);
}

AURORA_TEST_CASE(clear_resets_history_and_cursor) {
    // clear：历史与光标全部归零，之后仍可正常入栈与撤销。
    UndoStack stack;
    int value = 0;
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "A"});
    stack.push(
        UndoCommand{.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "B"});
    AURORA_TEST_REQUIRE_TRUE(stack.undo());

    stack.clear();
    AURORA_TEST_CHECK_EQ(stack.count(), 0U);
    AURORA_TEST_CHECK_EQ(stack.index(), 0U);
    AURORA_TEST_CHECK_FALSE(stack.can_undo());
    AURORA_TEST_CHECK_FALSE(stack.can_redo());
    AURORA_TEST_CHECK_FALSE(stack.undo());
    AURORA_TEST_CHECK_FALSE(stack.redo());

    stack.push(UndoCommand{
        .redo = [&]() -> void { value = 5; }, .undo = [&]() -> void { value = 0; }, .description = "after clear"});
    AURORA_TEST_CHECK_TRUE(stack.can_undo());
    AURORA_TEST_CHECK_EQ(stack.undo_description(), std::string{"after clear"});
    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_CHECK_EQ(value, 0);
}

AURORA_TEST_CASE(macro_replays_redo_in_order_and_undo_in_reverse) {
    // 宏命令：一次 push 整组按序重做、逆序撤销；空宏为安全空操作。
    std::vector<std::string> trace;
    const UndoCommand first{.redo = [&]() -> void { trace.emplace_back("redo:a"); },
                            .undo = [&]() -> void { trace.emplace_back("undo:a"); },
                            .description = "a"};
    const UndoCommand second{.redo = [&]() -> void { trace.emplace_back("redo:b"); },
                             .undo = [&]() -> void { trace.emplace_back("undo:b"); },
                             .description = "b"};

    auto batch = UndoStack::macro({first, second}, "batch");
    AURORA_TEST_CHECK_EQ(batch.description, std::string{"batch"});

    UndoStack stack;
    stack.push(std::move(batch));
    AURORA_TEST_CHECK_EQ(stack.count(), 1U);  // 宏以单条命令入栈
    AURORA_TEST_REQUIRE_THAT(trace, m::size_is(2U));
    AURORA_TEST_CHECK_EQ(trace[0], std::string{"redo:a"});
    AURORA_TEST_CHECK_EQ(trace[1], std::string{"redo:b"});

    AURORA_TEST_CHECK_TRUE(stack.undo());
    AURORA_TEST_REQUIRE_THAT(trace, m::size_is(4U));  // 逆序撤销整组
    AURORA_TEST_CHECK_EQ(trace[2], std::string{"undo:b"});
    AURORA_TEST_CHECK_EQ(trace[3], std::string{"undo:a"});

    AURORA_TEST_CHECK_TRUE(stack.redo());
    AURORA_TEST_REQUIRE_THAT(trace, m::size_is(6U));  // 整组按原顺序重做
    AURORA_TEST_CHECK_EQ(trace[4], std::string{"redo:a"});
    AURORA_TEST_CHECK_EQ(trace[5], std::string{"redo:b"});

    // 空宏：redo/undo 均为空操作，但仍占一条历史。
    UndoStack empty_stack;
    empty_stack.push(UndoStack::macro({}, "empty"));
    AURORA_TEST_CHECK_EQ(empty_stack.count(), 1U);
    AURORA_TEST_CHECK_TRUE(empty_stack.undo());
    AURORA_TEST_CHECK_TRUE(empty_stack.redo());
}

}  // namespace aurora::test_cases::utest_undo_stack
