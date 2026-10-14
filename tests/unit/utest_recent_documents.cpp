/// 测试类型: unit
/// 目标单元: include/aurora/app/recent_documents.h
/// 测试说明: 覆盖最近文档 store 的集合语义（同路径去重置顶、超容量淘汰最旧、新→旧序、clear 清空）、
/// 空口径（空路径 no-op、未 attach 时列表为空）、字符串级路径口径（不触碰文件系统、不做规范化），
/// 以及 Preferences 持久化往返（sync 后模拟新实例读回逐条一致、clear + sync 读回为空、
/// detach 后提交、信封损坏降级为空列表且不抛错）

#include <algorithm>
#include <string>
#include <vector>

#include "aurora/app/recent_documents.h"
#include "aurora/core/json.h"
#include "aurora/preferences/preferences.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_recent_documents {

using preferences::Preferences;

namespace {

/// 用例隔离：store 是进程级单例，每个用例起手复位（含持久化绑定）。
auto fresh() -> RecentDocuments & {
    auto &store = RecentDocuments::instance();
    store.clear_all();
    return store;
}

/// 分组视图：用例内反复用到，故取一次即可（`Group` 是轻量非拥有视图）。
auto envelope_group(Preferences &prefs) -> Preferences::Group {
    return prefs.group(RecentDocuments::AURORA_GROUP_NAME);
}

/// @brief 模拟「新实例」：单例是进程级的，没有第二个实例可用，故以复位（清内存 + 解绑）
///        再 attach 同一后端来逼近「进程重启后重新 attach」——随后的 list 走懒回读路径。
auto reattach_as_fresh(Preferences &prefs) -> RecentDocuments & {
    auto &store = RecentDocuments::instance();
    store.clear_all();
    store.attach(prefs);
    return store;
}

}  // namespace

AURORA_TEST_CASE(add_dedupes_same_path_and_moves_to_front) {
    auto &store = fresh();
    store.add("docs/alpha.txt");
    store.add("docs/beta.txt");
    store.add("docs/alpha.txt");  // 同路径第二次
    store.add("docs/alpha.txt");  // 同路径第三次：仍只占一条，且置顶
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{2});

    const auto items = store.list();
    AURORA_TEST_CHECK_EQ(items.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(items.front(), "docs/alpha.txt");  // 变异自证：把 add 里「先摘旧位再插队首」
    // 改成直接 push_back（不去重、不置顶），本用例转红。
    AURORA_TEST_CHECK_STREQ(items.back(), "docs/beta.txt");
}

AURORA_TEST_CASE(capacity_evicts_oldest_entry) {
    auto &store = fresh();
    constexpr std::size_t added = RecentDocuments::AURORA_MAX_ENTRIES + 3;
    for (std::size_t i = 0; i < added; ++i) {
        store.add("docs/f" + std::to_string(i) + ".txt");
    }
    AURORA_TEST_CHECK_EQ(store.size(), RecentDocuments::AURORA_MAX_ENTRIES);

    const auto items = store.list();
    AURORA_TEST_CHECK_EQ(items.size(), RecentDocuments::AURORA_MAX_ENTRIES);
    AURORA_TEST_CHECK_STREQ(items.front(), "docs/f" + std::to_string(added - 1) + ".txt");  // 最新在首
    AURORA_TEST_CHECK_STREQ(items.back(), "docs/f3.txt");  // 变异自证：把 add 里的 pop_back（淘汰最旧）
    // 改成 erase(begin())（淘汰最新），本用例转红。

    // 最旧的三条已被淘汰。
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string evicted = "docs/f" + std::to_string(i) + ".txt";
        AURORA_TEST_CHECK_TRUE(std::ranges::find(items, evicted) == items.end());
    }
}

AURORA_TEST_CASE(list_is_ordered_newest_first) {
    auto &store = fresh();
    store.add("docs/one.txt");
    store.add("docs/two.txt");
    store.add("docs/three.txt");

    const auto items = store.list();
    AURORA_TEST_CHECK_EQ(items, std::vector<std::string>{"docs/three.txt", "docs/two.txt", "docs/one.txt"});
}

AURORA_TEST_CASE(clear_empties_list_and_size) {
    auto &store = fresh();
    store.add("docs/one.txt");
    store.add("docs/two.txt");
    store.clear();

    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(store.list().empty());
}

AURORA_TEST_CASE(empty_path_add_is_noop) {
    auto &store = fresh();
    store.add("docs/one.txt");
    store.add("");
    store.add(std::string_view{});

    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});
    const auto items = store.list();
    AURORA_TEST_CHECK_EQ(items.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(items.front(), "docs/one.txt");
}

AURORA_TEST_CASE(path_is_stored_as_string_without_filesystem_access) {
    auto &store = fresh();
    // 磁盘上不存在的路径（含空格与百分号）同样入列：store 只做字符串级存取，不触碰文件系统。
    constexpr auto missing = "C:/aurora-no-such-dir/100% done/report.txt";
    store.add(missing);

    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(store.list().front(), missing);
}

AURORA_TEST_CASE(case_and_relative_paths_are_distinct_entries) {
    auto &store = fresh();
    store.add("docs/Same.txt");
    store.add("docs/same.txt");  // 仅大小写不同：原样字符串比较，视为另一条
    store.add("C:/work/docs/same.txt");  // 同一文件的绝对写法：不做规范化，仍视为另一条

    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(store.list(),
                         std::vector<std::string>{"C:/work/docs/same.txt", "docs/same.txt", "docs/Same.txt"});
}

AURORA_TEST_CASE(list_is_empty_without_attach) {
    auto &store = fresh();
    AURORA_TEST_CHECK_FALSE(store.attached());
    AURORA_TEST_CHECK_TRUE(store.list().empty());

    store.add("docs/one.txt");
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});

    store.sync();  // 未 attach：空操作，不丢内存列表
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(store.list().empty());
}

AURORA_TEST_CASE(attach_sync_then_fresh_instance_reads_back) {
    auto &store = fresh();
    Preferences prefs;  // 仅内存模式（不触碰磁盘）
    store.attach(prefs);
    AURORA_TEST_CHECK_TRUE(store.attached());

    store.add("docs/one.txt");
    store.add("docs/two.txt");
    store.add("docs/three.txt");
    AURORA_TEST_CHECK_FALSE(envelope_group(prefs).contains("documents"));  // attach / add 不自动落盘

    store.sync();
    AURORA_TEST_CHECK_TRUE(envelope_group(prefs).contains("documents"));  // 仅写 Preferences 内存

    auto &reborn = reattach_as_fresh(prefs);  // 以复位模拟新实例，走懒回读
    AURORA_TEST_CHECK_EQ(reborn.list(), std::vector<std::string>{"docs/three.txt", "docs/two.txt", "docs/one.txt"});
    AURORA_TEST_CHECK_EQ(reborn.size(), std::size_t{3});
}

AURORA_TEST_CASE(clear_then_sync_reads_back_empty) {
    auto &store = fresh();
    Preferences prefs;
    store.attach(prefs);
    store.add("docs/one.txt");
    store.add("docs/two.txt");
    store.sync();

    store.clear();
    AURORA_TEST_CHECK_TRUE(store.list().empty());
    store.sync();  // 墓碑落盘：后端信封被删除，读缺失即空列表

    auto &reborn = reattach_as_fresh(prefs);
    AURORA_TEST_CHECK_TRUE(reborn.list().empty());
    AURORA_TEST_CHECK_EQ(reborn.size(), std::size_t{0});
}

AURORA_TEST_CASE(detach_defers_write_and_reattach_commits) {
    auto &store = fresh();
    Preferences prefs;
    store.attach(prefs);
    store.add("docs/one.txt");

    store.detach();
    AURORA_TEST_CHECK_FALSE(store.attached());
    store.sync();  // 无后端：空操作，待落盘标记保留
    AURORA_TEST_CHECK_FALSE(envelope_group(prefs).contains("documents"));

    store.attach(prefs);  // 重新绑定后仍可提交
    store.sync();
    AURORA_TEST_CHECK_EQ(reattach_as_fresh(prefs).list(), std::vector<std::string>{"docs/one.txt"});
}

AURORA_TEST_CASE(corrupted_envelope_degrades_to_empty_list) {
    auto &store = fresh();
    Preferences prefs;
    // 手写非法信封：值不是数组（模拟 JSON 损坏或旧版本残留的非数组值）。
    envelope_group(prefs).set("documents", json::Value("{not-an-array"));
    store.attach(prefs);

    std::vector<std::string> items;
    AURORA_TEST_CHECK_NO_THROW(items = store.list());  // 损坏不抛错
    AURORA_TEST_CHECK_TRUE(items.empty());
    AURORA_TEST_CHECK_EQ(store.size(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(envelope_group(prefs).contains("documents"));  // 宽容降级：不清用户数据

    // 元素类型不符时跳过该条，其余条目照旧（同为宽容降级，不整份丢弃）。
    auto mixed = json::Value::array();
    mixed.push_back(json::Value("docs/keep.txt"));
    mixed.push_back(json::Value(42));  // 非字符串元素
    envelope_group(prefs).set("documents", std::move(mixed));

    store.clear_all();  // 复位以重新回读
    store.attach(prefs);
    AURORA_TEST_CHECK_EQ(store.list(), std::vector<std::string>{"docs/keep.txt"});
}

}  // namespace aurora::test_cases::utest_recent_documents
