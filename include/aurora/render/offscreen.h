#pragma once

#include <functional>

#include "aurora/core/types.h"
#include "aurora/environment/environment.h"
#include "aurora/render/painter.h"
#include "aurora/render/png.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 无头渲染（不依赖窗口系统）。
 *
 * 把「布局 + 绘制 + 写出 PNG / 逻辑快照」从 `HeadlessSurface` 头文件（window/surface.h）
 * 拆分到本文件，使渲染工具与具体 `Surface` 实现解耦（架构 §4.5 后端选择与工厂）。
 * 真实窗口 Surface 见 `aurora/window/native_surfaces.h`。
 */

/**
 * @brief 无头渲染：布局 + 绘制 + 写出 PNG。
 *
 * 流程：mount（注册响应式依赖）→ layout（两阶段测量）→ paint（软件栅格）→ PNG。
 *
 * @param root  根 widget（已构建的树）
 * @param width 画布宽（逻辑像素 = 设备像素）
 * @param height 画布高
 * @param path  输出 PNG 路径
 * @return 成功返回 true，失败返回带信息的 Error。
 */
[[nodiscard]] inline auto render_to_png(Node &root, int width, int height, const char *path) -> Result<bool> {
    BuildContext ctx; // 根环境（树内 Provider 注入子树环境）

    root->mount(ctx);

    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) };
    root->layout(c, ctx);

    Painter painter;
    painter.begin(width, height);
    root->paint(painter,
                Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                      .size = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) } },
                ctx);
    root.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                          .size = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) } });

    return write_png(path, width, height, painter.data());
}

/**
 * @brief 逻辑快照：布局后输出**平台无关**的 JSON 树。
 *
 * 每个节点形如 `{"type":..., "box":{"x","y","w","h"}, "children":[...]}`，
 * 不含任何像素位图，AI 可在无头环境验证 UI 结构与布局盒模型。
 * 布局是纯函数（mount → layout），故同输入必得同输出（确定性）。
 *
 * @param root  根 widget（已构建的树）
 * @param width 视口宽（逻辑像素）
 * @param height 视口高
 * @return 逻辑快照 JSON。
 */
[[nodiscard]] inline auto render_to_logical_snapshot(Node &root, int width, int height) -> Json {
    BuildContext ctx;
    root->mount(ctx);

    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) };
    root->layout(c, ctx);
    // 无头快照不进入 paint，故在此显式落定根盒到 Node（几何唯一权威在 Node）。
    root.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = root->size() });

    std::function<Json(const Node &)> snap = [&](const Node &n) -> Json {
        Json j;
        j["type"] = n.widget().type_name(); // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto [origin, size] = n.bounds();
        // NOLINTNEXTLINE(*-pro-bounds-avoid-unchecked-container-access)
        j["box"] = Json{ { "x", origin.x }, { "y", origin.y }, { "w", size.width }, { "h", size.height } };
        Json children = Json::array();
        for (const Node &child : n.widget().child_nodes()) {
            children.push_back(snap(child));
        }
        // NOLINTNEXTLINE(*-pro-bounds-avoid-unchecked-container-access)
        j["children"] = children;
        return j;
    };
    return snap(root);
}

} // namespace aurora
