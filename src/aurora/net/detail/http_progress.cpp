// http_progress.cpp —— 进度合流闸的槽位与闸号判定（CHG-010 §12「进度节流」）。
//
// 本文件没有 I/O、没有时钟、没有线程：它只回答「这一笔进度此刻发不发」。四条口径写在 `http_progress.h`
// 的文件注释里，此处逐条落码：写入与发射解耦、闸号（帧号或回合号）变了才发、收尾时末值恒发、无回调即全空。
#include "aurora/net/detail/http_progress.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "aurora/net/http.h"

namespace aurora::net::detail {

ProgressGate::ProgressGate(aurora::net::http::ProgressHandler handler, std::function<std::uint64_t()> frame_source)
    : handler_(std::move(handler)), frame_source_(std::move(frame_source)) {}

auto ProgressGate::slot_index(aurora::net::http::ProgressPhase phase) -> std::size_t {
    return phase == aurora::net::http::ProgressPhase::Upload ? std::size_t{0} : std::size_t{1};
}

auto ProgressGate::gate() const -> std::uint64_t {
    if (frame_source_) {
        return frame_source_();
    }
    return turn_;
}

auto ProgressGate::observe(const aurora::net::http::Progress &progress) -> void {
    if (!armed()) {
        return;
    }
    slots_.at(slot_index(progress.phase)).value = progress;
}

auto ProgressGate::flush() -> void {
    if (!armed()) {
        return;
    }
    // 回合闸：一次 flush 就是一个回合，闸号自增后再判；帧号闸则把推进权交给宿主，本处只读。
    if (!frame_source_) {
        ++turn_;
    }
    for (Slot &slot : slots_) {
        if (!slot.value.has_value()) {
            continue;
        }
        if (slot.emitted_any && slot.emitted_gate == gate()) {
            continue;  // 同一机会内已发过：本回合的新字节并到下一次机会上发
        }
        handler_(slot.value.value());
        slot.value.reset();
        slot.emitted_any = true;
        slot.emitted_gate = gate();
    }
}

auto ProgressGate::finish() -> void {
    if (!armed()) {
        return;
    }
    for (Slot &slot : slots_) {
        if (!slot.value.has_value()) {
            continue;
        }
        // 末值恒发：收尾时不再看闸号，否则调用方无法靠回调判定「收了摊」。
        handler_(slot.value.value());
        slot.value.reset();
        slot.emitted_any = true;
        slot.emitted_gate = gate();
    }
}

}  // namespace aurora::net::detail
