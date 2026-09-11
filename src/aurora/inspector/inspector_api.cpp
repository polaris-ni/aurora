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
// 交互模拟：合成事件经 EventDispatcher 派发（派发根 = 目标控件自身）
// ---------------------------------------------------------------------------
//
// 语义为「目标式」：以目标控件为派发根与坐标原点、指针取该控件中心。因此不依赖
// 控件在整棵树中的绝对位置（无需先绘制即可复现），代价是事件不沿命中链冒泡到
// 目标控件的祖先——要断言祖先（如外层的 Clickable）的响应须以该祖先为目标。
//
// 目标不可命中（未布局出可命中区域，或整棵子树都不参与命中）时**在派发前**返回错误，
// 因此失败的模拟不改变任何状态，调用方（AI Agent / 测试）也能据此区分「已派发」与
// 「无事发生」。

namespace {

/// @brief 交互模拟的焦点上下文：优先复用派发期的焦点管理器，无则就地构造一个。
///
/// 合成指针事件必须携带焦点管理器：`Widget::request_focus()` 读取
/// `current_focus_manager()`，为空时静默 no-op，于是点击输入框不获焦、后续文本输入
/// 也没有接收者。已有的派发上下文（`current_focus_manager()` 非空）优先沿用，
/// 避免与真实焦点状态脱节；无则用一个以目标控件为根的临时实例兜底。
struct SimFocusContext {
    FocusManager local;

    [[nodiscard]] auto resolve(Widget &target) -> FocusManager * {
        if (FocusManager *active = current_focus_manager()) {
            return active;
        }
        local.set_root(&target);
        return &local;
    }
};

/// @brief 目标控件中心点（控件局部坐标系；与「以控件为派发根」一致）。
[[nodiscard]] auto center_of(const Widget &w) -> Point {
    return Point{.x = w.size().width * 0.5F, .y = w.size().height * 0.5F};
}

}  // namespace

auto Inspector::simulate_click(Widget &w) -> Result<void> {
    SimFocusContext focus;
    FocusManager *fm = focus.resolve(w);
    const Point center = center_of(w);

    // 先按要求做一次与派发器同口径的命中测试：未命中则直接返回错误，不进入派发。
    // 否则 `dispatch_mouse` 会按「点击空白」语义清除当前焦点——一次失败的模拟点击
    // 不应改变任何状态。（命中测试是只读的，此处多跑一次不影响结果。）
    const Rect root_rect{.origin = Point{}, .size = w.size()};
    if (w.hit_test_chain(center, root_rect, BuildContext{}).empty()) {
        return make_error(ErrorCode::GeneralNotSupported,
                          "simulate_click: target widget has no hit-testable area at its center");
    }

    // 一次完整点击 = Press + Release，走完整的命中测试 + 冒泡派发路径。
    // 焦点经派发器在 Press 时交给命中链上最近的可获焦控件。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = center;
    EventDispatcher::dispatch(w, press, fm);

    MouseEvent release = press;
    release.action = MouseAction::Release;
    EventDispatcher::dispatch(w, release, fm);
    return Result<void>{};
}

auto Inspector::simulate_scroll(Widget &w, float dx, float dy) -> Result<void> {
    ScrollEvent e;
    e.delta_x = dx;
    e.delta_y = dy;
    e.position = center_of(w);
    // 同样先做与派发器同口径的命中测试（滚轮不改变焦点，故不经焦点管理器）。
    if (EventDispatcher::hit_test(w, e.position) == nullptr) {
        return make_error(ErrorCode::GeneralNotSupported,
                          "simulate_scroll: target widget has no hit-testable area at its center");
    }
    EventDispatcher::dispatch(w, e);
    return Result<void>{};
}

auto Inspector::simulate_text_input(Widget &w, std::string_view text) -> Result<void> {
    if (text.empty()) {
        return Result<void>{};  // 空片段无副作用，直接视为完成
    }
    SimFocusContext focus;
    FocusManager *fm = focus.resolve(w);
    // 目标语义：文本须落到指定控件，故先把它置为焦点——`TextInput::on_text_input`
    // 以 `is_focused()` 为前提，未获焦时直接丢弃输入。
    fm->set_focus(&w);

    TextInputEvent e;
    e.text = std::string(text);
    if (!EventDispatcher::dispatch(w, e, *fm)) {
        return make_error(ErrorCode::GeneralNotSupported,
                          "simulate_text_input: target widget did not accept the text input");
    }
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