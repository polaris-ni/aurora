// 主题风格对照载体：五风格档 × 明暗两档的十格并排 + 本机实况区 + 单格大图 + 实开对话框。
//
// 本载体只演示、不断言（不打印 PASS/FAIL），判读口径见 codespec/manual-test/13-theming.md。
// 四处可观察面：
//   ① 十格并排——同一控件套装在十份标定主题下的观感分野（底色 / 主色 / 圆角 / 控件高 / 内边距 /
//      字体族与字号）；同一行的左右两格只差明暗档，同一列的上下五格只差风格档。
//   ② 本机实况区——页面底色由载体经 `inherit_theme(ctx)` 读**根注入**主题（`Window` 的三级优先链），
//      风格 / 明暗两个下拉分别驱动 `Window::set_theme_style` 与 `Window::set_os_color_scheme`；
//      实况区的套装控件不显式设色，取色与度量应随注入变。
//   ③ 单格大图——点任一格右上角「Zoom」进大图：同套装放大 + 该档令牌数值表（含角色十六进制值）。
//   ④ 实开对话框——「Confirm dialog」按当前实况主题起 `confirm`，判读
//      `conventions.dialog_button_order` 的分档排布（AcceptFirst / AcceptLast / VerticalRightAligned）。
//
// 运行：cmake --build build --target demo_theme_styles && ./build/demo_theme_styles
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/dialog.h"
#include "aurora/widget/dropdown.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/show.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/stack.h"
#include "demo_common.h"

namespace {

constexpr std::array<au::ThemeStyle, 5> AURORA_STYLES{au::ThemeStyle::Material, au::ThemeStyle::Windows,
                                                      au::ThemeStyle::MacOs, au::ThemeStyle::Gnome,
                                                      au::ThemeStyle::Kde};
constexpr std::array<au::ColorScheme, 2> AURORA_CELL_SCHEMES{au::ColorScheme::Light, au::ColorScheme::Dark};
constexpr std::array<au::ColorScheme, 3> AURORA_SCHEME_CHOICES{au::ColorScheme::Unspecified, au::ColorScheme::Light,
                                                               au::ColorScheme::Dark};

constexpr float AURORA_CELL_WIDTH = 300.0F;  ///< 全览档单格宽（两格并排 + 间距 ≈ 620）。
constexpr float AURORA_WIDE_CELL_WIDTH = 580.0F;  ///< 大图档单格宽。
constexpr float AURORA_GRID_VIEWPORT_HEIGHT = 460.0F;  ///< 滚动视口高：全览五格行的可见高度。
constexpr float AURORA_PAGE_PADDING = 12.0F;  ///< 页面外边距（载体自身排布常量，非主题令牌）。

/// @brief 十格标定主题的网格：行 = 风格档（`AURORA_STYLES` 序），列 = 明暗档（`AURORA_CELL_SCHEMES` 序）。
///
/// 用 `std::array` 而非 C 数组：本载体的网格下标全部来自循环，`.at()` 既越界即抛，
/// 比静默读坏格更符合「演示数据出错要立刻可见」的诉求。
using ThemeGrid = std::array<std::array<au::Theme, 2>, 5>;

[[nodiscard]] auto style_label(au::ThemeStyle s) -> std::string {
    switch (s) {
        case au::ThemeStyle::Material:
            return "Material";
        case au::ThemeStyle::Windows:
            return "Windows";
        case au::ThemeStyle::MacOs:
            return "macOS";
        case au::ThemeStyle::Gnome:
            return "GNOME";
        case au::ThemeStyle::Kde:
            return "KDE";
    }
    return "?";
}

[[nodiscard]] auto scheme_label(au::ColorScheme c) -> std::string {
    switch (c) {
        case au::ColorScheme::Unspecified:
            return "Unspecified";
        case au::ColorScheme::Light:
            return "Light";
        case au::ColorScheme::Dark:
            return "Dark";
    }
    return "?";
}

[[nodiscard]] auto desktop_label(au::DesktopEnvironment e) -> std::string {
    switch (e) {
        case au::DesktopEnvironment::Unknown:
            return "Unknown";
        case au::DesktopEnvironment::Gnome:
            return "Gnome";
        case au::DesktopEnvironment::Kde:
            return "Kde";
        case au::DesktopEnvironment::Other:
            return "Other";
    }
    return "?";
}

[[nodiscard]] auto kind_label(au::PlatformKind k) -> std::string {
    switch (k) {
        case au::PlatformKind::Unknown:
            return "Unknown";
        case au::PlatformKind::Windows:
            return "Windows";
        case au::PlatformKind::MacOs:
            return "MacOs";
        case au::PlatformKind::Linux:
            return "Linux";
        case au::PlatformKind::Web:
            return "Web";
    }
    return "?";
}

[[nodiscard]] auto surface_label(au::SurfaceKind s) -> std::string {
    switch (s) {
        case au::SurfaceKind::Headless:
            return "Headless";
        case au::SurfaceKind::Win32:
            return "Win32";
        case au::SurfaceKind::Glfw:
            return "Glfw";
        case au::SurfaceKind::D3D11:
            return "D3D11";
        case au::SurfaceKind::X11:
            return "X11";
        case au::SurfaceKind::Wayland:
            return "Wayland";
        case au::SurfaceKind::MacOS:
            return "MacOS";
        case au::SurfaceKind::Wasm:
            return "Wasm";
    }
    return "?";
}

[[nodiscard]] auto order_label(au::DialogButtonOrder o) -> std::string {
    switch (o) {
        case au::DialogButtonOrder::AcceptFirst:
            return "AcceptFirst";
        case au::DialogButtonOrder::AcceptLast:
            return "AcceptLast";
        case au::DialogButtonOrder::VerticalRightAligned:
            return "VerticalRightAligned";
    }
    return "?";
}

[[nodiscard]] auto title_align_label(au::DialogTitleAlignment a) -> std::string {
    switch (a) {
        case au::DialogTitleAlignment::Leading:
            return "Leading";
        case au::DialogTitleAlignment::Centered:
            return "Centered";
    }
    return "?";
}

[[nodiscard]] auto placement_label(au::MenuBarPlacement p) -> std::string {
    switch (p) {
        case au::MenuBarPlacement::InWindow:
            return "InWindow";
        case au::MenuBarPlacement::NativePreferred:
            return "NativePreferred";
    }
    return "?";
}

[[nodiscard]] auto scrollbar_label(au::ScrollbarMode m) -> std::string {
    switch (m) {
        case au::ScrollbarMode::Classic:
            return "Classic";
        case au::ScrollbarMode::Overlay:
            return "Overlay";
    }
    return "?";
}

[[nodiscard]] auto ring_label(au::FocusRingStyle r) -> std::string {
    switch (r) {
        case au::FocusRingStyle::Outline:
            return "Outline";
        case au::FocusRingStyle::OuterGlow:
            return "OuterGlow";
        case au::FocusRingStyle::None:
            return "None";
    }
    return "?";
}

/// @brief 一位小数定长文本：避开 `std::format` 的工具链差异（同 demo_animation 的口径）。
/// @param v 非负度量值（令牌全档非负，故不处理符号）。
/// @return 形如 "10.5" 的文本。
[[nodiscard]] auto f1(float v) -> std::string {
    // 四舍五入交给 std::lround（.5 远离零），手写 +0.5F 再截断在非负值上同解，但那种写法
    // 会被读成「取整」而不是「舍入」，负值下还会分叉——这里要的是舍入语义本身。
    const long tenths = std::lround(v * 10.0F);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

/// @brief 角色色的十六进制文本（十六进制位手写，同样避开 `<format>` 依赖）。
/// @param c 待显示色（alpha 不参与，明暗档已由角色名区分）。
/// @return 形如 "#005A9E" 的文本。
[[nodiscard]] auto hex(au::Color c) -> std::string {
    static constexpr std::string_view DIGITS = "0123456789ABCDEF";
    const std::array<std::uint8_t, 3> parts{c.r, c.g, c.b};
    std::string out = "#";
    for (const std::uint8_t part : parts) {
        out.push_back(DIGITS.at((part >> 4U) & 0xFU));
        out.push_back(DIGITS.at(part & 0xFU));
    }
    return out;
}

/// @brief 大图档的令牌数值表：把该档的全部标定值就地读给人核对（数值不写进代码常量）。
/// @param t 该格的标定主题。
/// @return 逐行文本，供 `Text` 罗列。
[[nodiscard]] auto token_lines(const au::Theme &t) -> std::vector<std::string> {
    return {
        std::string("style=") + style_label(t.style) + "  scheme=" + scheme_label(t.scheme),
        "roles surface=" + hex(t.roles.surface) + "  container=" + hex(t.roles.surface_container) +
            "  primary=" + hex(t.roles.primary) + "  on_primary=" + hex(t.roles.on_primary),
        "roles on_surface=" + hex(t.roles.on_surface) + "  outline_variant=" + hex(t.roles.outline_variant) +
            "  error=" + hex(t.roles.error) + "  focus_ring=" + hex(t.roles.focus_ring),
        "shapes small/medium/large = " + f1(t.shapes.small) + " / " + f1(t.shapes.medium) + " / " + f1(t.shapes.large),
        "spacing small/large = " + f1(t.spacing.small) + " / " + f1(t.spacing.large) +
            "   control_height = " + f1(t.controls.control_height),
        "indicator = " + f1(t.controls.indicator_size) + "   separator = " + f1(t.controls.separator_thickness) +
            "   border = " + f1(t.controls.border_thickness) + "   scrollbar = " + f1(t.controls.scrollbar_thickness),
        "body = " + f1(t.type_scale.body_medium.size_pt) + " pt   family = " + t.type_scale.body_medium.family,
        std::string("dialog order = ") + order_label(t.conventions.dialog_button_order) +
            "   title = " + title_align_label(t.conventions.dialog_title_alignment),
        std::string("focus ring = ") + ring_label(t.conventions.focus_ring_style) +
            "   w/inset = " + f1(t.controls.focus_ring_width) + " / " + f1(t.controls.focus_ring_inset) +
            "   min_touch = " + f1(t.controls.min_touch_target),
        std::string("scrollbar mode = ") + scrollbar_label(t.conventions.scrollbar_mode) + "   menu bar = " +
            placement_label(t.conventions.menu_bar_placement) + "   motion = " + f1(t.conventions.motion_scale),
    };
}

/// @brief 实况背板：按**根注入**的主题铺满页面底色。
///
/// 载体刻意不写死底色——页面底色是唯一能证明「风格档经 `Window` 的优先链进入子树」的面，
/// 与 `demo_theme` 里 `Card` 把底色钉成 `pal::AURORA_SURFACE` 的做法相反（那份载体因此看不见风格轴）。
/// 取色走 `inherit_theme(ctx)`，与库内控件同源。
class LiveBackdrop : public au::LeafWidget {
  public:
    LiveBackdrop() { set_focusable(false); }

    void collect_signals(au::StateSink & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "LiveBackdrop"; }
    [[nodiscard]] auto describe() const -> au::WidgetDescriptor override {
        return au::WidgetDescriptor{.name = "LiveBackdrop", .children_policy = "none"};
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        // 只铺满给定盒：根 Stack 以 Passthrough 把窗口约束原样下发，故这里就是整窗尺寸。
        const float w = std::isfinite(c.max.width) ? c.max.width : 0.0F;
        const float h = std::isfinite(c.max.height) ? c.max.height : 0.0F;
        return c.constrain(au::Size{.width = w, .height = h});
    }

    void on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) override {
        p.fill_rect(b, au::inherit_theme(ctx).roles.surface);
    }
};

/// @brief 载体侧的对照状态：全览 / 十张大图各自的条件，加当前实况档位与对话框句柄。
struct Carrier {
    au::State<bool> overview{true};  ///< 十格全览是否可见。
    std::vector<std::shared_ptr<au::State<bool>>> zoom;  ///< 每格大图的可见条件（长度 = 10）。
    std::optional<au::ThemeStyle> style;  ///< 已显式钉住的风格档；空 = 交由 `Theme::default_style()` 探测。
    au::ColorScheme scheme = au::ColorScheme::Unspecified;  ///< 已推入的明暗档。
    au::Window *window = nullptr;  ///< 实况 setter 的目标窗口（`Application` 构造后回填）。
    std::shared_ptr<au::Dialog> dialog;  ///< 对话框宿主（常驻树内，内容按当前档重建）。
};

/// @brief 当前实况主题：与 `Window::resolved_root_theme()` 同式的载体侧重算（供对话框工厂形参）。
///
/// 工厂 `alert` / `confirm` 按形参取主题、不读 ctx（见 specification/04-widget.md §4.1），故载体必须
/// 自己给出「当前档」。这里 `style` 为空时取探测档、`scheme` 原样透传，`Unspecified` 由 `from_style`
/// 按明色列解出——与根注入的三级优先链逐值同解，两处若分叉即为本载体的判读点之一。
[[nodiscard]] auto live_theme(const Carrier &carrier) -> au::Theme {
    return au::Theme::from_style(carrier.style.value_or(au::Theme::default_style()), carrier.scheme);
}

/// @brief 套装控件：五控件 + 标题与正文，全部不显式设色（取主题角色）。
///
/// 字体族与字号取 `type_scale`——`Text` 目前不自动读字阶（消费面现状见
/// specification/07-environment-modifier.md §5.1），故由载体显式喂进该档的标定值。
/// @param t 本格的主题（标定档）。
/// @return 自上而下的子节点向量。
[[nodiscard]] auto control_kit(const au::Theme &t) -> std::vector<au::Node> {
    auto headline = au::Text{"Primary label"};
    headline.font_size(t.type_scale.title_medium.size_pt).bold();
    auto caption = au::Text{"The quick brown fox 0123456789"};
    caption.family(t.type_scale.body_medium.family).font_size(t.type_scale.body_medium.size_pt);

    auto slide = au::Slider{au::State<double>{0.35}};
    slide.set_range(0.0, 1.0);
    auto picker = au::Dropdown{std::vector<std::string>{"One", "Two", "Three"}, 1};

    return {
        std::move(headline),
        au::Divider{},
        au::Row{au::RowProps{.children = {au::Button{"Alpha"}, au::Checkbox{au::State<bool>{true}}}, .gap = 8.0F}},
        std::move(slide),
        std::move(picker),
        std::move(caption),
    };
}

/// @brief 一格（或一张大图）：标定主题注入 + 套装控件 + 卡片外框。
///
/// 卡片外框（底色 / 圆角 / 描边 / 内边距）由载体按该格已知的主题值显式取用——库内没有「卡片」控件，
/// 判据面是套装控件的 ctx 读取；外框只是让十格边界可读。
/// @param t 本格的主题。
/// @param wide 是否大图档（宽栏 + 令牌数值表 + 「Back」按钮）。
/// @param on_toggle 缩放按钮回调。
/// @return 已包进 `ThemeProvider` 的格节点。
[[nodiscard]] auto make_cell(const au::Theme &t, bool wide, std::function<void()> on_toggle) -> au::Node {
    auto title = au::Text{style_label(t.style) + " / " + scheme_label(t.scheme)};
    title.font_size(12.0F).bold();
    auto toggle = au::Button{wide ? "Back" : "Zoom"};
    toggle.set_on_click(std::move(on_toggle));

    auto kids = control_kit(t);
    kids.insert(kids.begin(), au::Row{au::RowProps{.children = {std::move(title), std::move(toggle)}, .gap = 8.0F}});
    if (wide) {
        kids.emplace_back(au::Divider{});
        for (const auto &line : token_lines(t)) {
            auto readout = au::Text{line};
            readout.font_size(11.0F).family("sans-serif");
            kids.emplace_back(std::move(readout));
        }
    }

    auto body = au::Column{au::ColumnProps{.children = std::move(kids), .gap = wide ? 8.0F : 6.0F}};
    body.set_modifier(au::Modifier{}
                          .width(wide ? AURORA_WIDE_CELL_WIDTH : AURORA_CELL_WIDTH)
                          .padding(wide ? t.spacing.large : t.spacing.medium)
                          .background(t.roles.surface_container, t.shapes.small)
                          .border(t.controls.border_thickness, t.roles.outline_variant));
    return au::ThemeProvider{t, std::move(body)};
}

/// @brief 缩放条件句柄（十格各一枚，初始全假）。
[[nodiscard]] auto make_zoom_flags() -> std::vector<std::shared_ptr<au::State<bool>>> {
    std::vector<std::shared_ptr<au::State<bool>>> flags;
    flags.reserve(AURORA_STYLES.size() * AURORA_CELL_SCHEMES.size());
    for (std::size_t i = 0; i < AURORA_STYLES.size() * AURORA_CELL_SCHEMES.size(); ++i) {
        flags.emplace_back(std::make_shared<au::State<bool>>(false));
    }
    return flags;
}

/// @brief 点格进大图 / 回全览的回调。
/// @param carrier 共享状态。
/// @param index 格序号（0..9，行 = 风格档、列 = 明暗档）。
/// @param enlarge 进大图为真，回全览为假。
[[nodiscard]] auto toggle_action(const std::shared_ptr<Carrier> &carrier, std::size_t index, bool enlarge) {
    return [carrier, index, enlarge]() -> void {
        if (enlarge) {
            carrier->overview.set(false);
            carrier->zoom.at(index)->set(true);
        } else {
            carrier->overview.set(true);
            carrier->zoom.at(index)->set(false);
        }
        AURORA_LOG_INFO("demo", "[demo_theme_styles] view=", enlarge ? "enlarged" : "overview", " index=", index);
    };
}

/// @brief 十格全览：五行 × 每行明暗两格。
[[nodiscard]] auto make_overview(const ThemeGrid &themes, const std::shared_ptr<Carrier> &carrier) -> au::Node {
    std::vector<au::Node> rows;
    rows.reserve(AURORA_STYLES.size());
    for (std::size_t s = 0; s < AURORA_STYLES.size(); ++s) {
        const std::size_t base = s * AURORA_CELL_SCHEMES.size();
        std::vector<au::Node> cells;
        cells.reserve(AURORA_CELL_SCHEMES.size());
        for (std::size_t c = 0; c < AURORA_CELL_SCHEMES.size(); ++c) {
            cells.emplace_back(make_cell(themes.at(s).at(c), false, toggle_action(carrier, base + c, true)));
        }
        rows.emplace_back(au::Row{au::RowProps{.children = std::move(cells), .gap = 12.0F}});
    }
    return au::Column{au::ColumnProps{.children = std::move(rows), .gap = 12.0F}};
}

/// @brief 一行下拉的选项文本（按档位数组顺序逐项标签化，避免手写死顺序）。
/// @tparam T 档位枚举。
/// @param values 档位数组。
/// @param label 档位 → 文本的标签函数。
/// @return 与 values 同序的选项向量。
template <typename T>
[[nodiscard]] auto option_labels(const std::vector<T> &values, auto (*label)(T)->std::string)
    -> std::vector<std::string> {
    std::vector<std::string> names;
    names.reserve(values.size());
    for (const T value : values) {
        names.emplace_back(label(value));
    }
    return names;
}

/// @brief 页面根：实况区（探测读数 + 两个下拉 + 实况套装）在上，滚动视口内是全览与十张大图。
[[nodiscard]] auto make_page(const ThemeGrid &themes, const std::shared_ptr<Carrier> &carrier) -> au::Node {
    const au::Platform probe = au::platform();

    auto title = au::Text{"Theme styles · side-by-side carrier"};
    title.font_size(18.0F).bold();
    std::vector<au::Node> kids{std::move(title)};

    const std::string line1 = "probe  " + kind_label(probe.kind) + " / " + surface_label(probe.surface) +
                              " / desktop=" + desktop_label(au::desktop_environment());
    const std::string line2 = "default=" + style_label(au::Theme::default_style()) +
                              "  os=" + scheme_label(au::os_color_scheme()) +
                              "  setter=" + (carrier->style ? style_label(*carrier->style) : std::string("(unset)"));
    // 三行读数只在启动时解算一次（页面不随下拉重建），故标 boot；切换后的动态证据是
    // 下方实况套装与整窗底色的实际变色，以及 stderr 上每次切换打的日志行。
    const std::string line3 = "boot readings above; live strip below must repaint";
    for (const auto &line : {line1, line2, line3}) {
        auto readout = au::Text{line};
        readout.font_size(11.0F).family("sans-serif");
        kids.emplace_back(std::move(readout));
    }
    kids.emplace_back(au::Divider{});

    // 风格档下拉：只钉风格轴，明暗继续走 OS 档（与 `Window::set_theme_style` 的语义同形）。
    // 初始选中项必须是「当前解算出的风格」而不是下标 0——未显式设档时根主题走 `default_style()`，
    // 若固定 0 会让下拉显示 Material 而实况区是 Windows，读数自相矛盾。
    const au::ThemeStyle resolved_style = carrier->style.value_or(au::Theme::default_style());
    const std::vector<au::ThemeStyle> styles(AURORA_STYLES.begin(), AURORA_STYLES.end());
    int initial_style_index = 0;
    for (std::size_t i = 0; i < styles.size(); ++i) {
        if (styles.at(i) == resolved_style) {
            initial_style_index = static_cast<int>(i);
        }
    }
    auto style_picker = au::Dropdown{option_labels(styles, &style_label), initial_style_index};
    // 该回调转入 std::function，本检查对任何可调用对象一律判为「不应抛出」；回调里的
    // std::string / std::vector 只在极端 OOM 下抛，示例载体不做吞异常的边界包装（与 main 同口径）。
    // NOLINTBEGIN(bugprone-exception-escape)
    style_picker.set_on_change([carrier, styles](int index) -> void {
        if (index < 0 || static_cast<std::size_t>(index) >= styles.size()) {
            return;
        }
        const au::ThemeStyle s = styles.at(static_cast<std::size_t>(index));
        carrier->style = s;
        if (carrier->window != nullptr) {
            carrier->window->set_theme_style(s);
        }
        AURORA_LOG_INFO("demo", "[demo_theme_styles] set_theme_style(", style_label(s), ")");
    });
    // NOLINTEND(bugprone-exception-escape)

    // 明暗下拉：三档含 `Unspecified`，用来目视「Unspecified ≡ 明色」这条不折平的口径。
    const std::vector<au::ColorScheme> schemes(AURORA_SCHEME_CHOICES.begin(), AURORA_SCHEME_CHOICES.end());
    const int initial_scheme_index = carrier->scheme == au::ColorScheme::Dark    ? 2
                                     : carrier->scheme == au::ColorScheme::Light ? 1
                                                                                 : 0;
    auto scheme_picker = au::Dropdown{option_labels(schemes, &scheme_label), initial_scheme_index};
    // 同风格下拉：回调转入 std::function，本检查对任何可调用对象一律判为「不应抛出」。
    // NOLINTBEGIN(bugprone-exception-escape)
    scheme_picker.set_on_change([carrier, schemes](int index) -> void {
        if (index < 0 || static_cast<std::size_t>(index) >= schemes.size()) {
            return;
        }
        const au::ColorScheme c = schemes.at(static_cast<std::size_t>(index));
        carrier->scheme = c;
        if (carrier->window != nullptr) {
            carrier->window->set_os_color_scheme(c);
        }
        AURORA_LOG_INFO("demo", "[demo_theme_styles] set_os_color_scheme(", scheme_label(c), ")");
    });
    // NOLINTEND(bugprone-exception-escape)

    kids.emplace_back(
        au::Row{au::RowProps{.children = {std::move(style_picker), std::move(scheme_picker)}, .gap = 12.0F}});

    auto open_dialog = au::Button{"Confirm dialog"};
    open_dialog.set_on_click([carrier]() -> void {
        const au::Theme t = live_theme(*carrier);
        auto body = au::confirm(
            "Dialog conventions", "Yes / No order follows conventions.dialog_button_order.",
            [carrier](bool accepted) -> void {
                AURORA_LOG_INFO("demo", "[demo_theme_styles] dialog result=", accepted ? "yes" : "no");
                carrier->dialog->close();
            },
            t);
        // 卡面由载体自绘：本库 `Dialog` 只铺遮罩 + 居中内容，不画卡（见 widget/dialog.h 的几何契约，
        // 卡片宽与底色归宿主）。这里顺带把 `roles.surface` / `shapes.medium` 两枚令牌用上，
        // 让「按钮排布」在深色遮罩上有可比对的底面。修饰链挂在控件上，故外面包一层 Column 承载。
        auto card = au::Column{au::ColumnProps{.children = {std::move(body)}}};
        card.set_modifier(au::Modifier{}
                              .padding(t.spacing.large + t.spacing.medium)
                              .background(t.roles.surface, t.shapes.medium)
                              .border(t.controls.border_thickness, t.roles.outline_variant));
        carrier->dialog->set_content(au::Node{std::move(card)});
        carrier->dialog->show();
        AURORA_LOG_INFO("demo", "[demo_theme_styles] dialog opened: style=", style_label(t.style),
                        " scheme=", scheme_label(t.scheme), " order=", order_label(t.conventions.dialog_button_order));
    });
    kids.emplace_back(std::move(open_dialog));
    kids.emplace_back(au::Divider{});

    // 实况套装：不给任何显式色，控件应随根注入变色。
    auto live_slide = au::Slider{au::State<double>{0.5}};
    live_slide.set_range(0.0, 1.0);
    kids.emplace_back(au::Row{
        au::RowProps{.children = {au::Button{"Live button"}, au::Checkbox{au::State<bool>{true}}}, .gap = 8.0F}});
    kids.emplace_back(std::move(live_slide));
    kids.emplace_back(au::Divider{});

    auto caption = au::Text{"Grid: 5 styles x 2 schemes -- click Zoom"};
    caption.font_size(12.0F).bold();

    std::vector<au::Node> scrollable;
    scrollable.emplace_back(au::Show{carrier->overview, make_overview(themes, carrier)});
    for (std::size_t s = 0; s < AURORA_STYLES.size(); ++s) {
        for (std::size_t c = 0; c < AURORA_CELL_SCHEMES.size(); ++c) {
            const std::size_t index = (s * AURORA_CELL_SCHEMES.size()) + c;
            scrollable.emplace_back(au::Show{
                *carrier->zoom.at(index), make_cell(themes.at(s).at(c), true, toggle_action(carrier, index, false))});
        }
    }
    auto body = au::Column{au::ColumnProps{.children = std::move(scrollable), .gap = 12.0F}};
    kids.emplace_back(std::move(caption));
    auto scroller = au::Scroll{au::ScrollProps{.child = std::move(body)}};
    scroller.set_modifier(au::Modifier{}.height(AURORA_GRID_VIEWPORT_HEIGHT));
    kids.emplace_back(std::move(scroller));

    auto page = au::Column{au::ColumnProps{.children = std::move(kids), .gap = 8.0F}};
    page.set_modifier(au::Modifier{}.padding(AURORA_PAGE_PADDING));
    return page;
}

/// @brief 启动前一致性：把十格主题的 `style` / `scheme` 字段与解算入参对齐，供读数核对。
[[nodiscard]] auto build_themes() -> const ThemeGrid & {
    static ThemeGrid themes;
    for (std::size_t s = 0; s < AURORA_STYLES.size(); ++s) {
        for (std::size_t c = 0; c < AURORA_CELL_SCHEMES.size(); ++c) {
            themes.at(s).at(c) = au::Theme::from_style(AURORA_STYLES.at(s), AURORA_CELL_SCHEMES.at(c));
        }
    }
    return themes;
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::enable_dpi_awareness();
    au::init_console();

    // 760 宽：让三行启动读数与图注在不折行的前提下完整可见（660 宽时 11pt 读数被视口裁掉）。
    constexpr float w = 760.0F;
    constexpr float h = 800.0F;

    auto carrier = std::make_shared<Carrier>();
    carrier->zoom = make_zoom_flags();
    carrier->scheme = au::os_color_scheme();
    carrier->dialog = std::make_shared<au::Dialog>();

    const ThemeGrid &themes = build_themes();
    // 层序：背板（读根注入底色）→ 页面 → 对话框（模态遮罩常驻，初始关闭态尺寸为 0）。
    au::Node root =
        au::Stack{std::vector<au::Node>{LiveBackdrop{}, make_page(themes, carrier), au::Node{carrier->dialog}}};

    au::WindowOptions wopts;
    wopts.size = au::Size{.width = w, .height = h};
    wopts.title = "Theme Styles · Aurora Demo";

    auto win_res = au::create_native_window(wopts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[demo_theme_styles] window creation failed: ", win_res.error().message,
                         ", falling back to headless render");
        std::error_code ec;
        std::filesystem::create_directories("build", ec);
        au::Scene scene{root};
        const auto r = scene.render_to_png("build/demo_theme_styles.png", static_cast<int>(w), static_cast<int>(h));
        if (r) {
            AURORA_LOG_INFO("demo", "[demo_theme_styles] rendered build/demo_theme_styles.png");
        } else {
            AURORA_LOG_ERROR("demo", "[demo_theme_styles] headless render failed: ", r.error().message);
        }
        return 0;
    }

    auto win = std::move(win_res.value());
    au::Application app{au::Scene{root}, std::move(win), wopts};
    carrier->window = app.window();  // 下拉回调的目标窗口（run() 之内才派发点击，回填时机安全）
#ifdef AURORA_BUILD_INSPECTOR_SERVER
    // 远程检视 opt-in：设 AURORA_INSPECTOR_PORT 才起服务，未设置时零成本。
    // 手工用例用它做「坐标即所见」的复核（/api/debug/pick 读布局盒、/api/input/click 驱动切换）。
    auto inspector = start_demo_inspector(
        [&app]() -> au::Node { return au::Node{app.scene().root_node()}; },
        [&app]() -> au::Surface * { return app.window() != nullptr ? &app.window()->surface() : nullptr; });
#endif
    AURORA_LOG_INFO("demo", "[demo_theme_styles] window shown (close window to exit): default_style=",
                    style_label(au::Theme::default_style()), " scheme=", scheme_label(carrier->scheme));
    app.run();
    return 0;
}
