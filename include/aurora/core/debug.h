#pragma once

#include <aurora/core/assert.h>

namespace aurora::debug {

/**
 * @brief 渲染纯度检查（GUIDELINE.md §27.5）。
 *
 * 在 `Widget::paint()` 首行接线，捕获「视图在渲染过程中读取可变全局 / 时钟」的反模式：
 * 声明式 UI 的渲染应是纯函数（同输入同输出）。
 *
 * 实现：每次进入 `Widget::paint()` 由 `PaintPurityGuard` 递增 `g_paint_depth`、退出递减
 * （支持 Drawer 等内部递归 paint 的嵌套）；`current_timestamp()` 守卫据此在「绘制过程中读
 * 全局时钟」时触发断言，真正兑现纯度约束。整套机制 `AURORA_ENABLE_DEBUG` 门控，release 编译掉、零开销。
 */
#ifdef AURORA_ENABLE_DEBUG
// 渲染遍历嵌套深度：每次进入 Widget::paint() +1、退出 -1。> 0 表示当前处于绘制上下文中，
// 此时读全局时钟（current_timestamp）属反模式。
inline int g_paint_depth = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables):
                               // 故意保留的全局可变深度计数器 （AURORA_ENABLE_DEBUG门控，Release 不存在），由
                               // PaintPurityGuard RAII 增/减以驱动绘制纯度守卫，不可改为 const。

// 在 Widget::paint() 首行构造的 RAII 守卫，维护 g_paint_depth 的进入 / 退出配对。
// 显式声明全部特殊成员（Rule of Five / cppcoreguidelines-special-member-functions）：
// 仅析构为「非默认」，其余显式 default / delete，避免隐式生成不明确行为。
struct PaintPurityGuard {
    PaintPurityGuard() { ++g_paint_depth; }
    PaintPurityGuard(const PaintPurityGuard &) = default;
    PaintPurityGuard(PaintPurityGuard &&) = default;
    auto operator=(const PaintPurityGuard &) -> PaintPurityGuard & = delete;
    auto operator=(PaintPurityGuard &&) -> PaintPurityGuard & = delete;
    ~PaintPurityGuard() { --g_paint_depth; }
};
#endif

inline auto check_render_purity() -> void {
#ifdef AURORA_ENABLE_DEBUG
    // 挂接点：必须在渲染 / 绘制上下文中调用（g_paint_depth > 0）。
    // 捕获脱离 render 循环、在绘制上下文之外直接调 paint / check_render_purity 的反模式。
    AURORA_ASSERT(g_paint_depth > 0,
                  "check_render_purity: 在 Widget::paint() 绘制上下文之外被调用——"
                  "视图不应脱离渲染遍历直接 paint（会读到过期布局 / 状态）");
#endif
}

}  // namespace aurora::debug