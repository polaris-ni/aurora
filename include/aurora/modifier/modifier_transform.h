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
    explicit AlignNode(Alignment align) : align_(align) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        child_size_ = child;
        Size self = c.max;  // 占满父级（Align 默认填满可用空间）
        if (!c.max.is_finite()) {
            self = child;  // 父级无限时退化为内容尺寸
        }
        return c.constrain(self);
    }

    [[nodiscard]] auto align() const -> Alignment { return align_; }
    [[nodiscard]] auto child_size() const -> Size { return child_size_; }

  private:
    Alignment align_;
    mutable Size child_size_{.width = 0.0F, .height = 0.0F};
};

/// @brief 偏移修饰（Transform 切片）：把内容按 (dx,dy) 视觉平移，不改变布局尺寸/命中逻辑。
class OffsetNode : public ModifierNode {
  public:
    OffsetNode(float dx, float dy) : dx_(dx), dy_(dy) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        child_size_ = child;
        return c.constrain(child);
    }

    [[nodiscard]] auto dx() const -> float { return dx_; }
    [[nodiscard]] auto dy() const -> float { return dy_; }

  private:
    float dx_ = 0.0F;
    float dy_ = 0.0F;
    mutable Size child_size_{.width = 0.0F, .height = 0.0F};
};

/// @brief 仿射变换修饰（Transform 切片）：旋转 / 缩放 / 任意矩阵，绕内容盒中心作用。
/// 不改变布局尺寸，仅影响绘制期几何与命中测试（命中测试用逆矩阵映射指针）。
class TransformNode : public ModifierNode {
  public:
    enum class Operation : std::uint8_t {
        Rotate,  ///< 绕内容中心旋转（角度）
        ScaleXY,  ///< 绕内容中心非均匀缩放
        Raw,  ///< 用户提供的原始矩阵（关于原点，自行负责中心化）
    };

    explicit TransformNode(float degrees) : op_(Operation::Rotate), a_(degrees) {}
    TransformNode(float sx, float sy) : op_(Operation::ScaleXY), a_(sx), b_(sy) {}
    explicit TransformNode(const Matrix2D &m) : op_(Operation::Raw), raw_(m) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 绕内容盒中心构造本节点矩阵（content 为当前已知内容盒尺寸）。
    [[nodiscard]] auto matrix(const Size &content) const -> Matrix2D {
        const Point center{.x = content.width / 2.0F, .y = content.height / 2.0F};
        switch (op_) {
            case Operation::Rotate:
                return Matrix2D::from_rotate_about(a_, center);
            case Operation::ScaleXY:
                return Matrix2D::from_scale_about(a_, b_, center);
            case Operation::Raw:
                return raw_;
        }
        return Matrix2D{};
    }

  private:
    Operation op_;
    float a_ = 0.0F;
    float b_ = 0.0F;
    Matrix2D raw_{};
};

}  // namespace aurora
