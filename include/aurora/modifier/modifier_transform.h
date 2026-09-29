#pragma once

/// @brief 几何变换修饰节点（Transform 切片）：AlignNode / OffsetNode / TransformNode。
/// 本文件为 modifier.h 的子切片；消费者通常直接 `#include` "aurora/modifier/modifier.h"。
/// @file modifier_transform.h

#include <limits>

#include "aurora/core/transform.h"
#include "aurora/modifier/modifier_base.h"
#include "aurora/widget/alignment.h"

namespace aurora {

/// @brief 对齐修饰（Transform 切片）：在父级所给的额外空间内把子项按 `align` 定位。
/// 布局时**逐轴**占满父约束的「既定槽位」轴（`Constraints::loose_*` 为 false 且有界即展开；
/// 无限轴或 Flex 主轴的按需剩余空间则退化为内容尺寸，不吞兄弟控件的空间），
/// 绘制时把内容平移到对齐子矩形；不影响命中（命中区随之平移）。
class AlignNode : public ModifierNode {
  public:
    /// @brief 以对齐方式构造。
    /// @param align 子项在额外空间内的对齐方式。
    explicit AlignNode(Alignment align) : align_(align) {}

    /// @brief 修饰种类。
    /// @return 恒为 Kind::Transform。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    /// @brief 逐轴占满父级既定槽位测量自身，并记录子项尺寸供绘制期对齐使用。
    /// @param c 父级下发的布局约束。
    /// @param measure_child 以给定约束测量子项的回调。
    /// @return 占满既定槽位轴（经 c.constrain 钳制）后的自身尺寸。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        child_size_ = child;
        // 逐轴判定（对标 Flutter RenderPositionedBox 的 shrinkWrapWidth/Height）：
        // 整块 is_finite() 判据会让「该轴其实有既定槽位」被另一轴的无限上限连带取消，交叉轴居中静默失效。
        // loose_* 标记的轴是 Flex 主轴的「按需剩余空间」而非既定槽位，在此轴展开会吞掉兄弟控件的空间。
        Size self = child;
        if (bounded(c.max.width) && !c.loose_width) {
            self.width = c.max.width;  // 该轴占满父级既定槽位（Align 默认填满可用空间）
        }
        if (bounded(c.max.height) && !c.loose_height) {
            self.height = c.max.height;
        }
        return c.constrain(self);
    }

    /// @brief 读取构造时给定的对齐方式。
    /// @return 本节点的 Alignment。
    [[nodiscard]] auto align() const -> Alignment { return align_; }
    /// @brief 最近一次 layout 记录的子项尺寸（未测量时为 0×0）。
    /// @return 子项 Size。
    [[nodiscard]] auto child_size() const -> Size { return child_size_; }
    /// @brief 判定约束值是否为有限值（非无穷大）。
    /// @param v 待判定的约束值。
    /// @return 有限为 true，无穷大为 false。
    [[nodiscard]] static auto bounded(float v) -> bool { return v != std::numeric_limits<float>::infinity(); }

  private:
    Alignment align_;
    mutable Size child_size_{.width = 0.0F, .height = 0.0F};
};

/// @brief 偏移修饰（Transform 切片）：把内容按 (dx,dy)
/// 视觉平移，不改变布局尺寸；命中测试的平移量与绘制保持一致（命中区随 offset 移动）。
class OffsetNode : public ModifierNode {
  public:
    /// @brief 以平移量构造。
    /// @param dx 水平平移量。
    /// @param dy 垂直平移量。
    OffsetNode(float dx, float dy) : dx_(dx), dy_(dy) {}

    /// @brief 修饰种类。
    /// @return 恒为 Kind::Transform。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    /// @brief 按子项尺寸原样测量（偏移不影响布局尺寸），并记录子项尺寸供绘制期平移使用。
    /// @param c 父级下发的布局约束。
    /// @param measure_child 以给定约束测量子项的回调。
    /// @return 子项尺寸经 c.constrain 钳制后的结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const Size child = measure_child(c);
        child_size_ = child;
        return c.constrain(child);
    }

    /// @brief 读取水平平移量。
    /// @return 构造时给定的 dx。
    [[nodiscard]] auto dx() const -> float { return dx_; }
    /// @brief 读取垂直平移量。
    /// @return 构造时给定的 dy。
    [[nodiscard]] auto dy() const -> float { return dy_; }

  private:
    float dx_ = 0.0F;
    float dy_ = 0.0F;
    mutable Size child_size_{.width = 0.0F, .height = 0.0F};
};

/// @brief 仿射变换修饰（Transform 切片）：旋转 /
/// 缩放绕内容盒中心作用；任意矩阵（Raw）按用户矩阵关于原点原样应用（如需绕中心请自行构造 from_*_about）。
/// 不改变布局尺寸，仅影响绘制期几何与命中测试（命中测试用逆矩阵映射指针）。
class TransformNode : public ModifierNode {
  public:
    enum class Operation : std::uint8_t {
        Rotate,  ///< 绕内容中心旋转（角度）
        ScaleXY,  ///< 绕内容中心非均匀缩放
        Raw,  ///< 用户提供的原始矩阵（关于原点，自行负责中心化）
    };

    /// @brief 绕内容中心旋转变体：以角度构造。
    /// @param degrees 旋转角度。
    explicit TransformNode(float degrees) : op_(Operation::Rotate), a_(degrees) {}
    /// @brief 绕内容中心非均匀缩放变体：以两轴缩放因子构造。
    /// @param sx 水平缩放因子。
    /// @param sy 垂直缩放因子。
    TransformNode(float sx, float sy) : op_(Operation::ScaleXY), a_(sx), b_(sy) {}
    /// @brief 原始矩阵变体：矩阵关于原点应用，绕中心需调用方自行构造 from_*_about。
    /// @param m 用户提供的仿射矩阵。
    explicit TransformNode(const Matrix2D &m) : op_(Operation::Raw), raw_(m) {}

    /// @brief 修饰种类。
    /// @return 恒为 Kind::Transform。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Transform; }

    /// @brief 按子项尺寸原样测量（变换不改变布局尺寸，仅作用于绘制与命中期几何）。
    /// @param c 父级下发的布局约束。
    /// @param measure_child 以给定约束测量子项的回调。
    /// @return 子项尺寸（未钳制）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 绕内容盒中心构造本节点矩阵（content 为当前已知内容盒尺寸）。
    /// @param content 当前内容盒尺寸（决定旋转/缩放中心）。
    /// @return Rotate/ScaleXY 为绕中心的对应矩阵；Raw 原样返回用户矩阵。
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
