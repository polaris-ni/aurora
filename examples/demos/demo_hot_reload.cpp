// JSON UI 热重载 demo（specification/08-tooling.md §2.7）：编辑 ui.json 即时重建控件树。
//
// 观察点：窗口内容随 ui.json 保存而重建；**JSON 未显式声明的属性**按树路径保留 ——
// 例如先在输入框里敲几个字，再去改 ui.json 里另一行的文本并保存：
// 那行文本变了，而敲进去的字仍在输入框里（`value` 未在 JSON 中声明，故被回填）。
// ui.json 不存在时本程序自动写入一份默认内容，载体自包含、不依赖外部夹具。
#include <fstream>
#include <memory>
#include <string>

#include "demo_common.h"

namespace {

constexpr const char *AURORA_UI_FILE = "ui.json";

// 只用已注册的控件类型（Text / TextInput）：from_json 的工厂不认 demo 私有控件。
// TextInput 刻意不带 "value" —— 它正是「JSON 未声明、故按树路径保留」的那个属性。
constexpr const char *AURORA_DEFAULT_UI = R"JSON({
  "type": "Column",
  "props": {},
  "children": [
    {
      "type": "Text",
      "props": { "content": "Edit ui.json, save, and this text is rebuilt live." }
    },
    {
      "type": "TextInput",
      "props": { "placeholder": "Type here first, then edit ui.json" }
    }
  ]
}
)JSON";

/// @brief 首次运行时写入默认 ui.json（返回是否真的写入），使载体不依赖外部夹具。
auto ensure_default_ui(const std::string &path) -> bool {
    std::ifstream probe(path);
    if (probe.good()) {
        return false;
    }
    std::ofstream out(path);
    if (!out.good()) {
        return false;
    }
    out << AURORA_DEFAULT_UI;
    return true;
}

/// @brief 用热重载产出的新树替换场景根，并同步焦点根。
/// 替换走 `scene().root_node()` 赋值（`Node` 移动语义），不销毁 `Node` 对象本身，
/// 故窗口侧持有的根引用不失效。
auto apply_tree(au::Application &app, std::shared_ptr<au::Widget> tree) -> void {
    app.scene().root_node() = au::Node{std::move(tree)};
    app.focus().set_root(&app.scene().root());
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    const std::string ui_path{AURORA_UI_FILE};
    if (ensure_default_ui(ui_path)) {
        AURORA_LOG_INFO("demo", "[hot_reload] wrote default ", ui_path, " -- edit it to trigger a reload");
    }

    au::HotReload reloader{ui_path};

    au::WindowOptions opts;
    opts.size = au::Size{.width = 640.0F, .height = 300.0F};
    opts.title = "Hot reload · Aurora Demo";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[hot_reload] window creation failed: ", win_res.error().message);
        return -1;
    }
    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};

    // 首帧前同步一次：窗口一出现就是 ui.json 描述的内容。
    if (auto first = reloader.try_sync()) {
        apply_tree(app, std::move(first));
    } else {
        AURORA_LOG_ERROR("demo", "[hot_reload] initial load failed -- check ", ui_path, " syntax");
    }

    // 每帧轮询：文件内容变化即重建整棵树。try_sync 返回 nullptr 表示「无变化」或「解析失败」，
    // 后者刻意不抛错、旧树保持不动 —— 这也是本载体的一个观察点。
    app.set_on_frame([&app, &reloader]() -> void {
        if (auto tree = reloader.try_sync()) {
            apply_tree(app, std::move(tree));
            AURORA_LOG_INFO("demo", "[hot_reload] tree rebuilt from ", AURORA_UI_FILE);
        }
    });

    app.run();
    return 0;
}
