#pragma once

#include <cmath>
#include <numbers>

#include "aurora/core/diagnostics.h"
#include "aurora/core/types.h"

namespace aurora {

/// @brief 2D 仿射矩阵（2x3），用于修饰节点的旋转 / 缩放 / 平移 / 任意仿射变换。
///
/// 采用行主序的 2x3 表示：
/// x' = m11 * x + m12 * y + tx
/// y' = m21 * x + m22 * y + ty
/// 仅表达平移 / 旋转 / 缩放（不含投影），求逆稳定、零堆分配，适合每帧热路径。
///
/// 退化（行列式≈0，如缩放为 0）时 `inverse()` 返回单位矩阵并上报
/// `Diagnostics::degraded`，避免崩溃（需求 SPEC.QUALITY.CORE.GRACEFUL-DEGRADATION.21 错误恢复与降级渲染）。
struct Matrix2D {
    float m11 = 1;  ///< 线性部分第 1 行第 1 列（x' = m11·x + m12·y + tx 的 x 系数），默认单位阵。
    float m12 = 0;  ///< 线性部分第 1 行第 2 列（y 对 x' 的贡献系数），默认单位阵。
    float m21 = 0;  ///< 线性部分第 2 行第 1 列（x 对 y' 的贡献系数），默认单位阵。
    float m22 = 1;  ///< 线性部分第 2 行第 2 列（y 系数），默认单位阵。
    float tx = 0;  ///< 平移分量：作用于 x' 的偏移（逻辑像素）。
    float ty = 0;  ///< 平移分量：作用于 y' 的偏移（逻辑像素）。

    /// @brief 平移矩阵。
    /// @param dx x 方向平移量（逻辑像素）。
    /// @param dy y 方向平移量（逻辑像素）。
    /// @return 线性部分为单位阵、平移分量为 (dx, dy) 的矩阵。
    [[nodiscard]] static auto from_translate(float dx, float dy) -> Matrix2D {
        return Matrix2D{.m11 = 1, .m12 = 0, .m21 = 0, .m22 = 1, .tx = dx, .ty = dy};
    }

    /// @brief 以原点为轴的旋转矩阵（角度，顺时针为正，与屏幕 y 轴向下一致）。
    /// 约定 apply(x,y) = (m11*x + m12*y, m21*x + m22*y)，旋转 90° 满足 (1,0) -> (0,1)。
    /// @param degrees 旋转角度（度，顺时针为正）。
    /// @return 绕原点纯旋转矩阵（tx/ty 为 0）。
    [[nodiscard]] static auto from_rotate(float degrees) -> Matrix2D {
        const float r = degrees * std::numbers::pi_v<float> / 180.0F;
        const float cs = std::cos(r);
        const float sn = std::sin(r);
        return Matrix2D{.m11 = cs, .m12 = -sn, .m21 = sn, .m22 = cs, .tx = 0, .ty = 0};
    }

    /// @brief 非均匀缩放矩阵。
    /// @param sx x 方向缩放系数（1 为原尺寸）。
    /// @param sy y 方向缩放系数（1 为原尺寸）。
    /// @return 以原点为轴的缩放矩阵（对角为 sx/sy，平移为 0）。
    [[nodiscard]] static auto from_scale(float sx, float sy) -> Matrix2D {
        return Matrix2D{.m11 = sx, .m12 = 0, .m21 = 0, .m22 = sy, .tx = 0, .ty = 0};
    }

    /// @brief 绕任意点旋转：translate(c) * rotate(deg) * translate(-c)。
    /// @param degrees 旋转角度（度，顺时针为正）。
    /// @param center 旋转轴心点（逻辑像素）。
    /// @return 等价于「先移轴心到原点、旋转、再移回」的合成矩阵。
    [[nodiscard]] static auto from_rotate_about(float degrees, Point center) -> Matrix2D {
        return from_translate(center.x, center.y)
            .compose(from_rotate(degrees))
            .compose(from_translate(-center.x, -center.y));
    }

    /// @brief 绕任意点缩放：translate(c) * scale(sx,sy) * translate(-c)。
    /// @param sx x 方向缩放系数（1 为原尺寸）。
    /// @param sy y 方向缩放系数（1 为原尺寸）。
    /// @param center 缩放不动点（逻辑像素）。
    /// @return 以 center 为不动点的缩放合成矩阵。
    [[nodiscard]] static auto from_scale_about(float sx, float sy, Point center) -> Matrix2D {
        return from_translate(center.x, center.y)
            .compose(from_scale(sx, sy))
            .compose(from_translate(-center.x, -center.y));
    }

    /// @brief 合成：返回 this * o（先应用 o，再应用 this）。
    /// @param o 右操作数（先作用的变换）。
    /// @return this ∘ o 的 2x3 仿射乘积矩阵。
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
    /// @return 本矩阵的逆；行列式 |det| < 1e-6 时为默认单位矩阵（伴 `matrix2d-degenerate` 降级记录）。
    [[nodiscard]] auto inverse() const -> Matrix2D {
        const float det = (m11 * m22) - (m12 * m21);
        if (std::fabs(det) < 1e-6F) {
            Diagnostics::degraded("Matrix2D determinant is near 0, degraded to identity matrix", "Matrix2D::inverse",
                                  "matrix2d-degenerate");
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
    /// @param p 待映射的点（逻辑像素）。
    /// @return 按 x' = m11·x + m12·y + tx、y' = m21·x + m22·y + ty 变换后的点。
    [[nodiscard]] auto apply_to_point(Point p) const -> Point {
        return Point{.x = (m11 * p.x) + (m12 * p.y) + tx, .y = (m21 * p.x) + (m22 * p.y) + ty};
    }

    /// @brief 是否近似单位矩阵（用于走恒等快速路径）。
    /// @return 六个分量按 == 与单位矩阵逐元素相等时为 true。
    [[nodiscard]] auto is_identity() const -> bool {
        return m11 == 1 && m12 == 0 && m21 == 0 && m22 == 1 && tx == 0 && ty == 0;
    }
};

}  // namespace aurora
