#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/state/binding.h"
#include "aurora/state/state.h"
#include "aurora/widget/props_io.h"

namespace aurora::preferences {

/**
 * @brief 轻量持久化配置（对标 Android SharedPreferences / iOS UserDefaults）。
 *
 * 设计要点（经需求澄清确认）：
 * - 配置以**单个 JSON 文件**为载体；应用初始化时通过构造参数**显式指定**文件路径。
 * - 未指定文件位置 → **仅内存**存储（`is_persistent() == false`），`flush`/`reload` 返回错误。
 * - 指定了文件位置 → 构造时加载该文件到内存；`set` 只更新内存与响应式 `State`，
 *   **不**自动写穿文件；落盘由 `flush()` **主动刷新**完成（业界主流的显式提交模型，
 *   对标 `SharedPreferences.edit().commit()` / `Settings.Save()`）。
 * - 内部复用现有响应式原语 `State<T>` / `Binding<T>`，已有 `Switch` / `TextInput` /
 *   `Slider` 等控件可直接绑定并自动落盘。**不新增任何 UI widget 类型**。
 *
 * 并发安全（本次新增）：
 * - **线程安全**：实例内部以 `std::shared_mutex` 保护内存 JSON 与 State 注册表，
 *   读操作（`get`/`contains`/`keys`/`watch`）走共享锁，写操作（`set`/`remove`/`clear`/
 *   `flush`/`reload`）走独占锁，可在多线程下安全读写。
 * - **进程安全**：`flush`/`reload` 期间对 `<file>.lock` 加跨平台 advisory 文件锁
 *   （Windows `LockFileEx` / POSIX `flock`），并采用「写临时文件 + 原子 `rename`」，
 *   避免多进程并发写导致半写损坏或互相覆盖。删除键采用**版本化 LWW + 墓碑（tombstone）+
 *   全局清空纪元**实现可靠语义：`remove` 写入墓碑并随 `flush` 持久化、跨进程传播，其他进程在
 *   下次 `flush`/`reload` 时学习墓碑并同步删除；`clear` 置全局清空纪元，所有旧版本键被各进程
 *   删除，清空后新 `set` 的键不受影响。详见 `specification/06-app-platform.md` §9.1。
 * - **单例**：`instance(name)` 提供按名注册表的全局单例访问（每个 name 唯一、懒构造、
 *   线程安全创建）；原构造器依然可用（内存模式 / 测试 / 非单例场景）。
 *
 * 支持的值类型：`bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象。
 *
 * 分组：通过 `group(name)` 获取作用域子视图（`Group`），其内 `get/set/watch/binding/
 * contains/keys/remove/clear` 自动限定在该命名分组下，并以嵌套 JSON 对象持久化
 * （如 `{"ui":{"theme":"dark"}}`）；分组的 `remove`/`clear` 同样走墓碑可靠删除。详见 `Group`。
 *
 * @note Thread: thread-safe with mutex (std::shared_mutex for read/write)
 * @note Side-effects: none (file I/O via flush/reload)
 * @note Rebuildable: yes, via from_json
 */

// ----- 嵌套 JSON 路径助手（复合点号键） -----
// 供头文件模板方法（`*_impl`）与 `preferences.cpp` 的 `reconcile` 共用：
// 分组键以点号路径（如 `"ui.theme"`）在嵌套 `m_root` 中寻址；顶层键（无点号）语义不变。
auto resolve_get(const Json &root, const std::string &composite) -> Json;
auto resolve_set(Json &root, const std::string &composite, Json value) -> void;
auto resolve_erase(Json &root, const std::string &composite) -> void;
auto flatten(const Json &root) -> std::unordered_map<std::string, Json>;

class Preferences {
  public:
    /// @brief 构造选项。
    struct Options {
        bool auto_create_dir{true};  ///< flush/reload 时若父目录不存在则自动创建（默认开启）。
        Options() = default;
    };

    /// @brief 内存模式：不绑定任何文件，所有写入仅存于内存。
    Preferences() = default;

    /// @brief 文件模式（默认 Options）：显式指定配置存储的 JSON 文件路径，构造即加载（文件不存在则为空对象）。
    /// @param file 配置文件的完整路径；可位于任意位置（含子目录，目录会自动创建）。
    explicit Preferences(std::filesystem::path file) : file_(std::move(file)), opts_(Options{}) {
        load_from_file();
    }

    /// @brief 文件模式：显式指定配置存储的 JSON 文件路径与选项，构造即加载（文件不存在则为空对象）。
    /// @param file 配置文件的完整路径；可位于任意位置（含子目录，目录会自动创建）。
    /// @param opts 选项（如 `auto_create_dir`）。
    explicit Preferences(std::filesystem::path file, Options opts) : file_(std::move(file)), opts_(std::move(opts)) {
        load_from_file();
    }

    /// @brief 便捷构造：在指定路径创建文件模式实例（默认 Options）。
    [[nodiscard]] static auto at(std::filesystem::path file) -> Preferences { return Preferences(std::move(file)); }
    /// @brief 便捷构造：在指定路径创建文件模式实例。
    [[nodiscard]] static auto at(std::filesystem::path file, Options opts) -> Preferences {
        return Preferences(std::move(file), std::move(opts));
    }

    /// @brief 便捷构造：在平台默认配置目录（`default_config_dir()`）下以 `name`（自动补 `.json`）命名配置文件。
    [[nodiscard]] static auto with_location(std::string name) -> Preferences {
        return with_location(std::move(name), default_config_dir());
    }
    /// @brief 便捷构造：在 `dir` 下以 `name`（自动补 `.json`）命名配置文件。
    [[nodiscard]] static auto with_location(std::string name, const std::filesystem::path &dir) -> Preferences {
        return with_location(std::move(name), dir, Options{});
    }
    /// @brief 便捷构造：在 `dir` 下以 `name`（自动补 `.json`）命名配置文件，并显式指定选项。
    /// @param dir 配置目录；默认取平台配置目录。
    [[nodiscard]] static auto with_location(std::string name, const std::filesystem::path &dir, Options opts) -> Preferences {
        if (!name.ends_with(".json")) {
            name += ".json";
        }
        return Preferences(dir / name, std::move(opts));
    }

    // ---------- 单例（按名注册表，线程安全懒构造） ----------

    /// @brief 全局单例（默认名 `"app"`，文件模式，使用平台默认配置目录）。
    [[nodiscard]] static auto instance(const std::string &name = "app") -> Preferences &;

    /// @brief 在 `dir` 下以 `name`（自动补 `.json`）命名的单例实例。
    [[nodiscard]] static auto instance(const std::string &name, const std::filesystem::path &dir) -> Preferences &;

    /// @brief 显式文件路径的单例实例（初始化时即指定存储位置；此后同名调用忽略路径参数）。
    [[nodiscard]] static auto instance_at(const std::string &name, std::filesystem::path file) -> Preferences &;

    /// @brief 解析平台默认配置目录（不依赖任何窗口后端）。
    /// 优先级：XDG_CONFIG_HOME → LOCALAPPDATA(Windows) → HOME/.config → 当前工作目录。
    [[nodiscard]] static auto default_config_dir() -> std::filesystem::path;

    /// @brief 是否绑定了文件（持久化模式）。
    [[nodiscard]] auto is_persistent() const -> bool { return !file_.empty(); }

    /// @brief 当前配置文件路径（内存模式返回空路径）。
    [[nodiscard]] auto file_path() const -> const std::filesystem::path & { return file_; }

    /// @brief 最近一次文件加载（`flush`/`reload` 不涉及）产生的错误；无错误则为 nullopt。
    [[nodiscard]] auto last_load_error() const -> std::optional<Error> {
        std::unique_lock lock(mutex_);
        return load_error_;
    }

    // ---------- 分组（作用域子视图） ----------

    /// @brief 命名分组的作用域子视图：把读写/订阅/删除限定在该分组下，数据以嵌套 JSON 持久化。
    /// 例：`prefs.group("ui").set("theme", "dark")` → 文件内 `{"ui":{"theme":"dark"}}`。
    /// 可链式嵌套：`prefs.group("ui").group("editor").set("font", 14)`。
    /// 与扁平 key 共存于同一实例/文件；分组的 `remove`/`clear` 同样走墓碑可靠删除。
    class Group {
      public:
        Group(Preferences *owner, std::string path) : owner_(owner), path_(std::move(path)) {}

        /// @brief 读取分组内键值；缺失/类型不匹配回退 `fallback`。
        template <typename T>
        [[nodiscard]] auto get(const std::string &key, T fallback) const -> T {
            return owner_->get_impl(path_, key, std::move(fallback));
        }

        /// @brief 写入分组内键值（仅内存 + State，不自动落盘）。
        template <typename T>
        auto set(const std::string &key, T value) -> void {
            owner_->set_impl(path_, key, std::move(value));
        }

        /// @brief 惰性创建分组内键的 `State<T>`，供控件订阅。
        template <typename T>
        [[nodiscard]] auto watch(const std::string &key, T fallback) -> std::shared_ptr<State<T>> {
            return owner_->watch_impl(path_, key, std::move(fallback));
        }

        /// @brief 分组内键的非拥有 `Binding<T>`（注入删除回调）。
        template <typename T>
        [[nodiscard]] auto binding(const std::string &key, T fallback) -> Binding<T> {
            return owner_->binding_impl(path_, key, std::move(fallback));
        }

        /// @brief 分组内是否含键（且非 null）。
        [[nodiscard]] auto contains(const std::string &key) const -> bool { return owner_->contains_impl(path_, key); }

        /// @brief 分组内所有直接子键名（不含分组前缀）。
        [[nodiscard]] auto keys() const -> std::vector<std::string> { return owner_->keys_impl(path_); }

        /// @brief 删除分组内键（墓碑可靠语义，需 flush 落盘）。
        auto remove(const std::string &key) const -> void { owner_->remove_impl(path_, key); }

        /// @brief 清空本分组子树（对该子树已知键打墓碑，不影响其他分组与顶层键）。
        auto clear() const -> void { owner_->clear_impl(path_); }

        /// @brief 链式嵌套子分组。
        [[nodiscard]] auto group(const std::string &name) const -> Group {
            return {owner_, path_.empty() ? name : path_ + "." + name};
        }

      private:
        Preferences *owner_;
        std::string path_;  // 复合前缀，如 "ui" / "ui.editor"
    };

    /// @brief 获取命名分组的作用域子视图（见 `Group`）。
    [[nodiscard]] auto group(const std::string &name) -> Group { return {this, name}; }

    // ---------- 读取（共享锁） ----------

    /// @brief 读取键值（根作用域）；类型不匹配或键缺失时回退 `fallback`。
    template <typename T>
    [[nodiscard]] auto get(const std::string &key, T fallback) const -> T {
        return get_impl("", key, std::move(fallback));
    }

    // ---------- 写入（仅内存 + 响应式 State；不写文件；独占锁） ----------

    /// @brief 写入键值（根作用域）：更新内存 JSON 与对应 `State`（通知订阅者），**不**自动落盘。
    /// 落盘须调用 `flush()`。
    template <typename T>
    auto set(const std::string &key, T value) -> void {
        set_impl("", key, std::move(value));
    }

    /// @brief 惰性创建并缓存该键的 `State<T>`，供控件订阅；初始值为当前存储值或 `fallback`。
    template <typename T>
    [[nodiscard]] auto watch(const std::string &key, T fallback) -> std::shared_ptr<State<T>> {
        return watch_impl("", key, std::move(fallback));
    }

    /// @brief 基于 `watch` 的 `State` 返回非拥有 `Binding<T>`：控件卸载时可调用 `binding.remove()`
    ///        删除对应持久化键（走墓碑可靠删除）；调用 `remove()` 后该 Binding 即失效。
    template <typename T>
    [[nodiscard]] auto binding(const std::string &key, T fallback) -> Binding<T> {
        return binding_impl("", key, std::move(fallback));
    }

    // ---------- 持久化（独占锁 + 进程文件锁） ----------

    /// @brief 主动将内存内容刷新（写穿）到文件。内存模式返回错误。
    [[nodiscard]] auto flush() -> Result<void>;

    /// @brief 从文件重新加载到内存，并通知所有已订阅的 `State`。内存模式返回错误。
    [[nodiscard]] auto reload() -> Result<void>;

    // ---------- 批量操作 ----------

    /// @brief 根作用域已存在的所有键（不含分组前缀）。
    [[nodiscard]] auto keys() const -> std::vector<std::string> { return keys_impl(""); }

    /// @brief 根作用域键是否存在（且非 null）。
    [[nodiscard]] auto contains(const std::string &key) const -> bool { return contains_impl("", key); }

    /// @brief 删除根作用域键（可靠语义：写入墓碑并随 `flush` 持久化、跨进程传播；需 `flush` 落盘）。
    auto remove(const std::string &key) -> void { remove_impl("", key); }

    /// @brief 清空全部（可靠语义：全局清空纪元 + 已知键墓碑；随 `flush` 传播到其他进程）。
    auto clear() -> void { clear_impl(""); }

  private:
    /// @brief 类型擦除的状态持有者，用于在 `set`/`reload` 时把 JSON 推回具体 `State<T>`。
    struct IStateHolder {
        IStateHolder() = default;
        virtual ~IStateHolder() = default;
        IStateHolder(const IStateHolder &) = delete;
        auto operator=(const IStateHolder &) -> IStateHolder & = delete;
        IStateHolder(IStateHolder &&) = delete;
        auto operator=(IStateHolder &&) -> IStateHolder & = delete;
        virtual void push(const Json &j) = 0;
    };

    template <typename T>
    struct StateHolder : IStateHolder {
        std::shared_ptr<State<T>> state;
        T fallback;
        StateHolder(std::shared_ptr<State<T>> s, T fb) : state(std::move(s)), fallback(std::move(fb)) {}
        void push(const Json &j) override {
            if (j.is_null()) {
                state->set(fallback);
                return;
            }
            try {
                state->set(j.get<T>());
            } catch (...) {
                state->set(fallback);
            }
        }
    };

    auto load_from_file() -> void;

    // ----- 作用域化内部实现（scope 为空 = 根作用域；Group 以 m_path 为 scope 委托） -----
    template <typename T>
    [[nodiscard]] auto get_impl(const std::string &scope, const std::string &key, T fallback) const -> T;
    template <typename T>
    auto set_impl(const std::string &scope, const std::string &key, T value) -> void;
    template <typename T>
    [[nodiscard]] auto watch_impl(const std::string &scope, const std::string &key, T fallback)
        -> std::shared_ptr<State<T>>;
    template <typename T>
    [[nodiscard]] auto binding_impl(const std::string &scope, const std::string &key, T fallback) -> Binding<T>;
    [[nodiscard]] auto contains_impl(const std::string &scope, const std::string &key) const -> bool;
    [[nodiscard]] auto keys_impl(const std::string &scope) const -> std::vector<std::string>;
    auto remove_impl(const std::string &scope, const std::string &key) -> void;
    auto clear_impl(const std::string &scope) -> void;

    static auto registry() -> std::unordered_map<std::string, std::unique_ptr<Preferences>> &;
    static auto registry_mutex() -> std::mutex &;

    std::filesystem::path file_;  // 空 = 内存模式
    Options opts_;  // 默认构造即 auto_create_dir=true（Options 为聚合类型，见 Options）
    Json root_ = Json::object();  // 内存 JSON 存储
    std::unordered_map<std::string, std::shared_ptr<IStateHolder>> states_;  // key -> State
    std::optional<Error> load_error_;
    // 用 std::mutex 而非 std::shared_mutex：MinGW-w64 winpthreads 的 rwlock 在多线程
    // 并发写锁竞争下会间歇性返回非零，触发 libstdc++ `__shared_mutex_pthread::lock()`
    // 的 `__ret == 0` 断言（test_preferences 稳定复现）。本类临界区均为内存操作、
    // 读多写少但单次耗时纳秒级，独占锁无可观测性能差异。
    mutable std::mutex mutex_;  // 保护上述可变状态

    // ----- 多进程可靠删除所需元数据（受 m_mutex 保护） -----
    /// @brief 键 -> 本进程显式 set 的写入版本（时间戳，LWW 依据之一）。不合并磁盘版本。
    std::unordered_map<std::string, double> versions_;
    /// @brief 已删除键的墓碑：键 -> 删除时间戳；随 flush 持久化并跨进程传播，保证删除可靠。
    std::unordered_map<std::string, double> tombstones_;
    /// @brief 全局清空纪元（clear 时置为时间戳）；所有版本早于纪元的键在各进程被删除。
    double cleared_at_ = 0.0;

    /// @brief 当前时间戳（秒，double），用于版本/墓碑/清空纪元的 LWW 排序。
    [[nodiscard]] static auto now_ts() -> double {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// @brief 依据当前 m_versions/m_tombstones/m_cleared_at 与磁盘数据重算 m_root
    ///         （合并远端新增键、应用墓碑与清空纪元、按版本 LWW 取舍），保证多进程一致。
    auto reconcile(const Json &on_disk, const std::unordered_map<std::string, double> &disk_versions) -> void;
};

// ----- Preferences 作用域化实现（模板，供头文件内联公共方法 / Group 委托） -----

template <typename T>
auto Preferences::get_impl(const std::string &scope, const std::string &key, T fallback) const -> T {
    std::unique_lock lock(mutex_);
    const std::string composite = scope.empty() ? key : scope + "." + key;
    const Json j = resolve_get(root_, composite);
    if (j.is_null()) {
        return fallback;
    }
    try {
        return j.get<T>();
    } catch (...) {
        return fallback;
    }
}

template <typename T>
auto Preferences::set_impl(const std::string &scope, const std::string &key, T value) -> void {
    const std::string composite = scope.empty() ? key : scope + "." + key;
    Json snapshot;
    std::shared_ptr<IStateHolder> holder;
    {
        std::unique_lock lock(mutex_);
        resolve_set(root_, composite, Json(std::move(value)));
        snapshot = resolve_get(root_, composite);
        versions_[composite] = now_ts();  // 记录写入版本（LWW 依据）
        tombstones_.erase(composite);  // 重新创建会取消墓碑
        if (const auto it = states_.find(composite); it != states_.end()) {
            holder = it->second;
        }
    }
    // 锁外推送，避免持有 Preferences 锁时重入 State 订阅回调导致死锁。
    if (holder) {
        holder->push(snapshot);
    }
}

template <typename T>
auto Preferences::watch_impl(const std::string &scope, const std::string &key, T fallback)
    -> std::shared_ptr<State<T>> {
    const std::string composite = scope.empty() ? key : scope + "." + key;
    std::unique_lock lock(mutex_);
    if (const auto it = states_.find(composite); it != states_.end()) {
        if (auto *h = dynamic_cast<StateHolder<T> *>(it->second.get())) {
            return h->state;
        }
        // 类型不一致（同键不同 T）：重建。
    }
    T initial = fallback;
    const Json j = resolve_get(root_, composite);
    if (!j.is_null()) {
        try {
            initial = j.get<T>();
        } catch (const nlohmann::json::exception &) {
            initial = fallback;  // JSON 类型转换失败，回退到默认值
        }
    }
    auto state = std::make_shared<State<T>>(initial);
    states_[composite] = std::make_shared<StateHolder<T>>(state, std::move(fallback));
    return state;
}

template <typename T>
auto Preferences::binding_impl(const std::string &scope, const std::string &key, T fallback) -> Binding<T> {
    auto *self = this;
    std::string s_scope = scope;
    std::string s_key = key;
    return Binding<T>(*watch_impl<T>(scope, key, std::move(fallback)),
                      [self, s_scope = std::move(s_scope), s_key = std::move(s_key)]() mutable -> auto {
                          self->remove_impl(s_scope, s_key);
                      });
}

}  // namespace aurora::preferences
