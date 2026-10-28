#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 取消等值去重的具名标记：创建来源或派生时传入，表示每次提交都通知。
struct AlwaysPublish {};
/// @brief `AlwaysPublish` 的便捷常量（`au::always_publish`）。
/// 公共标记对象按「表达式中的谓词」用 snake_case，与 `std::nullopt` / `std::in_place`
/// 一类 sentinel 同一惯例；全大写会读成可配置常量。
/// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr AlwaysPublish always_publish{};

/// @brief 内部实现细节：响应式节点与传播引擎，不属于公共 API 契约面。
namespace detail {

class Engine;
class Observer;
class RunRecord;

/// @brief 依赖收集作用域：一次求值或一次回调执行期间读取到的来源。
///
/// 派生重算与终端执行各自压入独立作用域，内层读取不会泄漏到外层；
/// 作用域外读取只取值，不建立依赖边。
class DepsCollector {
  public:
    /// @brief 虚析构（default）：收集器由栈上作用域对象引用，不拥有资源。
    virtual ~DepsCollector() = default;
    DepsCollector() = default;
    DepsCollector(const DepsCollector &) = delete;
    auto operator=(const DepsCollector &) -> DepsCollector & = delete;
    DepsCollector(DepsCollector &&) = delete;
    auto operator=(DepsCollector &&) -> DepsCollector & = delete;
    /// @brief 登记一次读取。
    /// @param src 被读取的来源节点。
    /// @param version 读取时的来源版本，供后续判断是否真的变化。
    virtual auto add(class SourceNode &src, std::uint64_t version) -> void = 0;
};

/// @brief 当前线程的依赖收集器（无作用域时为 `nullptr`）。
/// @return 正在求值或执行回调的收集器；不在作用域内时为空。
[[nodiscard]] auto current_collector() -> DepsCollector *;

/// @brief 依赖收集作用域的 RAII 守卫：构造压栈，析构恢复外层收集器。
class CollectorScope {
  public:
    /// @brief 进入给定收集器的作用域。
    /// @param collector 本次求值或回调执行的收集器。
    explicit CollectorScope(DepsCollector &collector) noexcept;
    /// @brief 恢复外层收集器。
    ~CollectorScope();
    CollectorScope(const CollectorScope &) = delete;
    auto operator=(const CollectorScope &) -> CollectorScope & = delete;
    CollectorScope(CollectorScope &&) = delete;
    auto operator=(CollectorScope &&) -> CollectorScope & = delete;

  private:
    DepsCollector *previous_ = nullptr;  ///< 外层收集器（析构时恢复）
};

/// @brief 临时关闭依赖收集的 RAII 守卫：其作用域内的读取不登记到外层。
///
/// 用于 `State::update` 的更新回调：调用方可能正处于 effect 执行体内，
/// 更新回调中的读取属于纯计算，不得扩充外层依赖。
class SuspendCollection {
  public:
    /// @brief 关闭当前线程的依赖收集。
    SuspendCollection() noexcept;
    /// @brief 恢复进入前的外层收集器。
    ~SuspendCollection();
    SuspendCollection(const SuspendCollection &) = delete;
    auto operator=(const SuspendCollection &) -> SuspendCollection & = delete;
    SuspendCollection(SuspendCollection &&) = delete;
    auto operator=(SuspendCollection &&) -> SuspendCollection & = delete;

  private:
    DepsCollector *previous_ = nullptr;  ///< 进入前的外层收集器（析构时恢复）
};

/// @brief 响应式图中的观察者：派生节点或终端运行记录。
///
/// 只承担「被标脏」与「按轮交付」两件事；标脏期间不执行任何用户代码。
class Observer {
  public:
    /// @brief 构造并分配全局登记序号（决定同轮交付顺序）。
    Observer();
    /// @brief 虚析构（default）：具体节点自行负责从依赖来源退订。
    virtual ~Observer() = default;
    Observer(const Observer &) = delete;
    auto operator=(const Observer &) -> Observer & = delete;
    Observer(Observer &&) = delete;
    auto operator=(Observer &&) -> Observer & = delete;

    /// @brief 全局登记序号。
    /// @return 构造顺序，数值小的先交付。
    [[nodiscard]] auto seq() const noexcept -> std::uint64_t { return seq_; }

    /// @brief 标记本节点受本轮影响；实现只置脏标志，不执行用户代码。
    virtual auto on_invalidate() -> void = 0;

    /// @brief 引擎在一轮中交付本节点；非终端节点为空操作。
    virtual auto flush() -> void = 0;

    /// @brief 若本节点同时是来源（派生），返回其来源视图。
    /// @return 派生节点的来源指针；终端记录为空。
    [[nodiscard]] virtual auto as_source() -> class SourceNode * { return nullptr; }

    /// @brief 若本节点是终端运行记录，返回可保活的共享句柄。
    /// @return 终端记录的共享指针；派生节点为空。
    [[nodiscard]] virtual auto as_terminal() -> std::shared_ptr<RunRecord> { return nullptr; }

  private:
    std::uint64_t seq_ = 0;
};

/// @brief 响应式图的来源节点：值、版本号与直接观察者表。
///
/// 提交时版本号自增并由引擎向可达终端传播；来源本身不执行任何回调。
class SourceNode : public std::enable_shared_from_this<SourceNode> {
  public:
    /// @brief 节点角色：供依赖图按角色分类展示（不参与传播判定）。
    enum class NodeRole : std::uint8_t {
        Source,  ///< 值来源（`State` / `ReadOnlyState` 持有的存储）
        Derived,  ///< 派生来源（`computed` 的缓存节点）
    };

    /// @brief 虚析构：解除与全部观察者的边。
    virtual ~SourceNode();
    SourceNode(const SourceNode &) = delete;
    auto operator=(const SourceNode &) -> SourceNode & = delete;
    SourceNode(SourceNode &&) = delete;
    auto operator=(SourceNode &&) -> SourceNode & = delete;

    /// @brief 稳定节点身份：独立于对象地址，复制句柄不新增节点。
    /// @return 进程内唯一且不复用的编号。
    [[nodiscard]] auto id() const noexcept -> std::uint64_t { return id_; }

    /// @brief 本节点的角色。
    /// @return 普通值来源为 `NodeRole::Source`，派生节点覆写为 `NodeRole::Derived`。
    [[nodiscard]] virtual auto role() const noexcept -> NodeRole { return NodeRole::Source; }

    /// @brief 依赖来源快照：供依赖图读出「读取」边，不扩大依赖。
    /// @return 普通来源不读取任何节点，返回空表；派生节点覆写为最近一次求值的读取集合。
    [[nodiscard]] virtual auto dependency_nodes() const -> std::vector<std::shared_ptr<SourceNode>> { return {}; }

    /// @brief 当前版本号：值为可观察等价时保持不变。
    /// @return 每次有效提交自增的计数。
    [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }

    /// @brief 保证本节点读数最新；来源为空操作，派生按需重算。
    virtual auto ensure_fresh() -> void {}

    /// @brief 类型擦除的读取：保鲜并登记依赖。
    ///
    /// 供只掌握 `SourceNode` 的采集方（控件信号采集）统一建立读取依赖。
    virtual auto read_erased() -> void {}

    /// @brief 登记观察者（去重由调用方保证）。
    /// @param observer 观察者。
    auto add_observer(Observer &observer) -> void;
    /// @brief 摘除观察者；不存在时为空操作。
    /// @param observer 观察者。
    auto remove_observer(Observer &observer) noexcept -> void;

  protected:
    /// @brief 构造并分配稳定身份。
    SourceNode();
    /// @brief 值已提交：版本号自增。
    auto bump() noexcept -> void { ++version_; }

  private:
    /// @brief 直接观察者表（裸指针；观察者析构时自行退订）。
    /// @return 观察者表引用。
    [[nodiscard]] auto observers() noexcept -> std::vector<Observer *> & { return observers_; }

    std::vector<Observer *> observers_;
    std::uint64_t id_ = 0;
    std::uint64_t version_ = 0;

    friend class Engine;
};

/// @brief 带类型读口的来源基类：状态来源与派生节点共用。
/// @tparam T 值的类型。
template <typename T>
class SourceOf : public SourceNode {
  public:
    /// @brief 当前值（不登记依赖、不触发重算）。
    /// @return 最近一次提交值的常引用。
    [[nodiscard]] virtual auto value() const -> const T & = 0;

    /// @brief 读取并登记依赖：先保鲜，再登记到当前收集器。
    /// @return 当前值的常引用。
    auto read() -> const T & {
        ensure_fresh();
        if (auto *collector = current_collector(); collector != nullptr) {
            collector->add(*this, version());
        }
        return value();
    }

    /// @brief 类型擦除读取：转发 `read()`。
    auto read_erased() -> void override { (void)read(); }
};

/// @brief 值的默认等价策略：可相等比较时用 `==`，否则视为永不相等。
/// @tparam T 值类型。
/// @return 等价判定可调用体；不可比较类型返回空函数。
template <typename T>
[[nodiscard]] auto default_equivalence() -> std::function<bool(const T &, const T &)> {
    if constexpr (std::equality_comparable<T>) {
        return [](const T &left, const T &right) -> bool { return left == right; };
    } else {
        return {};
    }
}

/// @brief 传播引擎：写队列、标脏与稳定轮次交付。
///
/// 写入在未进入入口时立即执行并排空队列；回调内的写入排到后续轮，
/// 当前轮读数不变。反馈环按因果链识别并有限停止。
class Engine {
  public:
    /// @brief 进程级单例。
    /// @return 引擎引用。
    static auto instance() -> Engine &;

    /// @brief 提交一次写入作业：作用域外立即执行，作用域内排队。
    /// @param job 执行远端存储变更的闭包。
    auto submit_job(std::function<void()> job) -> void;

    /// @brief 值已提交：版本自增并向可达终端交付一个稳定轮次。
    /// @param src 已提交的来源。
    auto write_committed(SourceNode &src) -> void;

    /// @brief 是否处于外层入口内。
    /// @return 已进入入口（含排空后续轮）时为 true。
    [[nodiscard]] auto in_entry() const noexcept -> bool { return depth_ > 0; }

  private:
    /// @brief 一次待执行写入：闭包与其因果链（由触发它的终端贡献）。
    struct Job {
        std::function<void()> fn;  ///< 写入闭包
        std::vector<std::uint64_t> chain;  ///< 执行前生效的因果链
    };

    Engine() = default;
    /// @brief 从提交来源出发标脏可达节点并收集终端。
    auto mark(SourceNode &src) -> void;
    /// @brief 按登记顺序交付本轮终端。
    auto deliver() -> void;
    /// @brief 在给定因果链下执行一个写入闭包。
    ///
    /// 闭包按常引用收：它只被 `operator()` 调用、不被转移也不被改写，按值收会为每轮
    /// 投递多付一次 `std::function` 的堆搬运。因果链仍按值收，直接换入 `active_chain_`。
    auto run_job(const std::function<void()> &fn, std::vector<std::uint64_t> chain) -> void;

    std::deque<Job> jobs_;
    std::vector<std::shared_ptr<RunRecord>> pending_;
    std::vector<std::uint64_t> active_chain_;
    std::unordered_set<std::uint64_t> seen_sources_;
    std::unordered_set<std::uint64_t> seen_observers_;
    std::uint32_t depth_ = 0;
};

/// @brief 值来源：状态句柄共享的存储与提交入口。
/// @tparam T 值的类型。
/// @note 不重复继承 `enable_shared_from_this`：统一经 `SourceNode` 的基类取自身句柄，
///       避免两个基类导致 `shared_from_this` 歧义与弱引用初始化落空。
template <typename T>
class Source final : public SourceOf<T> {
  public:
    /// @brief 以初值构造，采用默认等价策略。
    /// @param value 初始值。
    explicit Source(T value) : value_(std::move(value)), eq_(default_equivalence<T>()) {}

    /// @brief 以初值构造并关闭去重。
    /// @param value 初始值。
    /// @param marker `always_publish` 标记。
    Source(T value, AlwaysPublish marker) : value_(std::move(value)) { (void)marker; }

    /// @brief 以初值构造并注入自定义等价。
    /// @tparam Eq 等价判定类型。
    /// @param value 初始值。
    /// @param eq 等价判定：返回 true 表示可观察等价。
    template <typename Eq>
    Source(T value, Eq eq)
        requires(!std::same_as<std::remove_cvref_t<Eq>, AlwaysPublish>)
        : value_(std::move(value)), eq_(std::function<bool(const T &, const T &)>(std::move(eq))) {}

    /// @brief 当前值。
    /// @return 最近一次提交值的常引用。
    [[nodiscard]] auto value() const -> const T & override { return value_; }

    /// @brief 提交新值：等价时不替换、不通知；否则排队写入并通知。
    /// @param next 候选值。
    auto assign(T next) -> void {
        auto self = std::static_pointer_cast<Source>(this->shared_from_this());
        Engine::instance().submit_job([self, next = std::move(next)]() mutable {
            if (self->eq_ && self->eq_(next, self->value_)) {
                return;
            }
            self->value_ = std::move(next);
            Engine::instance().write_committed(*self);
        });
    }

    /// @brief 原地修改载荷后强制发布一次，不经过等价比较。
    auto force_publish() -> void {
        auto self = std::static_pointer_cast<Source>(this->shared_from_this());
        Engine::instance().submit_job([self]() { Engine::instance().write_committed(*self); });
    }

  private:
    T value_;
    std::function<bool(const T &, const T &)> eq_;
};

/// @brief 终端运行记录：订阅或副作用的一次执行单元。
///
/// 只保活自身与依赖来源；取消只解边与置标志，不打断正在执行的回调。
class RunRecord final : public Observer, public DepsCollector, public std::enable_shared_from_this<RunRecord> {
  public:
    /// @brief 以回调构造；尚未执行，也未登记依赖。
    /// @param fn 回调体，执行期间的读取成为依赖。
    explicit RunRecord(std::function<void()> fn);
    /// @brief 析构：取消并解边。
    ~RunRecord() override;
    RunRecord(const RunRecord &) = delete;
    auto operator=(const RunRecord &) -> RunRecord & = delete;
    RunRecord(RunRecord &&) = delete;
    auto operator=(RunRecord &&) -> RunRecord & = delete;

    /// @brief 标脏：仅置待交付标志。
    auto on_invalidate() -> void override;
    /// @brief 交付：依赖确实变化时执行回调，否则跳过。
    auto flush() -> void override;

    /// @brief 首次交付一次：入口外立即执行；回调内创建时排到当前轮之后。
    auto run_first() -> void;

    /// @brief 取消：解边并置标志；此后不再交付。
    auto cancel() noexcept -> void;

    /// @brief 是否已取消。
    /// @return 已取消时为 true。
    [[nodiscard]] auto cancelled() const noexcept -> bool { return cancelled_; }

    /// @brief 登记依赖（收集器实现）。
    /// @param src 被读取来源。
    /// @param version 读取时版本。
    auto add(SourceNode &src, std::uint64_t version) -> void override;

    /// @brief 终端共享句柄。
    /// @return 指向自身的共享指针。
    [[nodiscard]] auto as_terminal() -> std::shared_ptr<RunRecord> override;

    /// @brief 当前依赖来源快照（供依赖图读出「读取」边，不扩大依赖）。
    /// @return 依赖来源的共享句柄列表，顺序为最近一次执行时的读取顺序。
    [[nodiscard]] auto dependency_nodes() const -> std::vector<std::shared_ptr<SourceNode>> {
        std::vector<std::shared_ptr<SourceNode>> out;
        out.reserve(deps_.size());
        for (const auto &dep : deps_) {
            out.push_back(dep.node);
        }
        return out;
    }

  private:
    /// @brief 一条依赖边：来源与读取时版本；共享句柄使记录保活其依赖来源。
    struct Dep {
        std::shared_ptr<SourceNode> node;  ///< 来源节点（本记录持有）
        std::uint64_t seen = 0;  ///< 最近一次读取时的版本
    };

    /// @brief 按本轮读取集合替换依赖边。
    auto replace_deps() -> void;
    /// @brief 执行回调并以执行期读取重建依赖。
    auto execute() -> void;

    std::function<void()> fn_;
    std::vector<Dep> deps_;
    std::vector<Dep> next_;
    bool pending_ = false;
    bool cancelled_ = false;
    bool running_ = false;
};

/// @brief 派生节点：按需重算的缓存来源。
/// @tparam T 结果类型。
template <typename T>
class Derived final : public SourceOf<T>, public Observer, public DepsCollector {
  public:
    /// @brief 以计算体构造（惰性：首次读取时求值）。
    /// @param fn 纯计算体，执行期间读取的来源成为依赖。
    /// @param eq 等价判定；空函数表示每次重算都视为变化。
    Derived(std::function<T()> fn, std::function<bool(const T &, const T &)> eq)
        : fn_(std::move(fn)), eq_(std::move(eq)) {}
    Derived(const Derived &) = delete;
    auto operator=(const Derived &) -> Derived & = delete;
    Derived(Derived &&) = delete;
    auto operator=(Derived &&) -> Derived & = delete;

    /// @brief 析构：解除与上游来源的边。
    ~Derived() override {
        for (const auto &dep : deps_) {
            dep.node->remove_observer(*this);
        }
    }

    /// @brief 当前缓存值（不保鲜）。
    /// @return 最近一次成功求值的常引用；首次求值前读取属逻辑错误。
    [[nodiscard]] auto value() const -> const T & override {
        if (!value_) {
            throw std::logic_error("derived read before first evaluation");
        }
        return *value_;
    }

    /// @brief 若已标脏则重算；失败保留旧值与旧依赖并继续标脏。
    auto ensure_fresh() -> void override {
        if (!dirty_) {
            return;
        }
        if (computing_) {
            throw std::logic_error("derived reentered during evaluation");
        }
        computing_ = true;
        next_.clear();
        CollectorScope scope(*this);
        try {
            T candidate = fn_();
            const bool changed = !value_ || !eq_ || !eq_(candidate, *value_);
            replace_deps();
            if (changed) {
                value_ = std::move(candidate);
                this->bump();
            }
            dirty_ = false;
        } catch (...) {
            computing_ = false;
            throw;
        }
        computing_ = false;
    }

    /// @brief 标脏：不重算。
    auto on_invalidate() -> void override { dirty_ = true; }
    /// @brief 非终端：本轮不交付。
    auto flush() -> void override {}
    /// @brief 派生同时是来源：供引擎继续向下游传播。
    /// @return 本节点的来源视图。
    [[nodiscard]] auto as_source() -> SourceNode * override { return this; }
    /// @brief 本节点角色：派生来源。
    /// @return 恒为 `NodeRole::Derived`。
    [[nodiscard]] auto role() const noexcept -> SourceNode::NodeRole override { return SourceNode::NodeRole::Derived; }
    /// @brief 当前依赖来源快照（供依赖图读出「读取」边，不扩大依赖）。
    /// @return 依赖来源的共享句柄列表，顺序为最近一次求值时的读取顺序。
    [[nodiscard]] auto dependency_nodes() const -> std::vector<std::shared_ptr<SourceNode>> override {
        std::vector<std::shared_ptr<SourceNode>> out;
        out.reserve(deps_.size());
        for (const auto &dep : deps_) {
            out.push_back(dep.node);
        }
        return out;
    }
    /// @brief 收集一次求值期间的读取；同一来源重复读取只保留一条边。
    /// @param src 被读取来源。
    /// @param version 读取时版本（重复读取时取最近一次）。
    auto add(SourceNode &src, std::uint64_t version) -> void override {
        for (auto &dep : next_) {
            if (dep.node.get() == &src) {
                dep.seen = version;
                return;
            }
        }
        next_.push_back({src.shared_from_this(), version});
    }

  private:
    /// @brief 一条依赖边；共享句柄使派生保活其上游来源。
    struct Dep {
        std::shared_ptr<SourceNode> node;  ///< 来源节点（本节点持有）
        std::uint64_t seen = 0;  ///< 读取时版本
    };

    /// @brief 用本轮读取集合替换依赖边（去掉旧分支，补上新分支）。
    auto replace_deps() -> void {
        const auto contains = [](const std::vector<Dep> &list, const SourceNode *node) -> bool {
            return std::ranges::any_of(list, [node](const Dep &dep) -> bool { return dep.node.get() == node; });
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
        deps_ = next_;
        next_.clear();
    }

    std::function<T()> fn_;
    std::function<bool(const T &, const T &)> eq_;
    std::optional<T> value_;
    std::vector<Dep> deps_;
    std::vector<Dep> next_;
    bool dirty_ = true;
    bool computing_ = false;
};

}  // namespace detail
}  // namespace aurora
