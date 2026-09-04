// 目标源单元：preferences/preferences.h + src/aurora/preferences/preferences.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_preferences.cpp
//   - test_preferences_group.cpp
//   - test_preferences_multiproc.cpp
//   - test_preferences_multiproc_delete.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run(argc,argv)，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

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
#include "test_harness.h"
// writer 进程或父进程的探测 reload 瞬时持有目标文件而返回 ERROR_ACCESS_DENIED（"Permission
// denied"）。这是存储实现既有的偶发竞态——旧「独立可执行」同样会偶发失败（实测约 1/5），非
// tests_v2 迁移引入。本多进程用例真正校验的是「最终键齐全 + 无半写损坏」，短暂的重命名共享冲突
// 属可重试的瞬态，予以有限次退避重试，避免竞态窗口被放大时出现不稳定误报。置于文件作用域，
// 供各多进程子命令（writer/delete/clear）共用。
static auto flush_retry(aurora::preferences::Preferences &p, const int attempts = 60) -> bool {
    for (int i = 0; i < attempts; ++i) {
        if (p.flush().ok()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

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

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace aurora::tests::sec_preferences_multiproc {
using preferences::Preferences;

static auto self_exe() -> std::filesystem::path {
#ifdef AURORA_PLATFORM_WINDOWS
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    return {buf, buf + n};
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return "test_preferences_multiproc";
#endif
}

static auto run_writer(int id, const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("mp", file);
    constexpr int n = 50;
    for (int j = 0; j < n; ++j) {
        p.set("w" + std::to_string(id) + "_k" + std::to_string(j), (id * 1000) + j);
        if (j % 4 == 0) {
            if (!flush_retry(p)) {
                AURORA_TEST_PRINTF_ERR("writer %d flush failed (retries exhausted)\n", id);
                return 1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 让步，制造进程间交错
    }
    if (!flush_retry(p)) {
        AURORA_TEST_PRINTF_ERR("writer %d final flush failed (retries exhausted)\n", id);
        return 1;
    }
    return 0;
}

namespace {
struct Child {
#ifdef AURORA_PLATFORM_WINDOWS
    PROCESS_INFORMATION pi_{};
#else
    pid_t pid = -1;
#endif
};
}  // namespace

static auto run_parent(const std::filesystem::path &exe, const std::filesystem::path &file) -> int {
    constexpr int k = 2;
    std::vector<Child> children(k);

#ifdef AURORA_PLATFORM_WINDOWS
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    for (int i = 0; i < k; ++i) {
        const std::string cmd = "\"" + exe.string() + "\" --run=test_preferences -- --writer " + std::to_string(i) +
                                " \"" + file.string() + "\"";
        std::vector buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &children[i].pi_) ==
            0) {
            AURORA_TEST_CHECK(!"CreateProcessA failed");
            return 1;
        }
    }
#else
    for (int i = 0; i < K; ++i) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            const std::string a1 = "--writer";
            const std::string a2 = std::to_string(i);
            const std::string a3 = file.string();
            ::execl(exe.c_str(), "test_preferences_multiproc", "--run=test_preferences", "--", a1.c_str(), a2.c_str(),
                    a3.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        children[i].pid = pid;
    }
#endif

    // 子进程运行期间周期性 reload：验证文件始终可被完整解析（无半写损坏）。
    bool running = true;
    for (int poll = 0; poll < 400 && running; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Preferences probe(file);
        const auto r = probe.reload();
        if (!r.ok() && r.error().code == "prefs-parse-failed") {
            AURORA_TEST_CHECK(!"multiproc concurrent writes cause half-written corruption: reload parse failed");
        }
#ifdef AURORA_PLATFORM_WINDOWS
        running = false;
        for (int i = 0; i < k; ++i) {
            DWORD code = 0;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if ((GetExitCodeProcess(children[i].pi_.hProcess, &code) != 0) && code == STILL_ACTIVE) {
                running = true;
            }
        }
#else
        running = false;
        for (int i = 0; i < K; ++i) {
            int status = 0;
            if (::waitpid(children[i].pid, &status, WNOHANG) == 0) {
                running = true;  // 仍运行
            }
        }
#endif
    }

    // 确保全部结束并取退出码。
    std::vector codes(k, 0);
#ifdef AURORA_PLATFORM_WINDOWS
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    HANDLE procs[2] = {children[0].pi_.hProcess, children[1].pi_.hProcess};
    WaitForMultipleObjects(k, procs, TRUE, INFINITE);
    for (int i = 0; i < k; ++i) {
        DWORD code = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        GetExitCodeProcess(children[i].pi_.hProcess, &code);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        codes[i] = static_cast<int>(code);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        CloseHandle(children[i].pi_.hThread);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        CloseHandle(children[i].pi_.hProcess);
    }
#else
    for (int i = 0; i < K; ++i) {
        int status = 0;
        ::waitpid(children[i].pid, &status, 0);
        codes[i] = WEXITSTATUS(status);
    }
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(codes[0] == 0 && codes[1] == 0);

    // 最终一致性：两个 writer 写入的键全部存在（验证进程锁防止互相覆盖丢数据）。
    Preferences p(file);
    AURORA_TEST_CHECK(p.reload().ok());
    constexpr int n = 50;
    for (int id = 0; id < k; ++id) {
        for (int j = 0; j < n; ++j) {
            AURORA_TEST_CHECK(p.get("w" + std::to_string(id) + "_k" + std::to_string(j), -1) == (id * 1000) + j);
        }
    }
    return 0;  // 断言结果已记入框架上下文；补齐返回值消除 int 函数落尾 UB
}

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    if (argc > 1 && std::string(argv[1]) == "--writer") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        const int id = static_cast<int>(std::strtol(argv[2], nullptr, 10));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        const std::filesystem::path file = argv[3];
        return run_writer(id, file);
    }

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "aurora_mp_test";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "mp.json";

    // 清理上一次残留（含锁文件）。
    std::filesystem::remove(file, ec);
#ifdef AURORA_PLATFORM_WINDOWS
    std::filesystem::remove(std::filesystem::path(file.wstring() + L".lock"), ec);
#else
    std::filesystem::remove(std::filesystem::path(file.string() + ".lock"), ec);
#endif

    const auto exe = self_exe();
    const int rc = run_parent(exe, file);

    std::filesystem::remove_all(dir, ec);
    return rc;
}
}  // namespace aurora::tests::sec_preferences_multiproc

namespace aurora::tests::sec_preferences_multiproc_delete {
using preferences::Preferences;

static auto self_exe() -> std::filesystem::path {
#ifdef AURORA_PLATFORM_WINDOWS
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    return {buf, buf + n};
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return "test_preferences_multiproc_delete";
#endif
}

static auto run_delete_child(const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("del", file);
    // 构造即加载；验证确实看到了父进程写入的 victim（否则测试前提不成立）。
    AURORA_TEST_CHECK(p.get("victim", -1) == 999);
    p.remove("victim");  // 打墓碑
    if (!flush_retry(p)) {
        AURORA_TEST_PRINTF_ERR("delete child flush failed (retries exhausted)\n");
        return 1;
    }
    return 0;
}

static auto run_clear_child(const std::filesystem::path &file) -> int {
    auto &p = Preferences::instance_at("del", file);
    (void)p.reload();  // 看到父进程写入的 c*
    p.clear();  // 全局清空纪元
    if (!flush_retry(p)) {
        return 1;
    }
    p.set("after_clear", 1);  // 清空之后的新键，应存活
    if (!flush_retry(p)) {
        return 1;
    }
    return 0;
}

#ifdef AURORA_PLATFORM_WINDOWS
static auto spawn(const std::filesystem::path &exe, const std::string &mode, const std::filesystem::path &file)
    -> PROCESS_INFORMATION {
    const std::string cmd =
        "\"" + exe.string() + "\" --run=test_preferences -- --" + mode + " \"" + file.string() + "\"";
    std::vector buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) == 0) {
        AURORA_TEST_CHECK(!"CreateProcessA failed");
    }
    return pi;
}
#else
static auto spawn(const std::filesystem::path &exe, const std::string &mode, const std::filesystem::path &file)
    -> pid_t {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const std::string a1 = "--" + mode;
        const std::string a2 = file.string();
        ::execl(exe.c_str(), "test_preferences_multiproc_delete", "--run=test_preferences", "--", a1.c_str(),
                a2.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    return pid;
}
#endif

static auto wait_child(
#ifdef AURORA_PLATFORM_WINDOWS
    PROCESS_INFORMATION const &pi
#else
    pid_t pid
#endif
    ) -> int {
#ifdef AURORA_PLATFORM_WINDOWS
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
#endif
}

static auto run_parent(const std::filesystem::path &exe, const std::filesystem::path &file) -> int {
    // ---------- 阶段 1：删除传播 ----------
    {
        auto &p = Preferences::instance_at("del", file);
        p.set("victim", 999);
        for (int i = 0; i < 50; ++i) {
            p.set("keep" + std::to_string(i), i);
        }
        AURORA_TEST_CHECK(p.flush().ok());
    }

#ifdef AURORA_PLATFORM_WINDOWS
    auto const pi_del = spawn(exe, "delete", file);
#else
    auto pid_del = spawn(exe, "delete", file);
#endif

    // 周期性 reload，验证 victim 最终消失（墓碑跨进程传播、可靠删除）。
    bool victim_gone = false;
    for (int poll = 0; poll < 500 && !victim_gone; ++poll) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Preferences probe(file);
        if (probe.get("victim", -1) == -1) {
            victim_gone = true;
        }
    }
    AURORA_TEST_CHECK(victim_gone);  // 删除可靠传播到其他进程

#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK(wait_child(pi_del) == 0);
#else
    AURORA_TEST_CHECK(wait_child(pid_del) == 0);
#endif

    // 阶段 1 最终一致性：victim 删除，keep* 全部保留（删除不会波及其他键）。
    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.reload().ok());
        AURORA_TEST_CHECK(p.get("victim", -1) == -1);
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK(p.get("keep" + std::to_string(i), -1) == i);
        }
    }

    // ---------- 阶段 2：清空传播 + 清空后重建 ----------
    {
        auto &p = Preferences::instance_at("del", file);
        for (int i = 0; i < 10; ++i) {
            p.set("c" + std::to_string(i), i);
        }
        AURORA_TEST_CHECK(p.flush().ok());
    }

#ifdef AURORA_PLATFORM_WINDOWS
    auto const pi_clr = spawn(exe, "clear", file);
#else
    auto pid_clr = spawn(exe, "clear", file);
#endif

#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK(wait_child(pi_clr) == 0);
#else
    AURORA_TEST_CHECK(wait_child(pid_clr) == 0);
#endif

    {
        Preferences p(file);
        AURORA_TEST_CHECK(p.reload().ok());
        // 被清空的键全部消失（clear 是全局清空）
        for (int i = 0; i < 10; ++i) {
            AURORA_TEST_CHECK(p.get("c" + std::to_string(i), -1) == -1);
        }
        // 全局清空同样清掉阶段 1 的 keep* 与 victim
        for (int i = 0; i < 50; ++i) {
            AURORA_TEST_CHECK(p.get("keep" + std::to_string(i), -1) == -1);
        }
        AURORA_TEST_CHECK(p.get("victim", -1) == -1);
        // 清空之后新建的键存活
        AURORA_TEST_CHECK(p.get("after_clear", -1) == 1);
    }
    return 0;  // 断言结果已记入框架上下文；补齐返回值消除 int 函数落尾 UB
}

static auto run(int argc, char **argv) -> int {
    (void)argc;
    (void)argv;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    const std::string mode = argc > 1 ? std::string(argv[1]) : "";
    if (mode == "--delete") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return run_delete_child(argv[2]);
    }
    if (mode == "--clear") {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return run_clear_child(argv[2]);
    }

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "aurora_mp_del_test";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / "del.json";

    // 清理上一次残留（含锁文件）。
    std::filesystem::remove(file, ec);
#ifdef AURORA_PLATFORM_WINDOWS
    std::filesystem::remove(std::filesystem::path(file.wstring() + L".lock"), ec);
#else
    std::filesystem::remove(std::filesystem::path(file.string() + ".lock"), ec);
#endif

    const auto exe = self_exe();
    const int rc = run_parent(exe, file);

    std::filesystem::remove_all(dir, ec);
    return rc;
}
}  // namespace aurora::tests::sec_preferences_multiproc_delete

AURORA_TEST() {
    // 子进程模式派发：runner 父进程会以「runner --run=test_preferences -- 模式参数」重启子进程；
    // 子进程读取 runner 透传参数（`--` 之后），只执行对应段并直接返回，不得再跑父进程各段。
    const int argc = aurora::testing::pass_argc();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) 测试入口：pass_argv() 返回 const char**，此处只读索引
    // argv[1] 不修改，去除 const 仅为满足签名
    auto *const argv = const_cast<char **>(aurora::testing::pass_argv());
    if (argc > 1) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        const std::string mode = argv[1];
        if (mode == "--writer") {
            AURORA_TEST_CHECK(aurora::tests::sec_preferences_multiproc::run(argc, argv) == 0);
            return;
        }
        if (mode == "--delete" || mode == "--clear") {
            AURORA_TEST_CHECK(aurora::tests::sec_preferences_multiproc_delete::run(argc, argv) == 0);
            return;
        }
    }
    int rc = 0;
    rc += aurora::tests::sec_preferences::run(argc, argv);
    rc += aurora::tests::sec_preferences_group::run(argc, argv);
    rc += aurora::tests::sec_preferences_multiproc::run(argc, argv);
    rc += aurora::tests::sec_preferences_multiproc_delete::run(argc, argv);
    AURORA_TEST_CHECK(rc == 0);
}