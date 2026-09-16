/// 测试类型: unit
/// 目标单元: include/aurora/app/scroll_storage.h
/// 测试说明: 覆盖滚动位置注册表的会话内读写与清除、会话作用域隔离（含嵌套恢复）、Preferences 懒回读与
/// sync 批量写穿（值 + 墓碑）、待落盘条目的键数上界（同键重复写节流），以及同键多持有者认领的一次性诊断

#include <string>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/diagnostics.h"
#include "aurora/preferences/preferences.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scroll_storage {

using preferences::Preferences;

namespace {

/// 用例隔离：注册表是进程级单例，每个用例起手清空（含持久化绑定与作用域）。
auto fresh() -> ScrollStorage & {
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    return storage;
}

/// @brief 判定诊断列表中是否含「同键多认领」提示。
auto has_claim_notice(const std::vector<Diagnostic> &diags) -> bool {
    for (const auto &d : diags) {
        if (std::string{d.where} == "scroll_storage") {
            return true;
        }
    }
    return false;
}

}  // namespace

AURORA_TEST_CASE(write_read_clear_roundtrip) {
    auto &storage = fresh();
    AURORA_TEST_CHECK_FALSE(storage.read("list").has_value());

    storage.write("list", 120.0F);
    AURORA_TEST_CHECK_NEAR(storage.read("list").value_or(-1.0F), 120.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(storage.size(), std::size_t{1});

    storage.write("list", 40.0F);  // 后写覆盖
    AURORA_TEST_CHECK_NEAR(storage.read("list").value_or(-1.0F), 40.0F, 1e-6F);

    storage.clear("list");
    AURORA_TEST_CHECK_FALSE(storage.read("list").has_value());
}

AURORA_TEST_CASE(scope_isolates_keys_and_nesting_restores) {
    auto &storage = fresh();
    storage.write("shared", 10.0F);  // 无作用域：全局单桶

    {
        const ScrollStorage::Scope win_a{"win-a"};
        AURORA_TEST_CHECK_EQ(storage.current_scope(), std::string{"win-a"});
        AURORA_TEST_CHECK_FALSE(storage.read("shared").has_value());  // 隔离：看不到全局桶
        storage.write("shared", 30.0F);

        {
            const ScrollStorage::Scope win_b{"win-b"};
            AURORA_TEST_CHECK_FALSE(storage.read("shared").has_value());  // 隔离：看不到 win-a
            storage.write("shared", 50.0F);
            AURORA_TEST_CHECK_NEAR(storage.read("shared").value_or(-1.0F), 50.0F, 1e-6F);
        }

        // 内层析构后恢复外层作用域与其值。
        AURORA_TEST_CHECK_EQ(storage.current_scope(), std::string{"win-a"});
        AURORA_TEST_CHECK_NEAR(storage.read("shared").value_or(-1.0F), 30.0F, 1e-6F);
    }

    // 外层析构后回到全局单桶。
    AURORA_TEST_CHECK_TRUE(storage.current_scope().empty());
    AURORA_TEST_CHECK_NEAR(storage.read("shared").value_or(-1.0F), 10.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(storage.size(), std::size_t{3});  // 三个键空间各一份
}

AURORA_TEST_CASE(pending_writes_are_bounded_by_key_count) {
    auto &storage = fresh();
    for (int i = 0; i < 100; ++i) {
        storage.write("hot", static_cast<float>(i));  // 模拟滚动帧：反复写同一键
    }
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{1});  // 节流：同键只占 1 条待落盘
    AURORA_TEST_CHECK_NEAR(storage.read("hot").value_or(-1.0F), 99.0F, 1e-6F);
}

AURORA_TEST_CASE(attach_and_sync_write_through_preferences) {
    auto &storage = fresh();
    Preferences prefs;  // 仅内存模式（不触碰磁盘）
    storage.attach(prefs);
    AURORA_TEST_CHECK_TRUE(storage.attached());

    storage.write("feed", 88.0F);
    auto group = prefs.group(ScrollStorage::AURORA_GROUP_NAME);
    AURORA_TEST_CHECK_FALSE(group.contains("feed"));  // attach 不自动落盘：须显式 sync

    storage.sync();
    AURORA_TEST_CHECK_TRUE(group.contains("feed"));
    AURORA_TEST_CHECK_NEAR(group.get<float>("feed", -1.0F), 88.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{0});

    storage.sync();  // 幂等：无待落盘项时不产生副作用
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{0});
}

AURORA_TEST_CASE(read_falls_back_to_preferences_lazily) {
    auto &storage = fresh();
    Preferences prefs;
    // 模拟「上一次运行留下的记录」：直接写进 Preferences，内存注册表里没有。
    prefs.group(ScrollStorage::AURORA_GROUP_NAME).set("feed", 140.0F);
    storage.attach(prefs);

    AURORA_TEST_CHECK_NEAR(storage.read("feed").value_or(-1.0F), 140.0F, 1e-6F);  // 懒回读
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{0});  // 回读不产生待落盘（值已在后端）
    AURORA_TEST_CHECK_EQ(storage.size(), std::size_t{1});            // 已填充内存缓存
}

AURORA_TEST_CASE(clear_marks_tombstone_and_sync_removes_backend_key) {
    auto &storage = fresh();
    Preferences prefs;
    storage.attach(prefs);
    storage.write("feed", 12.0F);
    storage.sync();
    AURORA_TEST_CHECK_TRUE(prefs.group(ScrollStorage::AURORA_GROUP_NAME).contains("feed"));

    storage.clear("feed");
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{1});  // 墓碑
    storage.sync();
    AURORA_TEST_CHECK_FALSE(prefs.group(ScrollStorage::AURORA_GROUP_NAME).contains("feed"));
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{0});
}

AURORA_TEST_CASE(detach_keeps_memory_values_and_sync_is_noop) {
    auto &storage = fresh();
    Preferences prefs;
    storage.attach(prefs);
    storage.write("feed", 7.0F);
    storage.detach();
    AURORA_TEST_CHECK_FALSE(storage.attached());

    storage.sync();  // 无后端：空操作，队列保留
    AURORA_TEST_CHECK_EQ(storage.pending_writes(), std::size_t{1});
    AURORA_TEST_CHECK_NEAR(storage.read("feed").value_or(-1.0F), 7.0F, 1e-6F);

    storage.attach(prefs);  // 重新绑定后仍可提交
    storage.sync();
    AURORA_TEST_CHECK_NEAR(prefs.group(ScrollStorage::AURORA_GROUP_NAME).get<float>("feed", -1.0F), 7.0F, 1e-6F);
}

AURORA_TEST_CASE(duplicate_claim_warns_once_per_key) {
    auto &storage = fresh();
    (void)Diagnostics::take();  // 清场

    int owner_a = 0;
    int owner_b = 0;
    storage.claim("feed", &owner_a);
    AURORA_TEST_CHECK_EQ(storage.owner_count("feed"), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(has_claim_notice(Diagnostics::take()));  // 单持有者不提示

    storage.claim("feed", &owner_a);  // 同实例重复认领（重布局 / 重建同实例）不提示
    AURORA_TEST_CHECK_EQ(storage.owner_count("feed"), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(has_claim_notice(Diagnostics::take()));

    storage.claim("feed", &owner_b);  // 不同实例争用同一键：提示一次
    AURORA_TEST_CHECK_EQ(storage.owner_count("feed"), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(has_claim_notice(Diagnostics::take()));

    int owner_c = 0;
    storage.claim("feed", &owner_c);  // 每键一次：不再重复提示
    AURORA_TEST_CHECK_EQ(storage.owner_count("feed"), std::size_t{3});
    AURORA_TEST_CHECK_FALSE(has_claim_notice(Diagnostics::take()));

    storage.release("feed", &owner_b);
    storage.release("feed", &owner_c);
    AURORA_TEST_CHECK_EQ(storage.owner_count("feed"), std::size_t{1});
}

}  // namespace aurora::test_cases::utest_scroll_storage
