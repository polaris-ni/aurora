#include <cstring>

#include "aurora/core/platform.h"
#include "aurora/event/event.h"

#ifdef AURORA_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
#include <dlfcn.h>
#endif

namespace aurora {

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
// ── Linux：经 GLib GSettings 读取系统双击速度（零构建期依赖，缺库即降级）──
// 刻意不引 <gio/gio.h> / <glib.h>：符号全在运行时 dlsym 取得，仓库不绑定桌面依赖，
// 也不需要本机装 GLib 开发包（CI 与各平台开发机同样能编译本文件）。
// 读取源：org.gnome.desktop.peripherals.mouse → double-click（毫秒），即 GNOME / 控制面板 /
// Tweaks 设定的真实系统值；KDE 等经 xsettings 桥同样落到该 schema。
// 任何一步失败（无会话、缺 libgio/libgobject、schema 未装）均回退库级兜底常量。

// GLib GSettings 符号 ABI（取自 GLib 2.x，长期稳定）：
//   GSettings *g_settings_new(const gchar *schema_id);
//   gint      g_settings_get_int(GSettings *settings, const gchar *key);
//   void      g_object_unref(gpointer object);
using FnGSettingsNew = void *(*)(const char *);
using FnGSettingsGetInt = int (*)(void *, const char *);
using FnGObjectUnref = void (*)(void *);

struct GlibSettingsApi {
    FnGSettingsNew settings_new = nullptr;  ///< `g_settings_new`
    FnGSettingsGetInt settings_get_int = nullptr;  ///< `g_settings_get_int`
    FnGObjectUnref object_unref = nullptr;  ///< `g_object_unref`
    bool ok = false;  ///< 三个核心符号是否齐全
};

[[nodiscard]] auto glib_settings_api() -> const GlibSettingsApi & {
    static const GlibSettingsApi API = [] {
        GlibSettingsApi a;
        void *gio = ::dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (gio == nullptr) {
            gio = ::dlopen("libgio-2.0.so", RTLD_LAZY | RTLD_LOCAL);
        }
        void *gobj = ::dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (gobj == nullptr) {
            gobj = ::dlopen("libgobject-2.0.so", RTLD_LAZY | RTLD_LOCAL);
        }
        if (gio == nullptr || gobj == nullptr) {
            if (gio != nullptr) {
                ::dlclose(gio);
            }
            if (gobj != nullptr) {
                ::dlclose(gobj);
            }
            return a;  // 缺 GLib：ok=false，调用方回退常量
        }
        bool ok = true;
        const auto bind = [&ok](const char *name, void *h, auto &dst) {
            void *p = ::dlsym(h, name);
            if (p == nullptr) {
                ok = false;
                return;
            }
            // dlsym 的返回只可能是 `void *`，目标却是函数指针类型别名：逐字节 memcpy 落值是
            // 刻意避开 reinterpret_cast（对象指针→函数指针在 POSIX/dlopen ABI 下合法但不被标准允许）。
            // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast,bugprone-multi-level-implicit-pointer-conversion)
            std::memcpy(&dst, &p, sizeof(dst));
        };
        bind("g_settings_new", gio, a.settings_new);
        bind("g_settings_get_int", gio, a.settings_get_int);
        bind("g_object_unref", gobj, a.object_unref);
        a.ok = ok;
        return a;
    }();
    return API;
}

[[nodiscard]] auto linux_double_click_ms() -> std::uint32_t {
    // GLib API 解析只做一次（进程级静态，句柄不 dlclose，符号地址长期有效）；每次调用仍实时读
    // GSettings，故运行期改系统设置后重建派发器或重读 os_double_click_ms() 即反映新值
    // （与 Windows GetDoubleClickTime 同语义，不做进程级值缓存）。
    const GlibSettingsApi &api = glib_settings_api();
    if (!api.ok) {
        return AURORA_DEFAULT_CLICK_WINDOW_MS;
    }
    void *settings = api.settings_new("org.gnome.desktop.peripherals.mouse");
    if (settings == nullptr) {
        return AURORA_DEFAULT_CLICK_WINDOW_MS;
    }
    const int ms = api.settings_get_int(settings, "double-click");
    api.object_unref(settings);
    if (ms <= 0) {
        return AURORA_DEFAULT_CLICK_WINDOW_MS;
    }
    return static_cast<std::uint32_t>(ms);
}
#endif

auto os_double_click_ms() -> std::uint32_t {
#ifdef AURORA_PLATFORM_WINDOWS
    // 已是毫秒，返回系统真实值（通常 100–1000）。失败（极罕见）回退到库内兜底常量。
    const UINT ms = ::GetDoubleClickTime();
    if (ms == 0U) {
        return AURORA_DEFAULT_CLICK_WINDOW_MS;
    }
    return static_cast<std::uint32_t>(ms);
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    return linux_double_click_ms();
#else
    // macOS / Wasm / Headless / Android 无「可零依赖读取」的公开系统双击速度 API，回退库内常量。
    return AURORA_DEFAULT_CLICK_WINDOW_MS;
#endif
}

}  // namespace aurora
