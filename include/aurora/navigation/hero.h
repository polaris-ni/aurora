#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "aurora/core/types.h"
#include "aurora/environment/environment.h" // 完整定义 Environment（会随后包含 build_context.h）
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief Hero 配对条目：转场期间由页内 Hero 在 paint 阶段记录其绝对包围盒与子节点，
 * 供 `TransitionLayer` 覆盖层做几何插值绘制。
 */
struct HeroEntry {
    Rect bounds{}; ///< 该 Hero 在所属页面内的绝对包围盒（paint 阶段捕获）。
    Node child;  ///< 共享元素内容（用于覆盖层插值绘制）。
};

/**
 * @brief 转场期间由 `NavigatorHost` 注入到子树环境的值，供页内 Hero 上报几何与读取 morphing 标记。
 *
 * 宿主每帧在绘制前把上一帧计算出的 `morphing` 集合写入，Hero 据以跳过自绘；
 * 绘制旧页/新页时分别把当前 Hero 几何记录到 `source`/`target`，供覆盖层配对。
 */
struct HeroRegistry {
    enum class CaptureMode : std::uint8_t { None, Source, Target };

    CaptureMode capture_mode = CaptureMode::None;      ///< 当前 paint 正在捕获旧页(Source)还是新页(Target)。
    std::unordered_map<std::string, HeroEntry> source; ///< 旧页 Hero 几何（按 tag）。
    std::unordered_map<std::string, HeroEntry> target; ///< 新页 Hero 几何（按 tag）。
    std::unordered_set<std::string> morphing;          ///< 本帧处于 morphing 的 tag 集合（Hero 据以跳过自绘）。
};

/**
 * @brief 共享元素转场控件（对照 Flutter Hero）。
 *
 * 持有一个 `tag` 与一个 `child`。同 `tag` 的源/目标 Hero 在 `Navigator` 转场期间被
 * `NavigatorHost` 配对，于 `TransitionLayer` 覆盖层上做矩形 `lerp` + 交叉淡变「形变飞入」。
 * 常态零开销：仅当所属 `NavigatorHost` 注入 `HeroRegistry` 且本 tag 处于 morphing 时跳过自绘。
 */
class Hero : public SingleChild {
  public:
    Hero(std::string tag, Node child) : SingleChild(std::move(child)), m_tag(std::move(tag)) {}

    [[nodiscard]] auto tag() const -> const std::string & { return m_tag; }

    /// @brief 设置共享元素标签（序列化/运行时均可改；转场期按 tag 配对）。
    auto set_tag(std::string t) -> void { m_tag = std::move(t); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["tag"] = m_tag;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("tag")) {
            m_tag = props["tag"].get<std::string>();
        }
    }
    /// @brief 单子控件：序列化重建时取首个子节点作为 child。
    auto adopt_children(std::vector<Node> &&kids) -> void override { // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
        if (!kids.empty()) {
            m_child = std::move(kids.front());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Hero"; }

    // 绘制阶段向 HeroRegistry 上报几何（副作用）且转场期按 morphing 跳过自绘：
    // 缓存回放会跳过 on_paint，导致注册丢失 / 形变飞入缺失，故不可缓存 Display List。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{
            .name = "Hero",
            .properties = {
                { .name="tag", .type="string", .default_value="", .required=false, .note="共享元素配对键" },
            },
            .events = {},
            .children_policy = "single",
            .examples = { R"(au::Hero("logo", au::Container{ au::Text{"Aurora"} }))" },
        };
    }

    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        return m_child.widget().layout(c, ctx);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const auto *regp = ctx.environment<std::shared_ptr<HeroRegistry>>();
        if (regp != nullptr) {
            const auto reg = *regp; // 拷贝 shared_ptr 以获得非 const 访问（共享同一 HeroRegistry 实例）。
            // 始终先捕获几何（即便处于 morphing 也要供覆盖层绘制），再决定是否跳过自绘。
            if (reg->capture_mode == HeroRegistry::CaptureMode::Source) {
                reg->source[m_tag] = HeroEntry{ .bounds = bounds, .child = m_child };
            } else if (reg->capture_mode == HeroRegistry::CaptureMode::Target) {
                reg->target[m_tag] = HeroEntry{ .bounds = bounds, .child = m_child };
            }
            if (reg->morphing.contains(m_tag)) {
                return; // 转场期交给覆盖层绘制，跳过自绘，避免双重影像。
            }
        }
        m_child.widget().paint(p, bounds, ctx);
    }

  private:
    std::string m_tag;
};

} // namespace aurora
