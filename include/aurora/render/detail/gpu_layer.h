#pragma once

#include <atomic>
#include <cstdint>

namespace aurora::render::detail {

/// @brief GPU 层缓存键分配：进程内唯一（0 保留为无效）。
///
/// 由带 cache_layer 修饰的控件在首次进入 GPU/录制层路径时惰性取号，层纹理（FBO 常驻层 /
/// 软件仿真存储）均以该键寻址。分配器无锁，键永不回收（控件存续期即键存续期）。
/// @return 单调递增的 64 位层缓存键；首次调用返回 1，0 保留为「无效/未分配」。
inline auto next_gpu_layer_key() -> std::uint64_t {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// @brief GPU 层代际（epoch）：消费端层存储被整体丢弃时递增，控件据此整体失效层缓存。
///
/// 消费端（GPU 后端重建、软件仿真存储冷启动等）在 `DrawLayer` 查不到层纹理时 bump 一次，
/// 全部控件的 GPU 层缓存于下一帧失效并重录 BeginLayer——单帧自愈，不逐帧抖动。
inline std::atomic<std::uint64_t> g_gpu_layer_epoch{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// @brief 读取当前 GPU 层代际计数：控件每帧比对以确定缓存是否仍有效。
/// @return 自进程启动至今 `bump_gpu_layer_epoch` 被调用的次数（relaxed 序读取）。
inline auto gpu_layer_epoch() -> std::uint64_t { return g_gpu_layer_epoch.load(std::memory_order_relaxed); }

/// @brief 递增 GPU 层代际：消费端整体丢弃层存储后调用一次，让所有控件在下帧重录 BeginLayer。
inline auto bump_gpu_layer_epoch() -> void { g_gpu_layer_epoch.fetch_add(1, std::memory_order_relaxed); }

}  // namespace aurora::render::detail
