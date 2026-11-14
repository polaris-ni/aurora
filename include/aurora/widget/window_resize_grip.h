#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/widget/widget.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_chrome.h"
#include "aurora/window/window_state.h"

namespace aurora {

/// @brief 窗口边缘带默认厚度（dp）：Windows 10/11 的不可见缩放边框约 4–8 dp、
///        GNOME 的自绘装饰取 6，取中位数。角隅按两倍带宽判定（见 `WindowResizeGrip`）。
inline constexpr float AURORA_WINDOW_RESIZE_BAND = 6.0F;

/// @brief 无边框窗口的拖边缩放（specification/04-widget.md 容器族）：
///        单子容器，把「在窗口边缘按下」翻译成 `WindowChrome::begin_resize(edge)`。
///
/// 用法：用它包住整个自绘 chrome（典型形态 `WindowResizeGrip{ Column{ TitleBar{…}, 内容 } }`）。
/// 边缘带内按下即发起系统级缩放并消费该事件，带外一律透传给子件；本控件自身不绘制任何像素。
///
/// 手势必须在 Press 的派发栈内同步发起（Wayland 的 `xdg_toplevel.resize` 带 serial，过期即被
/// 合成器丢弃），故不采用「先记录坐标、等 Move 再判断」的写法——那套在 Wayland 上必然失效。
class WindowResizeGrip : public SingleChild {
  public:
    /// @brief 构造：装载内容子树与边缘带厚度。
    /// @param child 被包裹的内容（布局取满约束，落在窗口边缘带之内）。
    /// @param band 边缘带厚度（dp；非正值等价于关闭该手势）。
    explicit WindowResizeGrip(Node child, float band = AURORA_WINDOW_RESIZE_BAND)
        : SingleChild(std::move(child)), band_(band) {}

    /// @brief 控件类型名（结构快照 JSON 用）。
    /// @return 字面量 `"WindowResizeGrip"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "WindowResizeGrip"; }

    /// @brief 静态自描述：一个 `band` 属性行、无事件、单子策略。
    /// @return `WindowResizeGrip` 的 `WidgetDescriptor`（与实例侧 `describe()` 同构）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "WindowResizeGrip",
            .properties =
                {
                    {.name = "band",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "Edge band thickness in dp; 0 disables the gesture",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {},
            .children_policy = "single",
            .examples = {R"(au::WindowResizeGrip{au::Column{})"},
        };
    }

    /// @brief 实例侧自描述，直接转调 `describe_static()`。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 本控件无绑定的响应式信号，空实现（不参与信号订阅收集）。
    auto collect_signals(StateSink & /*out*/) -> void override {}

    /// @brief 本控件自身零绘制（只委托子件），可安全缓存 Display List。
    /// @return 恒为 true。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return true; }

    /// @brief 边缘带内按下即接管（自身入命中链，不需要点击语义也能被判定为命中）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 设置边缘带厚度。
    /// @param band 厚度（dp；非正值等价于关闭该手势）。
    /// @return 自身引用（链式调用）。
    auto set_band(float band) -> WindowResizeGrip & {
        band_ = band;
        mark_needs_paint();
        return *this;
    }

    /// @brief 读取边缘带厚度。
    /// @return 构造或 `set_band` 传入的值（未按窗口尺寸钳制，钳制在使用点发生）。
    [[nodiscard]] auto band() const -> float { return band_; }

    /// @brief 悬停光标（按当前指针所在的边档给出，见 `cursor_shape` 的实现说明）。
    /// @return 带内返回对应方向的光标形状；带外返回 `nullopt`（交由命中链更深处与祖先决定）。
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override {
        switch (hover_edge_) {
            case WindowResizeEdge::Top:
            case WindowResizeEdge::Bottom:
                return CursorShape::ResizeNS;
            case WindowResizeEdge::Left:
            case WindowResizeEdge::Right:
                return CursorShape::ResizeEW;
            case WindowResizeEdge::TopLeft:
            case WindowResizeEdge::BottomRight:
                return CursorShape::ResizeNWSE;
            case WindowResizeEdge::TopRight:
            case WindowResizeEdge::BottomLeft:
                return CursorShape::ResizeNESW;
            case WindowResizeEdge::None:
                break;
        }
        return std::nullopt;
    }

    /// @brief 指针交互：边缘带内按下（左键）发起系统级缩放并消费。
    /// @param e 鼠标事件（局部坐标；命中即置 `is_handled`，未命中转基类）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
            const WindowResizeEdge edge = edge_at(e.local_position, last_size_, env_);
            if (edge != WindowResizeEdge::None) {
                if (const WindowChrome *chrome = chrome_of(env_); chrome != nullptr) {
                    chrome->begin_resize(edge);
                }
                // 未注入 chrome（无头 / 非窗口场景）同样消费：命中分支照常执行，
                // 与本仓 TitleBar 在无环境时的既有口径一致。
                e.is_handled = true;
                return;
            }
        }
        Widget::on_pointer_event(e);
    }

  protected:
    /// @brief 布局：记下环境（事件阶段读 chrome 与窗口模式），以满约束布局子件。
    /// @param c 父级约束（本控件取满可用空间，不向父级多要）。
    /// @param ctx 构建上下文（环境链在此刷新）。
    /// @return 本控件尺寸（`c.constrain(c.max)`）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        env_ = ctx.env;  // 事件阶段读 chrome 与窗口模式用（树存活期内环境链稳定）
        if (child_) {
            // 登记布局父节点：失效沿布局父链向上传播依赖此链完整（同 NavigatorHost 的理由）。
            child_.widget().set_layout_parent(this);
            child_.widget().layout(c, ctx);
        }
        const Size size = c.constrain(c.max);
        last_size_ = size;
        return size;
    }

    /// @brief 命中链：带内只交自身（不下降），带外交给子件。
    ///
    /// 带内不下降是必须的：边缘带压在内容之上，若继续下降，拖动边缘会先落到内容里的按钮上。
    /// 自身入链由基类按 `wants_click()` 补入，故带内返回空链即可得到 `[this]`。
    /// 命中测试阶段顺带定档，`cursor_shape()` 随后按档返回——命中测试先于悬停光标解析发生，
    /// 指针沿边框移动时形状即刻跟随；改在 Move 事件里推光标会在离开后留下粘住的上一个形状。
    /// @param local 命中点（内容盒局部坐标）。
    /// @param bounds 本控件内容盒（尺寸即本控件布局尺寸）。
    /// @param ctx 构建上下文（读环境判窗口模式）。
    /// @return 带内为空链（仅自身）；带外为子件的命中链。
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        // 环境在此刷新一次：事件派发先命中测试再投递，故按下时读到的窗口模式 / chrome 与
        // 本次命中判定同源（布局缓存命中时 on_layout 不会重跑，只靠它会让窗口模式变档后仍读旧值）。
        env_ = ctx.env;
        const WindowResizeEdge edge = edge_at(local, bounds.size, ctx.env);
        if (edge != WindowResizeEdge::None) {
            hover_edge_ = edge;
            return {};
        }
        hover_edge_ = WindowResizeEdge::None;
        return child_ ? child_.widget().hit_test_chain(local, bounds, ctx) : std::vector<HitNode>{};
    }

  private:
    /// @brief 取环境里的 chrome 服务（未注入返回空）。
    /// @param env 环境（可空）。
    /// @return chrome 指针；无环境或无该服务时为空。
    [[nodiscard]] static auto chrome_of(const Environment *env) -> const WindowChrome * {
        return env != nullptr ? env->get<WindowChrome>() : nullptr;
    }

    /// @brief 窗口当前是否允许拖边缩放：仅 `Normal` 档为真。
    /// @param env 环境（可空；无环境视为允许，便于无头夹具直接验几何）。
    /// @return 非最大化 / 非全屏 / 非最小化时为 true。
    [[nodiscard]] static auto resizable_in(const Environment *env) -> bool {
        if (env == nullptr) {
            return true;
        }
        const auto *mode = env->get<WindowMode>();
        return mode == nullptr || *mode == WindowMode::Normal;
    }

    /// @brief 边档判定（角落优先；带宽按窗口尺寸收缩）。
    /// @param local 命中点（内容盒局部坐标）。
    /// @param size 本控件尺寸。
    /// @param env 环境（可空；非 `Normal` 窗口模式下一律 `None`）。
    /// @return 命中的边档；不在任何带内或手势关闭时为 `None`。
    [[nodiscard]] auto edge_at(const Point &local, const Size &size, const Environment *env = nullptr) const
        -> WindowResizeEdge {
        if (!resizable_in(env)) {
            return WindowResizeEdge::None;
        }
        const float min_side = std::min(size.width, size.height);
        // 窄窗防护：带宽不超过最短边的四分之一，否则边缘带会把内容吃满、内容再也点不到。
        const float band = std::clamp(band_, 0.0F, min_side * 0.25F);
        if (band <= 0.0F) {
            return WindowResizeEdge::None;
        }
        // 角隅两倍带宽：只有 band × band 时指针必须落进 6×6 的格子，实测斜向缩放几乎抓不到。
        const float corner = std::min(band * 2.0F, min_side * 0.5F);
        const float right_x = size.width - band;
        const float bottom_y = size.height - band;
        const bool near_left = local.x < corner;
        const bool near_right = local.x >= size.width - corner;
        const bool near_top = local.y < corner;
        const bool near_bottom = local.y >= size.height - corner;
        if (near_left && near_top) {
            return WindowResizeEdge::TopLeft;
        }
        if (near_right && near_top) {
            return WindowResizeEdge::TopRight;
        }
        if (near_left && near_bottom) {
            return WindowResizeEdge::BottomLeft;
        }
        if (near_right && near_bottom) {
            return WindowResizeEdge::BottomRight;
        }
        if (local.y < band) {
            return WindowResizeEdge::Top;
        }
        if (local.y >= bottom_y) {
            return WindowResizeEdge::Bottom;
        }
        if (local.x < band) {
            return WindowResizeEdge::Left;
        }
        if (local.x >= right_x) {
            return WindowResizeEdge::Right;
        }
        return WindowResizeEdge::None;
    }

    float band_ = AURORA_WINDOW_RESIZE_BAND;
    Size last_size_{};
    WindowResizeEdge hover_edge_ = WindowResizeEdge::None;  ///< 最近一次命中测试定下的边档（供光标）
    const Environment *env_ = nullptr;  ///< 事件阶段读 chrome 与窗口模式（on_layout 时刷新）
};

}  // namespace aurora
