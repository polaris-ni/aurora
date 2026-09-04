#pragma once

#include <cmath>
#include <functional>
#include <numbers>

#include "aurora/event/event.h"

namespace aurora {

/**
 * @brief 捏合（Pinch）手势识别器：追踪双指距离变化，输出缩放比例。
 *
 * 用法：
 * @code
 *   PinchRecognizer pinch;
 *   // 每帧触摸事件：
 *   pinch.on_touch(touch_event);
 *   float scale = pinch.scale();  // 相对初始距离的缩放比
 * @endcode
 */
class PinchRecognizer {
  public:
    /// @brief 处理触摸事件，更新内部状态。
    /// 并发场景下锁定一对 pointer id（激活瞬间取前两个活跃点），后续始终追踪该对，
    /// 避免第三指插入导致距离跳变（仍回退到前两活跃点距离以保持稳健）。
    auto on_touch(const TouchEvent &e) -> void {
        if (e.active_count() < 2) {
            reset();
            return;
        }
        auto pa = e.point_by_id(id_a_);
        auto pb = e.point_by_id(id_b_);
        if (!pa || !pb || !pa->is_active || !pb->is_active) {
            id_a_ = id_b_ = -1;  // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.is_active) {
                    continue;
                }
                if (id_a_ < 0) {
                    id_a_ = p.id;
                } else if (id_b_ < 0) {
                    id_b_ = p.id;
                }
            }
            pa = e.point_by_id(id_a_);
            pb = e.point_by_id(id_b_);
        }
        const float dist = (pa && pb)
                               ? std::sqrt(((pa->position.x - pb->position.x) * (pa->position.x - pb->position.x)) +
                                           ((pa->position.y - pb->position.y) * (pa->position.y - pb->position.y)))
                               : e.pinch_distance();
        if (!active_) {
            initial_distance_ = dist;
            active_ = true;
        }
        current_distance_ = dist;
    }

    /// @brief 当前缩放比例（相对初始双指距离）。未激活时返回 1.0。
    [[nodiscard]] auto scale() const -> float {
        if (!active_ || initial_distance_ < 0.001F) {
            return 1.0F;
        }
        return current_distance_ / initial_distance_;
    }

    /// @brief 是否正在识别中（双指活跃）。
    [[nodiscard]] auto is_active() const -> bool { return active_; }

    /// @brief 重置状态。
    auto reset() -> void {
        active_ = false;
        initial_distance_ = 0.0F;
        current_distance_ = 0.0F;
        id_a_ = id_b_ = -1;
    }

  private:
    bool active_ = false;
    float initial_distance_ = 0.0F;
    float current_distance_ = 0.0F;
    int id_a_ = -1;  ///< 锁定的第一指 pointer id
    int id_b_ = -1;  ///< 锁定的第二指 pointer id
};

/**
 * @brief 旋转手势识别器：追踪双指角度变化，输出旋转增量（度）。
 */
class RotationRecognizer {
  public:
    /// @brief 处理触摸事件，更新内部状态。
    /// 并发场景下锁定一对 pointer id（激活瞬间取前两个活跃点），后续始终追踪该对角度。
    auto on_touch(const TouchEvent &e) -> void {
        if (e.active_count() < 2) {
            reset();
            return;
        }
        auto pa = e.point_by_id(id_a_);
        auto pb = e.point_by_id(id_b_);
        if (!pa || !pb || !pa->is_active || !pb->is_active) {
            id_a_ = id_b_ = -1;  // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.is_active) {
                    continue;
                }
                if (id_a_ < 0) {
                    id_a_ = p.id;
                } else if (id_b_ < 0) {
                    id_b_ = p.id;
                }
            }
            pa = e.point_by_id(id_a_);
            pb = e.point_by_id(id_b_);
        }
        const float angle =
            (pa && pb) ? std::atan2(pb->position.y - pa->position.y, pb->position.x - pa->position.x) : e.pinch_angle();
        if (!active_) {
            initial_angle_ = angle;
            active_ = true;
        }
        current_angle_ = angle;
    }

    /// @brief 旋转增量（度，相对初始角度）。未激活时返回 0。
    [[nodiscard]] auto angle_delta() const -> float {
        if (!active_) {
            return 0.0F;
        }
        float delta = current_angle_ - initial_angle_;
        // 归一化到 [-180, 180]
        while (delta > std::numbers::pi_v<float>) {
            delta -= 2.0F * std::numbers::pi_v<float>;
        }
        while (delta < -std::numbers::pi_v<float>) {
            delta += 2.0F * std::numbers::pi_v<float>;
        }
        return delta * 180.0F / std::numbers::pi_v<float>;
    }

    /// @brief 是否正在识别中。
    [[nodiscard]] auto is_active() const -> bool { return active_; }

    /// @brief 重置状态。
    auto reset() -> void {
        active_ = false;
        initial_angle_ = 0.0F;
        current_angle_ = 0.0F;
        id_a_ = id_b_ = -1;
    }

  private:
    bool active_ = false;
    float initial_angle_ = 0.0F;
    float current_angle_ = 0.0F;
    int id_a_ = -1;  ///< 锁定的第一指 pointer id
    int id_b_ = -1;  ///< 锁定的第二指 pointer id
};

}  // namespace aurora
