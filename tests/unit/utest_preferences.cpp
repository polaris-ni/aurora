/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: preferences 单元测试（单进程段；多进程并发段见 utest_preferences_multiproc）
///

// 目标源单元：preferences/preferences.h + src/aurora/preferences/preferences.cpp
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/preferences/preferences.h"
#include "aurora_test_harness.h"

namespace aurora::tests::sec_preferences {
using preferences::Preferences;

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aurora_prefs_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // 1. 内存模式：未指定文件位置 → 仅内存，flush 失败
    {
        Preferences mem;
        AURORA_TEST_CHECK(!mem.is_persistent());
        mem.set("theme", std::string("dark"));
        AURORA_TEST_CHECK(mem.get("theme", std::string("light")) == "dark");
        AURORA_TEST_CHECK(!mem.flush().ok());
        AURORA_TEST_CHECK(!mem.reload().ok());
    }

    // 2. 文件模式：显式指定存储位置，初始为空；set 不自动写文件
    const auto file = dir / "config.json";
    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.is_persistent());
        AURORA_TEST_CHECK(p.file_path() == file);
        AURORA_TEST_CHECK(!p.contains("volume"));
        p.set("volume", 7);
        p.set("enabled", true);
        p.set("name", std::string("aurora"));
        AURORA_TEST_CHECK(!std::filesystem::exists(file));  // set 仅更新内存
        AURORA_TEST_CHECK(p.flush().ok());  // 主动刷新到文件
        AURORA_TEST_CHECK(std::filesystem::exists(file));
    }

    // 3. 重新加载：新实例从文件恢复内容；缺失键回退默认值
    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.get("volume", 0) == 7);
        AURORA_TEST_CHECK(p.get("enabled", false) == true);
        AURORA_TEST_CHECK(p.get("name", std::string("")) == "aurora");
        AURORA_TEST_CHECK(p.get("missing", 42) == 42);
    }

    // 4. 不同位置 → 不同文件，互不干扰
    {
        const auto file2 = dir / "sub" / "other.json";
        Preferences p(file2);
        p.set("x", 1);
        AURORA_TEST_CHECK(!std::filesystem::exists(file2));
        AURORA_TEST_CHECK(p.flush().ok());
        AURORA_TEST_CHECK(std::filesystem::exists(file2));

        Preferences p1(file);
        AURORA_TEST_CHECK(p1.get("x", -1) == -1);  // 不受影响
    }

    // 5. watch 返回的 State 随 set 更新，并在重新加载后反映文件值
    {
        Preferences p(dir / "watch.json");
        auto s = p.watch("counter", 0);
        AURORA_TEST_CHECK(s->get() == 0);
        p.set("counter", 5);
        AURORA_TEST_CHECK(s->get() == 5);
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "watch.json");
        auto s2 = p2.watch("counter", 0);
        AURORA_TEST_CHECK(s2->get() == 5);
    }

    // 6. binding 双向：写回内部 State（响应式），持久化需经 prefs.set + flush
    {
        Preferences p(dir / "bind.json");
        auto b = p.binding("flag", false);
        AURORA_TEST_CHECK(b.get() == false);
        b.set(true);
        AURORA_TEST_CHECK(b.get() == true);  // 响应式 State 已更新
        AURORA_TEST_CHECK(p.get("flag", false) == false);  // 尚未经 prefs.set，内存 JSON 未变
        p.set("flag", true);  // 经 set 写穿内存 JSON
        AURORA_TEST_CHECK(p.get("flag", false) == true);
    }

    // 7. reload：内存修改后从文件恢复旧值
    {
        Preferences p(file);
        p.set("volume", 100);  // 仅内存
        AURORA_TEST_CHECK(p.get("volume", 0) == 100);
        AURORA_TEST_CHECK(p.reload().ok());
        AURORA_TEST_CHECK(p.get("volume", 0) == 7);  // 文件里仍是 7
    }

    // 8. 容器 / 对象值往返
    {
        Preferences p(dir / "obj.json");
        p.set("tags", std::vector<std::string>{"a", "b"});
        AURORA_TEST_CHECK(p.flush().ok());
        Preferences p2(dir / "obj.json");
        auto tags = p2.get<std::vector<std::string>>("tags", {});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(tags.size() == 2 && tags[0] == "a" && tags[1] == "b");
    }

    // 9. with_location 自动创建目录
    {
        const auto loc = dir / "appdata";
        Preferences p = Preferences::with_location("myapp", loc);
        AURORA_TEST_CHECK(p.is_persistent());
        p.set("k", 1);
        AURORA_TEST_CHECK(p.flush().ok());
        AURORA_TEST_CHECK(std::filesystem::exists(loc / "myapp.json"));
    }

    // 10. 类型不匹配 → 回退默认值
    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.get("volume", std::string("fb")) == "fb");  // volume 是 int 7
    }

    // 11. default_config_dir 返回有效目录
    {
        AURORA_TEST_CHECK(!Preferences::default_config_dir().empty());
    }

    // 12. 单例：同名返回同一实例；不同名返回不同实例
    {
        auto &a = Preferences::instance("singleton_test", dir);
        auto &b = Preferences::instance("singleton_test", dir);
        AURORA_TEST_CHECK(&a == &b);  // 全局唯一
        auto &c = Preferences::instance("singleton_other", dir);
        AURORA_TEST_CHECK(&a != &c);
        a.set("singleton_key", 123);
        AURORA_TEST_CHECK(b.get("singleton_key", 0) == 123);  // 经单例共享
    }

    // 13. 多线程读写安全：并发 set 不同键，flush 后全部落盘、无数据竞争
    {
        auto &p = Preferences::instance("concurrent_test", dir);
        constexpr int n = 8;
        std::vector<std::thread> ts;
        ts.reserve(n);
        for (int i = 0; i < n; ++i) {
            ts.emplace_back([&p, i]() -> void { p.set("k" + std::to_string(i), i); });
        }
        for (auto &t : ts) {
            t.join();
        }
        AURORA_TEST_CHECK(p.flush().ok());
        // 重新加载验证全部键均无丢失
        Preferences p2(dir / "concurrent_test.json");
        for (int i = 0; i < n; ++i) {
            AURORA_TEST_CHECK(p2.get("k" + std::to_string(i), -1) == i);
        }
    }

    // 14. 进程安全：连续两次 flush 不互锁（文件锁正确释放）
    {
        auto &p = Preferences::instance("lock_test", dir);
        p.set("x", 1);
        AURORA_TEST_CHECK(p.flush().ok());
        p.set("x", 2);
        AURORA_TEST_CHECK(p.flush().ok());
    }

    // 15. 可靠删除语义（单进程下的墓碑/版本基础）：删除可持久化、删除后重建可恢复。
    {
        auto &p = Preferences::instance("delete_test", dir);
        p.set("gone", 1);
        p.set("alive", 2);
        AURORA_TEST_CHECK(p.flush().ok());

        p.remove("gone");
        AURORA_TEST_CHECK(p.get("gone", -1) == -1);  // 内存立即不可见
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "delete_test.json");
        AURORA_TEST_CHECK(p2.reload().ok());
        AURORA_TEST_CHECK(p2.get("gone", -1) == -1);  // 落盘后删除持久化
        AURORA_TEST_CHECK(p2.get("alive", -1) == 2);  // 其他键不受影响

        // 删除后重建：set 应取消墓碑并恢复可见
        p2.set("gone", 99);
        AURORA_TEST_CHECK(p2.flush().ok());
        Preferences p3(dir / "delete_test.json");
        AURORA_TEST_CHECK(p3.reload().ok());
        AURORA_TEST_CHECK(p3.get("gone", -1) == 99);  // 重建成功
    }

    // 16. Binding 删除路径：binding.remove() 经注入的删除回调删除持久化键（可靠语义）。
    {
        auto &p = Preferences::instance("binding_del_test", dir);
        p.set("temp_key", 7);
        AURORA_TEST_CHECK(p.flush().ok());

        auto b = p.binding<int>("temp_key", 0);
        AURORA_TEST_CHECK(b.bound());
        AURORA_TEST_CHECK(b.removable());  // Preferences::binding 注入了删除回调
        AURORA_TEST_CHECK(b.get() == 7);  // 绑定可见当前值

        b.remove();  // 经回调删除对应键（内存即不可见）
        AURORA_TEST_CHECK(p.get("temp_key", -1) == -1);

        // 纯 State 绑定（无 Preferences 注入）remove() 为空操作且不崩溃。
        State bare{42};
        Binding bare_b(bare);
        AURORA_TEST_CHECK(!bare_b.removable());
        bare_b.remove();  // 安全空操作
        AURORA_TEST_CHECK(bare.get() == 42);

        // 落盘后，删除在另一实例上仍可见（墓碑跨进程可靠删除）。
        AURORA_TEST_CHECK(p.flush().ok());
        Preferences p2(dir / "binding_del_test.json");
        AURORA_TEST_CHECK(p2.reload().ok());
        AURORA_TEST_CHECK(p2.get("temp_key", -1) == -1);
    }

    std::filesystem::remove_all(dir, ec);
    return 0;  // 进程内段：断言结果已由 AURORA_TEST_CHECK 记录，返回码仅供上层 rc 聚合，避免 int run 无返回值 UB
}
}  // namespace aurora::tests::sec_preferences

namespace aurora::tests::sec_preferences_group {
using preferences::Preferences;

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aurora_prefs_group_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // 1. 分组内 set/get/contains/keys；根作用域不含分组键
    {
        Preferences p(dir / "g1.json");
        auto ui = p.group("ui");
        ui.set("theme", std::string("dark"));
        ui.set("font_size", 14);
        AURORA_TEST_CHECK(ui.get("theme", std::string("light")) == "dark");
        AURORA_TEST_CHECK(ui.get("font_size", 0) == 14);
        AURORA_TEST_CHECK(ui.contains("theme"));
        AURORA_TEST_CHECK(!ui.contains("missing"));
        auto ks = ui.keys();
        AURORA_TEST_CHECK(ks.size() == 2);
        // 根作用域看不到分组内的键（分组键带前缀）；但根含分组容器名 "ui"
        AURORA_TEST_CHECK(!p.contains("theme"));
        auto root_keys = p.keys();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(root_keys.size() == 1 && root_keys[0] == "ui");
    }

    // 2. 链式嵌套分组
    {
        Preferences p(dir / "g2.json");
        p.group("ui").group("editor").set("font", std::string("Mono"));
        p.group("ui").group("editor").set("size", 12);
        AURORA_TEST_CHECK(p.group("ui").group("editor").get("font", std::string("")) == "Mono");
        AURORA_TEST_CHECK(p.group("ui").group("editor").get("size", 0) == 12);
        // 中间分组可见子分组名
        auto ui_keys = p.group("ui").keys();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(ui_keys.size() == 1 && ui_keys[0] == "editor");
        auto ed_keys = p.group("ui").group("editor").keys();
        AURORA_TEST_CHECK(ed_keys.size() == 2);
    }

    // 3. watch 作用域隔离：分组 State 与根、其他分组相互独立
    {
        Preferences p(dir / "g3.json");
        auto s_root = p.watch("theme", std::string("light"));
        auto s_ui = p.group("ui").watch("theme", std::string("light"));
        p.set("theme", std::string("dark"));
        AURORA_TEST_CHECK(s_root->get() == "dark");
        AURORA_TEST_CHECK(s_ui->get() == "light");  // 分组独立
        p.group("ui").set("theme", std::string("blue"));
        AURORA_TEST_CHECK(s_ui->get() == "blue");
        AURORA_TEST_CHECK(s_root->get() == "dark");  // 根不受影响
    }

    // 4. 分组 remove 墓碑语义（跨进程可靠删除 + 删除后重建）
    {
        auto &p = Preferences::instance("group_del_test", dir);
        p.group("ui").set("a", 1);
        p.group("ui").set("b", 2);
        p.group("ui").set("c", 3);
        AURORA_TEST_CHECK(p.flush().ok());

        p.group("ui").remove("b");
        AURORA_TEST_CHECK(p.group("ui").get("b", -1) == -1);
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "group_del_test.json");
        AURORA_TEST_CHECK(p2.reload().ok());
        AURORA_TEST_CHECK(p2.group("ui").get("b", -1) == -1);  // 墓碑跨进程删除
        AURORA_TEST_CHECK(p2.group("ui").get("a", -1) == 1);
        AURORA_TEST_CHECK(p2.group("ui").get("c", -1) == 3);

        // 删除后重建：set 取消墓碑
        p2.group("ui").set("b", 99);
        AURORA_TEST_CHECK(p2.flush().ok());
        Preferences p3(dir / "group_del_test.json");
        AURORA_TEST_CHECK(p3.reload().ok());
        AURORA_TEST_CHECK(p3.group("ui").get("b", -1) == 99);
    }

    // 5. 分组 clear 仅清子树（不影响其他分组与顶层键）
    {
        auto &p = Preferences::instance("group_clear_test", dir);
        p.set("top", 1);
        p.group("ui").set("a", 2);
        p.group("ui").set("b", 3);
        p.group("net").set("x", 9);
        AURORA_TEST_CHECK(p.flush().ok());

        p.group("ui").clear();
        AURORA_TEST_CHECK(p.group("ui").get("a", -1) == -1);
        AURORA_TEST_CHECK(p.group("ui").get("b", -1) == -1);
        AURORA_TEST_CHECK(p.get("top", -1) == 1);
        AURORA_TEST_CHECK(p.group("net").get("x", -1) == 9);
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "group_clear_test.json");
        AURORA_TEST_CHECK(p2.reload().ok());
        AURORA_TEST_CHECK(p2.group("ui").get("a", -1) == -1);
        AURORA_TEST_CHECK(p2.group("ui").get("b", -1) == -1);
        AURORA_TEST_CHECK(p2.get("top", -1) == 1);
        AURORA_TEST_CHECK(p2.group("net").get("x", -1) == 9);
    }

    // 6. 分组与扁平键共存；flush 后以嵌套 JSON 持久化、重新加载可恢复
    {
        Preferences p(dir / "mixed.json");
        p.set("flat_key", std::string("v"));
        p.group("ui").set("theme", std::string("dark"));
        p.group("ui").group("editor").set("font", std::string("Mono"));
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "mixed.json");
        AURORA_TEST_CHECK(p2.get("flat_key", std::string("")) == "v");
        AURORA_TEST_CHECK(p2.group("ui").get("theme", std::string("")) == "dark");
        AURORA_TEST_CHECK(p2.group("ui").group("editor").get("font", std::string("")) == "Mono");
    }

    // 7. 旧扁平文件格式兼容：加载旧格式后仍能正常工作并新增分组
    {
        const auto flat_file = dir / "legacy_flat.json";
        {
            std::ofstream o(flat_file, std::ios::binary | std::ios::trunc);
            o << R"({"theme":"dark","volume":7})";
        }
        Preferences p(flat_file);
        AURORA_TEST_CHECK(p.get("theme", std::string("")) == "dark");
        AURORA_TEST_CHECK(p.get("volume", 0) == 7);
        // 在旧扁平文件上新增分组 → 应合并为嵌套结构
        p.group("ui").set("lang", std::string("zh"));
        AURORA_TEST_CHECK(p.flush().ok());
        Preferences p2(flat_file);
        AURORA_TEST_CHECK(p2.get("theme", std::string("")) == "dark");
        AURORA_TEST_CHECK(p2.group("ui").get("lang", std::string("")) == "zh");
    }

    // 8. 分组 binding 删除路径（可靠墓碑跨进程删除）
    {
        auto &p = Preferences::instance("group_bind_test", dir);
        p.group("ui").set("temp", 7);
        AURORA_TEST_CHECK(p.flush().ok());

        auto b = p.group("ui").binding<int>("temp", 0);
        AURORA_TEST_CHECK(b.bound());
        AURORA_TEST_CHECK(b.removable());
        AURORA_TEST_CHECK(b.get() == 7);

        b.remove();  // 经回调删除对应分组键
        AURORA_TEST_CHECK(p.group("ui").get("temp", -1) == -1);
        AURORA_TEST_CHECK(p.flush().ok());

        Preferences p2(dir / "group_bind_test.json");
        AURORA_TEST_CHECK(p2.reload().ok());
        AURORA_TEST_CHECK(p2.group("ui").get("temp", -1) == -1);
    }

    std::filesystem::remove_all(dir, ec);
    return 0;  // 进程内段：断言结果已由 AURORA_TEST_CHECK 记录，返回码仅供上层 rc 聚合，避免 int run 无返回值 UB
}
}  // namespace aurora::tests::sec_preferences_group

namespace aurora::test_cases::utest_preferences {

AURORA_TEST() {
    const int argc = aurora::testing::pass_argc();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) 测试入口：pass_argv() 返回 const char**，此处只读索引
    // argv[1] 不修改，去除 const 仅为满足签名
    auto *const argv = const_cast<char **>(aurora::testing::pass_argv());  // NOLINT
    int rc = 0;
    rc += aurora::tests::sec_preferences::run(argc, argv);
    rc += aurora::tests::sec_preferences_group::run(argc, argv);
    AURORA_TEST_CHECK(rc == 0);
}

}  // namespace aurora::test_cases::utest_preferences
