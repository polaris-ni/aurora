// AT-SPI2 平台桥实现：libdbus dlopen 绑定 + a11y 总线连接 + Embed 握手 + 方法面应答。
//
// 分层：所有「语义 → 协议值」的折算都在 `atspi_protocol.cpp`（无头可测）；本文件只剩
// D-Bus 编解码与生命周期粘合。协议签名与成员名 2026-09-20 逐字核对上游
// at-spi2-core main @ 2.60 线的 `xml/*.xml` 与 libatspi 消费者侧读法。
//
// 线程模型：全部在 UI 线程（宿主帧循环 pump()），与 UIA 桥同口径 —— in-proc provider
// 由平台在 UI 线程回调是 UIA 侧的既定事实，AT-SPI 侧 D-Bus 消息只在我们 pump 时到达。

#include "aurora/window/detail/atspi_bridge.h"

#if defined(AURORA_PLATFORM_LINUX) && (defined(AURORA_BACKEND_X11) || defined(AURORA_BACKEND_WAYLAND))

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/diagnostics.h"

namespace aurora::detail {
namespace {

// ============================================================================
// libdbus 扁平 API 的 dlopen 绑定（D15 同款：任一核心符号缺失 ⇒ 整桥降级 no-op）
// ============================================================================

using Conn = struct DBusConnection *;
using Msg = struct DBusMessage *;
using Watch = struct DBusWatch *;

/// @brief DBusMessageIter 的中立替身：真实体是 `struct { void *dummy[5]; }`（40B，dbus-macros.h），
///        本侧只经指针交给 API、永不解引用字段 ⇒ 64B 对齐缓冲足够且留余量。
struct alignas(8) Iter {
    std::uint64_t w[8];
};

/// @brief DBusError 的中立替身（64B 缓冲）。首成员跨版本恒为 `char *name`（libdbus 保持该
///        ABI）；我们只用 `name() != null` 判错，不读其余字段。
struct alignas(8) Err {
    std::uint64_t w[8];
    [[nodiscard]] auto name() const -> const char * {
        return reinterpret_cast<const char *>(static_cast<std::uintptr_t>(w[0]));  // NOLINT
    }
};

using Fn_watch_add = int (*)(Watch, void *);
using Fn_watch_remove = void (*)(Watch, void *);
using Fn_watch_toggle = void (*)(Watch, void *);
using Fn_free = void (*)(void *);
using Fn_filter = int (*)(Msg, void *, Err *);

/// @brief 用到的 libdbus 符号集（34 个；类型码用 D-Bus 单字符常量）。
struct LibDbus {
    bool loaded = false;
    void *handle = nullptr;

    Conn (*bus_get)(int, Err *) = nullptr;
    const char *(*bus_get_unique_name)(Conn) = nullptr;
    int (*bus_register)(Conn, Err *) = nullptr;
    Conn (*connection_open_private)(const char *, Err *) = nullptr;
    void (*connection_set_exit_on_disconnect)(Conn, int) = nullptr;
    int (*connection_set_watch_functions)
    (Conn, Fn_watch_add, Fn_watch_remove, Fn_watch_toggle, void *, Fn_free) = nullptr;
    int (*connection_add_filter)(Conn, Fn_filter, void *, Fn_free) = nullptr;
    int (*watch_get_unix_fd)(Watch) = nullptr;
    unsigned (*watch_get_flags)(Watch) = nullptr;
    int (*watch_handle)(Watch, unsigned) = nullptr;
    int (*connection_dispatch)(Conn) = nullptr;
    int (*connection_read_write)(Conn, int) = nullptr;
    void (*connection_flush)(Conn) = nullptr;
    void (*connection_close)(Conn) = nullptr;
    void (*connection_unref)(Conn) = nullptr;
    int (*connection_send)(Conn, Msg, std::uint32_t *) = nullptr;
    Msg (*connection_send_with_reply_and_block)(Conn, Msg, int) = nullptr;
    Msg (*message_new_method_call)(const char *, const char *, const char *, const char *) = nullptr;
    Msg (*message_new_method_return)(Msg) = nullptr;
    Msg (*message_new_error)(Msg, const char *, const char *) = nullptr;
    void (*message_unref)(Msg) = nullptr;
    int (*message_get_type)(Msg) = nullptr;
    const char *(*message_get_path)(Msg) = nullptr;
    const char *(*message_get_interface)(Msg) = nullptr;
    const char *(*message_get_member)(Msg) = nullptr;
    const char *(*message_get_sender)(Msg) = nullptr;
    int (*message_get_no_reply_expected)(Msg) = nullptr;
    void (*iter_init_append)(Msg, Iter *) = nullptr;
    int (*iter_append_basic)(Iter *, int, const void *) = nullptr;
    int (*iter_open_container)(Iter *, int, const char *, Iter *) = nullptr;
    int (*iter_close_container)(Iter *, Iter *) = nullptr;
    int (*iter_init)(Msg, Iter *) = nullptr;
    int (*iter_get_arg_type)(Iter *) = nullptr;
    int (*iter_next)(Iter *) = nullptr;
    void (*iter_recurse)(Iter *, Iter *) = nullptr;
    void (*iter_get_basic)(Iter *, void *) = nullptr;
    void (*error_init)(Err *) = nullptr;
    void (*error_free)(Err *) = nullptr;

    [[nodiscard]] static auto instance() -> const LibDbus &;
};

auto LibDbus::instance() -> const LibDbus & {
    static const LibDbus api = [] {
        LibDbus a;
        void *h = ::dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (h == nullptr) {
            h = ::dlopen("libdbus-1.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (h == nullptr) {
            return a;  // 无 libdbus：loaded=false，桥整体降级
        }
        bool ok = true;
        const auto bind = [&ok, h](const char *name, auto &dst) {
            void *p = ::dlsym(h, name);
            if (p == nullptr) {
                ok = false;
                return;
            }
            std::memcpy(&dst, &p, sizeof(dst));  // void* → 函数指针的可移植落法
        };
        bind("dbus_bus_get", a.bus_get);
        bind("dbus_bus_get_unique_name", a.bus_get_unique_name);
        bind("dbus_bus_register", a.bus_register);
        bind("dbus_connection_open_private", a.connection_open_private);
        bind("dbus_connection_set_exit_on_disconnect", a.connection_set_exit_on_disconnect);
        bind("dbus_connection_set_watch_functions", a.connection_set_watch_functions);
        bind("dbus_connection_add_filter", a.connection_add_filter);
        bind("dbus_watch_get_unix_fd", a.watch_get_unix_fd);
        bind("dbus_watch_get_flags", a.watch_get_flags);
        bind("dbus_watch_handle", a.watch_handle);
        bind("dbus_connection_dispatch", a.connection_dispatch);
        bind("dbus_connection_read_write", a.connection_read_write);
        bind("dbus_connection_flush", a.connection_flush);
        bind("dbus_connection_close", a.connection_close);
        bind("dbus_connection_unref", a.connection_unref);
        bind("dbus_connection_send", a.connection_send);
        bind("dbus_connection_send_with_reply_and_block", a.connection_send_with_reply_and_block);
        bind("dbus_message_new_method_call", a.message_new_method_call);
        bind("dbus_message_new_method_return", a.message_new_method_return);
        bind("dbus_message_new_error", a.message_new_error);
        bind("dbus_message_unref", a.message_unref);
        bind("dbus_message_get_type", a.message_get_type);
        bind("dbus_message_get_path", a.message_get_path);
        bind("dbus_message_get_interface", a.message_get_interface);
        bind("dbus_message_get_member", a.message_get_member);
        bind("dbus_message_get_sender", a.message_get_sender);
        bind("dbus_message_get_no_reply_expected", a.message_get_no_reply_expected);
        bind("dbus_message_iter_init_append", a.iter_init_append);
        bind("dbus_message_iter_append_basic", a.iter_append_basic);
        bind("dbus_message_iter_open_container", a.iter_open_container);
        bind("dbus_message_iter_close_container", a.iter_close_container);
        bind("dbus_message_iter_init", a.iter_init);
        bind("dbus_message_iter_get_arg_type", a.iter_get_arg_type);
        bind("dbus_message_iter_next", a.iter_next);
        bind("dbus_message_iter_recurse", a.iter_recurse);
        bind("dbus_message_iter_get_basic", a.iter_get_basic);
        bind("dbus_error_init", a.error_init);
        bind("dbus_error_free", a.error_free);
        if (!ok) {
            ::dlclose(h);
            return a;
        }
        a.handle = h;
        a.loaded = true;
        return a;
    }();
    return api;
}

// D-Bus 常量（libdbus ABI 数值；仅本文件用到的子集）。
constexpr int mt_method_call = 1;
constexpr int handler_more = 1;
constexpr int handler_ok = 2;
constexpr int dispatch_complete = 2;
constexpr unsigned watch_readable = 1U;
constexpr unsigned watch_writeable = 2U;
constexpr char ty_bool = 'b';
constexpr char ty_int32 = 'i';
constexpr char ty_uint32 = 'u';
constexpr char ty_double = 'd';
constexpr char ty_string = 's';
constexpr char ty_objpath = 'o';
constexpr char ty_variant = 'v';
constexpr char ty_array = 'a';
constexpr char ty_struct = '(';
constexpr char ty_dict = '{';

constexpr const char *err_unknown_object = "org.freedesktop.DBus.Error.UnknownObject";
constexpr const char *err_unknown_method = "org.freedesktop.DBus.Error.UnknownMethod";
constexpr const char *err_invalid_args = "org.freedesktop.DBus.Error.InvalidArgs";
constexpr const char *iface_props = "org.freedesktop.DBus.Properties";
constexpr const char *iface_introspect = "org.freedesktop.DBus.Introspectable";
constexpr const char *iface_peer = "org.freedesktop.DBus.Peer";

// ============================================================================
// 编解码小工具（签名已知，刻意不做大一统抽象；每处机械可核对）
// ============================================================================

auto put_string(const LibDbus &L, Iter &it, char kind, const std::string &v) -> void {
    const char *p = v.c_str();
    L.iter_append_basic(&it, kind, &p);
}

auto put_int(const LibDbus &L, Iter &it, std::int32_t v) -> void {
    L.iter_append_basic(&it, ty_int32, &v);
}

auto put_uint(const LibDbus &L, Iter &it, std::uint32_t v) -> void {
    L.iter_append_basic(&it, ty_uint32, &v);
}

auto put_bool(const LibDbus &L, Iter &it, bool v) -> void {
    const int raw = v ? 1 : 0;
    L.iter_append_basic(&it, ty_bool, &raw);
}

auto put_double(const LibDbus &L, Iter &it, double v) -> void {
    L.iter_append_basic(&it, ty_double, &v);
}

/// @brief 追加 `(so)` 对象引用。
auto put_so(const LibDbus &L, Iter &it, const AtspiRef &r) -> void {
    Iter sub{};
    if (L.iter_open_container(&it, ty_struct, "so", &sub) == 0) {
        return;
    }
    put_string(L, sub, ty_string, r.bus);
    put_string(L, sub, ty_objpath, r.path);
    L.iter_close_container(&it, &sub);
}

/// @brief 把模型属性值以 variant 落线（Kind::None ⇒ false，调用方转 InvalidArgs）。
auto put_variant(const LibDbus &L, Iter &it, const AtspiPropValue &v) -> bool {
    const char *sig = nullptr;
    switch (v.kind) {
        case AtspiPropValue::Kind::Str:
            sig = "s";
            break;
        case AtspiPropValue::Kind::I32:
            sig = "i";
            break;
        case AtspiPropValue::Kind::U32:
            sig = "u";
            break;
        case AtspiPropValue::Kind::Bool:
            sig = "b";
            break;
        case AtspiPropValue::Kind::Dbl:
            sig = "d";
            break;
        case AtspiPropValue::Kind::Ref:
            sig = "(so)";
            break;
        case AtspiPropValue::Kind::None:
            return false;
    }
    Iter sub{};
    if (L.iter_open_container(&it, ty_variant, sig, &sub) == 0) {
        return false;
    }
    switch (v.kind) {
        case AtspiPropValue::Kind::Str:
            put_string(L, sub, ty_string, v.str);
            break;
        case AtspiPropValue::Kind::I32:
            put_int(L, sub, v.i32);
            break;
        case AtspiPropValue::Kind::U32:
            put_uint(L, sub, v.u32);
            break;
        case AtspiPropValue::Kind::Bool:
            put_bool(L, sub, v.boolean);
            break;
        case AtspiPropValue::Kind::Dbl:
            put_double(L, sub, v.dbl);
            break;
        case AtspiPropValue::Kind::Ref:
            put_so(L, sub, v.ref);
            break;
        case AtspiPropValue::Kind::None:
            break;
    }
    L.iter_close_container(&it, &sub);
    return true;
}

/// @brief 入站参数游标（消息顶层 / 容器内子层共用）。
class Cur {
  public:
    Cur() = default;
    Cur(const LibDbus &lib, Msg m) : L(&lib) { alive_ = lib.iter_init(m, &it_) != 0; }
    Cur(const LibDbus &lib, Iter sub) : L(&lib), it_(sub) { alive_ = L->iter_get_arg_type(&it_) != 0; }

    [[nodiscard]] auto ty() const -> int { return alive_ ? L->iter_get_arg_type(&it_) : 0; }
    [[nodiscard]] auto done() const -> bool { return !alive_; }
    [[nodiscard]] auto iter() -> Iter & { return it_; }
    auto next() -> void {
        if (alive_) {
            alive_ = L->iter_next(&it_) != 0;
        }
    }

    auto take_int(std::int32_t &v) -> bool {
        if (ty() != ty_int32) {
            return false;
        }
        L->iter_get_basic(&it_, &v);
        next();
        return true;
    }
    auto take_uint(std::uint32_t &v) -> bool {
        if (ty() != ty_uint32) {
            return false;
        }
        L->iter_get_basic(&it_, &v);
        next();
        return true;
    }
    auto take_bool(bool &v) -> bool {
        if (ty() != ty_bool) {
            return false;
        }
        int raw = 0;
        L->iter_get_basic(&it_, &raw);
        v = raw != 0;
        next();
        return true;
    }
    auto take_double(double &v) -> bool {
        if (ty() != ty_double) {
            return false;
        }
        L->iter_get_basic(&it_, &v);
        next();
        return true;
    }
    auto take_string(std::string &v) -> bool {
        const int t = ty();
        if (t != ty_string && t != ty_objpath && t != 'g') {
            return false;
        }
        const char *p = nullptr;
        L->iter_get_basic(&it_, &p);
        v = (p != nullptr) ? std::string{p} : std::string{};
        next();
        return true;
    }
    /// @brief 进入容器（'(' / 'a' / 'v'）填充 `sub`；返回 false = 类型不符（不消费游标）。
    ///        父游标的推进归调用方（读完子层后 `next()`）。
    auto into(int kind, Cur &sub) -> bool {
        if (ty() != kind) {
            return false;
        }
        Iter inner{};
        L->iter_recurse(&it_, &inner);
        sub = Cur{*L, inner};
        return true;
    }

  private:
    const LibDbus *L = nullptr;
    mutable Iter it_{};  // 不透明缓冲：const 查询口（ty()）只透传指针给 libdbus，不解引用
    bool alive_ = false;
};

auto a11y_bus_address(const LibDbus &L) -> const std::string & {
    // 进程级一次：env 覆盖优先，其次 org.a11y.Bus.GetAddress（会自动拉起 launcher+总线）。
    static const std::string addr = [&L] {
        if (const char *over = std::getenv("AT_SPI_BUS_ADDRESS"); over != nullptr && over[0] != '\0') {
            return std::string{over};
        }
        Conn session = nullptr;
        Err e{};
        L.error_init(&e);
        session = L.bus_get(0 /*SESSION*/, &e);
        L.error_free(&e);
        if (session == nullptr) {
            return std::string{};
        }
        Msg c = L.message_new_method_call(atspi::k_a11y_bus_service, atspi::k_a11y_bus_path,
                                          atspi::k_a11y_bus_iface, "GetAddress");
        if (c == nullptr) {
            return std::string{};
        }
        Msg r = L.connection_send_with_reply_and_block(session, c, 3000);
        L.message_unref(c);
        std::string out;
        if (r != nullptr) {
            Cur cur{L, r};
            cur.take_string(out);
            L.message_unref(r);
        }
        return out;
    }();
    return addr;
}

// ============================================================================
// AtspiBridge::Impl
// ============================================================================

// 注：Impl 是头文件里声明的嵌套类，定义必须留在具名命名空间（匿名 ns 内定义嵌套类是
// 硬错误）；上方匿名 ns 的替身类型/常量对本 TU 仍然可见。
}  // namespace

struct AtspiBridge::Impl {
    const LibDbus &L = LibDbus::instance();
    AtspiModel model;
    Conn conn = nullptr;
    std::string unique_name;      ///< 本连接唯一总线名（":1.57"）
    bool provider_registered = false;
    bool active = false;
    bool dirty = true;
    bool tearing_down = false;    ///< 根销毁门闩（UIA #8 同款：断开期间不再重建）
    bool client_seen = false;     ///< 收到过外部方法调用 ⇒ 判定「有 AT 客户端在线」
    Widget *root = nullptr;
    a11y::TreeSnapshot snap{};
    std::vector<Watch> watches;

    explicit Impl(AtspiEnv env) : model{std::move(env)} {}

    // ---- 建连 / 握手 ----

    [[nodiscard]] auto connect_and_embed() -> bool {
        Err e{};
        L.error_init(&e);
        const std::string &addr = a11y_bus_address(L);
        if (addr.empty()) {
            return false;
        }
        conn = L.connection_open_private(addr.c_str(), &e);
        if (conn == nullptr) {
            L.error_free(&e);
            return false;
        }
        L.error_init(&e);
        if (L.bus_register(conn, &e) == 0) {
            L.error_free(&e);
            close_conn();
            return false;
        }
        L.error_free(&e);
        const char *un = L.bus_get_unique_name(conn);
        unique_name = (un != nullptr) ? un : "";
        if (unique_name.empty()) {
            close_conn();
            return false;
        }
        L.connection_set_exit_on_disconnect(conn, 0);
        if (L.connection_set_watch_functions(conn, &Impl::watch_added, &Impl::watch_removed,
                                             &Impl::watch_toggled, this, nullptr) == 0) {
            close_conn();
            return false;
        }
        if (L.connection_add_filter(conn, &Impl::filter_tramp, this, nullptr) == 0) {
            close_conn();
            return false;
        }
        model.env_mut().self_bus = unique_name;

        // Socket.Embed：plug = (本总线名, 本 app 根路径)；注册表回其根引用。
        Msg c = L.message_new_method_call(atspi::k_registry_bus, atspi::k_registry_root_path,
                                          atspi::k_iface_socket, "Embed");
        if (c == nullptr) {
            close_conn();
            return false;
        }
        Iter arg_top{};
        L.iter_init_append(c, &arg_top);
        Iter plug{};
        if (L.iter_open_container(&arg_top, ty_struct, "so", &plug) != 0) {
            put_string(L, plug, ty_string, unique_name);
            put_string(L, plug, ty_objpath, model.env().base_path);
            L.iter_close_container(&arg_top, &plug);
        }
        Msg r = L.connection_send_with_reply_and_block(conn, c, 3000);
        L.message_unref(c);
        if (r == nullptr) {
            close_conn();
            return false;  // 注册表不可达/超时（含 registryd 未装）
        }
        // 回包 `(so)` = 注册表总线名 + 其根路径（registry.c::socket_embed）。
        std::string reg_bus;
        std::string reg_path;
        Cur top{L, r};
        Cur so{};
        if (top.into(ty_struct, so)) {
            so.take_string(reg_bus);
            so.take_string(reg_path);  // objpath 也走 take_string（类型集含 'o'）
        }
        L.message_unref(r);
        if (reg_path.empty()) {
            reg_path = atspi::k_registry_root_path;
        }
        model.env_mut().registry_root = AtspiRef{.bus = std::move(reg_bus), .path = std::move(reg_path)};
        return true;
    }

    auto close_conn() -> void {
        if (conn == nullptr) {
            return;
        }
        L.connection_close(conn);
        L.connection_unref(conn);
        conn = nullptr;
        watches.clear();
    }

    // ---- 帧循环集成 ----

    [[nodiscard]] auto poll_watches() const -> std::vector<AtspiBridge::WatchFd> {
        std::vector<AtspiBridge::WatchFd> out;
        for (Watch w : watches) {
            const unsigned flags = L.watch_get_flags(w);
            if ((flags & (watch_readable | watch_writeable)) == 0U) {
                continue;  // 禁用的 watch
            }
            AtspiBridge::WatchFd fd;
            fd.fd = L.watch_get_unix_fd(w);
            fd.events = static_cast<short>(
                ((flags & watch_readable) != 0U ? POLLIN : 0) |
                ((flags & watch_writeable) != 0U ? POLLOUT : 0));
            if (fd.fd >= 0) {
                out.push_back(fd);
            }
        }
        return out;
    }

    auto pump() -> void {
        if (conn == nullptr) {
            return;
        }
        [[maybe_unused]] const int touched = L.connection_read_write(conn, 0);  // 非阻塞收发
        while (L.connection_dispatch(conn) == dispatch_complete) {
            // 逐条派发；filter 内完成应答（应答已入队）
        }
        L.connection_flush(conn);  // 把派发期产生的回复一次写回传输
    }

    // ---- libdbus 回调（静态蹦床 → 本实例；仅 UI 线程触发）----

    static int watch_added(Watch w, void *ud) {
        static_cast<Impl *>(ud)->watches.push_back(w);
        return 1;
    }
    static void watch_removed(Watch w, void *ud) {
        auto &v = static_cast<Impl *>(ud)->watches;
        v.erase(std::ranges::remove(v, w).begin(), v.end());
    }
    static void watch_toggled(Watch, void *) {}

    static int filter_tramp(Msg m, void *ud, Err *) {
        auto &self = *static_cast<Impl *>(ud);
        return self.on_message(m) ? handler_ok : handler_more;
    }

    // ---- 入站消息路由 ----

    [[nodiscard]] auto on_message(Msg m) -> bool {
        if (L.message_get_type(m) != mt_method_call) {
            return false;  // 信号/回报交 libdbus 内部处理
        }
        sync_point();  // D9：平台查询到达是唯一的建树同步点
        const char *raw_path = L.message_get_path(m);
        const char *raw_iface = L.message_get_interface(m);
        const char *raw_member = L.message_get_member(m);
        const std::string path{raw_path != nullptr ? raw_path : ""};
        const std::string iface{raw_iface != nullptr ? raw_iface : ""};
        const std::string member{raw_member != nullptr ? raw_member : ""};
        if (!client_seen) {
            client_seen = true;
            current_accessibility_settings().screen_reader_active = true;  // G4/R9 heuristic 回填
        }
        if (path == atspi::k_cache_path) {
            return dispatch_cache(m, member);
        }
        std::optional<std::uint64_t> id;
        if (path == model.env().base_path) {
            id = k_atspi_app_id;
        } else {
            id = model.id_of_path(path);
        }
        if (!id.has_value()) {
            return error_reply(m, err_unknown_object, "no such accessible object");
        }
        return dispatch_node(m, *id, iface, member);
    }

    // ---- 应答基元 ----

    [[nodiscard]] auto reply_begin(Msg call, Iter &top) -> Msg {
        Msg r = L.message_new_method_return(call);
        if (r == nullptr) {
            return nullptr;
        }
        L.iter_init_append(r, &top);
        return r;
    }

    auto reply_end(Msg r) -> bool {
        L.connection_send(conn, r, nullptr);
        L.message_unref(r);
        return true;
    }

    [[nodiscard]] auto reply_void(Msg call) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        return r != nullptr && reply_end(r);
    }

    [[nodiscard]] auto error_reply(Msg call, const char *name, const char *text) -> bool {
        if (L.message_get_no_reply_expected(call) != 0) {
            return true;  // 客户端没等回复：不回，只消费
        }
        Msg e = L.message_new_error(call, name, text);
        if (e == nullptr) {
            return false;
        }
        L.connection_send(conn, e, nullptr);
        L.message_unref(e);
        return true;
    }

    /// @brief 单 int 回复。
    [[nodiscard]] auto reply_i(Msg call, std::int32_t v) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_int(L, top, v);
        return reply_end(r);
    }

    [[nodiscard]] auto reply_u(Msg call, std::uint32_t v) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_uint(L, top, v);
        return reply_end(r);
    }

    [[nodiscard]] auto reply_s(Msg call, const std::string &v) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_string(L, top, ty_string, v);
        return reply_end(r);
    }

    [[nodiscard]] auto reply_b(Msg call, bool v) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_bool(L, top, v);
        return reply_end(r);
    }

    [[nodiscard]] auto reply_so(Msg call, const AtspiRef &ref) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_so(L, top, ref);
        return reply_end(r);
    }

    [[nodiscard]] auto reply_i4(Msg call, const AtspiRectI &v) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        put_int(L, top, v.x);
        put_int(L, top, v.y);
        put_int(L, top, v.width);
        put_int(L, top, v.height);
        return reply_end(r);
    }

    // ---- 方法面：Accessible / Application / Component / Text / Action / Socket / Properties ----

    [[nodiscard]] auto dispatch_node(Msg m, std::uint64_t id, const std::string &iface,
                                     const std::string &member) -> bool {
        // 通用总线接口
        if (member == "Introspect" || iface == iface_introspect) {
            return reply_introspect(m, id);
        }
        if (iface == iface_peer) {
            if (member == "Ping") {
                return reply_void(m);
            }
            return error_reply(m, err_unknown_method, "GetMachineId unsupported");
        }
        if (iface == iface_props) {
            if (member == "Get") {
                return prop_get_call(m, id);
            }
            if (member == "Set") {
                return prop_set_call(m, id);
            }
            if (member == "GetAll") {
                return prop_get_all(m, id);
            }
            return error_reply(m, err_unknown_method, member.c_str());
        }

        // Accessible（所有对象都有）
        if (member == "GetRole") {
            return reply_u(m, model.role(id));
        }
        if (member == "GetRoleName" || member == "GetLocalizedRoleName") {
            return reply_s(m, model.role_name(id));
        }
        if (member == "GetState") {
            const auto states = model.states(id);
            Iter top{};
            Msg r = reply_begin(m, top);
            if (r == nullptr) {
                return false;
            }
            // 上游线格式 = 前置计数 int + 状态数组（libatspi `extract_int_as_guint32` 先读 int，
            // 且其 `get_state_set` 对缺计数的应答会静默丢状态集 ⇒ 计数必须发）。
            put_int(L, top, static_cast<std::int32_t>(states.size()));
            Iter arr{};
            if (L.iter_open_container(&top, ty_array, "u", &arr) != 0) {
                for (const std::uint32_t s : states) {
                    put_uint(L, arr, s);
                }
                L.iter_close_container(&top, &arr);
            }
            return reply_end(r);
        }
        if (member == "GetInterfaces") {
            return reply_strings(m, model.interfaces(id));
        }
        if (member == "GetIndexInParent") {
            return reply_i(m, model.index_in_parent(id));
        }
        if (member == "GetChildAtIndex") {
            Cur cur{L, m};
            std::int32_t index = -1;
            if (!cur.take_int(index)) {
                return error_reply(m, err_invalid_args, "expect i");
            }
            const auto kid = model.child_at(id, index);
            return reply_so(m, model.ref_of(kid.value_or(0ULL)));
        }
        if (member == "GetChildren") {
            Iter top{};
            Msg r = reply_begin(m, top);
            if (r == nullptr) {
                return false;
            }
            Iter arr{};
            if (L.iter_open_container(&top, ty_array, "(so)", &arr) != 0) {
                for (const std::uint64_t kid : model.children(id)) {
                    put_so(L, arr, model.ref_of(kid));
                }
                L.iter_close_container(&top, &arr);
            }
            return reply_end(r);
        }
        if (member == "GetApplication") {
            return reply_so(m, model.application());
        }
        if (member == "GetRelationSet") {
            return reply_empty_typed(m, "(ua(ii))");  // 关系集：本增量申报为空（申报见头注）
        }
        if (member == "GetAttributes") {
            return reply_empty_typed(m, "{ss}");
        }

        // Application（仅 App 根）
        if (id == k_atspi_app_id && iface == atspi::k_iface_application) {
            if (member == "GetLocale") {
                return reply_s(m, "C");
            }
            if (member == "GetApplicationBusAddress") {
                return reply_s(m, unique_name);
            }
        }
        // Socket（仅 App 根）
        if (id == k_atspi_app_id && iface == atspi::k_iface_socket) {
            if (member == "Embedded") {
                return reply_so(m, model.env().registry_root);
            }
            if (member == "Available") {
                return reply_empty_typed(m, "(so)");
            }
            if (member == "Unembed") {
                return reply_void(m);
            }
        }

        // Component（App 根无几何 ⇒ 模型自然回零盒）
        if (member == "GetExtents" || member == "GetPosition" || member == "GetSize") {
            std::uint32_t coord = 0;
            Cur cur{L, m};
            if (member == "GetSize") {
                coord = atspi::coord_screen;  // GetSize 无参
            } else if (!cur.take_uint(coord)) {
                return error_reply(m, err_invalid_args, "expect coord u");
            }
            const AtspiRectI e = model.extents(id, coord);
            if (member == "GetExtents") {
                return reply_i4(m, e);
            }
            if (member == "GetPosition") {
                Iter top{};
                Msg r = reply_begin(m, top);
                if (r == nullptr) {
                    return false;
                }
                put_int(L, top, e.x);
                put_int(L, top, e.y);
                return reply_end(r);
            }
            Iter top{};
            Msg r = reply_begin(m, top);
            if (r == nullptr) {
                return false;
            }
            put_int(L, top, e.width);
            put_int(L, top, e.height);
            return reply_end(r);
        }
        if (member == "Contains") {
            Cur cur{L, m};
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::uint32_t coord = 0;
            if (!cur.take_int(x) || !cur.take_int(y) || !cur.take_uint(coord)) {
                return error_reply(m, err_invalid_args, "expect iiu");
            }
            return reply_b(m, model.contains(id, x, y, coord));
        }
        if (member == "GetAccessibleAtPoint") {
            Cur cur{L, m};
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::uint32_t coord = 0;
            if (!cur.take_int(x) || !cur.take_int(y) || !cur.take_uint(coord)) {
                return error_reply(m, err_invalid_args, "expect iiu");
            }
            return reply_so(m, model.accessible_at_point(id, x, y, coord));
        }
        if (member == "GetLayer") {
            Iter top{};
            Msg r = reply_begin(m, top);
            if (r == nullptr) {
                return false;
            }
            put_int(L, top, 0);  // layer：本增量不建模叠层（申报）；depth
            put_int(L, top, 0);
            return reply_end(r);
        }
        if (member == "GrabFocus") {
            // 尽力而为：经动作通道请求获焦（控件不支持即静默；调用本身按成功应答）。
            if (const a11y::NodeSnapshot *n = model.node(id); n != nullptr && n->widget != nullptr) {
                if (model.env().perform) {
                    model.env().perform(const_cast<Widget *>(n->widget),
                                        AccessibilityActionRequest{.action = AccessibilityAction::Focus});
                }
            }
            return reply_void(m);
        }

        // Text（码点偏移）
        if (member == "GetText") {
            Cur cur{L, m};
            std::int32_t start = 0;
            std::int32_t end = -1;
            if (!cur.take_int(start) || !cur.take_int(end)) {
                return error_reply(m, err_invalid_args, "expect ii");
            }
            return reply_s(m, model.text_slice(id, start, end));
        }
        if (member == "GetCharacterCount") {
            return reply_i(m, model.text_char_count(id));
        }
        if (member == "GetCaretOffset") {
            return reply_i(m, model.text_caret(id));
        }
        if (member == "SetCaretOffset") {
            Cur cur{L, m};
            std::int32_t offset = 0;
            if (!cur.take_int(offset)) {
                return error_reply(m, err_invalid_args, "expect i");
            }
            // 设 caret 需要控件侧接口（现状：TextRange 走 UIA 独有）；本增量接受并空回复。
            return reply_void(m);
        }
        if (member == "GetCharacterExtents") {
            Cur cur{L, m};
            std::int32_t offset = 0;
            std::uint32_t coord = 0;
            if (!cur.take_int(offset) || !cur.take_uint(coord)) {
                return error_reply(m, err_invalid_args, "expect iu");
            }
            return reply_i4(m, model.text_char_extents(id, offset, coord));
        }

        // Action
        if (member == "GetNActions") {
            return reply_i(m, static_cast<std::int32_t>(model.actions(id).size()));
        }
        if (member == "GetActions") {
            Iter top{};
            Msg r = reply_begin(m, top);
            if (r == nullptr) {
                return false;
            }
            Iter arr{};
            if (L.iter_open_container(&top, ty_array, "(sss)", &arr) != 0) {
                for (const auto &row : model.actions(id)) {
                    Iter st{};
                    if (L.iter_open_container(&arr, ty_struct, "sss", &st) != 0) {
                        continue;
                    }
                    put_string(L, st, ty_string, row.name);
                    put_string(L, st, ty_string, row.localized);
                    put_string(L, st, ty_string, row.keybinding);
                    L.iter_close_container(&arr, &st);
                }
                L.iter_close_container(&top, &arr);
            }
            return reply_end(r);
        }
        if (member == "DoAction") {
            Cur cur{L, m};
            std::int32_t index = -1;
            if (!cur.take_int(index)) {
                return error_reply(m, err_invalid_args, "expect i");
            }
            const bool done = model.do_action(id, index);
            if (done) {
                dirty = true;  // 动作可能改变语义树（勾选 / 滚动量等）
            }
            return reply_b(m, done);
        }
        if (member == "GetName" || member == "GetLocalizedName") {
            Cur cur{L, m};
            std::int32_t index = -1;
            if (!cur.take_int(index)) {
                return error_reply(m, err_invalid_args, "expect i");
            }
            const auto rows = model.actions(id);
            if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) {
                return error_reply(m, err_invalid_args, "action index out of range");
            }
            const auto &row = rows[static_cast<std::size_t>(index)];
            return reply_s(m, member == "GetName" ? row.name : row.localized);
        }
        if (member == "GetDescription") {
            if (iface == atspi::k_iface_action) {
                Cur cur{L, m};
                std::int32_t index = -1;
                if (!cur.take_int(index)) {
                    return error_reply(m, err_invalid_args, "expect i");
                }
                return reply_s(m, "");  // Action 行无描述字段（Aurora 语义未导出）
            }
            return error_reply(m, err_unknown_method, member.c_str());
        }
        if (member == "GetKeyBinding") {
            return reply_s(m, "");  // 键绑定未导出（申报）
        }

        if (!iface.empty() && !AtspiModel::handles(iface, member)) {
            return error_reply(m, err_unknown_method, "unsupported interface/member");
        }
        return error_reply(m, err_unknown_method, member.c_str());
    }

    [[nodiscard]] auto reply_strings(Msg call, const std::vector<std::string> &vals) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        Iter arr{};
        if (L.iter_open_container(&top, ty_array, "s", &arr) != 0) {
            for (const std::string &s : vals) {
                put_string(L, arr, ty_string, s);
            }
            L.iter_close_container(&top, &arr);
        }
        return reply_end(r);
    }

    /// @brief 空数组回复（elem = 元素签名字符串）。
    [[nodiscard]] auto reply_empty_typed(Msg call, const char *elem) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        Iter arr{};
        if (L.iter_open_container(&top, ty_array, elem, &arr) != 0) {
            L.iter_close_container(&top, &arr);
        }
        return reply_end(r);
    }

    // ---- Properties ----

    [[nodiscard]] auto prop_get_call(Msg call, std::uint64_t id) -> bool {
        Cur cur{L, call};
        std::string iface;
        std::string prop;
        if (!cur.take_string(iface) || !cur.take_string(prop)) {
            return error_reply(call, err_invalid_args, "expect ss");
        }
        const AtspiPropValue v = model.prop_get(id, iface, prop);
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        if (!put_variant(L, top, v)) {
            L.message_unref(r);
            return error_reply(call, err_invalid_args, "no such property");
        }
        return reply_end(r);
    }

    auto prop_set_call(Msg call, std::uint64_t id) -> bool {
        Cur cur{L, call};
        std::string iface;
        std::string prop;
        if (!cur.take_string(iface) || !cur.take_string(prop)) {
            return error_reply(call, err_invalid_args, "expect ss");
        }
        AtspiPropValue v;
        Cur var{};
        if (!cur.into(ty_variant, var)) {
            return error_reply(call, err_invalid_args, "expect v");
        }
        std::int32_t i32 = 0;
        std::uint32_t u32 = 0;
        double dbl = 0.0;
        bool boolean = false;
        std::string str;
        if (var.take_int(i32)) {
            v.kind = AtspiPropValue::Kind::I32;
            v.i32 = i32;
        } else if (var.take_uint(u32)) {
            v.kind = AtspiPropValue::Kind::U32;
            v.u32 = u32;
        } else if (var.take_double(dbl)) {
            v.kind = AtspiPropValue::Kind::Dbl;
            v.dbl = dbl;
        } else if (var.take_bool(boolean)) {
            v.kind = AtspiPropValue::Kind::Bool;
            v.boolean = boolean;
        } else if (var.take_string(str)) {
            v.kind = AtspiPropValue::Kind::Str;
            v.str = str;
        } else {
            return error_reply(call, err_invalid_args, "unsupported variant");
        }
        if (!model.prop_set(id, iface, prop, v)) {
            return error_reply(call, err_invalid_args, "property is read-only");
        }
        dirty = true;
        return reply_void(call);
    }

    [[nodiscard]] auto prop_get_all(Msg call, std::uint64_t id) -> bool {
        Cur cur{L, call};
        std::string iface;
        if (!cur.take_string(iface)) {
            return error_reply(call, err_invalid_args, "expect s");
        }
        static const std::vector<std::string> accessible_props{
            "version", "Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId", "HelpText"};
        static const std::vector<std::string> application_props{
            "ToolkitName", "Version", "ToolkitVersion", "AtspiVersion", "InterfaceVersion", "Id"};
        static const std::vector<std::string> text_props{"version", "CharacterCount", "CaretOffset"};
        static const std::vector<std::string> value_props{
            "version", "MinimumValue", "MaximumValue", "MinimumIncrement", "CurrentValue", "Text"};
        const std::vector<std::string> *props = nullptr;
        if (iface == atspi::k_iface_accessible) {
            props = &accessible_props;
        } else if (iface == atspi::k_iface_application) {
            props = &application_props;
        } else if (iface == atspi::k_iface_text) {
            props = &text_props;
        } else if (iface == atspi::k_iface_value) {
            props = &value_props;
        }
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        Iter arr{};
        if (L.iter_open_container(&top, ty_array, "{sv}", &arr) != 0) {
            if (props != nullptr) {
                for (const std::string &p : *props) {
                    const AtspiPropValue v = model.prop_get(id, iface, p);
                    if (v.kind == AtspiPropValue::Kind::None) {
                        continue;
                    }
                    Iter ent{};
                    if (L.iter_open_container(&arr, ty_dict, "sv", &ent) != 0) {
                        continue;
                    }
                    put_string(L, ent, ty_string, p);
                    put_variant(L, ent, v);
                    L.iter_close_container(&arr, &ent);
                }
            }
            L.iter_close_container(&top, &arr);
        }
        return reply_end(r);
    }

    // ---- Cache ----

    [[nodiscard]] auto dispatch_cache(Msg m, const std::string &member) -> bool {
        const std::string iface{L.message_get_interface(m) != nullptr ? L.message_get_interface(m) : ""};
        if (iface == iface_props) {
            if (member == "Get") {
                // Cache 接口唯一属性：version = u2（走通用 path：k_cache_path 不是对象 id ⇒
                // 用 App 根的 prop_get 不行，单独给常量）。
                Cur cur{L, m};
                std::string want_iface;
                std::string prop;
                if (!cur.take_string(want_iface) || !cur.take_string(prop)) {
                    return error_reply(m, err_invalid_args, "expect ss");
                }
                if (want_iface == atspi::k_iface_cache && prop == "version") {
                    Iter top{};
                    Msg r = reply_begin(m, top);
                    if (r == nullptr) {
                        return false;
                    }
                    AtspiPropValue v;
                    v.kind = AtspiPropValue::Kind::U32;
                    v.u32 = 2;
                    return put_variant(L, top, v) && reply_end(r);
                }
                return error_reply(m, err_invalid_args, "no such cache property");
            }
            if (member == "GetAll") {
                Iter top{};
                Msg r = reply_begin(m, top);
                if (r == nullptr) {
                    return false;
                }
                Iter arr{};
                if (L.iter_open_container(&top, ty_array, "{sv}", &arr) != 0) {
                    Iter ent{};
                    if (L.iter_open_container(&arr, ty_dict, "sv", &ent) != 0) {
                        return reply_end(r);
                    }
                    put_string(L, ent, ty_string, "version");
                    AtspiPropValue v;
                    v.kind = AtspiPropValue::Kind::U32;
                    v.u32 = 2;
                    put_variant(L, ent, v);
                    L.iter_close_container(&arr, &ent);
                    L.iter_close_container(&top, &arr);
                }
                return reply_end(r);
            }
            return error_reply(m, err_unknown_method, member.c_str());
        }
        if (member == "GetItems") {
            return cache_get_items(m);
        }
        if (member == "Introspect") {
            return reply_introspect_xml(m, std::string{atspi::k_iface_cache} + "\n");
        }
        return error_reply(m, err_unknown_method, member.c_str());
    }

    /// @brief Cache.GetItems：全量行（签名 `a((so)(so)(so)iiassusau)`）。
    [[nodiscard]] auto cache_get_items(Msg call) -> bool {
        Iter top{};
        Msg r = reply_begin(call, top);
        if (r == nullptr) {
            return false;
        }
        Iter rows{};
        if (L.iter_open_container(&top, ty_array, "(so)(so)(so)iiassusau", &rows) != 0) {
            for (const AtspiCacheRow &row : model.cache_rows()) {
                Iter st{};
                if (L.iter_open_container(&rows, ty_struct, "(so)(so)(so)iiassusau", &st) == 0) {
                    continue;
                }
                put_so(L, st, row.self);
                put_so(L, st, row.app);
                put_so(L, st, row.parent);
                put_int(L, st, row.index_in_parent);
                put_int(L, st, row.child_count);
                Iter ifaces{};
                if (L.iter_open_container(&st, ty_array, "s", &ifaces) != 0) {
                    for (const std::string &s : row.interfaces) {
                        put_string(L, ifaces, ty_string, s);
                    }
                    L.iter_close_container(&st, &ifaces);
                }
                put_string(L, st, ty_string, row.name);
                put_uint(L, st, row.role);
                put_string(L, st, ty_string, row.description);
                Iter states{};
                if (L.iter_open_container(&st, ty_array, "u", &states) != 0) {
                    for (const std::uint32_t s : row.states) {
                        put_uint(L, states, s);
                    }
                    L.iter_close_container(&st, &states);
                }
                L.iter_close_container(&rows, &st);
            }
            L.iter_close_container(&top, &rows);
        }
        return reply_end(r);
    }

    // ---- Introspect ----

    [[nodiscard]] auto reply_introspect(Msg call, std::uint64_t id) -> bool {
        std::string xml = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
                          "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
        for (const std::string &i : model.interfaces(id)) {
            xml += "  <interface name=\"" + i + "\"/>\n";
        }
        xml += "  <interface name=\"" + std::string{iface_props} + "\"/>\n";
        xml += "  <interface name=\"" + std::string{iface_introspect} + "\"/>\n</node>";
        return reply_s(call, xml);
    }

    [[nodiscard]] auto reply_introspect_xml(Msg call, const std::string &extra) -> bool {
        std::string xml = "<node>\n  <interface name=\"" + std::string{atspi::k_iface_cache} +
                          "\"><method name=\"GetItems\"><arg type=\"a((so)(so)(so)iiassusau)\" "
                          "direction=\"out\"/></method></interface>\n  <interface name=\"" +
                          std::string{iface_introspect} + "\"/>\n</node>";
        (void)extra;
        return reply_s(call, xml);
    }

    // ---- 快照同步 ----

    auto sync_point() -> void {
        if (!dirty) {
            return;
        }
        dirty = false;
        if (tearing_down || root == nullptr) {
            return;  // 拆除期只读旧快照（UIA #8 同款门闩）
        }
        // 单参重载：根几何按 root.size() 实时取，尺寸变化无需宿主重新 set_root。
        snap = a11y::build_tree_snapshot(*root);
        model.sync(snap);
    }
};

// ============================================================================
// AtspiBridge 门面
// ============================================================================

auto AtspiBridge::create(AtspiEnv env) -> std::unique_ptr<AtspiBridge> {
    if (const char *off = std::getenv("NO_AT_BRIDGE");
        off != nullptr && off[0] != '\0' && std::strcmp(off, "0") != 0) {
        return nullptr;  // GNOME 惯例显式免提
    }
    const auto &L = LibDbus::instance();
    if (!L.loaded) {
        Diagnostics::warn("AtspiBridge: libdbus-1 unavailable, AT-SPI2 bridge disabled",
                          "aurora.atspi", {});
        return nullptr;
    }
    if (env.base_path.empty()) {
        env.base_path = atspi::k_registry_root_path;  // 规范 App 根路径（连接唯一 ⇒ 不撞号）
    }
    auto d = std::make_unique<AtspiBridge::Impl>(std::move(env));
    if (!d->connect_and_embed()) {
        return nullptr;  // 总线/注册表不可达：静默降级（无会话总线是 Linux 常态）
    }
    auto bridge = std::unique_ptr<AtspiBridge>(new AtspiBridge(std::move(d)));
    bridge->activate();
    return bridge;
}

AtspiBridge::AtspiBridge(std::unique_ptr<Impl> d) : d_(std::move(d)) {}

AtspiBridge::~AtspiBridge() {
    deactivate();
}

auto AtspiBridge::activate() -> void {
    if (d_->active) {
        return;
    }
    d_->active = true;
    if (!d_->provider_registered) {
        a11y::register_provider(*this);
        d_->provider_registered = true;
    }
}

auto AtspiBridge::deactivate() -> void {
    if (!d_->active) {
        return;
    }
    if (d_->client_seen) {
        current_accessibility_settings().screen_reader_active = false;
        d_->client_seen = false;
    }
    if (d_->provider_registered) {
        a11y::unregister_provider(*this);
        d_->provider_registered = false;
    }
    d_->tearing_down = true;
    d_->close_conn();
    d_->active = false;
}

auto AtspiBridge::sync_if_dirty() -> void {
    d_->sync_point();
}

auto AtspiBridge::mark_dirty() -> void {
    d_->dirty = true;
}

[[nodiscard]] auto AtspiBridge::is_active() const -> bool {
    return d_->active;
}

[[nodiscard]] auto AtspiBridge::name() const -> std::string {
    return "atspi2-dbus";
}

auto AtspiBridge::set_root(Widget *root) -> void {
    if (root == nullptr) {
        return;
    }
    if (root != d_->root) {
        d_->tearing_down = false;  // 换根重建：解除拆除门闩（UIA reset_teardown 同理）
        d_->root = root;
        d_->dirty = true;
    }
}

auto AtspiBridge::on_announcement(const std::string &, const Widget *) -> void {
    // 动态播报（G4 平台直译）随事件信号增量落地（申报见头注）。
}

auto AtspiBridge::on_widget_destroying(const Widget *w) -> void {
    if (w != nullptr && w == d_->root) {
        d_->root = nullptr;
        d_->tearing_down = true;  // 悬垂根门闩：pump 期查询只读旧快照
    }
}

auto AtspiBridge::poll_watches() const -> std::vector<WatchFd> {
    return d_->poll_watches();
}

auto AtspiBridge::pump() -> void {
    d_->pump();
}

auto AtspiBridge::set_window_origin(std::int32_t x, std::int32_t y) -> void {
    d_->model.env_mut().window_origin_x = x;
    d_->model.env_mut().window_origin_y = y;
}

auto AtspiBridge::set_window_title(std::string title) -> void {
    d_->model.env_mut().window_title = std::move(title);
    d_->dirty = true;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && (AURORA_BACKEND_X11 || AURORA_BACKEND_WAYLAND)
