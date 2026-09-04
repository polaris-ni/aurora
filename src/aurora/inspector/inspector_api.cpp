// Aurora Inspector API — 统一门面实现。
// 查询方法委托 inspect.h 自由函数，零新运行时开销。

#include "aurora/inspector/inspector_api.h"

#include <mutex>
#include <unordered_map>

#include "aurora/app/validate.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/focus.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/inspect.h"
#include "aurora/widget/serialization.h"

namespace aurora {

// ---------------------------------------------------------------------------
// 树查询（委托 inspect.h）
// ---------------------------------------------------------------------------

auto Inspector::tree_text(const Node &root) -> std::string { return dump_tree(root); }

auto Inspector::tree_rich(const Node &root) -> std::string { return dump_tree_rich(root); }

auto Inspector::tree_json(const Node &root) -> Json { return dump_tree_json(root); }

auto Inspector::tree_json_full(const Node &root) -> Json { return dump_tree_json_full(root); }

auto Inspector::widget_info(const Widget &w) -> Json { return get_widget_props(w); }

auto Inspector::query(std::string_view type, const Node &root) -> std::vector<Node> {
    return aurora::query(type, root);
}

auto Inspector::get_state(std::string_view path, const Node &root) -> Json { return aurora::get_state(path, root); }

auto Inspector::find_node(const Node &root, std::string_view path) -> Node { return find_node_by_path(root, path); }

// ---------------------------------------------------------------------------
// 属性读写
// ---------------------------------------------------------------------------

auto Inspector::get_prop(const Widget &w) -> Json { return get_widget_props(w); }

auto Inspector::get_prop_value(const Widget &w, std::string_view key) -> Json {
    Json props = Json::object();
    w.serialize_props(props);
    if (props.contains(std::string(key))) {
        return props[std::string(key)];  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
    }
    return Json{};
}

auto Inspector::set_prop(Widget &w, std::string_view key, const Json &val) -> Result<void> {
    set_widget_prop(w, key, val);
    return Result<void>{};
}

auto Inspector::apply_patch(Node &root, const Json &patch) -> Result<void> {
    if (!patch.is_array()) {
        return make_error(ErrorCode::GeneralNotSupported, "patch must be a JSON array of {path, value}");
    }
    for (const auto &op : patch) {
        if (!op.is_object() || !op.contains("path") || !op.contains("value")) {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const std::string path_str = op["path"].get<std::string>();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json &value = op["value"];
        // path 格式: "/widget_path/prop_name" — 最后一段为属性名
        const auto last_slash = path_str.rfind('/');
        if (last_slash == std::string::npos) {
            // 根节点属性
            set_widget_prop(root.widget(), path_str, value);
        } else {
            std::string widget_path = path_str.substr(0, last_slash);
            std::string prop_name = path_str.substr(last_slash + 1);
            // 跳过前导 '/'
            if (!widget_path.empty() && widget_path.at(0) == '/') {
                widget_path = widget_path.substr(1);
            }
            Node target = find_node_by_path(root, widget_path);
            if (target) {
                set_widget_prop(target.widget(), prop_name, value);
            }
        }
    }
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// 交互模拟（需要事件系统支持，当前返回 GeneralNotSupported）
// ---------------------------------------------------------------------------

auto Inspector::simulate_click(Widget &w) -> Result<void> {
    // 在 Widget 自身中心触发一次 press + release，走完整的命中测试 + 冒泡派发路径。
    const Point center{.x = w.size().width * 0.5F, .y = w.size().height * 0.5F};
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = center;
    EventDispatcher::dispatch(w, press);
    MouseEvent release = press;
    release.action = MouseAction::Release;
    EventDispatcher::dispatch(w, release);
    return Result<void>{};
}

auto Inspector::simulate_scroll(Widget &w, float dx, float dy) -> Result<void> {
    ScrollEvent e;
    e.delta_x = dx;
    e.delta_y = dy;
    e.position = Point{.x = w.size().width * 0.5F, .y = w.size().height * 0.5F};
    EventDispatcher::dispatch(w, e);
    return Result<void>{};
}

auto Inspector::simulate_text_input(Widget &w, std::string_view text) -> Result<void> {
    if (text.empty()) {
        return Result<void>{};
    }
    // 文本输入派发到当前焦点 widget，这里以目标 w 为焦点根并直接置焦。
    FocusManager fm;
    fm.set_root(&w);
    fm.set_focus(&w);
    set_current_focus_manager(&fm);
    TextInputEvent e;
    e.text = std::string(text);
    EventDispatcher::dispatch(w, e, fm);
    set_current_focus_manager(nullptr);
    return Result<void>{};
}

// ---------------------------------------------------------------------------
// 组件发现
// ---------------------------------------------------------------------------

auto Inspector::components() -> std::vector<Json> { return list_all_schemas(); }

auto Inspector::component_schema(std::string_view name) -> Json {
    return aurora::describe_component(std::string(name));
}

// ---------------------------------------------------------------------------
// 代码生成
// ---------------------------------------------------------------------------

auto Inspector::to_code(const Node &root) -> std::string { return serialization::to_code(root.widget()); }

// ---------------------------------------------------------------------------
// 验证
// ---------------------------------------------------------------------------

auto Inspector::validate(const Node &root) -> std::vector<Diagnostic> {
    auto result = aurora::validate(root);
    std::vector<Diagnostic> diags;
    if (!result) {
        Diagnostic d;
        d.severity = ErrorSeverity::Error;
        d.category = ErrorCategory::General;
        d.message = result.error().message;
        d.where = result.error().where;
        d.code = result.error().code;
        diags.push_back(std::move(d));
    }
    return diags;
}

// ---------------------------------------------------------------------------
// 变化订阅
// ---------------------------------------------------------------------------

namespace {
auto subscribers_mutex() -> std::mutex & {
    static std::mutex mtx;
    return mtx;
}

auto subscribers() -> auto & {
    static std::unordered_map<std::size_t, Inspector::ChangeCallback> subs;
    return subs;
}

auto next_sub_id() -> std::size_t & {
    static std::size_t id = 0;
    return id;
}
}  // namespace

auto Inspector::subscribe_changes(ChangeCallback cb) -> std::size_t {
    std::scoped_lock lock(subscribers_mutex());
    const std::size_t id = ++next_sub_id();
    subscribers()[id] = std::move(cb);
    return id;
}

auto Inspector::unsubscribe(std::size_t id) -> void {
    std::scoped_lock lock(subscribers_mutex());
    subscribers().erase(id);
}

void Inspector::notify_changes(const Json &patch) {
    std::scoped_lock lock(subscribers_mutex());
    for (auto &cb : subscribers() | std::views::values) {
        if (cb) {
            cb(patch);
        }
    }
}

}  // namespace aurora