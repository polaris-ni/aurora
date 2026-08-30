#pragma once

/// @file modifier_transform.h
/// @brief 几何变换修饰节点（Transform 切片）：AlignNode / OffsetNode / TransformNode。
/// 本文件为 modifier.h 的子切片；消费者通常直接 #include "aurora/modifier/modifier.h"。

#include "aurora/modifier/modifier_base.h"
#include "aurora/widget/alignment.h"

namespace aurora {

/// @brief 对齐修饰（Transform 切片）：在父级所给的额外空间内把子项按 `align` 定位。
/// 布局时占满父约束（fill），绘制时把内容平移到对齐子矩形；不影响命中（命中区随之平移）。
class AlignNode : public ModifierNode {
  public:
    explicit AlignNode(Alignment align) : m_align(align) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        m_child_size = child;
        Size self = c.max; // 占满父级（Align 默认填满可用空间）
        if (!c.max.is_finite()) {
            self = child; // 父级无限时退化为内容尺寸
        }
        return c.constrain(self);
    }

    [[nodiscard]] auto align() const -> Alignment { return m_align; }
    [[nodiscard]] auto child_size() const -> Size { return m_child_size; }

  private:
    Alignment m_align;
    mutable Size m_child_size{ .width = 0.0f, .height = 0.0f };
};

/// @brief 偏移修饰（Transform 切片）：把内容按 (dx,dy) 视觉平移，不改变布局尺寸/命中逻辑。
class OffsetNode : public ModifierNode {
  public:
    OffsetNode(float dx, float dy) : m_dx(dx), m_dy(dy) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        m_child_size = child;
        return c.constrain(child);
    }

    [[nodiscard]] auto dx() const -> float { return m_dx; }
    [[nodiscard]] auto dy() const -> float { return m_dy; }

  private:
    float m_dx = 0.0f;
    float m_dy = 0.0f;
    mutable Size m_child_size{ .width = 0.0f, .height = 0.0f };
};

/// @brief 仿射变换修饰（Transform 切片）：旋转 / 缩放 / 任意矩阵，绕内容盒中心作用。
/// 不改变布局尺寸，仅影响绘制期几何与命中测试（命中测试用逆矩阵映射指针）。
class TransformNode : public ModifierNode {
  public:
    enum class Operation : std::uint8_t {
        Rotate,  ///< 绕内容中心旋转（角度）
        ScaleXY, ///< 绕内容中心非均匀缩放
        Raw,     ///< 用户提供的原始矩阵（关于原点，自行负责中心化）
    };

    explicit TransformNode(float degrees) : m_op(Operation::Rotate), m_a(degrees) {}
    TransformNode(float sx, float sy) : m_op(Operation::ScaleXY), m_a(sx), m_b(sy) {}
    explicit TransformNode(const Matrix2D &m) : m_op(Operation::Raw), m_raw(m) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 绕内容盒中心构造本节点矩阵（content 为当前已知内容盒尺寸）。
    [[nodiscard]] auto matrix(const Size &content) const -> Matrix2D {
        const Point center{ .x = content.width / 2.0f, .y = content.height / 2.0f };
        switch (m_op) {
        case Operation::Rotate: return Matrix2D::from_rotate_about(m_a, center);
        case Operation::ScaleXY: return Matrix2D::from_scale_about(m_a, m_b, center);
        case Operation::Raw: return m_raw;
        }
        return Matrix2D{};
    }

  private:
    Operation m_op;
    float m_a = 0.0f;
    float m_b = 0.0f;
    Matrix2D m_raw{};
};

} // namespace aurora
