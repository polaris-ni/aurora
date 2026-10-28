#include "aurora/state/detail/reactor.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"

namespace aurora::detail {
namespace {

/// @brief 当前线程的依赖收集器（作用域外为空）。
///
/// 以「函数内 `thread_local` + 访问器」提供，而非命名空间级可变全局变量——与
/// `src/aurora/event/focus.cpp` 的派发槽同形态：本状态是**求值上下文**（进入收集时
/// 设置、退出时复原），只由本文件的配对 RAII 对象读写，不该暴露成可寻址的全局设施。
[[nodiscard]] auto collector_slot() noexcept -> DepsCollector *& {
    thread_local DepsCollector *slot = nullptr;
    return slot;
}

/// @brief 来源节点的稳定身份分配器。
///
/// 函数内静态：初值是编译期常量，无静态初始化顺序问题；跨 TU 不可见，身份唯一性由
/// 「本文件是来源节点唯一定义处」保证，而不是靠一枚全局变量。
[[nodiscard]] auto next_source_id() -> std::atomic<std::uint64_t> & {
    static std::atomic<std::uint64_t> value{1};
    return value;
}

/// @brief 观察者的全局登记序号分配器（同上，函数内静态而非全局）。
[[nodiscard]] auto next_observer_seq() -> std::atomic<std::uint64_t> & {
    static std::atomic<std::uint64_t> value{1};
    return value;
}

}  // namespace

auto current_collector() -> DepsCollector * { return collector_slot(); }

CollectorScope::CollectorScope(DepsCollector &collector) noexcept : previous_(collector_slot()) {
    collector_slot() = &collector;
}

CollectorScope::~CollectorScope() { collector_slot() = previous_; }

SuspendCollection::SuspendCollection() noexcept : previous_(collector_slot()) { collector_slot() = nullptr; }

SuspendCollection::~SuspendCollection() { collector_slot() = previous_; }

Observer::Observer() : seq_(next_observer_seq().fetch_add(1)) {}

SourceNode::SourceNode() : id_(next_source_id().fetch_add(1)) {}

SourceNode::~SourceNode() = default;

auto SourceNode::add_observer(Observer &observer) -> void {
    for (const auto *existing : observers_) {
        if (existing == &observer) {
            return;
        }
    }
    observers_.push_back(&observer);
}

auto SourceNode::remove_observer(Observer &observer) noexcept -> void {
    const auto removed = std::remove(observers_.begin(), observers_.end(), &observer);
    observers_.erase(removed, observers_.end());
}

RunRecord::RunRecord(std::function<void()> fn) : fn_(std::move(fn)) {}

RunRecord::~RunRecord() { cancel(); }

auto RunRecord::on_invalidate() -> void { pending_ = true; }

auto RunRecord::add(SourceNode &src, std::uint64_t version) -> void {
    // 同一来源在一轮回调内重复读取只保留一条边：版本取最近一次读取，
    // 使「本轮是否真的变化」的判定与最后读数一致。
    for (auto &dep : next_) {
        if (dep.node.get() == &src) {
            dep.seen = version;
            return;
        }
    }
    next_.push_back(Dep{.node = src.shared_from_this(), .seen = version});
}

auto RunRecord::as_terminal() -> std::shared_ptr<RunRecord> { return shared_from_this(); }

auto RunRecord::cancel() noexcept -> void {
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    pending_ = false;
    for (const auto &dep : deps_) {
        if (dep.node) {
            dep.node->remove_observer(*this);
        }
    }
    deps_.clear();
    next_.clear();
}

auto RunRecord::replace_deps() -> void {
    const auto contains = [](const std::vector<Dep> &list, const SourceNode *node) -> bool {
        return std::any_of(list.begin(), list.end(), [node](const Dep &dep) -> bool { return dep.node.get() == node; });
    };
    for (const auto &old : deps_) {
        if (!contains(next_, old.node.get())) {
            old.node->remove_observer(*this);
        }
    }
    for (const auto &fresh : next_) {
        if (!contains(deps_, fresh.node.get())) {
            fresh.node->add_observer(*this);
        }
    }
    deps_ = std::move(next_);
    next_.clear();
}

auto RunRecord::execute() -> void {
    if (running_) {
        throw std::logic_error("run record reentered during execution");
    }
    running_ = true;
    next_.clear();
    CollectorScope scope(*this);
    try {
        if (fn_) {
            fn_();
        }
    } catch (...) {
        running_ = false;
        throw;
    }
    replace_deps();
    running_ = false;
}

auto RunRecord::run_first() -> void {
    if (cancelled_) {
        return;
    }
    pending_ = false;
    // 首跑同样走引擎入口：入口外立即执行，回调内创建时排到当前轮之后，
    // 使首跑内的写入进入后续轮而不是打断当前求值栈。
    auto self = shared_from_this();
    Engine::instance().submit_job([self] { self->execute(); });
}

auto RunRecord::flush() -> void {
    if (cancelled_ || !pending_) {
        return;
    }
    pending_ = false;
    bool changed = false;
    for (auto &dep : deps_) {
        dep.node->ensure_fresh();
        if (dep.node->version() != dep.seen) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    try {
        execute();
    } catch (...) {
        pending_ = false;
        throw;
    }
}

auto Engine::instance() -> Engine & {
    static auto *engine = new Engine();  // 进程级常驻：覆盖静态销毁期的最后一次注销
    return *engine;
}

auto Engine::submit_job(std::function<void()> job) -> void {
    if (depth_ > 0) {
        jobs_.push_back({.fn = std::move(job), .chain = active_chain_});
        return;
    }
    ++depth_;
    try {
        run_job(job, {});
        while (!jobs_.empty()) {
            Job next = std::move(jobs_.front());
            jobs_.pop_front();
            run_job(next.fn, std::move(next.chain));
        }
    } catch (...) {
        jobs_.clear();
        active_chain_.clear();
        --depth_;
        throw;
    }
    active_chain_.clear();
    --depth_;
}

auto Engine::run_job(const std::function<void()> &fn, std::vector<std::uint64_t> chain) -> void {
    std::vector<std::uint64_t> saved = std::move(active_chain_);
    active_chain_ = std::move(chain);
    try {
        fn();
    } catch (...) {
        active_chain_ = std::move(saved);
        throw;
    }
    active_chain_ = std::move(saved);
}

auto Engine::write_committed(SourceNode &src) -> void {
    src.bump();
    pending_.clear();
    seen_sources_.clear();
    seen_observers_.clear();
    mark(src);
    deliver();
}

auto Engine::mark(SourceNode &src) -> void {
    if (!seen_sources_.insert(src.id()).second) {
        return;
    }
    for (Observer *observer : src.observers()) {
        if (!seen_observers_.insert(observer->seq()).second) {
            continue;
        }
        observer->on_invalidate();
        if (auto *downstream = observer->as_source(); downstream != nullptr) {
            mark(*downstream);
        } else if (auto record = observer->as_terminal(); record != nullptr) {
            pending_.push_back(std::move(record));
        }
    }
}

auto Engine::deliver() -> void {
    std::vector<std::shared_ptr<RunRecord>> batch = std::move(pending_);
    pending_.clear();
    std::sort(batch.begin(), batch.end(),
              [](const std::shared_ptr<RunRecord> &left, const std::shared_ptr<RunRecord> &right) -> bool {
                  return left->seq() < right->seq();
              });
    for (auto &record : batch) {
        if (!record || record->cancelled()) {
            continue;
        }
        if (std::find(active_chain_.begin(), active_chain_.end(), record->seq()) != active_chain_.end()) {
            AURORA_LOG_ERROR("state", "reactive feedback loop cut");
            continue;
        }
        active_chain_.push_back(record->seq());
        try {
            record->flush();
        } catch (const std::exception &error) {
            AURORA_LOG_ERROR("state", std::string("reactive delivery failed: ") + error.what());
        } catch (...) {
            AURORA_LOG_ERROR("state", "reactive delivery failed");
        }
        active_chain_.pop_back();
    }
}

}  // namespace aurora::detail
