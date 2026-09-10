/// 测试类型: unit
/// 目标单元: include/aurora/preferences/preferences.h
/// 测试说明: Preferences 内存/文件双模式、显式
/// flush/reload、点号路径助手、墓碑删除与清空纪元、分组作用域、watch/binding 响应式、单例与并发冒烟

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "aurora/preferences/preferences.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_preferences {

namespace prefs = aurora::preferences;
namespace m = aurora::testing::matchers;  // 匹配器工厂别名

/// @brief 本用例的临时目录：框架每用例接管 TMP/TEMP/TMPDIR，temp_directory_path() 已是
/// 用例唯一目录，再挂固定子目录并先行清场，保证幂等与跨用例/跨进程并行安全。
[[nodiscard]] auto make_case_dir(std::string_view tag) -> std::filesystem::path {
    return std::filesystem::temp_directory_path() / "aurora_utest_prefs" / std::filesystem::path{tag};
}

AURORA_TEST_CASE(memory_mode_basic_get_set) {
    // 内存模式：未绑定文件；缺失键/类型不匹配回退 fallback；flush/reload 返回结构化错误。
    prefs::Preferences p;
    AURORA_TEST_CHECK(!p.is_persistent());
    AURORA_TEST_CHECK(p.file_path().empty());

    AURORA_TEST_CHECK_EQ(p.get<int>("missing", 5), 5);
    p.set("count", 42);
    AURORA_TEST_CHECK_EQ(p.get<int>("count", 0), 42);
    AURORA_TEST_CHECK(p.contains("count"));
    p.set("flag", std::string("on"));
    AURORA_TEST_CHECK_EQ(p.get<std::string>("flag", ""), std::string("on"));
    AURORA_TEST_CHECK_EQ(p.get<int>("flag", -1), -1);  // string 存储读 int → 回退
    const std::vector<int> nums{1, 2, 3};
    p.set("nums", nums);
    AURORA_TEST_CHECK(p.get<std::vector<int>>("nums", {}) == nums);
    AURORA_TEST_CHECK_THAT(p.keys(), m::contains(std::string{"count"}));

    AURORA_TEST_CHECK(!p.flush().ok());  // 内存模式不支持落盘
    AURORA_TEST_CHECK(!p.reload().ok());  // 内存模式不支持重载
    AURORA_TEST_CHECK(!p.last_load_error().has_value());
}

AURORA_TEST_CASE(path_helpers_flatten_nested_keys) {
    // 公共点号路径助手：嵌套寻址写读删 + flatten 拍平（分组持久化的底层语义）。
    aurora::Json root = aurora::Json::object();
    prefs::resolve_set(root, "ui.theme", aurora::Json{"dark"});
    prefs::resolve_set(root, "ui.editor.font", 14);
    AURORA_TEST_CHECK_EQ(prefs::resolve_get(root, "ui.theme"), aurora::Json{"dark"});
    AURORA_TEST_CHECK_EQ(prefs::resolve_get(root, "ui.editor.font").get<int>(), 14);
    AURORA_TEST_CHECK(prefs::resolve_get(root, "ui.missing").is_null());
    AURORA_TEST_CHECK(prefs::resolve_get(root, "a.b.c").is_null());  // 路径中断返回 null
    prefs::resolve_erase(root, "ui.theme");
    AURORA_TEST_CHECK(prefs::resolve_get(root, "ui.theme").is_null());
    AURORA_TEST_CHECK(!prefs::resolve_get(root, "ui.editor.font").is_null());  // 兄弟键不受影响

    const auto flat = prefs::flatten(root);
    AURORA_TEST_CHECK_THAT(flat, m::size_is(1));
    AURORA_TEST_CHECK(flat.contains("ui.editor.font"));
}

AURORA_TEST_CASE(file_mode_flush_and_reload_roundtrip) {
    // 文件模式：set 不写穿、flush 显式落盘、构造即加载、reload 丢弃本地未落盘修改以磁盘为准。
    const auto dir = make_case_dir("flush_reload");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    // with_location 自动补 .json；auto_create_dir 默认开启 → 深层目录在 flush 时自动创建。
    prefs::Preferences writer = prefs::Preferences::with_location("cfg", dir / "nested" / "deep");
    AURORA_TEST_CHECK(writer.is_persistent());
    AURORA_TEST_CHECK_EQ(writer.file_path().filename(), std::filesystem::path{"cfg.json"});
    writer.set("theme", std::string("dark"));
    writer.set("volume", 0.5);
    AURORA_TEST_CHECK(!std::filesystem::exists(writer.file_path()));  // set 只写内存
    AURORA_TEST_REQUIRE(writer.flush().ok());  // 显式提交落盘
    AURORA_TEST_CHECK(std::filesystem::exists(writer.file_path()));

    prefs::Preferences reader{writer.file_path()};  // 另一实例构造即加载
    AURORA_TEST_CHECK_EQ(reader.get<std::string>("theme", "light"), std::string("dark"));
    AURORA_TEST_CHECK_NEAR(reader.get<double>("volume", 0.0), 0.5, 1e-9);
    AURORA_TEST_CHECK(!reader.last_load_error().has_value());

    // reload 契约：丢弃本地未落盘修改，完全以磁盘为准。
    writer.set("theme", std::string("pending-local"));  // 未 flush
    reader.set("theme", std::string("from-reader"));
    AURORA_TEST_REQUIRE(reader.flush().ok());
    AURORA_TEST_REQUIRE(writer.reload().ok());
    AURORA_TEST_CHECK_EQ(writer.get<std::string>("theme", ""), std::string("from-reader"));
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(corrupt_file_yields_error_and_empty_store) {
    // 错误路径：损坏 JSON → 构造期记入 last_load_error、存储为空；flush 以内存内容自愈覆盖。
    const auto dir = make_case_dir("corrupt");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const auto file = dir / "broken.json";
    {
        std::ofstream out{file, std::ios::binary};
        out << "{not-valid-json";
    }
    prefs::Preferences p{file};
    AURORA_TEST_REQUIRE(p.last_load_error().has_value());  // 构造即加载失败可见
    AURORA_TEST_CHECK(p.keys().empty());
    AURORA_TEST_CHECK_EQ(p.get<int>("any", -1), -1);
    AURORA_TEST_REQUIRE(p.flush().ok());  // 以空内存覆盖损坏文件
    prefs::Preferences fresh{file};
    AURORA_TEST_CHECK(!fresh.last_load_error().has_value());
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(remove_and_clear_tombstone_semantics) {
    // remove 写墓碑并随 flush 持久化（阻止新实例复活）；clear 置全局清空纪元，纪元后新键存活。
    const auto dir = make_case_dir("tombstone");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const auto file = dir / "prefs.json";

    prefs::Preferences p{file};
    p.set("gone", 1);
    p.set("kept", 2);
    AURORA_TEST_REQUIRE(p.flush().ok());
    p.remove("gone");
    AURORA_TEST_CHECK(!p.contains("gone"));
    AURORA_TEST_REQUIRE(p.flush().ok());  // 墓碑随 flush 持久化
    {
        prefs::Preferences fresh{file};
        AURORA_TEST_CHECK(!fresh.contains("gone"));  // 墓碑阻止复活
        AURORA_TEST_CHECK(fresh.contains("kept"));
    }
    p.set("gone", 42);  // 重新创建同键：取消墓碑
    AURORA_TEST_CHECK(p.contains("gone"));
    AURORA_TEST_REQUIRE(p.flush().ok());
    {
        prefs::Preferences fresh{file};
        AURORA_TEST_CHECK_EQ(fresh.get<int>("gone", -1), 42);
    }

    // clear：全局清空纪元；清空后新写入的键不受纪元影响。
    prefs::Preferences q{dir / "clear.json"};
    q.set("a", 1);
    q.set("b", 2);
    q.clear();
    AURORA_TEST_CHECK(q.keys().empty());
    AURORA_TEST_CHECK(!q.contains("a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 保证新写入时间戳严格晚于清空纪元
    q.set("post", 3);
    AURORA_TEST_REQUIRE(q.flush().ok());
    {
        prefs::Preferences fresh{dir / "clear.json"};
        const auto keys = fresh.keys();
        AURORA_TEST_CHECK_THAT(keys, m::contains(std::string{"post"}));  // 纪元后新键存活
        AURORA_TEST_CHECK_THAT(keys, m::negated(m::contains(std::string{"a"})));  // 旧键被清空纪元清除
        AURORA_TEST_CHECK_THAT(keys, m::negated(m::contains(std::string{"b"})));
        AURORA_TEST_CHECK_EQ(fresh.get<int>("post", -1), 3);
    }
    std::filesystem::remove_all(dir, ec);
}

AURORA_TEST_CASE(group_scope_nested_and_isolation) {
    // 分组作用域：读写/删除限定在前缀内，以嵌套 JSON 表达；分组 clear 不影响顶层键。
    prefs::Preferences p;  // 内存模式足以覆盖分组作用域逻辑
    p.set("top", 1);
    p.group("ui").set("theme", std::string("dark"));
    p.group("ui").set("font", 14);
    p.group("ui").group("editor").set("autosave", true);

    AURORA_TEST_CHECK_EQ(p.group("ui").get<std::string>("theme", "light"), std::string("dark"));
    AURORA_TEST_CHECK_EQ(p.group("ui").get<int>("font", 0), 14);
    AURORA_TEST_CHECK_EQ(p.get<std::string>("ui.theme", ""), std::string("dark"));  // 与复合键互通
    AURORA_TEST_CHECK(p.contains("ui.theme"));
    AURORA_TEST_CHECK(!p.contains("theme"));  // 裸键对外层不可见
    AURORA_TEST_CHECK(p.group("ui").group("editor").contains("autosave"));
    AURORA_TEST_CHECK(p.group("ui").contains("editor"));

    const auto ui_keys = p.group("ui").keys();  // 分组内直接子键不含前缀
    AURORA_TEST_CHECK_THAT(ui_keys, m::contains(std::string{"theme"}));
    AURORA_TEST_CHECK_THAT(ui_keys, m::contains(std::string{"font"}));
    AURORA_TEST_CHECK_THAT(ui_keys, m::contains(std::string{"editor"}));

    p.group("ui").remove("theme");
    AURORA_TEST_CHECK(!p.group("ui").contains("theme"));
    AURORA_TEST_CHECK(p.group("ui").contains("font"));  // 其余键不受影响
    p.group("ui").clear();
    AURORA_TEST_CHECK(!p.group("ui").contains("font"));
    AURORA_TEST_CHECK(!p.group("ui").group("editor").contains("autosave"));
    AURORA_TEST_CHECK_EQ(p.get<int>("top", -1), 1);  // 顶层键不受分组 clear 影响
}

AURORA_TEST_CASE(watch_state_receives_updates) {
    // watch：惰性创建 State<T>，set 推送订阅者；初始值取既有存储；同键同类型复用同一 State。
    prefs::Preferences p;
    auto state = p.watch<int>("counter", 0);
    AURORA_TEST_REQUIRE(state != nullptr);
    AURORA_TEST_CHECK_EQ(state->get(), 0);  // 初始为 fallback
    p.set("counter", 5);  // 写入推送已订阅的 State
    AURORA_TEST_CHECK_EQ(state->get(), 5);

    p.set("volume", 7);
    auto existing = p.watch<int>("volume", 0);
    AURORA_TEST_CHECK_EQ(existing->get(), 7);  // 初始值来自存储而非 fallback

    AURORA_TEST_CHECK(p.watch<int>("counter", 0) == state);  // 缓存复用
}

AURORA_TEST_CASE(binding_remove_deletes_key) {
    // binding 实际语义（以运行时行为为准）：Binding 是 watch State 的单向下游视图——
    // binding.set 只更新 State（控件侧可见），不写回存储；存储写回仍走 p.set。
    // remove() 触发注入的删除回调（墓碑语义），删除存储键。
    prefs::Preferences p;
    auto binding = p.binding<std::string>("session", std::string(""));
    AURORA_TEST_CHECK(binding.bound());
    AURORA_TEST_CHECK(binding.removable());  // 已注入删除回调
    binding.set(std::string("token"));
    AURORA_TEST_CHECK_EQ(binding.get(), std::string("token"));  // State 侧可见
    AURORA_TEST_CHECK(!p.contains("session"));  // 不写回存储
    p.set("session", std::string("token"));  // 存储写回走 set_impl
    AURORA_TEST_CHECK_EQ(p.get<std::string>("session", ""), std::string("token"));
    binding.remove();
    AURORA_TEST_CHECK(!p.contains("session"));
    // 契约：remove 后 Binding 失效（上游 State 随注册表清除），不再 get/set。
}

AURORA_TEST_CASE(singleton_same_name_returns_same_instance) {
    // 单例冒烟：同名（唯一 name + 用例级临时目录，避免跨用例干扰）返回同一实例；只读不改全局状态。
    const auto dir = make_case_dir("singleton");
    std::error_code ec;
    std::filesystem::create_directories(dir);
    const std::string name = "utest_singleton_cfg";
    auto& first = prefs::Preferences::instance(name, dir);
    auto& second = prefs::Preferences::instance(name, dir);
    AURORA_TEST_CHECK(&first == &second);
    AURORA_TEST_CHECK(first.is_persistent());
    AURORA_TEST_CHECK_EQ(first.file_path().filename(), std::filesystem::path{"utest_singleton_cfg.json"});
    std::filesystem::remove_all(dir, ec);  // 实例仍留在注册表，但无打开句柄，可安全清理
}

AURORA_TEST_CASE(concurrent_read_write_smoke) {
    AURORA_TEST_REQUIRE_THREADS();
    // 并发冒烟：每线程独占写自己的键（最终值必为该线程最后一次写入），只验证无崩溃与串行化正确。
    prefs::Preferences p;
    constexpr int thread_count = 4;
    constexpr int iter_count = 50;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&p, t]() -> void {
            for (int i = 1; i <= iter_count; ++i) {
                p.set("w" + std::to_string(t), (i * 10) + t);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (int t = 0; t < thread_count; ++t) {
        AURORA_TEST_CHECK_EQ(p.get<int>("w" + std::to_string(t), -1), (iter_count * 10) + t);
    }
    AURORA_TEST_CHECK_THAT(p.keys(), m::size_is(static_cast<std::size_t>(thread_count)));
}

}  // namespace aurora::test_cases::utest_preferences
