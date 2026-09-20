#pragma once

// AT-SPI2 平台桥的**平台中立折算层**：语义树快照 → AT-SPI2 对象模型（角色/状态/接口/
// 对象路径/方法应答值）。纯 C++、零 D-Bus、零平台头 —— 与 `ime_composition` /
// `title_bar_painter` 同列「先纯后桥」纪律：全部折算逻辑无头可单测（Windows CI 亦覆盖），
// D-Bus 编解码只剩机械粘合（`atspi_bridge.cpp`）。
//
// 协议事实来源（2026-09-20 逐字核对上游 at-spi2-core main @ 2.60 线，WSL 网络直取）：
//  * 角色/状态**序号**：`atspi/atspi-constants.h` 两个枚举逐行数到（无显式赋值，唯二例外
//    `ATSPI_ROLE_PUSH_BUTTON = ATSPI_ROLE_BUTTON(43)` 别名与 `ATSPI_STATE_DEFAULT` 别名）。
//    数值只追加不改序（`*_LAST_DEFINED` + G_STATIC_ASSERT 守门），故 0..LAST 全段稳定。
//  * 方法面与签名：`xml/{Accessible,Application,Component,Text,Value,Action,Cache,Socket}.xml`。
//  * 注册握手：`registryd/registry.c::socket_embed` —— 应用对注册表调
//    `org.a11y.atspi.Socket.Embed((so)plug)`，注册表回 `(so)` 注册表根，并异步对本应用
//    根对象发 `org.freedesktop.DBus.Properties.Set("org.a11y.atspi.Application","Id",i)`。
//  * 文本偏移单位：`xml/Text.xml` GetText 文档明言「字符偏移、UTF-8 变宽 ⇒ 返回字节数与
//    偏移差不等」⇒ 偏移按**码点**计（与 UIA 的 UTF-16 口径不同，换算只在本桥边界）。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/core/a11y_diff.h"

namespace aurora::detail {

// ============================================================================
// 协议常量（单一来源；桥与折算层共用）
// ============================================================================

namespace atspi {

/// @brief 接口全名。`GetInterfaces` 返回值与 `Properties.Get` 的 iface 参数都用全名
///        （libatspi `_atspi_accessible_is_a` 以全名比对，注册表自身同样返回全名）。
inline constexpr const char *k_iface_accessible = "org.a11y.atspi.Accessible";
inline constexpr const char *k_iface_application = "org.a11y.atspi.Application";
inline constexpr const char *k_iface_component = "org.a11y.atspi.Component";
inline constexpr const char *k_iface_text = "org.a11y.atspi.Text";
inline constexpr const char *k_iface_value = "org.a11y.atspi.Value";
inline constexpr const char *k_iface_action = "org.a11y.atspi.Action";
inline constexpr const char *k_iface_cache = "org.a11y.atspi.Cache";
inline constexpr const char *k_iface_socket = "org.a11y.atspi.Socket";
inline constexpr const char *k_iface_registry = "org.a11y.atspi.Registry";

/// @brief 注册表总线名与其根对象路径（Embed 的目标；`registryd/paths.h`）。
inline constexpr const char *k_registry_bus = "org.a11y.atspi.Registry";
inline constexpr const char *k_registry_root_path = "/org/a11y/atspi/accessible/root";
/// @brief 空引用占位路径（Cache 行「无父」时按规范回填）。
inline constexpr const char *k_null_path = "/org/a11y/atspi/null";
/// @brief 本端 Cache 接口固定挂载路径（Cache.xml：应用把 Cache 暴露在 /org/a11y/atspi/cache）。
inline constexpr const char *k_cache_path = "/org/a11y/atspi/cache";
/// @brief 会话总线上的无障碍总线地址提供者（org.a11y.Bus.GetAddress）。
inline constexpr const char *k_a11y_bus_service = "org.a11y.Bus";
inline constexpr const char *k_a11y_bus_path = "/org/a11y/bus";
inline constexpr const char *k_a11y_bus_iface = "org.a11y.Bus";

/// @brief AtspiRole 序号（本桥用到的子集；其余以序号注释回查 upstream 枚举）。
enum Role : std::uint32_t {
    role_application = 75,
    role_frame = 23,
    role_panel = 39,
    role_push_button = 43,         ///< == BUTTON（上游为别名，值同为 43）
    role_toggle_button = 62,
    role_check_box = 7,
    role_radio_button = 44,
    role_switch = 130,
    role_slider = 51,
    role_progress_bar = 42,
    role_text = 61,
    role_entry = 79,
    role_password_text = 40,
    role_static = 116,
    role_label = 29,
    role_image = 27,
    role_list = 31,
    role_list_item = 32,
    role_heading = 83,
    role_dialog = 16,
    role_section = 85,
    role_notification = 101,
    role_unknown = 67,
};

/// @brief AtspiStateType 序号（本桥用到的子集）。
enum State : std::uint32_t {
    state_active = 1,
    state_checked = 4,
    state_defunct = 6,
    state_editable = 7,
    state_enabled = 8,
    state_expandable = 9,
    state_expanded = 10,
    state_focusable = 11,
    state_focused = 12,
    state_multi_line = 17,
    state_opaque = 19,
    state_pressed = 20,
    state_selectable = 22,
    state_selected = 23,
    state_sensitive = 24,
    state_showing = 25,
    state_single_line = 26,
    state_visible = 30,
    state_selectable_text = 38,
    state_checkable = 41,
    state_read_only = 43,
};

/// @brief AtspiCoordType（Component/Text 几何坐标系参数）。
enum CoordType : std::uint32_t {
    coord_screen = 0,
    coord_window = 1,
    coord_parent = 2,
};

}  // namespace atspi

/// @brief 对象引用 (bus name, object path) 的中立值形态（D-Bus 签名 `(so)`）。
struct AtspiRef {
    std::string bus;   ///< 唯一总线名（":1.23"）；空 = 无
    std::string path;  ///< 对象路径；`k_null_path` = 空引用

    [[nodiscard]] static auto null() -> AtspiRef { return {"", std::string{atspi::k_null_path}}; }
    [[nodiscard]] auto is_null() const -> bool { return bus.empty(); }
};

/// @brief 整数矩形（物理像素；Component 应答的 `(iiii)`）。
struct AtspiRectI {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

/// @brief Properties.Get 的中立取值（桥按签名落线）。
struct AtspiPropValue {
    enum class Kind : std::uint8_t { None, Str, I32, U32, Bool, Dbl, Ref };
    Kind kind = Kind::None;
    std::string str;
    std::int32_t i32 = 0;
    std::uint32_t u32 = 0;
    bool boolean = false;
    double dbl = 0.0;
    AtspiRef ref;
};

/// @brief Action 接口的一行（GetActions 的 `(sss)` 三元组 + 本端动作位）。
struct AtspiActionRow {
    std::string name;        ///< 稳定英文名（"press" / "toggle" ...）
    std::string localized;   ///< 本地化显示名（当前同名；i18n 接入后替换）
    std::string keybinding;  ///< 键绑定（Aurora 控件语义未导出 ⇒ 恒空）
    AccessibilityAction action = AccessibilityAction::None;
};

/// @brief Cache.GetItems 的一行（签名 `((so)(so)(so)iiassusau)` 的中立值形态）。
struct AtspiCacheRow {
    AtspiRef self;
    AtspiRef app;
    AtspiRef parent;             ///< 无父 ⇒ null
    std::int32_t index_in_parent = -1;
    std::int32_t child_count = 0;
    std::vector<std::string> interfaces;  ///< 接口全名表
    std::string name;
    std::uint32_t role = 0;
    std::string description;
    std::vector<std::uint32_t> states;
};

/// @brief 折算层的语义树节点保留 id（合成 App/Frame 两个非控件节点）。
inline constexpr std::uint64_t k_atspi_app_id = ~0ULL;
inline constexpr std::uint64_t k_atspi_frame_id = ~0ULL - 1ULL;

/// @brief 宿主注入的环境量（几何换算、动作执行、标题等**平台事实**）。
///
/// 折算层保持纯函数性的关键：所有对外应答都可由 (快照 + 本表 + 入参) 决定。
/// 函数成员在 UI 线程同步调用（与桥同线程模型）。
struct AtspiEnv {
    std::string base_path;  ///< 本窗口的应用根对象路径（每桥唯一，如 "/org/a11y/atspi/accessible/root"）
    std::string app_name;   ///< 应用名（AT-SPI 桌面树里显示的 app Name）
    std::string toolkit_name = "Aurora";
    std::string toolkit_version;            ///< Aurora 库版本（构造期填）
    std::string self_bus;                   ///< 本连接唯一总线名（桥连接后回填）
    std::string window_title;               ///< FRAME 节点 Name
    AtspiRef registry_root;                 ///< Embed 回包（父的父链终点）
    std::function<AtspiRectI(const Rect &)> to_screen_px;  ///< 窗口本地 DIP → 物理屏幕 px
    std::function<AtspiRectI(const Rect &)> to_window_px;  ///< DIP → 窗口本地物理 px（WINDOW 系）
    std::int32_t window_origin_x = 0;  ///< 客户区原点的屏幕物理 px（点坐标反推 WINDOW/PARENT 系用）
    std::int32_t window_origin_y = 0;
    std::function<bool(Widget *, const AccessibilityActionRequest &)> perform;  ///< 动作执行（解引用活控件）
};

/// @brief AT-SPI2 折算模型：快照 + 路径分配 + 方法面纯应答。
///
/// 树裁剪口径：仅剔除 `!is_control && !is_content`（装饰节点，G23「完全忽略」档）并把其
/// 子节点上挂到最近的存活祖先 —— 与 UIA 桥「控制视图可见性」同源但按其语义放宽：AT-SPI2
/// 没有 control/content 双视图概念，纯布局容器（is_control）照常入树（atk 应用同此形态）。
/// @note Thread: main-thread only（与桥/快照同线程）
class AtspiModel {
  public:
    explicit AtspiModel(AtspiEnv env) : env_(std::move(env)) {}

    /// @brief 快照更新：重建存活表 + 增量分配对象路径（同 id 保旧路径 ⇒ 引用跨帧稳定）。
    auto sync(const a11y::TreeSnapshot &snap) -> void;

    // ---- 寻址 ----
    [[nodiscard]] auto id_of_path(const std::string &path) const -> std::optional<std::uint64_t>;
    [[nodiscard]] auto path_of_id(std::uint64_t id) const -> std::string;  ///< 未命中 ⇒ null 路径
    /// @brief 快照里的活节点（App/Frame 合成节点返回 nullptr）。
    [[nodiscard]] auto node(std::uint64_t id) const -> const a11y::NodeSnapshot *;
    /// @brief 对象存活（false ⇒ 已移除，桥按规范回 `UnknownObject`）。
    [[nodiscard]] auto exists(std::uint64_t id) const -> bool;

    // ---- org.a11y.atspi.Accessible ----
    [[nodiscard]] auto name(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto description(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto help_text(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto accessible_id(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto role(std::uint64_t id) const -> std::uint32_t;
    [[nodiscard]] auto role_name(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto states(std::uint64_t id) const -> std::vector<std::uint32_t>;
    [[nodiscard]] auto interfaces(std::uint64_t id) const -> std::vector<std::string>;
    [[nodiscard]] auto child_count(std::uint64_t id) const -> std::int32_t;
    [[nodiscard]] auto children(std::uint64_t id) const -> std::vector<std::uint64_t>;
    [[nodiscard]] auto child_at(std::uint64_t id, std::int32_t index) const -> std::optional<std::uint64_t>;
    [[nodiscard]] auto parent(std::uint64_t id) const -> std::uint64_t;  ///< 父节点 id；App 的父 = 0（无）
    [[nodiscard]] auto index_in_parent(std::uint64_t id) const -> std::int32_t;
    [[nodiscard]] auto application() const -> AtspiRef;
    [[nodiscard]] auto parent_ref(std::uint64_t id) const -> AtspiRef;
    [[nodiscard]] auto ref_of(std::uint64_t id) const -> AtspiRef;

    // ---- org.a11y.atspi.Component ----
    [[nodiscard]] auto extents(std::uint64_t id, std::uint32_t coord) const -> AtspiRectI;
    [[nodiscard]] auto contains(std::uint64_t id, std::int32_t x, std::int32_t y, std::uint32_t coord) const -> bool;
    /// @brief 屏幕点命中（本子树内、先序**最深**且面积最小者）；未命中 = null 引用。
    [[nodiscard]] auto accessible_at_point(std::uint64_t id, std::int32_t x, std::int32_t y,
                                           std::uint32_t coord) const -> AtspiRef;

    // ---- org.a11y.atspi.Text（码点偏移）----
    [[nodiscard]] auto has_text(std::uint64_t id) const -> bool;
    [[nodiscard]] auto text_char_count(std::uint64_t id) const -> std::int32_t;
    /// @brief `[start, end)` 码点子串；`end < 0` = 到文末；越界夹紧（不报错）。
    [[nodiscard]] auto text_slice(std::uint64_t id, std::int32_t start, std::int32_t end) const -> std::string;
    [[nodiscard]] auto text_caret(std::uint64_t id) const -> std::int32_t;
    [[nodiscard]] auto text_char_extents(std::uint64_t id, std::int32_t offset, std::uint32_t coord) const
        -> AtspiRectI;

    // ---- org.a11y.atspi.Value ----
    [[nodiscard]] auto has_value(std::uint64_t id) const -> bool;
    [[nodiscard]] auto value_min(std::uint64_t id) const -> double;
    [[nodiscard]] auto value_max(std::uint64_t id) const -> double;
    [[nodiscard]] auto value_increment(std::uint64_t id) const -> double;
    [[nodiscard]] auto value_current(std::uint64_t id) const -> double;
    [[nodiscard]] auto value_set(std::uint64_t id, double v) const -> bool;

    // ---- org.a11y.atspi.Action ----
    [[nodiscard]] auto actions(std::uint64_t id) const -> std::vector<AtspiActionRow>;
    [[nodiscard]] auto do_action(std::uint64_t id, std::int32_t index) const -> bool;

    // ---- org.freedesktop.DBus.Properties ----
    [[nodiscard]] auto prop_get(std::uint64_t id, const std::string &iface, const std::string &prop) const
        -> AtspiPropValue;
    /// @brief Properties.Set：只接受 `Application.Id`（注册表回填）与 `Value.CurrentValue`
    ///        （经动作通道转设值）；其余属性只读 ⇒ false = 桥回 `org.freedesktop.DBus.Error.InvalidArgs`。
    auto prop_set(std::uint64_t id, const std::string &iface, const std::string &prop, const AtspiPropValue &v) -> bool;
    [[nodiscard]] auto app_id() const -> std::int32_t { return app_id_; }

    // ---- org.a11y.atspi.Cache ----
    [[nodiscard]] auto cache_rows() const -> std::vector<AtspiCacheRow>;

    /// @brief 可变环境量：`self_bus` / `registry_root` / 窗口原点等是**建连之后**才回填的
    ///        事实，桥据其单源更新（模型是 env 的唯一持有者，避免副本漂移）。
    [[nodiscard]] auto env_mut() -> AtspiEnv & { return env_; }
    [[nodiscard]] auto env() const -> const AtspiEnv & { return env_; }

    /// @brief 本模型支撑某 (path, iface, member) 方法调用吗（桥的 UnknownMethod 判定同源）。
    ///
    /// 折算层与桥共用这张成员表：成员名单漂移只会在此一处发生。
    [[nodiscard]] static auto handles(const std::string &iface, const std::string &member) -> bool;

  private:
    struct LiveNode {
        std::uint64_t id = 0;
        std::uint64_t parent_id = 0;  ///< 已按裁剪上挂；Frame 的父 = App；App 的父 = 0（null/注册表根）
        std::string path;
    };

    [[nodiscard]] auto live(std::uint64_t id) const -> const LiveNode *;
    [[nodiscard]] auto text_of(std::uint64_t id) const -> std::string;
    [[nodiscard]] auto widget_of(std::uint64_t id) const -> Widget *;

    AtspiEnv env_;
    const a11y::TreeSnapshot *snap_ = nullptr;
    std::vector<LiveNode> order_;  ///< App、Frame、…先序存活表
    std::unordered_map<std::uint64_t, std::size_t> by_id_;
    std::unordered_map<std::string, std::uint64_t> by_path_;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> kids_;
    /// @brief id → 对象路径的跨帧缓存：`sync` 开头 swap 出来作旧表、结尾按存活集重建，
    ///        保证「同 id ⇒ 同路径」不变式（消失 id 的路径随之丢弃，编号单调不复用）。
    std::unordered_map<std::uint64_t, std::string> by_path_id_cache_;
    std::uint64_t path_counter_ = 0;  ///< 已分配路径计数（单调，不复用 ⇒ 引用不串号）
    std::int32_t app_id_ = -1;        ///< 注册表 Embed 后回填
};

// ============================================================================
// 公开纯函数（单测直接钉表的入口；模型内部同样调用它们）
// ============================================================================

/// @brief Aurora 语义角色 → AtspiRole 序号（含 password/multiline 细化）。
[[nodiscard]] auto atspi_role_of(const AccessibilityNode &n) -> std::uint32_t;
/// @brief AtspiRole 序号 → 英文名（与 libatspi `_atspi_role_get_name` 表同源）。
[[nodiscard]] auto atspi_role_name(std::uint32_t role) -> std::string;
/// @brief 状态位集折算（App/Frame 由模型直接给常量表）。
[[nodiscard]] auto atspi_states_of(const AccessibilityNode &n, const std::vector<std::string> &ifaces)
    -> std::vector<std::uint32_t>;
/// @brief 接口可用性折算（决定 GetInterfaces / Cache 行 / 方法路由三处，单一来源）。
[[nodiscard]] auto atspi_interfaces_of(const AccessibilityNode &n) -> std::vector<std::string>;
/// @brief 动作位掩码 → Action 行（确定序：press, toggle, select, scroll up/down/left/right,
///        scroll to visible；同名去重，空 ⇒ 不暴露 Action 接口）。
[[nodiscard]] auto atspi_actions_of(const AccessibilityNode &n) -> std::vector<AtspiActionRow>;

/// @brief UTF-8 码点数（Text 接口偏移单位）。
[[nodiscard]] auto atspi_cp_count(std::string_view utf8) -> std::size_t;
/// @brief 取 `[start, end)` 码点子串（越界夹紧；`end<0` = 文末）。
[[nodiscard]] auto atspi_cp_slice(std::string_view utf8, std::int64_t start, std::int64_t end) -> std::string;

}  // namespace aurora::detail
