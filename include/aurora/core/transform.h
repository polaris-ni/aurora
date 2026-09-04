#pragma once

#include <cmath>
#include <numbers>

#include "aurora/core/diagnostics.h"
#include "aurora/core/types.h"

namespace aurora {

/**
 * @brief 2D 仿射矩阵（2x3），用于修饰节点的旋转 / 缩放 / 平移 / 任意仿射变换。
 *
 * 采用行主序的 2x3 表示：
 *   x' = m11 * x + m12 * y + tx
 *   y' = m21 * x + m22 * y + ty
 * 仅表达平移 / 旋转 / 缩放（不含投影），求逆稳定、零堆分配，适合每帧热路径。
 *
 * 退化（行列式≈0，如缩放为 0）时 `inverse()` 返回单位矩阵并上报
 * `Diagnostics::degraded`，避免崩溃（需求 #21 错误恢复与降级渲染）。
 */
struct Matrix2D {
    float m11 = 1;
    float m12 = 0;
    float m21 = 0;
    float m22 = 1;
    float tx = 0;
    float ty = 0;

    /// @brief 平移矩阵。
    [[nodiscard]] static auto from_translate(float dx, float dy) -> Matrix2D {
        return Matrix2D{.m11 = 1, .m12 = 0, .m21 = 0, .m22 = 1, .tx = dx, .ty = dy};
    }

    /// @brief 以原点为轴的旋转矩阵（角度，顺时针为正，与屏幕 y 轴向下一致）。
    /// 约定 apply(x,y) = (m11*x + m12*y, m21*x + m22*y)，旋转 90° 满足 (1,0) -> (0,1)。
    [[nodiscard]] static auto from_rotate(float degrees) -> Matrix2D {
        const float r = degrees * std::numbers::pi_v<float> / 180.0F;
        const float cs = std::cos(r);
        const float sn = std::sin(r);
        return Matrix2D{.m11 = cs, .m12 = -sn, .m21 = sn, .m22 = cs, .tx = 0, .ty = 0};
    }

    /// @brief 非均匀缩放矩阵。
    [[nodiscard]] static auto from_scale(float sx, float sy) -> Matrix2D {
        return Matrix2D{.m11 = sx, .m12 = 0, .m21 = 0, .m22 = sy, .tx = 0, .ty = 0};
    }

    /// @brief 绕任意点旋转：translate(c) * rotate(deg) * translate(-c)。
    [[nodiscard]] static auto from_rotate_about(float degrees, Point center) -> Matrix2D {
        return from_translate(center.x, center.y)
            .compose(from_rotate(degrees))
            .compose(from_translate(-center.x, -center.y));
    }

    /// @brief 绕任意点缩放：translate(c) * scale(sx,sy) * translate(-c)。
    [[nodiscard]] static auto from_scale_about(float sx, float sy, Point center) -> Matrix2D {
        return from_translate(center.x, center.y)
            .compose(from_scale(sx, sy))
            .compose(from_translate(-center.x, -center.y));
    }

    /// @brief 合成：返回 this * o（先应用 o，再应用 this）。
    [[nodiscard]] auto compose(const Matrix2D &o) const -> Matrix2D {
        return Matrix2D{
            .m11 = (m11 * o.m11) + (m12 * o.m21),
            .m12 = (m11 * o.m12) + (m12 * o.m22),
            .m21 = (m21 * o.m11) + (m22 * o.m21),
            .m22 = (m21 * o.m12) + (m22 * o.m22),
            .tx = (m11 * o.tx) + (m12 * o.ty) + tx,
            .ty = (m21 * o.tx) + (m22 * o.ty) + ty,
        };
    }

    /// @brief 求逆；退化时返回单位矩阵并上报降级诊断。
    [[nodiscard]] auto inverse() const -> Matrix2D {
        const float det = (m11 * m22) - (m12 * m21);
        if (std::fabs(det) < 1e-6F) {
            Diagnostics::degraded("Matrix2D 行列式≈0，已降级为单位矩阵", "Matrix2D::inverse", "matrix2d-degenerate");
            return Matrix2D{};
        }
        const float ia = m22 / det;
        const float ib = -m12 / det;
        const float ic = -m21 / det;
        const float id = m11 / det;
        return Matrix2D{
            .m11 = ia,
            .m12 = ib,
            .m21 = ic,
            .m22 = id,
            .tx = -((ia * tx) + (ib * ty)),
            .ty = -((ic * tx) + (id * ty)),
        };
    }

    /// @brief 点映射。
    [[nodiscard]] auto apply_to_point(Point p) const -> Point {
        return Point{.x = (m11 * p.x) + (m12 * p.y) + tx, .y = (m21 * p.x) + (m22 * p.y) + ty};
    }

    /// @brief 是否近似单位矩阵（用于走恒等快速路径）。
    [[nodiscard]] auto is_identity() const -> bool {
        return m11 == 1 && m12 == 0 && m21 == 0 && m22 == 1 && tx == 0 && ty == 0;
    }
};

}  // namespace aurora
