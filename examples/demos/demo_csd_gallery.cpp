// CSD（客户端自绘装饰）全景 demo：单一控制窗 + 按所选策略重建的预览窗。
//
// 控制窗（系统装饰、易关闭）列出 WindowStyleOptions::decoration 的全部五档策略按钮，
// 点选后重建一个预览窗按该档位运行，并实时显示其 content_inset()：
//   Auto        —— 优先系统 SSD；Win32 下即 DWM 系统标题栏，host 自绘不激活
//   ServerSide  —— 强制系统标题栏（KDE / Win32 DWM），无 host 自绘
//   ClientSide  —— 强制 host 自绘 CSD 标题栏（本次 的 Win32 宿主路径）：
//                  预览窗内置按钮可运行时循环切换 from_preset 的**全部六档**预设
//                  （Adwaita / Windows / Mac 三套视觉语言 × 明暗两档）/ 切换窗框投影 /
//                  放置或清除标题栏图标（演示 set_title_bar_style / set_window_frame_style /
//                  set_title_bar_icon 三个运行期 setter）
//   Borderless  —— 无标题栏、保留八向缩放边框；预览窗嵌入控件级 TitleBar widget
//                  （经 WindowChrome 程序化移动 / 最小化 / 最大化 / 关闭，演示 app-drawn 互补路径）
//   Frameless   —— 完全无装饰；同样嵌入控件级 TitleBar 供应用自绘全部 UI
//
// 运行：
//   cmake --build build --target demo_csd_gallery && ./build/demo_csd_gallery
// ⚠️ host 自绘（ClientSide）与 WM_NCHITTEST 热区 / WM_NCCALCSIZE 吞框 / 释放触发等
//    实现仅 Win32 / X11 后端生效；其余平台对应 setter 为空实现（窗口仍可开，只是系统装饰）。
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/aurora.h"
#include "aurora/core/image.h"
#include "aurora/widget/title_bar.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window.h"
#include "aurora/window/window_frame_style.h"
#include "demo_common.h"

namespace {

/// @brief 策略档位中文标签（各分支均为字面量，返回视图避免每次调用都构造 std::string）。
[[nodiscard]] auto policy_label(au::DecorationPolicy p) -> std::string_view {
    switch (p) {
        case au::DecorationPolicy::Auto:
            return "Auto";
        case au::DecorationPolicy::ServerSide:
            return "ServerSide";
        case au::DecorationPolicy::ClientSide:
            return "ClientSide";
        case au::DecorationPolicy::Borderless:
            return "Borderless";
        case au::DecorationPolicy::Frameless:
            return "Frameless";
    }
    return "?";
}

/// @brief 策略档位的使用要点（预览窗顶部说明）；同样返回字面量视图。
[[nodiscard]] auto policy_description(au::DecorationPolicy p) -> std::string_view {
    switch (p) {
        case au::DecorationPolicy::Auto:
            return "Auto: prefers the system title bar (SSD). On Win32 the DWM bar is shown and the host "
                   "self-draw path is inactive; on compositors without SSD it falls back to ClientSide.";
        case au::DecorationPolicy::ServerSide:
            return "ServerSide: forces the OS-provided title bar (KDE / Win32 DWM). No host self-draw.";
        case au::DecorationPolicy::ClientSide:
            return "ClientSide: the host draws the title bar itself (Win32 WM_NCCALCSIZE swallows the non-client "
                   "area, WM_NCHITTEST reports hot zones). Use the buttons below to retheme it at runtime.";
        case au::DecorationPolicy::Borderless:
            return "Borderless: no title bar but keeps the eight-direction resize frame. Drag the widget bar "
                   "above to move; hold Alt/Meta and drag anywhere as a fallback.";
        case au::DecorationPolicy::Frameless:
            return "Frameless: fully undecorated. The widget-level TitleBar above is the only chrome; the app "
                   "owns all UI and drives window state via WindowChrome.";
    }
    return "";
}

/// @brief 演示用窗口图标（内联 SVG 矢量图，经内置光栅器解码）。
///
/// ⚠️ 内置 SVG 光栅器仅支持 rect/circle/ellipse/line/polygon/polyline 基元，**不支持 `<path>`**
/// （见 src/aurora/core/image_svg.cpp 顶部注释）—— 微软 Fluent 官方图标库的资源几乎全是 `<path>`，
/// 直接下载会渲染为空图。故此处用基元手绘一枚「带标题栏的窗口」图标（同风格、矢量、零外部依赖），
/// 既替换掉原先的占位色块，也演示 `set_title_bar_icon` 的真实矢量图标路径。
/// 标题栏图标槽仅 16dp，故先光栅化为 64px 再交给绘制层缩放进槽，保证小尺寸下仍清晰。
[[nodiscard]] auto make_demo_icon() -> std::shared_ptr<au::Image> {
    constexpr auto window_icon_svg = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">)"
                                     R"(<rect x="3" y="4" width="18" height="16" rx="2" fill="#2563EB"/>)"
                                     R"(<line x1="3" y1="9" x2="21" y2="9" stroke="#FFFFFF" stroke-width="2"/>)"
                                     R"(<rect x="6" y="12" width="12" height="6" rx="1" fill="#BFDBFE"/>)"
                                     R"(</svg>)";
    const std::string doc{window_icon_svg};
    auto r = au::detail::load_image_svg(std::vector<std::uint8_t>{doc.begin(), doc.end()}, 64, 64);
    if (!r) {
        return nullptr;  // 光栅失败时退化为「无图标」（开关仍可用，不崩）
    }
    return std::make_shared<au::Image>(std::move(r.value()));
}

/// @brief 主题预设序号 → 枚举 + 名称（ClientSide 预览窗循环切换用）。
///
/// 六档全覆盖（三套视觉语言 × 明暗两档）：漏掉 Mac 两档会让 `set_title_bar_style` 的
/// 红绿灯分支在本 demo 里永远走不到，回归时只能靠单测兜底。
[[nodiscard]] auto theme_preset(int idx) -> au::CsdPreset {
    switch (idx % 6) {
        case 0:
            return au::CsdPreset::AdwaitaDark;
        case 1:
            return au::CsdPreset::AdwaitaLight;
        case 2:
            return au::CsdPreset::WindowsDark;
        case 3:
            return au::CsdPreset::WindowsLight;
        case 4:
            return au::CsdPreset::MacDark;
        default:
            return au::CsdPreset::MacLight;
    }
}

[[nodiscard]] auto theme_name(int idx) -> std::string_view {
    switch (idx % 6) {
        case 0:
            return "AdwaitaDark";
        case 1:
            return "AdwaitaLight";
        case 2:
            return "WindowsDark";
        case 3:
            return "WindowsLight";
        case 4:
            return "MacDark";
        default:
            return "MacLight";
    }
}

/// @brief 经 id 解析预览窗对象（回调触发时窗可能尚未登记完，故 nullptr 守卫）。
[[nodiscard]] auto resolve_preview(const au::Application &app, const std::shared_ptr<au::WindowId> &self_id)
    -> au::Window * {
    auto *host = app.window_host(*self_id);
    if (host == nullptr || !host->has_window()) {
        return nullptr;
    }
    return host->window();
}

/// @brief 预览窗 UI 前向声明（open_preview 在定义处需引用）。
[[nodiscard]] auto build_preview_ui(au::Application &app, const std::shared_ptr<au::WindowId> &self_id,
                                    au::DecorationPolicy policy,
                                    const std::shared_ptr<au::State<au::LocalizedString>> &preview_info,
                                    const std::shared_ptr<au::State<au::LocalizedString>> &note) -> au::Node;

/// @brief 重建预览窗：关闭旧的（若有），按所选策略新建并登记到 Application。
auto open_preview(au::Application &app, au::DecorationPolicy p, const std::shared_ptr<au::WindowId> &preview_id,
                  const std::shared_ptr<au::State<au::LocalizedString>> &preview_info,
                  const std::shared_ptr<au::State<au::LocalizedString>> &note, au::DecorationPolicy &preview_policy)
    -> void {
    if (*preview_id != au::AURORA_INVALID_WINDOW_ID) {
        app.close_window(*preview_id);
    }
    *preview_id = au::AURORA_INVALID_WINDOW_ID;
    preview_policy = p;

    au::WindowOptions po;
    // 520dp 定高：预览内容（标题 + 说明 + 读数 + 三钮 + 状态 + 关闭钮 ≈ 505dp）完整呈现，
    // 不给「内容溢出裁断」留观感歧义——白条修复验证要看的是几何贴合，不是溢出裁剪。
    po.size = au::Size{.width = 560.0F, .height = 520.0F};
    po.title = std::string(policy_label(p)) + " preview";
    po.style.decoration = p;
    po.role = au::WindowRole::Auxiliary;
    auto wr = au::create_native_window(po);
    if (!wr) {
        note->set(au::LocalizedString{"preview creation failed: " + wr.error().message});
        return;
    }
    const auto sid = std::make_shared<au::WindowId>(au::AURORA_INVALID_WINDOW_ID);
    *sid = app.open_window(std::move(wr.value()), au::Scene{build_preview_ui(app, sid, p, preview_info, note)}, po);
    *preview_id = *sid;
    preview_info->set(au::LocalizedString{std::string(policy_label(p)) + " | opening…"});
}

/// @brief 预览窗 UI：展示策略说明 + 档位专属的 CSD 交互（host 自绘定制 / 控件级 TitleBar）。
[[nodiscard]] auto build_preview_ui(au::Application &app, const std::shared_ptr<au::WindowId> &self_id,
                                    au::DecorationPolicy policy,
                                    const std::shared_ptr<au::State<au::LocalizedString>> &preview_info,
                                    const std::shared_ptr<au::State<au::LocalizedString>> &note) -> au::Node {
    std::vector<au::Node> kids;
    kids.emplace_back(GradientTitle{std::string(policy_label(policy)) + " — preview"});
    kids.emplace_back(gap(10));
    kids.emplace_back(au::Text{std::string(policy_description(policy))});
    kids.emplace_back(gap(10));
    kids.emplace_back(au::Text{au::TextProps{.content = *preview_info}});
    kids.emplace_back(gap(10));

    if (policy == au::DecorationPolicy::ClientSide) {
        // host 自绘路径：运行时经三个 setter 改外观，下一帧 present 即重绘装饰带。
        auto theme_idx = std::make_shared<int>(0);
        auto shadow_on = std::make_shared<bool>(true);
        auto icon_on = std::make_shared<bool>(false);

        au::Button theme_btn{au::ButtonProps{.label = "Cycle title-bar theme (host self-draw)"}};
        theme_btn.on_click = [self_id, &app, theme_idx, note]() -> void {
            *theme_idx = (*theme_idx + 1) % 6;
            const std::string label{theme_name(*theme_idx)};
            if (auto *w = resolve_preview(app, self_id)) {
                w->set_title_bar_style(au::TitleBarStyle::from_preset(theme_preset(*theme_idx)));
                note->set(au::LocalizedString{std::string{"host title-bar theme → "} + label});
                w->set_title(label);
            }
        };
        kids.emplace_back(std::move(theme_btn));
        kids.emplace_back(gap(6));

        au::Button shadow_btn{au::ButtonProps{.label = "Toggle window frame shadow"}};
        shadow_btn.on_click = [self_id, &app, shadow_on, note]() -> void {
            *shadow_on = !*shadow_on;
            if (const auto *w = resolve_preview(app, self_id)) {
                w->set_window_frame_style(au::WindowFrameStyle{.shadow_enabled = *shadow_on, .shadow_margin_dp = 10});
                note->set(au::LocalizedString{std::string("window frame shadow → ") + (*shadow_on ? "on" : "off")});
            }
        };
        kids.emplace_back(std::move(shadow_btn));
        kids.emplace_back(gap(6));

        au::Button icon_btn{au::ButtonProps{.label = "Toggle title-bar icon"}};
        icon_btn.on_click = [self_id, &app, icon_on, note]() -> void {
            *icon_on = !*icon_on;
            if (const auto *w = resolve_preview(app, self_id)) {
                w->set_title_bar_icon(*icon_on ? make_demo_icon() : nullptr);
                note->set(au::LocalizedString{std::string("title-bar icon → ") + (*icon_on ? "set" : "cleared")});
            }
        };
        kids.emplace_back(std::move(icon_btn));
        kids.emplace_back(gap(10));
        kids.emplace_back(au::Text{au::TextProps{.content = *note}});
    } else if (policy == au::DecorationPolicy::Borderless || policy == au::DecorationPolicy::Frameless) {
        // 控件级 TitleBar widget：应用自绘标题栏，经 WindowChrome 驱动窗口状态
        // （headless 下 chrome 缺失，交互安全 no-op）。
        au::TitleBar tb;
        tb.set_title(std::string(policy_label(policy)) + " (widget)")
            .set_subtitle("app-drawn via WindowChrome")
            .add_action({.label = "Info", .on_click = []() -> void { AURORA_LOG_INFO("demo", "csdg: info clicked"); }});
        kids.emplace_back(std::move(tb));
        kids.emplace_back(gap(8));
        kids.emplace_back(au::Text{"Drag the bar above to move; its buttons minimize / maximize / close."});
    } else if (policy == au::DecorationPolicy::Auto) {
        kids.emplace_back(
            au::Text{"The OS title bar above is the system frame. Switch to ClientSide to "
                     "see the host self-draw path on this platform."});
    } else {
        kids.emplace_back(au::Text{"The OS title bar above is forced by ServerSide. No host self-draw."});
    }

    kids.emplace_back(gap(14));
    au::Button close_btn{au::ButtonProps{.label = "Close this preview"}};
    close_btn.on_click = [self_id, &app, note]() -> void {
        app.close_window(*self_id);
        note->set(au::LocalizedString{"preview closed"});
    };
    kids.emplace_back(std::move(close_btn));

    return au::Column{au::ColumnProps{.children = std::move(kids)}};
}

/// @brief 控制窗 UI：五档策略按钮 + 实时 content_inset 读数 + 退出。
[[nodiscard]] auto build_main_ui(au::Application &app, const std::shared_ptr<au::WindowId> &preview_id,
                                 const std::shared_ptr<au::State<au::LocalizedString>> &preview_info,
                                 const std::shared_ptr<au::State<au::LocalizedString>> &note,
                                 au::DecorationPolicy &preview_policy) -> au::Node {
    const auto add_policy = [&](au::DecorationPolicy p, std::vector<au::Node> &kids) -> void {
        au::Button b{au::ButtonProps{.label = std::string(policy_label(p)) + " preview"}};
        b.on_click = [&app, preview_id, preview_info, note, &preview_policy, p]() -> void {
            open_preview(app, p, preview_id, preview_info, note, preview_policy);
        };
        kids.emplace_back(std::move(b));
        kids.emplace_back(gap(6));
    };

    std::vector<au::Node> kids;
    kids.emplace_back(GradientTitle{"CSD Gallery"});
    kids.emplace_back(gap(8));
    kids.emplace_back(
        au::Text{"Pick a decoration policy to open a live preview window. The preview "
                 "demonstrates the host self-drawn CSD (ClientSide) or the app-drawn "
                 "TitleBar widget (Borderless / Frameless)."});
    kids.emplace_back(gap(12));
    add_policy(au::DecorationPolicy::Auto, kids);
    add_policy(au::DecorationPolicy::ServerSide, kids);
    add_policy(au::DecorationPolicy::ClientSide, kids);
    add_policy(au::DecorationPolicy::Borderless, kids);
    add_policy(au::DecorationPolicy::Frameless, kids);
    kids.emplace_back(gap(8));
    kids.emplace_back(au::Text{au::TextProps{.content = *preview_info}});
    kids.emplace_back(gap(8));

    au::Button quit_btn{au::ButtonProps{.label = "Quit"}};
    quit_btn.on_click = [&app]() -> void { app.quit(); };
    kids.emplace_back(std::move(quit_btn));

    return au::Column{au::ColumnProps{.children = std::move(kids)}};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::enable_dpi_awareness();  // 必须在建窗前，否则 DPI 感知失败（scale=1.0，高分屏发虚）
    au::init_console();

    auto preview_id = std::make_shared<au::WindowId>(au::AURORA_INVALID_WINDOW_ID);
    auto preview_policy = au::DecorationPolicy::Auto;
    auto preview_info = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Preview: none"});
    const auto note =
        std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"Pick a policy above to open a preview."});

    au::WindowOptions opts;
    opts.size = au::Size{.width = 480.0F, .height = 560.0F};
    opts.title = "Aurora CSD Gallery";
    auto win_res = au::create_native_window(opts);
    if (!win_res) {
        AURORA_LOG_ERROR("demo", "[csdg] main window creation failed: ", win_res.error().message);
        return -1;
    }

    au::Application app{au::Scene{au::Column{}}, std::move(win_res.value()), opts};
    app.scene().root_node() = build_main_ui(app, preview_id, preview_info, note, preview_policy);
    app.focus().set_root(&app.scene().root());

    // 每帧把当前预览窗的 content_inset 同步进 preview_info（控制窗与预览窗共用该 State）。
    app.set_on_frame([&app, preview_id, preview_info, &preview_policy]() -> void {
        if (*preview_id == au::AURORA_INVALID_WINDOW_ID) {
            preview_info->set(au::LocalizedString{"Preview: none"});
            return;
        }
        auto *host = app.window_host(*preview_id);
        if (host == nullptr || !host->has_window()) {
            preview_info->set(au::LocalizedString{std::string(policy_label(preview_policy)) + " | opening…"});
            return;
        }
        const auto ins = host->window()->content_inset();
        const std::string s = std::string(policy_label(preview_policy)) + " | inset top=" + std::to_string(ins.top) +
                              " left=" + std::to_string(ins.left) + " right=" + std::to_string(ins.right) +
                              " bottom=" + std::to_string(ins.bottom);
        preview_info->set(au::LocalizedString{s});
    });

    AURORA_LOG_INFO("demo", "[csdg] gallery shown (close the main window to exit)");
    app.run();
    return 0;
}
