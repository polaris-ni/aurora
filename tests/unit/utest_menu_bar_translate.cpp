/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/menu_bar_translate.h 、 include/aurora/app/native_menu_bar.h
/// 测试说明: 原生菜单栏的**零平台头折算层**与它两侧的契约——`MenuModel` 树摊平为扁平项表（前序遍历、
/// 深度/父索引/各项属性逐值保真、空 id 项照常建出不丢弃），以及经进程级渲染通路单槽驱动**公共门面本身**
/// （`NativeMenuBar::install`/`uninstall`/`is_installed` 的安装-替换-卸载契约、点击经 `id` 回投
/// `CommandRegistry`）。
///
/// 为何单测能跑产品代码路径：渲染通路是折算层持有的进程级单槽（`MenuBarBackend`），无头侧由本文件注入
/// 伪造后端、真机侧由 Win32 桥注入同名后端——两侧走的是 `install()` 的同一条代码，被测的不是它的复刻。
/// 原生 `HMENU` 的构建与 `WM_COMMAND` 回投由 `tools/verify/win32_menu_bar_live_probe.cpp` 真机验收。
///
/// 变异自证（改坏实现应转红的几条）：
///  * 漏传/错传父索引 → `flatten_preserves_depth_and_parent_index` 转红；
///  * 把空 id 项在折算期丢弃 → `flatten_keeps_empty_id_items` 转红（菜单看着建好了却点不动）；
///  * 重复 install 改成叠加而非替换 → `install_replaces_previous_tree` 转红；
///  * 路由把空 id 也回投 → `empty_id_click_is_not_routed` 转红。

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "aurora/app/menu.h"  // MenuModel
#include "aurora/app/native_menu_bar.h"  // NativeMenuBar 门面
#include "aurora/commands.h"  // CommandRegistry / Command（点击路由出口）
#include "aurora/core/result.h"
#include "aurora/window/detail/menu_bar_translate.h"
#include "aurora/window/platform.h"  // Platform / SurfaceKind / PlatformCapabilities
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_menu_bar_translate {

using menu_bar_translate::AURORA_MENU_BAR_NO_PARENT;
using menu_bar_translate::FlatMenuItem;
using menu_bar_translate::flatten_menu_models;
using menu_bar_translate::MenuBarBackend;

namespace {

/// 用例隔离：通路是进程级单槽，用例进出各清一次，避免残留流向其它套件。
class BackendGuard {
  public:
    BackendGuard() { menu_bar_translate::unregister_menu_bar_backend(); }
    ~BackendGuard() { menu_bar_translate::unregister_menu_bar_backend(); }
    BackendGuard(const BackendGuard &) = delete;
    auto operator=(const BackendGuard &) -> BackendGuard & = delete;
    BackendGuard(BackendGuard &&) = delete;
    auto operator=(BackendGuard &&) -> BackendGuard & = delete;
};

/// 伪造后端的观测状态（进程级单槽后端只持一枚，观测器随之进程级）。
struct FakeBackend {
    bool install_called = false;
    bool uninstall_called = false;
    std::vector<std::string> seen_ids;  ///< install 收到的扁平项里的模型 id（含空串，验证空 id 不丢）
    std::function<void(std::string)> click;  ///< install 注入的点击路由回调（测试手动回投）
};

auto fake_backend() -> FakeBackend & {
    static FakeBackend observed;
    return observed;
}

/// 登记一枚伪造后端并复位观测器。
auto install_fake_backend() -> FakeBackend & {
    fake_backend() = FakeBackend{};
    menu_bar_translate::register_menu_bar_backend(MenuBarBackend{
        .install = [](const std::vector<FlatMenuItem> &items,
                      std::function<void(std::string)> on_click) -> Result<void> {
            fake_backend().install_called = true;
            fake_backend().click = std::move(on_click);
            for (const auto &it : items) {
                fake_backend().seen_ids.push_back(it.id);
            }
            return Result<void>{};
        },
        .uninstall = []() -> void {
            fake_backend().uninstall_called = true;
            fake_backend().install_called = false;
        },
        .is_installed = []() -> bool { return fake_backend().install_called; },
    });
    return fake_backend();
}

/// 构造一棵典型的菜单树：File(>New, 分隔符, Open(>Open Recent(>doc))) + Edit(>Undo)。
auto sample_tree() -> std::vector<MenuModel> {
    MenuModel file;
    file.id = "file";
    file.label = "File";
    MenuModel nw;
    nw.id = "file.new";
    nw.label = "New";
    nw.shortcut_text = "Ctrl+N";
    MenuModel sep;
    sep.separator = true;
    MenuModel open;
    open.id = "file.open";
    open.label = "Open";
    open.checkable = true;
    open.checked = true;
    MenuModel recent;
    recent.id = "file.open.recent";
    recent.label = "Open Recent";
    MenuModel doc;
    doc.id = "file.open.recent.1";
    doc.label = "doc1.txt";
    recent.children.push_back(std::move(doc));
    open.children.push_back(std::move(recent));
    file.children.push_back(std::move(nw));
    file.children.push_back(std::move(sep));
    file.children.push_back(std::move(open));

    MenuModel edit;
    edit.id = "edit";
    edit.label = "Edit";
    MenuModel undo;
    undo.id = "edit.undo";
    undo.label = "Undo";
    undo.enabled = false;
    edit.children.push_back(std::move(undo));

    std::vector<MenuModel> models;
    models.push_back(std::move(file));
    models.push_back(std::move(edit));
    return models;
}

}  // namespace

// ---------- 折算层纯逻辑 ----------

AURORA_TEST_CASE(flatten_preserves_depth_and_parent_index) {
    const auto flat = flatten_menu_models(sample_tree());
    // 预期前序：file(0) → file.new(1) → sep(2) → file.open(3) → file.open.recent(4) → doc(5)
    //           → edit(6) → edit.undo(7)
    AURORA_TEST_CHECK_EQ(flat.size(), std::size_t{8});

    AURORA_TEST_CHECK_EQ(flat[0].depth, std::size_t{0});
    AURORA_TEST_CHECK_EQ(flat[0].parent_index, AURORA_MENU_BAR_NO_PARENT);
    AURORA_TEST_CHECK_EQ(flat[0].id, std::string{"file"});

    AURORA_TEST_CHECK_EQ(flat[1].depth, std::size_t{1});
    AURORA_TEST_CHECK_EQ(flat[1].parent_index, std::size_t{0});  // 父是 file
    AURORA_TEST_CHECK_EQ(flat[1].id, std::string{"file.new"});

    AURORA_TEST_CHECK_TRUE(flat[2].separator);  // 分隔符原样保留
    AURORA_TEST_CHECK_EQ(flat[2].parent_index, std::size_t{0});

    AURORA_TEST_CHECK_EQ(flat[3].depth, std::size_t{1});
    AURORA_TEST_CHECK_EQ(flat[3].parent_index, std::size_t{0});
    AURORA_TEST_CHECK_TRUE(flat[3].checkable);
    AURORA_TEST_CHECK_TRUE(flat[3].checked);

    AURORA_TEST_CHECK_EQ(flat[4].depth, std::size_t{2});
    AURORA_TEST_CHECK_EQ(flat[4].parent_index, std::size_t{3});  // 父是 file.open
    AURORA_TEST_CHECK_EQ(flat[5].depth, std::size_t{3});
    AURORA_TEST_CHECK_EQ(flat[5].parent_index, std::size_t{4});

    AURORA_TEST_CHECK_EQ(flat[6].depth, std::size_t{0});
    AURORA_TEST_CHECK_EQ(flat[6].parent_index, AURORA_MENU_BAR_NO_PARENT);  // 第二个顶层菜单
    AURORA_TEST_CHECK_EQ(flat[7].depth, std::size_t{1});
    AURORA_TEST_CHECK_EQ(flat[7].parent_index, std::size_t{6});
    AURORA_TEST_CHECK_FALSE(flat[7].enabled);  // 禁用态透传
}

AURORA_TEST_CASE(flatten_keeps_field_fidelity) {
    const auto flat = flatten_menu_models(sample_tree());
    // 快捷键文本随 label 透传（桥侧负责拼接 \t，此处只验证字段保真）。
    AURORA_TEST_CHECK_EQ(flat[1].shortcut_text, std::string{"Ctrl+N"});
    AURORA_TEST_CHECK_EQ(flat[1].label, std::string{"New"});
    // 分隔符的 label 为空（构造使然），但仍占用一项、不丢序。
    AURORA_TEST_CHECK_TRUE(flat[2].label.empty());
}

AURORA_TEST_CASE(flatten_keeps_empty_id_items) {
    // 空 id 项照常建出（不丢弃）：原生路径跨重建只能靠稳定 id 路由点击，空 id 路由不了，
    // 但菜单项本身要显示——故折算层不替调用方决定丢弃与否。
    MenuModel m;
    m.label = "Anonymous";  // 无 id
    m.children.emplace_back(MenuModel{});  // 子项也无 id
    std::vector<MenuModel> models;
    models.push_back(std::move(m));

    const auto flat = flatten_menu_models(models);
    AURORA_TEST_CHECK_EQ(flat.size(), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(flat[0].id.empty());
    AURORA_TEST_CHECK_TRUE(flat[1].id.empty());
}

AURORA_TEST_CASE(empty_tree_yields_empty_flat_without_error) {
    const std::vector<MenuModel> empty;
    const auto flat = flatten_menu_models(empty);
    AURORA_TEST_CHECK_TRUE(flat.empty());  // 空输入产出空表，合法而非错误
}

// ---------- 能力位矩阵 ----------

AURORA_TEST_CASE(capability_bit_is_true_only_for_win32_and_d3d11) {
    // 逐 SurfaceKind 申报「OS 侧有该机制且本库已落桥」。翻位纪律：桥未落地的方向不预先声称支持——
    // `MacOS` 本次为 false（NSMenu 桥未落地），Linux 三形态 / Glfw / Wasm / Headless 恒 false
    // （OS 无该机制或桥未落地），回落自绘 MenuBar。
    struct Expectation {
        SurfaceKind kind;
        bool expected;
    };
    const Expectation table[] = {
        {.kind = SurfaceKind::Headless, .expected = false}, {.kind = SurfaceKind::Win32, .expected = true},
        {.kind = SurfaceKind::D3D11, .expected = true},     {.kind = SurfaceKind::Glfw, .expected = false},
        {.kind = SurfaceKind::X11, .expected = false},      {.kind = SurfaceKind::Wayland, .expected = false},
        {.kind = SurfaceKind::MacOS, .expected = false},    {.kind = SurfaceKind::Wasm, .expected = false},
    };
    for (const Expectation &row : table) {
        Platform p;
        p.surface = row.kind;
        const auto caps = p.capabilities();
        AURORA_TEST_CHECK_EQ(static_cast<int>(caps.native_menu_bar), static_cast<int>(row.expected));
    }
}

// ---------- 门面契约（进程级单槽 + 点击路由）----------

AURORA_TEST_CASE(install_sets_installed_and_uninstall_clears) {
    BackendGuard guard;
    auto &nb = NativeMenuBar::instance();
    nb.uninstall();  // 起点清零
    auto &obs = install_fake_backend();

    AURORA_TEST_CHECK_FALSE(nb.is_installed());
    const auto res = nb.install(sample_tree());
    AURORA_TEST_CHECK_TRUE(res.ok());
    AURORA_TEST_CHECK_TRUE(nb.is_installed());
    AURORA_TEST_CHECK_TRUE(obs.install_called);

    nb.uninstall();
    AURORA_TEST_CHECK_FALSE(nb.is_installed());
    AURORA_TEST_CHECK_TRUE(obs.uninstall_called);
    nb.set_command_registry(nullptr);  // 复位，避免泄漏到其它套件
}

AURORA_TEST_CASE(install_replaces_previous_tree) {
    // 重复 install 替换旧树（不叠加）：伪造后端被再次调用一次，调用计数 == 2。
    BackendGuard guard;
    auto &nb = NativeMenuBar::instance();
    nb.uninstall();
    auto &obs = install_fake_backend();

    (void)nb.install(sample_tree());
    (void)nb.install(sample_tree());  // 第二次应替换，而非叠加
    AURORA_TEST_CHECK_TRUE(nb.is_installed());
    AURORA_TEST_CHECK_EQ(obs.seen_ids.size(), std::size_t{16});  // 8 项 × 2 次（替换语义）
    nb.uninstall();
}

AURORA_TEST_CASE(empty_id_items_are_forwarded_to_backend) {
    // 空 id 项经门面照常进入扁平表、交给后端；点击路由由后端/桥侧按空 id 留 WARN 处理。
    BackendGuard guard;
    auto &nb = NativeMenuBar::instance();
    nb.uninstall();
    auto &obs = install_fake_backend();

    MenuModel m;
    m.label = "NoId";
    std::vector<MenuModel> models;
    models.push_back(std::move(m));
    (void)nb.install(models);

    AURORA_TEST_CHECK_EQ(obs.seen_ids.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(obs.seen_ids[0].empty());  // 空 id 透传到后端，未丢弃
    nb.uninstall();
}

AURORA_TEST_CASE(click_routes_to_command_registry_by_id) {
    // 点击经门面 route_click → CommandRegistry::invoke(id)：验证稳定 id 正确路由到对应命令。
    BackendGuard guard;
    auto &nb = NativeMenuBar::instance();
    nb.uninstall();
    auto &obs = install_fake_backend();

    CommandRegistry reg;
    int fired = 0;
    Command cmd;
    cmd.id = "file.new";
    cmd.action = [&fired]() -> void { ++fired; };
    reg.add(std::move(cmd));
    nb.set_command_registry(&reg);

    (void)nb.install(sample_tree());
    AURORA_TEST_CHECK_TRUE(obs.click != nullptr);
    obs.click("file.new");  // 模拟原生菜单点击回投
    AURORA_TEST_CHECK_EQ(fired, 1);  // 命中命令

    nb.uninstall();
    nb.set_command_registry(nullptr);
}

AURORA_TEST_CASE(empty_id_click_is_not_routed) {
    // 空 id 点击不路由（留 WARN 而非调用未知命令）：fired 不增。
    BackendGuard guard;
    auto &nb = NativeMenuBar::instance();
    nb.uninstall();
    auto &obs = install_fake_backend();

    CommandRegistry reg;
    int fired = 0;
    Command cmd;
    cmd.id = "file.new";
    cmd.action = [&fired]() -> void { ++fired; };
    reg.add(std::move(cmd));
    nb.set_command_registry(&reg);

    (void)nb.install(sample_tree());
    obs.click("");  // 空 id 回投
    AURORA_TEST_CHECK_EQ(fired, 0);  // 不路由到任何命令

    nb.uninstall();
    nb.set_command_registry(nullptr);
}

}  // namespace aurora::test_cases::utest_menu_bar_translate
