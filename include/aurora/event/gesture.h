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
        auto pa = e.point_by_id(m_id_a);
        auto pb = e.point_by_id(m_id_b);
        if (!pa || !pb || !pa->active || !pb->active) {
            m_id_a = m_id_b = -1; // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.active) {
                    continue;
                }
                if (m_id_a < 0) {
                    m_id_a = p.id;
                } else if (m_id_b < 0) {
                    m_id_b = p.id;
                }
            }
            pa = e.point_by_id(m_id_a);
            pb = e.point_by_id(m_id_b);
        }
        const float dist = (pa && pb)
                               ? std::sqrt(((pa->position.x - pb->position.x) * (pa->position.x - pb->position.x)) +
                                           ((pa->position.y - pb->position.y) * (pa->position.y - pb->position.y)))
                               : e.pinch_distance();
        if (!m_active) {
            m_initial_distance = dist;
            m_active = true;
        }
        m_current_distance = dist;
    }

    /// @brief 当前缩放比例（相对初始双指距离）。未激活时返回 1.0。
    [[nodiscard]] auto scale() const -> float {
        if (!m_active || m_initial_distance < 0.001f) {
            return 1.0f;
        }
        return m_current_distance / m_initial_distance;
    }

    /// @brief 是否正在识别中（双指活跃）。
    [[nodiscard]] auto is_active() const -> bool { return m_active; }

    /// @brief 重置状态。
    auto reset() -> void {
        m_active = false;
        m_initial_distance = 0.0f;
        m_current_distance = 0.0f;
        m_id_a = m_id_b = -1;
    }

  private:
    bool m_active = false;
    float m_initial_distance = 0.0f;
    float m_current_distance = 0.0f;
    int m_id_a = -1; ///< 锁定的第一指 pointer id
    int m_id_b = -1; ///< 锁定的第二指 pointer id
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
        auto pa = e.point_by_id(m_id_a);
        auto pb = e.point_by_id(m_id_b);
        if (!pa || !pb || !pa->active || !pb->active) {
            m_id_a = m_id_b = -1; // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.active) {
                    continue;
                }
                if (m_id_a < 0) {
                    m_id_a = p.id;
                } else if (m_id_b < 0) {
                    m_id_b = p.id;
                }
            }
            pa = e.point_by_id(m_id_a);
            pb = e.point_by_id(m_id_b);
        }
        const float angle =
            (pa && pb) ? std::atan2(pb->position.y - pa->position.y, pb->position.x - pa->position.x) : e.pinch_angle();
        if (!m_active) {
            m_initial_angle = angle;
            m_active = true;
        }
        m_current_angle = angle;
    }

    /// @brief 旋转增量（度，相对初始角度）。未激活时返回 0。
    [[nodiscard]] auto angle_delta() const -> float {
        if (!m_active) {
            return 0.0f;
        }
        float delta = m_current_angle - m_initial_angle;
        // 归一化到 [-180, 180]
        while (delta > std::numbers::pi_v<float>) {
            delta -= 2.0f * std::numbers::pi_v<float>;
        }
        while (delta < -std::numbers::pi_v<float>) {
            delta += 2.0f * std::numbers::pi_v<float>;
        }
        return delta * 180.0f / std::numbers::pi_v<float>;
    }

    /// @brief 是否正在识别中。
    [[nodiscard]] auto is_active() const -> bool { return m_active; }

    /// @brief 重置状态。
    auto reset() -> void {
        m_active = false;
        m_initial_angle = 0.0f;
        m_current_angle = 0.0f;
        m_id_a = m_id_b = -1;
    }

  private:
    bool m_active = false;
    float m_initial_angle = 0.0f;
    float m_current_angle = 0.0f;
    int m_id_a = -1; ///< 锁定的第一指 pointer id
    int m_id_b = -1; ///< 锁定的第二指 pointer id
};

} // namespace aurora
