#include "aurora/widget/serialization.h"

#include <cctype>
#include <tuple>

#include "aurora/core/diagnostics.h"
#include "aurora/media/video_controls.h"
#include "aurora/media/video_player.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/divider.h"
#include "aurora/widget/grid.h"
#include "aurora/widget/placeholder.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/rich_text.h"
#include "aurora/widget/rich_text_edit.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/switch.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/timer.h"
#include "aurora/widget/yaml.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/navigation/hero.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/widget/chip.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/data_widgets.h"
#include "aurora/widget/drawer.h"
#include "aurora/widget/dropdown.h"
#include "aurora/widget/expansion_panel.h"
#include "aurora/widget/form.h"
#include "aurora/widget/image_widget.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/widget/menu_bar.h"
#include "aurora/widget/pickers.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/radio_spin.h"
#include "aurora/widget/segmented_control.h"
#include "aurora/widget/show.h"
#include "aurora/widget/spacer.h"
#include "aurora/widget/splitter.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/stepper.h"
#include "aurora/widget/tab_bar.h"
#include "aurora/widget/title_bar.h"
#include "aurora/widget/toast.h"
#include "aurora/widget/toolbar.h"

namespace aurora {
namespace serialization {

auto WidgetRegistry::make(const std::string &type, const Json &props) const -> Result<std::shared_ptr<Widget>> {
    auto it = m_factories.find(type);
    if (it == m_factories.end()) {
        return make_error(ErrorCode::WidgetUnknownType, "serialization: unregistered widget type '" + type +
                                                            "' (please first use "
                                                            "registerFactory to register)");
    }
    auto w = it->second(props);
    if (!w) {
        return w;
    }
    // 构建期约束校验：控件可在 validate_props() 中报告非法内部属性状态；
    // 失败以 degraded 上报（严格模式下由 Diagnostics 升级为硬失败，debug 下 AURORA_ASSERT 触发）。
    if (auto vr = w.value()->validate_props(); !vr) {
        Diagnostics::degraded("Property validation failed: " + vr.error().message, type, vr.error().code);
    }
    return w;
}

auto WidgetRegistry::list_types() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(m_factories.size());
    for (const auto &kv : m_factories | std::views::keys) {
        out.push_back(kv);
    }
    return out;
}

auto to_json(const Widget &w) -> Json {
    Json j = Json::object();
    j["type"] = w.type_name();

    Json props = Json::object();
    w.serialize_props(props);
    j["props"] = props;

    Json children = Json::array();
    w.for_each_child([&](const Widget &c) -> void { children.push_back(to_json(c)); });
    if (!children.empty()) {
        j["children"] = children;
    }
    return j;
}

namespace {

/// @brief 注册「默认构造 + 反序列化属性」的控件工厂（库控件最常见形态）。
/// 构造参数打包为 tuple 按值捕获，调用时经 std::apply 展开给 std::make_shared（C++20 兼容）。
template<class T, class... CtorArgs> auto reg_default(const char *name, CtorArgs... ctor_args) -> void {
    auto captured = std::make_tuple(std::move(ctor_args)...);
    WidgetRegistry::instance().register_factory(
        name, [captured = std::move(captured)](const Json &props) -> Result<std::shared_ptr<Widget>> {
            auto w = std::apply(
                []<class... Args>(Args &&...a) -> std::shared_ptr<T> { // NOLINT
                    return std::make_shared<T>(std::forward<Args>(a)...);
                },
                captured);
            w->deserialize_props(props);
            return std::static_pointer_cast<Widget>(w);
        });
}

/// @brief 注册「默认构造、无属性反序列化」的控件工厂（Provider 系、运行时态控件等）。
template<class T, class... CtorArgs> auto reg_no_props(const char *name, CtorArgs... ctor_args) -> void {
    auto captured = std::make_tuple(std::move(ctor_args)...);
    WidgetRegistry::instance().register_factory(
        name, [captured = std::move(captured)](const Json & /*props*/) -> Result<std::shared_ptr<Widget>> {
            auto w = std::apply(
                []<class... Args>(Args &&...a) -> std::shared_ptr<T> { // NOLINT
                    return std::make_shared<T>(std::forward<Args>(a)...);
                },
                captured);
            return std::static_pointer_cast<Widget>(w);
        });
}

/// @brief 注册「每次调用都需新建构造参数」的控件工厂。
///
/// `reg_default` 把构造参数在**注册时**求值一次并按值捕获，这对标量/空容器无碍，但若某个
/// 参数本身是 `shared_ptr` 持有的控件（如 `Show` 的占位子节点），所有反序列化出来的实例
/// 会共用同一个子控件对象。此重载改为每次调用工厂时执行 `make`，恢复「一实例一子树」。
template<class Make> auto reg_fresh(const char *name, Make make) -> void {
    WidgetRegistry::instance().register_factory(
        name, [make = std::move(make)](const Json &props) -> Result<std::shared_ptr<Widget>> {
            auto w = make();
            w->deserialize_props(props);
            return std::static_pointer_cast<Widget>(w);
        });
}

/// @brief 注册「已知类型但不可从静态 JSON 重建」的控件工厂，给出友好错误。
auto reg_error(const char *name, ErrorCode code, std::string msg) -> void {
    WidgetRegistry::instance().register_factory(
        name, [code, msg = std::move(msg)](const Json & /*props*/) -> Result<std::shared_ptr<Widget>> {
            return make_error(code, msg);
        });
}

} // namespace

auto register_core_widgets() -> void {
    // ---- 默认构造 + 属性反序列化（绝大多数库控件）----
    reg_default<Text>("Text");
    reg_default<Button>("Button");
    reg_default<Column>("Column");
    reg_default<Row>("Row");
    reg_default<ImageView>("Image", Image{});
    // 媒体子系统（v0.20.0）：VideoPlayer 默认含 VideoControls 叠层；后者控制器由父播放器注入，
    // 独立反序列化时传 nullptr（控件内部已做空指针保护）。
    reg_default<VideoPlayer>("VideoPlayer");
    reg_default<VideoControls>("VideoControls", nullptr);
    reg_default<Stack>("Stack", std::vector<Node>{});
    reg_default<Spacer>("Spacer");
    // 占位子节点须每次新建：若在注册时构造一次，所有 Show 实例会共用同一个 Spacer。
    reg_fresh("Show", []() -> std::shared_ptr<Show> {
        return std::make_shared<Show>(false, Node{ std::make_shared<Spacer>(false) });
    });
    // 降级视觉占位控件（需求 #18）：可安全从静态 JSON 重建，便于在错误/缺失处渲染占位盒。
    reg_default<Placeholder>("Placeholder");
    reg_default<RichText>("RichText");
    reg_default<Grid>("Grid");
    reg_default<Scroll>("Scroll");
    // 交互原语：属性完整可序列化，默认可构造后回填属性即可重建。
    reg_default<Checkbox>("Checkbox");
    reg_default<Switch>("Switch");
    reg_default<Slider>("Slider");
    reg_default<ProgressIndicator>("ProgressIndicator");
    reg_default<Divider>("Divider");
    reg_default<Hero>("Hero", "", Node{});
    reg_default<Popup>("Popup");
    reg_default<OverlayHost>("OverlayHost");
    reg_default<TabBar>("TabBar");
    reg_default<Splitter>("Splitter");
    reg_default<MenuBar>("MenuBar");
    reg_default<ToolBar>("ToolBar");
    reg_default<StatusBar>("StatusBar");
    reg_default<TitleBar>("TitleBar");
    reg_default<Form>("Form");
    reg_default<FormField>("FormField");
    reg_default<ToastHost>("ToastHost");
    reg_default<Dropdown>("Dropdown");
    reg_default<ExpansionPanel>("ExpansionPanel");
    reg_default<RadioGroup>("RadioGroup");
    reg_default<SpinBox>("SpinBox");
    reg_default<Drawer>("Drawer");
    reg_default<ProgressDialog>("ProgressDialog");
    reg_default<PageView>("PageView");
    reg_default<DatePicker>("DatePicker");
    reg_default<TimePicker>("TimePicker");
    reg_default<ColorPicker>("ColorPicker");
    reg_default<DataTable>("DataTable");
    reg_default<TreeView>("TreeView");
    reg_default<ListView>("ListView");
    reg_default<PerfOverlay>("PerfOverlay");
    reg_default<RichTextEdit>("RichTextEdit");
    reg_default<Chip>("Chip");
    reg_default<Badge>("Badge");
    reg_default<SegmentedControl>("SegmentedControl");
    reg_default<Stepper>("Stepper");

    // ---- 默认构造、无属性反序列化（Provider 系 / 运行时态控件）----
    // Provider 系：值（T）不参与序列化，构造占位后由 from_json 的 adopt_children 挂入真实子节点。
    reg_no_props<Provider<Theme>>("ThemeProvider", Theme{}, Node{});
    reg_no_props<Provider<Locale>>("LocaleProvider", Locale{}, Node{});
    reg_no_props<Provider<MediaQuery>>("MediaQueryProvider", MediaQuery{}, Node{});
    reg_no_props<TextInput>("TextInput");
    // Timer 持运行时回调（TickBuilder），与 Repeater/Canvas 同理不可从静态 JSON 重建；
    // 但注册为已知类型以便 API 描述（gen_api_tools）收录其自描述元数据。
    reg_no_props<Timer>("Timer", std::chrono::seconds(1), [](const SignalView<int> &) -> Node { return Node{}; });
    // LazyList / LazyRow 持运行时 ItemBuilder，注册供 API 描述收录。
    reg_no_props<LazyList>("LazyList", 0, nullptr);
    reg_no_props<LazyRow>("LazyRow", 0, nullptr);
    // BottomNavBar 持运行时图标绘制器与回调，注册供 API 描述收录。
    reg_no_props<BottomNavBar>("BottomNavBar");

    // ---- 已知类型但不可从静态 JSON 重建：给出友好错误（避免被当作未知类型静默失败）----
    // Canvas / Repeater 持运行时回调/State，无法从静态 JSON 重建：注册为已知类型。
    reg_error("Canvas", ErrorCode::GeneralNotSupported, "Canvas 的绘制回调不可序列化，无法从 JSON 重建");
    reg_error("Repeater", ErrorCode::GeneralNotSupported, "Repeater 的数据源为运行时 State，无法从 JSON 重建");
    // 兜底：其它未知 T 的 Provider 给出友好错误。
    reg_error("Provider", ErrorCode::GeneralNotSupported,
              "Provider<T> 的 T 未注册具名工厂，无法从 JSON 重建（请为具体类型注册如 ThemeProvider 的工厂）");
}

namespace {

/// @brief `from_json` 的递归实现，带深度上限。
///
/// JSON 可能来自不可信来源（CLI 载入的 tree.json、MCP/JSON-RPC 线协议的 `args["tree"]`），
/// 而 nlohmann 的解析器与析构器都是迭代式的，故一份十万层嵌套的
/// `{"type":"Column","children":[…]}` 能顺利解析，只在此处递归时耗尽调用栈。
/// 上限与 `validate_ui` / `Repeater` 一致取 `AURORA_DEFAULT_MAX_WIDGET_DEPTH`。
auto from_json_impl(const Json &j, std::size_t depth) -> Result<std::shared_ptr<Widget>> {
    if (depth > AURORA_DEFAULT_MAX_WIDGET_DEPTH) {
        return make_error(ErrorCode::WidgetDepthExceeded, "serialization: widget tree nesting depth exceeds limit (" +
                                                              std::to_string(AURORA_DEFAULT_MAX_WIDGET_DEPTH) + "）");
    }
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        return make_error(ErrorCode::IOParseFailed, "serialization: node JSON must be an object with a string 'type' field");
    }
    const std::string type = j["type"].get<std::string>();
    const Json props = j.value("props", Json::object());

    auto wres = WidgetRegistry::instance().make(type, props);
    if (!wres) {
        return wres;
    }
    std::shared_ptr<Widget> w = std::move(wres.value());

    if (j.contains("children") && j["children"].is_array()) {
        std::vector<Node> kids;
        for (const auto &cj : j["children"]) {
            auto cres = from_json_impl(cj, depth + 1);
            if (!cres) {
                return cres;
            }
            kids.emplace_back(std::move(cres.value()));
        }
        w->adopt_children(std::move(kids));
    }
    return w;
}

} // namespace

auto from_json(const Json &j) -> Result<std::shared_ptr<Widget>> {
    register_core_widgets();
    return from_json_impl(j, 0);
}

namespace {

// 对象差异：新增键 add、消失键 remove、共同键递归。
auto diff_objects(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void {
    for (auto it = b.begin(); it != b.end(); ++it) {
        if (!a.contains(it.key())) {
            out.push_back(JsonPatchOp{ .op = "add", .path = path + "/" + it.key(), .value = it.value() });
        }
    }
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (!b.contains(it.key())) {
            out.push_back(JsonPatchOp{ .op = "remove", .path = path + "/" + it.key(), .value = Json() });
        }
    }
    for (auto it = b.begin(); it != b.end(); ++it) {
        if (a.contains(it.key())) {
            diff_into(a.at(it.key()), it.value(), path + "/" + it.key(), out);
        }
    }
}

// 数组差异：按索引对齐，越界侧 add/remove，其余递归。
auto diff_arrays(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void {
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::string ip = path + "/" + std::to_string(i);
        if (i >= a.size()) {
            out.push_back(JsonPatchOp{ .op = "add", .path = ip, .value = b.at(i) });
        } else if (i >= b.size()) {
            out.push_back(JsonPatchOp{ .op = "remove", .path = ip, .value = Json() });
        } else {
            diff_into(a.at(i), b.at(i), ip, out);
        }
    }
}

} // namespace

auto diff_into(const Json &a, const Json &b, const std::string &path, std::vector<JsonPatchOp> &out) -> void {
    if (a == b) {
        return;
    }
    if (a.is_object() && b.is_object()) {
        diff_objects(a, b, path, out);
    } else if (a.is_array() && b.is_array()) {
        diff_arrays(a, b, path, out);
    } else {
        out.push_back(JsonPatchOp{ .op = "replace", .path = path, .value = b });
    }
}

auto diff(const Json &a, const Json &b) -> std::vector<JsonPatchOp> {
    std::vector<JsonPatchOp> out;
    diff_into(a, b, "", out);
    return out;
}

auto apply_patch(Json &target, const std::vector<JsonPatchOp> &patch) -> void {
    for (const auto &op : patch) {
        const nlohmann::json::json_pointer ptr(op.path);
        // nlohmann 3.11 起 json_pointer 的 string 转换已弃用（contains/erase/operator[] 内部使用），
        // 局部抑制该告警；行为保持幂等（路径不存在时 contains 返回 false，不会抛异常）。
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (op.op == "remove") {
            if (target.contains(ptr)) {
                target.erase(ptr);
            }
        } else {
            target[ptr] = op.value; // replace / add
        }
#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }
}

namespace {
auto is_container_type(const std::string &t) -> bool {
    return t == "Column" || t == "Row" || t == "Stack" || t == "Grid" || t == "Scroll";
}
} // namespace

auto component_schema(const std::string &name) -> Json { // NOLINT(readability-function-cognitive-complexity)
    Json w = Json::object();
    w["type"] = name;
    w["container"] = is_container_type(name);
    w["is_container"] = is_container_type(name);
    w["is_layout"] = is_container_type(name); // 多子布局容器即 layout 型
    w["is_clickable"] = name == "Button";
    w["dynamic_children"] = (name == "Repeater" || name == "Canvas");
    w["thread"] = "main";
    Json props = Json::object();
    auto inst = WidgetRegistry::instance().make(name, Json::object());
    if (inst) {
        inst.value()->serialize_props(props);
        w["default_props"] = props; // serialize_props 已写入含默认值的属性对象

        // 附录 B 自描述元数据（v0.7.0 新增）
        const WidgetDescriptor desc = inst.value()->describe();
        Json prop_desc = Json::array();
        for (const auto &pd : desc.properties) {
            prop_desc.push_back(descriptor_to_json(pd));
        }
        w["prop_descriptors"] = prop_desc;
        Json events = Json::array();
        for (const auto &e : desc.events) {
            events.push_back(e);
        }
        w["events"] = events;
        w["children_policy"] = desc.children_policy;
        Json examples = Json::array();
        for (const auto &ex : desc.examples) {
            examples.push_back(ex);
        }
        w["examples"] = examples;

        // ---- Schema 扩展：props_schema / children_types / constraints ----
        Json props_schema = Json::object();
        Json constraints = Json::array();
        for (const auto &pd : desc.properties) {
            Json ps = Json::object();
            if (!pd.json_type.empty()) {
                ps["type"] = pd.json_type;
            }
            if (!pd.enum_values.empty()) {
                Json ev = Json::array();
                for (const auto &v : pd.enum_values) {
                    ev.push_back(v);
                }
                ps["enum"] = ev;
            }
            if (!pd.min_value.empty()) {
                ps["minimum"] = pd.min_value;
            }
            if (!pd.max_value.empty()) {
                ps["maximum"] = pd.max_value;
            }
            if (!pd.default_value.empty()) {
                ps["default"] = pd.default_value;
            }
            if (!pd.note.empty()) {
                ps["description"] = pd.note;
            }
            if (!pd.constraint.empty()) {
                ps["constraint"] = pd.constraint;
                constraints.push_back(pd.constraint);
            }
            if (!ps.empty()) {
                props_schema[pd.name] = ps;
            }
        }
        w["props_schema"] = props_schema;
        if (!constraints.empty()) {
            w["constraints"] = constraints;
        }

        Json children_types = Json::array();
        for (const auto &ct : desc.allowed_child_types) {
            children_types.push_back(ct);
        }
        if (!children_types.empty()) {
            w["children_types"] = children_types;
        }

        Json invariants = Json::array();
        for (const auto &inv : desc.invariants) {
            invariants.push_back(inv);
        }
        if (!invariants.empty()) {
            w["invariants"] = invariants;
        }
    } else {
        w["default_props"] = Json::object();
        w["prop_descriptors"] = Json::array();
        w["events"] = Json::array();
        w["children_policy"] = "none";
        w["examples"] = Json::array();
    }
    Json prop_keys = Json::array();
    for (auto it = props.begin(); it != props.end(); ++it) {
        prop_keys.push_back(it.key());
    }
    w["props"] = prop_keys;
    return w;
}

} // namespace serialization

namespace serialization {

auto to_yaml(const Widget &w) -> std::string {
    const Json j = to_json(w);
    return to_yaml(j); // calls yaml.h's inline to_yaml(const Json&, int=0)
}

} // namespace serialization

auto list_all_components() -> std::vector<std::string> {
    serialization::register_core_widgets();
    return serialization::WidgetRegistry::instance().list_types();
}

auto describe_component(const std::string &name) -> Json {
    serialization::register_core_widgets();
    return serialization::component_schema(name);
}

auto search_components(const std::string &query) -> std::vector<Json> {
    serialization::register_core_widgets();
    auto lower = [](std::string s) -> std::string {
        for (char &c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string needle = lower(query);
    std::vector<Json> out;
    for (const std::string &t : serialization::WidgetRegistry::instance().list_types()) {
        if (lower(t).find(needle) != std::string::npos) {
            out.push_back(serialization::component_schema(t));
        }
    }
    return out;
}

auto list_all_schemas() -> std::vector<Json> {
    serialization::register_core_widgets();
    std::vector<Json> out;
    for (const std::string &t : serialization::WidgetRegistry::instance().list_types()) {
        out.push_back(serialization::component_schema(t));
    }
    return out;
}

} // namespace aurora
