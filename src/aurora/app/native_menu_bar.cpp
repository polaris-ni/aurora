// native_menu_bar.cpp — 原生菜单栏进程级门面落地（install/uninstall/is_installed + 点击路由）。
//
// 门面只做三件事：① 把 `MenuModel` 树交给折算层摊平；② 把扁平项表与点击路由回调一起交给平台桥
// （进程级渲染通路单槽，无头 CI 由伪造后端登记、真机由 Win32 桥登记）；③ 点击回来时按 `id` 反查
// 命令注册表。原生菜单对象的构建与消息处理全在平台桥，门面不碰任何平台头。
#include "aurora/app/native_menu_bar.h"

#include "aurora/commands.h"  // CommandRegistry（route_click 经它 invoke；头文件只前向声明，此处需完整定义）
#include "aurora/core/log.h"
#include "aurora/window/detail/menu_bar_translate.h"

namespace aurora {

auto NativeMenuBar::instance() -> NativeMenuBar & {
    static NativeMenuBar instance;  // 进程级单例：单线程 UI，无锁
    return instance;
}

auto NativeMenuBar::set_command_registry(CommandRegistry *registry) -> void { registry_ = registry; }

auto NativeMenuBar::install(const std::vector<MenuModel> &models) -> Result<void> {
    const auto *backend = menu_bar_translate::menu_bar_backend();
    if (backend == nullptr || !backend->install) {
        // 本进程没有任何已登记的平台桥：要么平台无原生菜单栏机制（能力位 false），要么桥未被创建。
        return make_error(ErrorCode::NativeMenuUnsupported,
                          "native-menu-bar: no platform bridge registered (capability native_menu_bar is false here)");
    }
    // 折算层纯函数摊平（保序 + 字段保真 + 空 id 照常建出），与点击路由回调一并交给桥。
    // 路由回调捕获的是本单例持有的注册表指针（单例进程级长存，回调捕获安全）。
    auto flat = menu_bar_translate::flatten_menu_models(models);
    auto result = backend->install(flat, [this](const std::string &id) -> void { route_click(id); });
    installed_ = result.ok();
    return result;
}

auto NativeMenuBar::uninstall() -> void {
    const auto *backend = menu_bar_translate::menu_bar_backend();
    if (backend != nullptr && backend->uninstall) {
        backend->uninstall();
    }
    installed_ = false;
}

auto NativeMenuBar::is_installed() const -> bool { return installed_; }

auto NativeMenuBar::route_click(const std::string &id) const -> void {
    if (id.empty()) {
        // 空 id 的项照常建出、照常显示，但原生路径跨重建只能靠稳定 id 路由点击——空 id 路由不了。
        // 不静默丢弃（菜单看着建好了却点不动最难排查），也不伪造 id，只留一条诊断。
        AURORA_LOG_WARN("native-menu-bar",
                        "menu item clicked but its id is empty; click ignored (declare a stable id)");
        return;
    }
    if (registry_ == nullptr) {
        AURORA_LOG_WARN("native-menu-bar", "menu item '", id,
                        "' clicked but no CommandRegistry is registered; click ignored");
        return;
    }
    // 行为与自绘路径共用同一出口：菜单点击 = 调对应命令。
    registry_->invoke(id);
}

}  // namespace aurora
