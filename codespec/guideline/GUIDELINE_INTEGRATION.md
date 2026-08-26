# GUIDELINE_INTEGRATION

> 本文件由 [`GUIDELINE.md`](../GUIDELINE.md) 划分而出（持久化配置 / 视频播放器 / 字体文本 / Inspector / AI-First 工厂 / 测试原语 / 控件样式）。
> 返回主线见 [`GUIDELINE.md`](../GUIDELINE.md)。
>
> 片段约定：本文所有片段使用 `namespace au = aurora;`（即 `au::` 前缀）。复制任意单段时，请确保该别名（或 `using namespace aurora;`）已在所处 TU 声明，否则 `au::Xxx` 编译失败。

**本文包含章节：**

- [14. 持久化配置（Preferences）](#14-持久化配置preferences)
- [15. 视频播放器（VideoPlayer）](#15-视频播放器videoplayer)
- [16. 字体与文本渲染（FreeType）](#16-字体与文本渲染freetype)
- [17. Inspector 面板（树形浏览器 + 属性编辑器）](#17-inspector-面板树形浏览器--属性编辑器)
- [17b. Inspector 远程 HTTP 接口（InspectorServer）](#17b-inspector-远程-http-接口inspectorserver)
- [18. AI-First 声明式工厂层（aurora::ui）](#18-ai-first-声明式工厂层auroraui)
- [18b. 滚动容器（Scroll）](#18b-滚动容器scroll)
- [19. 测试原语（aurora::test）](#19-测试原语auroratest)
- [20. 控件样式定制与继承](#20-控件样式定制与继承)

## 14. 持久化配置（Preferences）

轻量键值配置，以单个 JSON 文件为后端（对标 `SharedPreferences` / `UserDefaults`）。
初始化时**显式指定文件位置**；未指定则仅存内存。`set` 只写内存与响应式 `State`，落盘由 `flush()` 主动触发。

```cpp
#include "aurora/aurora.h"
using namespace aurora;
using namespace aurora::preferences;

// 1) 单例（推荐，应用级全局配置）：初始化时显式指定文件位置，同名全局唯一、线程安全
Preferences &prefs = Preferences::instance("app", Preferences::default_config_dir());
//    或显式绝对路径：Preferences &prefs = Preferences::instance_at("app", "C:/data/settings.json");
//    （普通构造器仍可用：内存模式 Preferences mem; 文件模式 Preferences p{ "C:/data/settings.json" };）

// 2) 响应式绑定到已有控件（Switch 等）
au::Switch sw{ prefs.binding<bool>("dark_mode", false),
    [&prefs](bool v) { prefs.set("dark_mode", v); prefs.flush(); } };

// 3) 读取：类型不匹配或缺失回退默认值
int volume = prefs.get("volume", 5);

// 4) 主动刷新到文件（进程锁 + 原子写）；重新启动后构造即加载，reload 可从文件恢复
if (auto r = prefs.flush(); !r.ok()) { /* r.error() */ }
```

- **单例粒度**：`instance(name)` 按名注册表，每个 `name` 唯一；首次调用按参数创建，后续忽略路径参数。
- **并发安全**：实例内部 `std::shared_mutex` 保护读写；`flush`/`reload` 经跨平台文件锁（`<file>.lock`）+ 原子 `rename` 保证多进程安全。可多线程并发 `set`/`get`/`flush`。
- 支持值类型：`bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象。
- `watch<T>` 返回 `std::shared_ptr<State<T>>`，可手动订阅；`binding<T>` 返回 `Binding<T>`（非拥有，须保持 `Preferences` 存活）。

### 14.1 分组（group）

`group(name)` 返回作用域子视图，把相关配置组织到嵌套 JSON 对象里持久化；根 API 不变，分组与扁平键可共存。

```cpp
// 把界面偏好分组到 "ui"：文件内表现为 {"ui":{"theme":"dark","font_size":14}}
auto ui = prefs.group("ui");
ui.set("theme", std::string("dark"));
ui.set("font_size", 14);

// 控件直接绑定分组内的键（删除走分组墓碑路径）
au::Switch sw{ ui.binding<bool>("dark_mode", false),
    [&prefs](bool v) { prefs.group("ui").set("dark_mode", v); prefs.flush(); } };

// 链式嵌套分组
prefs.group("ui").group("editor").set("font", std::string("Mono"));

// 仅清空 ui 分组子树（不影响其他分组与顶层键）
ui.clear();
```

- 分组键以复合点号路径索引（`"ui.theme"`、`"ui.editor.font"`），旧版扁平文件仍可正确加载（向后兼容）。
- `Group::remove` / `Group::clear` 沿用版本化 LWW + 墓碑 + 清空纪元，随 `flush` 多进程最终一致；`Group` 是轻量视图，须在其所属 `Preferences` 存活期内使用。

---

## 15. 视频播放器（VideoPlayer）

`media/` 子系统以抽象 `VideoSource` 为解码扩展点，内置零依赖 `ImageSequenceSource`（图片序列/动画）作为自包含样例。播放由 `Application::tick` 自动驱动，控件与 UI 可绑定 `Reactive` 播放状态。

```cpp
#include "aurora/aurora.h"
using namespace au;

// 1) 内置零依赖源：把若干已解码帧当作定帧率动画
auto src = std::make_shared<ImageSequenceSource>();
src->set_fps(24.0);
src->set_frames({ Image{ 160, 90, /*...*/ }, Image{ 160, 90, /*...*/ } });

// 2) 播放器（继承 Container，可叠加控件叠层）；默认带底部 VideoControls
auto player = std::make_unique<VideoPlayer>();
player->set_source(src);
player->width(px(640));
player->height(px(360));

// 3) 播放控制 / 响应式状态
player->toggle_play();
player->seek_fraction(0.5);          // 跳到一半
player->set_volume(0.8);

// 4) 易于继承定制（四类扩展点）：
//    - 可插拔源：继承 VideoSource 实现 frame_at + 播放控制（ffmpeg / 平台媒体 / 相机…）
//    - 可子类化本体：覆写 on_frame / on_playback_tick / on_paint / on_layout
//    - 可定制 UI：set_controls(...) 整体替换叠层，或子类化 VideoControls 换肤/重排
//    - 可插拔手势：覆写 on_tap / on_double_tap 或 set_on_tap / set_on_double_tap

// 4a) 在 on_paint 中读取当前帧并叠加水印（current_frame() / paint_frame() 为受保护钩子）
class WatermarkPlayer : public VideoPlayer {
  protected:
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        VideoPlayer::on_paint(p, b, ctx);   // 先画默认帧
        p.fill_rect(Rect{ b.pos + Point{ 8, 8 }, Size{ 120, 24 } }, Color{ 0, 0, 0, 120 });
        p.draw_text("Aurora", b.pos + Point{ 12, 12 }, Font{ .size_pt = 14 }, Color::white());
    }
};

// 4b) 替换默认控件叠层：子类化 VideoPlayer，覆写 create_default_controls()（挂载期生效）
class MinimalPlayer : public VideoPlayer {
  protected:
    [[nodiscard]] auto create_default_controls() -> std::unique_ptr<Widget> override {
        return std::make_unique<VideoControls>(this);   // 或返回自定义控件条
    }
};

// 4c) 换肤 / 重排底部控件条：子类化 VideoControls，访问 play_button()/time_text()/mute_button()
class ThemedControls : public VideoControls {
  public:
    explicit ThemedControls(VideoController *c) : VideoControls(c) {
        if (play_button()) play_button()->color(Color::blue());   // 构造后直接改样式
    }
  protected:
    auto build_children() -> void override {    // 或直接覆写 build_children 重建条
        VideoControls::build_children();        // 先建默认子控件
        if (time_text()) time_text()->color(Color::yellow());
    }
};
```

---

## 16. 字体与文本渲染（FreeType）

Aurora 以 **FreeType** 为字体内核（栅格化）、**HarfBuzz** 为文本 shaping（均经 `third_party/` 源码构建编入静态库），默认字体为内置 **Noto Sans（OFL）**，跨平台（含 Headless）确定性。
无需任何配置即可使用默认字体；仅在需要自定义/嵌入字体时显式注册。

```cpp
#include "aurora/render/font_engine.h"

// 1) 使用内置默认字体（无需注册）：
au::Text("Hello, 世界").font_size(16);          // 默认 Noto Sans，CJK 走系统回退链

// 2) 从文件注册（按 family 名引用）：
au::FontEngine::register_font("MyBrand", "C:/Fonts/Brand-Regular.ttf");
au::Text("Branded").font_family("MyBrand");

// 3) 从内存字节注册（跨平台自包含、无外部文件依赖）：
std::vector<std::uint8_t> ttf = load_embedded_font(); // 你的嵌入 TTF 字节
au::FontEngine::register_font_from_memory("Embedded", ttf);
au::Text("Embedded font").font_family("Embedded");

// 4) 设为默认 sans-serif（family 留空 "" 即默认链）；
//    set_default_font 仍可用，但内置 Noto 已自动注册为默认，通常无需调用。
au::FontEngine::set_default_font("C:/Fonts/Fallback.ttf");

// 5) 抗锯齿策略（默认 Supersample = FT_RENDER_MODE_NORMAL A8 灰度；
//    颜色安全、截图/任意显示均不发虚。仍可通过显式调用切到 ClearType）：
au::FontEngine::set_text_aa_mode(au::TextAAMode::ClearType);
```

- `family` 为空字符串 / `"sans-serif"` / `"Noto Sans"` / `"default"` 均指向内置默认字体。
- 缺字（如未注册 CJK 的 Latin 字体）自动沿系统字体回退链（msyh/simsun/MSGOTHIC 等）查找，避免豆腐块。
- 度量（`measure_width`/`caret_x`/`hit_test_char`）与绘制（`draw_text`）在逻辑 dp 空间一致；`display_*` 系列为兼容保留的薄转发，恒等于对应自然版。

---

## 17. Inspector 面板（树形浏览器 + 属性编辑器）

`InspectorPanel` 提供类似 Chrome DevTools / Flutter Inspector 的 Widget 树检视能力：左侧树形浏览器展示层级，右侧属性面板展示选中 Widget 的类型与属性值。支持“Export Code”按钮一键导出当前树为 C++ 源码。

```cpp
#include "aurora/aurora.h"
using namespace au;

// 1) 构建目标 UI 树
auto target_tree = []() -> Node {
    return Node{ Column{ ColumnProps{ .children = {
        Node{ Text{ "Hello" } },
        Node{ Button{ ButtonProps{ .label = "Click" } } },
    } } } };
};

// 2) 创建 InspectorPanel（接受目标树获取函数 + 左侧占比）
InspectorPanel inspector{ target_tree, 0.35f };

// 3) 监听选中事件
inspector.on_select_widget = [](Widget *w) {
    AURORA_LOG_INFO("app", "选中: ", w->type_name());
};

// 4) 监听导出代码事件
inspector.on_export_code = [](const std::string &code) {
    AURORA_LOG_INFO("app", "导出代码:\n", code);
};

// 5) 手动导出代码
std::string code = inspector.export_code();

// 6) 使用 inspect.h 扩展函数
Node root = target_tree();
auto items = widget_tree_to_items(root);      // Widget 树 → TreeItem 树
auto json  = dump_tree_json_full(root);       // 含属性的完整 JSON 快照
auto node  = find_node_by_path(root, "0/1");  // 按路径定位节点
auto props = get_widget_props(root.widget()); // 属性快照
set_widget_prop(root.widget(), "gap", Json(12.0f)); // 属性回写
```

---

## 17b. Inspector 远程 HTTP 接口（InspectorServer）

`InspectorServer` 提供 localhost-only HTTP 服务器，暴露 REST 端点供外部工具远程访问运行时 widget 树。跨平台（Windows 走 Winsock2、Linux/macOS 走 BSD sockets），须开启 CMake 开关 `AURORA_BUILD_INSPECTOR_SERVER`。

```cpp
#include "aurora/aurora.h"
#include "aurora/inspector/inspector_server.h"
using namespace au;

// 1) 构建 UI 树
auto target_tree = []() -> Node {
    return Node{ Column{ ColumnProps{ .children = {
        Node{ Text{ "Hello" } },
        Node{ Button{ ButtonProps{ .label = "Click" } } },
    } } } };
};

// 2) 创建并启动 InspectorServer
InspectorServer server{ target_tree };
if (server.start(6280)) {   // 默认端口 6280，后台线程运行
    AURORA_LOG_INFO("app", "InspectorServer running on port ", server.port());
}

// 3) REST 端点（可用 curl 测试）：
//    GET  http://localhost:6280/api/tree           — 完整 widget 树 JSON
//    GET  http://localhost:6280/api/widget/0/1     — 路径 0/1 的 widget 属性
//    PUT  http://localhost:6280/api/widget/0/1/gap — 回写属性（请求体为 JSON 值）
//    GET  http://localhost:6280/api/components     — 全部组件 schema
//    GET  http://localhost:6280/api/yaml           — 当前树的 YAML
//    POST http://localhost:6280/api/to_code        — UI 树 → C++ 代码

// 4) 停止服务器
server.stop();
```

> 注：`InspectorServer` 内部以 `std::mutex` 保护 widget 树访问，`root_getter` 回调在 HTTP 工作线程中被调用。链接依赖：Windows 为 `ws2_32`（Winsock2），Linux/macOS 为 `pthread`（经 `find_package(Threads)`，CMake 自动注入）。

---

## 18. AI-First 声明式工厂层（aurora::ui）

`aurora::ui`（别名 `au::ui`，头 `include/aurora/ui/factories.h`，经 `aurora.h` 暴露）提供一组**声明式工厂函数**：每个接受父 `Container&` 与内容/属性，**自动把新建控件加入父容器**并返回**强类型指针**，免去 `new`/手动 `add`、压缩 token 与出错面。覆盖规范 §7 全套：`label` / `button` / `input` / `checkbox` / `slider` / `vbox`(=`Column`) / `hbox`(=`Row`) / `stack` / `grid` / `scroll`。

```cpp
#include "aurora/ui/factories.h"
using namespace aurora::ui;

au::Column root;
auto* title = label(root, "标题");                       // Text*，自动加为 root 子节点
auto* ok = button(root, "确定", {}, []{ /* 点击回调 */ }); // Button*；第四参 on_click 可选
auto* name = input(root, "默认值");                      // TextInput*
auto* flag = checkbox(root, au::reactive(true));          // Checkbox*（或 checkbox(root, true)）
auto* vol = slider(root, au::reactive(0.5));              // Slider*（或 slider(root, 0.5)）
auto* box = vbox(root);                                   // Column*；hbox→Row*，stack→Stack*，grid→Grid*，scroll→Scroll*
```

> 工厂返回的裸指针生命周期由父树 `shared_ptr` 持有，**父树销毁后即失效**，勿长期持有。容器类工厂第二参数为既有 `XxxProps`（默认 `{}`）；`label`/`button` 首参为主文案，覆盖对应 Props 字段。底层控件类型不变，等价手写声明式构造。

## 18b. 滚动容器（Scroll）

单子垂直滚动容器：固定视口裁切内容、滚轮增量滚动；内容量再大也只缓冲「视口 ×(1+2×overscan)」的有界离屏窗口（G-8 滑动窗口缓冲），纯滚动帧整页仅一次平移合成、不重栅——滚动流畅度专项的设计目标。

```cpp
au::Column list{ au::ColumnProps{.children = {
    au::Text("行 1"), au::Text("行 2"), /* …长列表… */ } } };
// 方式 1：配置块构造（可设 step / overscan）
au::Scroll scroll{ au::ScrollProps{ .child = std::move(list), .step = 16.0f } };
// 方式 2：便捷构造（取首项为唯一子节点）
au::Node n = au::Scroll{ au::Column{ au::Text("A"), au::Text("B") } };
```

- `ScrollProps` 字段：`.child`（唯一子节点 `Node`）、`.step`（每单位滚轮增量的滚动像素，默认 16）、`.overscan`（视口上下各留 overscan 屏缓冲，默认 1；缓冲内存随内容量 ×10 不增长）。
- 继承式双模：`.step` 可直接赋值（`scroll.step = 16`），或 `Scroll{ ScrollProps{...} }` 配置块构造（规格 §1）。
- 工厂层等价：`au::ui::scroll(parent, ...)`（`aurora::ui` 见 §18）。

## 19. 测试原语（aurora::test）

`aurora::test`（头 `include/aurora/test_helpers.h`，**不进 `aurora.h`**）薄封装 `HeadlessSurface` + `TCHECK*` + `EventDispatcher`，用于编写**确定性、可文本验证**的测试。**使用前提：测试 TU 须先 `#include "tests/test_harness.h"`**（提供 `TCHECK*` 断言宏）。

```cpp
#include "tests/test_harness.h"
#include "aurora/aurora.h"
#include "aurora/test_helpers.h"
#include "aurora/ui/factories.h"
using namespace aurora;
using namespace aurora::ui;
using namespace aurora::test;

auto env = init_headless(320, 240);                  // 无头环境（确定性，无需 GUI 后端）
auto* b = button(*env.root_widget, "Go", {}, [&]{ clicked = true; });
pump(env);                                           // 推进一帧：mount + layout
expect_text(env.root, "Go");                         // 断言树中存在含该文本的控件
expect_tree_contains(env.root, "Button");
expect_count(env.root, "Text", 0);
expect_visible(env.root);
tap(env, *b);                                        // 在控件中心合成 press+release → 触发 on_click
type_text(env, *inputWidget, "abc");                 // 逐字符喂入已聚焦输入控件
```

> 富格式文本化：`dump_tree_rich(node)`（`include/aurora/widget/inspect.h`）输出 `#id` / `bounds` / `visible` / `text` / `style` / `listeners` 及 `├─└─` 树形符，供 AI 断言 / diff / 定位。给节点命名：`node.set_id("my-id")`（新增于 `Node`）。

---

## 20. 控件样式定制与继承

全部交互控件遵循统一的可定制性契约（见 `SPECIFICATIONS.md` §#H.14.1）：主题回退 + 状态反馈 + protected 绘制分阶段钩子。

**链式样式定制（组合方式）**：

```cpp
// Outlined 风格按钮：透明背景 + 描边 + 自定义悬停/按下色 + 最小尺寸
auto btn = au::Button("Save");
btn.background(au::colors::Transparent)
   .text_color(au::Color{ 0, 90, 200, 255 })
   .set_border(au::Color{ 0, 90, 200, 255 }, 1.5f)
   .set_hover_color(au::Color{ 0, 90, 200, 24 })   // 不设则由背景色自动调暗
   .set_min_size(96.0f, 36.0f);

// 步进滑块 + 自定义轨道/滑块；active_color 不设则跟随主题 primary
auto sl = au::Slider();
sl.set_range(0, 100).set_step(5).set_track_height(8.0f).set_thumb_size(22.0f);

// 密码输入框：掩码 + 限长 + 提交回调
auto pwd = au::TextInput();
pwd.set_placeholder("Password").set_obscure_text(true).set_max_length(32)
   .set_on_submit([](const std::string &v) { /* login */ });
```

**继承方式（单点覆盖绘制阶段）**：各控件 `on_paint` 已分解为 protected 虚钩子，子类只覆盖需要的阶段：

```cpp
// 自定义滑块外观的 Slider：只改滑块，轨道/填充沿用基类
class DiamondSlider : public au::Slider {
  protected:
    auto paint_thumb(au::Painter &p, const au::Rect &bounds, const au::Rect &track, au::Color c)
        -> void override {
        const float cx = track.origin.x + value_fraction() * track.size.width;
        const float cy = bounds.origin.y + bounds.size.height * 0.5f;
        p.fill_rounded_rect({ { cx - 6.0f, cy - 6.0f }, { 12.0f, 12.0f } }, 3.0f, c); // 方块滑块
    }
};

// 自定义背景的 Button：只改背景（如渐变），文字/边框/状态色逻辑不变
class GradientButton : public au::Button {
  protected:
    auto paint_background(au::Painter &p, const au::Rect &b, au::Color bg) -> void override {
        p.draw_linear_gradient(b, b.origin, { b.right(), b.bottom() },
                               { bg, bg.shaded(0.7f) }, { 0.0f, 1.0f });
    }
};
```

可用钩子速查：Button `resolve_background/resolve_text_color/paint_background/paint_border/paint_label`；
Slider `paint_track/paint_active_track/paint_thumb`；Switch `paint_track/paint_thumb`；ProgressIndicator
`paint_track/paint_fill`；RadioGroup `paint_option`；SpinBox `paint_box/paint_value/paint_arrows`；Dropdown
`paint_box/paint_item`；TabBar `paint_tab`；SegmentedControl `paint_segment`；Chip `paint_background/paint_content`；
TextInput `paint_frame`。状态色派生用 `Color::shaded(k)`（调暗/调亮）与 `Color::with_alpha(a)`（淡色底）。

---

