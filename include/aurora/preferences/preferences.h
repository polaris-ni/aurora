#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "aurora/core/json.h"
#include "aurora/core/result.h"
#include "aurora/state/binding.h"
#include "aurora/state/state.h"

namespace aurora::preferences {

/// @file preferences.h
/// @brief 轻量持久化配置（对标 Android SharedPreferences / iOS UserDefaults）。
///
/// 设计要点（经需求澄清确认）：
/// - 配置以**单个 JSON 文件**为载体；应用初始化时通过构造参数**显式指定**文件路径。
/// - 未指定文件位置 → **仅内存**存储（`is_persistent() == false`），`flush`/`reload` 返回错误。
/// - 指定了文件位置 → 构造时加载该文件到内存；`set` 只更新内存与响应式 `State`，
///   **不**自动写穿文件；落盘由 `flush()` **主动刷新**完成（业界主流的显式提交模型，
///   对标 `SharedPreferences.edit().commit()` / `Settings.Save()`）。
/// - 内部复用现有响应式原语 `State<T>` / `Binding<T>`：`watch`/`binding` 是**存储 → State
///   的单向投递视图**——存储写入（`set`）自动推送到已订阅的 `State`/`Binding`，控件可经
///   `binding.get()` 读取；但 `binding.set()` 只更新下游 `State`（控件侧可见），**不写回
///   存储**，写回须调用 `set`。`binding.remove()` 触发注入的删除回调（墓碑语义）。
///   **不新增任何 UI widget 类型**。
///
/// 并发安全：
/// - **线程安全**：实例内部以 `std::mutex` 保护内存 JSON 与 State 注册表，
///   读写操作（`get`/`contains`/`keys`/`watch`/`set`/`remove`/`clear`/`flush`/`reload`）
///   均走独占锁（不采用 `std::shared_mutex`：MinGW-w64 winpthreads 的 rwlock 在多线程并发
///   写锁竞争下会触发 libstdc++ 断言，见下方 `mutex_` 声明），可在多线程下安全读写。
/// - **进程安全**：`flush`/`reload` 期间对 `<file>.lock` 加跨平台 advisory 文件锁
///   （Windows `LockFileEx` / POSIX `flock`），并采用「写临时文件 + 原子 `rename`」，
///   避免多进程并发写导致半写损坏或互相覆盖。删除键采用**版本化 LWW + 墓碑（tombstone）+
///   全局清空纪元**实现可靠语义：`remove` 写入墓碑并随 `flush` 持久化、跨进程传播，其他进程在
///   下次 `flush`/`reload` 时学习墓碑并同步删除；`clear` 置全局清空纪元，所有旧版本键被各进程
///   删除，清空后新 `set` 的键不受影响。详见 `specification/06-app-platform.md` §9.1。
/// - **单例**：`instance(name)` 提供按名注册表的全局单例访问（每个 name 唯一、懒构造、
///   线程安全创建）；原构造器依然可用（内存模式 / 测试 / 非单例场景）。
///
/// 支持的值类型：`bool` / 整数 / 浮点 / `std::string`，以及整值 `json::Value`（含嵌套对象与数组，
/// 直存直取不做转换）。
///
/// 分组：通过 `group(name)` 获取作用域子视图（`Group`），其内 `get/set/watch/binding/
/// contains/keys/remove/clear` 自动限定在该命名分组下，并以嵌套 JSON 对象持久化
/// （如 `{"ui":{"theme":"dark"}}`）；分组的 `remove`/`clear` 同样走墓碑可靠删除。详见 `Group`。
///
/// @note Thread: thread-safe with mutex (std::mutex, exclusive lock for read and write)
/// @note Side-effects: none (file I/O via flush/reload)
/// @note Rebuildable: yes, via from_json

/// @brief 嵌套 JSON 路径助手（复合点号键），供头文件模板方法（`*_impl`）与 `preferences.cpp` 的
///        `reconcile` 共用：分组键以点号路径（如 `"ui.theme"`）在嵌套 `root_` 中寻址；
///        顶层键（无点号）语义不变。
/// @param root 待寻址的嵌套 JSON 根对象。
/// @param composite 点号复合键；无点号时即顶层键。
/// @return 路径终点的值；任一段缺失或中途遇到非对象时返回 null。
auto resolve_get(const json::Value &root, const std::string &composite) -> json::Value;

/// @brief 按复合点号键写入嵌套值；路径上缺失或非对象的中间段自动重建为对象容器。
/// @param root 就地修改的嵌套 JSON 根对象。
/// @param composite 点号复合键，最后一段为落点叶子键。
/// @param value 写入的 JSON 值（按值移入）。
auto resolve_set(json::Value &root, const std::string &composite, json::Value value) -> void;

/// @brief 按复合点号键删除嵌套叶子值；路径中断或键不存在时无操作。
/// @param root 就地修改的嵌套 JSON 根对象。
/// @param composite 指向待删叶子键的点号复合键。
auto resolve_erase(json::Value &root, const std::string &composite) -> void;

/// @brief 把嵌套 JSON 根拍平为「点号复合键 → 叶子值」映射；中间对象层只作前缀不落独立条目。
/// @param root 待拍平的嵌套 JSON 根对象。
/// @return 复合键到叶子值的映射；非对象根返回空表。
auto flatten(const json::Value &root) -> std::unordered_map<std::string, json::Value>;

/// @brief 轻量持久化配置入口：以单个 JSON 文件为载体的点号复合键内存存储，
///        并经 `watch`/`binding` 与响应式 `State`/`Binding` 桥接。
/// @note 写入只更新内存，落盘由 `flush()` 显式提交；删除走「版本化 LWW + 墓碑 + 全局清空
///        纪元」跨进程可靠语义，设计要点与并发契约详见本头文件顶部的文件级注释。
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
    explicit Preferences(std::filesystem::path file) : file_(std::move(file)) { load_from_file(); }

    /// @brief 文件模式：显式指定配置存储的 JSON 文件路径与选项，构造即加载（文件不存在则为空对象）。
    /// @param file 配置文件的完整路径；可位于任意位置（含子目录，目录会自动创建）。
    /// @param opts 选项（如 `auto_create_dir`）。
    explicit Preferences(std::filesystem::path file, Options opts) : file_(std::move(file)), opts_(opts) {
        load_from_file();
    }

    /// @brief 便捷构造：在指定路径创建文件模式实例（默认 Options）。
    /// @param file 配置文件的完整路径，构造即加载。
    /// @return 绑定 `file` 的 Preferences 实例。
    [[nodiscard]] static auto at(std::filesystem::path file) -> Preferences { return Preferences(std::move(file)); }
    /// @brief 便捷构造：在指定路径创建文件模式实例。
    /// @param file 配置文件的完整路径，构造即加载。
    /// @param opts 构造选项（如 `auto_create_dir`）。
    /// @return 以 `opts` 绑定 `file` 的 Preferences 实例。
    [[nodiscard]] static auto at(std::filesystem::path file, Options opts) -> Preferences {
        return Preferences(std::move(file), opts);
    }

    /// @brief 便捷构造：在平台默认配置目录（`default_config_dir()`）下以 `name`（自动补 `.json`）命名配置文件。
    /// @param name 配置文件名（不含目录；无 `.json` 后缀时自动补齐）。
    /// @return 位于平台默认配置目录的 Preferences 实例。
    [[nodiscard]] static auto with_location(std::string name) -> Preferences {
        return with_location(std::move(name), default_config_dir());
    }
    /// @brief 便捷构造：在 `dir` 下以 `name`（自动补 `.json`）命名配置文件。
    /// @param name 配置文件名（不含目录；无 `.json` 后缀时自动补齐）。
    /// @param dir 配置目录。
    /// @return 绑定 `dir/name.json` 的 Preferences 实例（默认 Options）。
    [[nodiscard]] static auto with_location(std::string name, const std::filesystem::path &dir) -> Preferences {
        return with_location(std::move(name), dir, Options{});
    }
    /// @brief 便捷构造：在 `dir` 下以 `name`（自动补 `.json`）命名配置文件，并显式指定选项。
    /// @param name 配置文件名称
    /// @param dir 配置目录；默认取平台配置目录。
    /// @param opts 构造选项
    /// @return 以 `opts` 绑定 `dir/name.json` 的 Preferences 实例。
    [[nodiscard]] static auto with_location(std::string name, const std::filesystem::path &dir, Options opts)
        -> Preferences {
        if (!name.ends_with(".json")) {
            name += ".json";
        }
        return Preferences(dir / name, opts);
    }

    // ---------- 单例（按名注册表，线程安全懒构造） ----------

    /// @brief 全局单例（默认名 `"app"`，文件模式，使用平台默认配置目录）。
    /// @param name 单例注册名；同名多次调用返回同一实例。
    /// @return `name` 对应的全局唯一实例（首次调用时在平台默认配置目录下懒构造）。
    [[nodiscard]] static auto instance(const std::string &name = "app") -> Preferences &;

    /// @brief 在 `dir` 下以 `name`（自动补 `.json`）命名的单例实例。
    /// @param name 单例注册名，同时用作配置文件名（无 `.json` 后缀时自动补齐）。
    /// @param dir 配置文件所在目录。
    /// @return `name` 对应的全局唯一实例；已存在时直接返回，忽略 `dir` 参数。
    [[nodiscard]] static auto instance(const std::string &name, const std::filesystem::path &dir) -> Preferences &;

    /// @brief 显式文件路径的单例实例（初始化时即指定存储位置；此后同名调用忽略路径参数）。
    /// @param name 单例注册名。
    /// @param file 配置文件完整路径（首次构造该 name 时生效）。
    /// @return `name` 对应的全局唯一实例。
    [[nodiscard]] static auto instance_at(const std::string &name, std::filesystem::path file) -> Preferences &;

    /// @brief 解析平台默认配置目录（不依赖任何窗口后端）。
    /// 优先级：XDG_CONFIG_HOME → LOCALAPPDATA(Windows) → HOME/.config → 当前工作目录。
    /// @return 可用的配置目录；上述环境变量均缺失时回退为当前工作目录。
    [[nodiscard]] static auto default_config_dir() -> std::filesystem::path;

    /// @brief 是否绑定了文件（持久化模式）。
    /// @return true 表示文件模式（`flush`/`reload` 可用），false 表示仅内存模式。
    [[nodiscard]] auto is_persistent() const -> bool { return !file_.empty(); }

    /// @brief 当前配置文件路径（内存模式返回空路径）。
    /// @return 配置文件路径的 const 引用；内存模式为空路径。
    [[nodiscard]] auto file_path() const -> const std::filesystem::path & { return file_; }

    /// @brief 最近一次文件加载（`flush`/`reload` 不涉及）产生的错误；无错误则为 nullopt。
    /// @return 加载错误副本（打开失败/JSON 解析失败）；无错误或内存模式为 nullopt。
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
        /// @brief 构造分组视图：由 `Preferences::group()` 生成，供嵌套分组链式派生，不面向使用者直接构造。
        /// @param owner 所属 Preferences 实例（视图为非拥有引用，宿主须比视图活得久）。
        /// @param path 复合分组前缀（如 `"ui"` 或 `"ui.editor"`），空串表示根作用域。
        Group(Preferences *owner, std::string path) : owner_(owner), path_(std::move(path)) {}

        /// @brief 读取分组内键值；缺失/类型不匹配回退 `fallback`。
        /// @tparam T 读取值类型（`bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象）。
        /// @param key 分组内键名（不含分组前缀）。
        /// @param fallback 复合键缺失或类型转换失败时返回的兜底值。
        /// @return 分组内键值；读不到或类型不符时为 `fallback`。
        template <typename T>
        [[nodiscard]] auto get(const std::string &key, T fallback) const -> T {
            return owner_->get_impl(path_, key, std::move(fallback));
        }

        /// @brief 写入分组内键值（仅内存 + State，不自动落盘）。
        /// @tparam T 写入值类型；内部按值移入并转为 Json。
        /// @param key 分组内键名（不含分组前缀）。
        /// @param value 写入的值。
        template <typename T>
        auto set(const std::string &key, T value) -> void {
            owner_->set_impl(path_, key, std::move(value));
        }

        /// @brief 惰性创建分组内键的 `State<T>`，供控件订阅。
        /// @tparam T 状态值类型。
        /// @param key 分组内键名（不含分组前缀）。
        /// @param fallback 键缺失时的初始值，亦是后续推送类型不符时的兜底值。
        /// @return 共享 `State<T>`；同键同类型重复调用返回同一实例。
        template <typename T>
        [[nodiscard]] auto watch(const std::string &key, T fallback) -> std::shared_ptr<State<T>> {
            return owner_->watch_impl(path_, key, std::move(fallback));
        }

        /// @brief 分组内键的非拥有 `Binding<T>`（注入删除回调）。
        /// @tparam T 绑定值类型。
        /// @param key 分组内键名（不含分组前缀）。
        /// @param fallback 状态缺失时的兜底值。
        /// @return 绑定分组内复合键的 Binding；`remove()` 将按墓碑语义删除该键。
        template <typename T>
        [[nodiscard]] auto binding(const std::string &key, T fallback) -> Binding<T> {
            return owner_->binding_impl(path_, key, std::move(fallback));
        }

        /// @brief 分组内是否含键（且非 null）。
        /// @param key 分组内键名（不含分组前缀）。
        /// @return true 表示分组内该键存在且值非 null。
        [[nodiscard]] auto contains(const std::string &key) const -> bool { return owner_->contains_impl(path_, key); }

        /// @brief 分组内所有直接子键名（不含分组前缀）。
        /// @return 子键名列表；分组子树不存在或非对象时为空列表。
        [[nodiscard]] auto keys() const -> std::vector<std::string> { return owner_->keys_impl(path_); }

        /// @brief 删除分组内键（墓碑可靠语义，需 flush 落盘）。
        /// @param key 分组内键名（不含分组前缀）。
        auto remove(const std::string &key) const -> void { owner_->remove_impl(path_, key); }

        /// @brief 清空本分组子树（对该子树已知键打墓碑，不影响其他分组与顶层键）。
        auto clear() const -> void { owner_->clear_impl(path_); }

        /// @brief 链式嵌套子分组。
        /// @param name 子分组名，追加到当前分组前缀之后（如 `"ui"` + `"editor"`）。
        /// @return 指向更深一层作用域的新 Group 视图。
        [[nodiscard]] auto group(const std::string &name) const -> Group {
            return {owner_, path_.empty() ? name : path_ + "." + name};
        }

      private:
        Preferences *owner_;  ///< 所属 Preferences 实例，作用域化实现（`*_impl`）经它委托。
        std::string path_;  ///< 复合前缀，如 "ui" / "ui.editor"
    };

    /// @brief 获取命名分组的作用域子视图（见 `Group`）。
    /// @param name 分组名，作为存储中的嵌套对象键（可再经 `Group::group` 链式嵌套）。
    /// @return 限定在 `name` 分组下的 Group 视图（轻量值对象，每次调用新建）。
    [[nodiscard]] auto group(const std::string &name) -> Group { return {this, name}; }

    // ---------- 读取（共享锁） ----------

    /// @brief 读取键值（根作用域）；类型不匹配或键缺失时回退 `fallback`。
    /// @tparam T 读取值类型（`bool` / 整数 / 浮点 / `std::string` / `std::vector` / JSON 对象）。
    /// @param key 顶层键名（含点号时按复合路径寻址嵌套对象）。
    /// @param fallback 键缺失或类型转换失败时返回的兜底值。
    /// @return 键对应的值；读不到或类型不符时为 `fallback`。
    template <typename T>
    [[nodiscard]] auto get(const std::string &key, T fallback) const -> T {
        return get_impl("", key, std::move(fallback));
    }

    // ---------- 写入（仅内存 + 响应式 State；不写文件；独占锁） ----------

    /// @brief 写入键值（根作用域）：更新内存 JSON 与对应 `State`（通知订阅者），**不**自动落盘。
    /// 落盘须调用 `flush()`。
    /// @tparam T 写入值类型；内部按值移入并转为 Json。
    /// @param key 顶层键名（含点号时按复合路径写入嵌套对象）。
    /// @param value 写入的值。
    template <typename T>
    auto set(const std::string &key, T value) -> void {
        set_impl("", key, std::move(value));
    }

    /// @brief 惰性创建并缓存该键的 `State<T>`，供控件订阅；初始值为当前存储值或 `fallback`。
    /// @tparam T 状态值类型。
    /// @param key 顶层键名。
    /// @param fallback 键缺失时的初始值，亦是后续推送类型不符时的兜底值。
    /// @return 共享 `State<T>`；同键同类型重复调用返回同一缓存实例，类型不符则重建。
    template <typename T>
    [[nodiscard]] auto watch(const std::string &key, T fallback) -> std::shared_ptr<State<T>> {
        return watch_impl("", key, std::move(fallback));
    }

    /// @brief 基于 `watch` 的 `State` 返回非拥有 `Binding<T>`：控件卸载时可调用 `binding.remove()`
    ///        删除对应持久化键（走墓碑可靠删除）；调用 `remove()` 后该 Binding 即失效。
    /// @tparam T 绑定值类型。
    /// @param key 顶层键名。
    /// @param fallback 状态缺失时的兜底值。
    /// @return 绑定该键 State 的 Binding，附带写回存储的删除回调。
    template <typename T>
    [[nodiscard]] auto binding(const std::string &key, T fallback) -> Binding<T> {
        return binding_impl("", key, std::move(fallback));
    }

    // ---------- 持久化（独占锁 + 进程文件锁） ----------

    /// @brief 主动将内存内容刷新（写穿）到文件。内存模式返回错误。
    /// @return 成功返回 Ok；内存模式、获取文件锁失败或写盘/重命名失败时返回对应错误。
    [[nodiscard]] auto flush() -> Result<void>;

    /// @brief 从文件重新加载到内存，并通知所有已订阅的 `State`。内存模式返回错误。
    /// @return 成功返回 Ok；内存模式、获取文件锁失败或 JSON 解析失败时返回对应错误。
    [[nodiscard]] auto reload() -> Result<void>;

    // ---------- 批量操作 ----------

    /// @brief 根作用域已存在的所有键（不含分组前缀）。
    /// @return 顶层非 null 值的键名列表。
    [[nodiscard]] auto keys() const -> std::vector<std::string> { return keys_impl(""); }

    /// @brief 根作用域键是否存在（且非 null）。
    /// @param key 顶层键名。
    /// @return true 表示键存在且值非 null。
    [[nodiscard]] auto contains(const std::string &key) const -> bool { return contains_impl("", key); }

    /// @brief 删除根作用域键（可靠语义：写入墓碑并随 `flush` 持久化、跨进程传播；需 `flush` 落盘）。
    /// @param key 顶层键名。
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
        virtual void push(const json::Value &j) = 0;
    };

    /// @brief 具体 `State<T>` 的类型擦除实现：保存状态指针与兜底值，供 `push` 做类型安全回写。
    /// @tparam T 所持 `State<T>` 的值类型。
    template <typename T>
    struct StateHolder : IStateHolder {
        std::shared_ptr<State<T>> state;  ///< 所持的响应式状态，push 经它回写。
        T fallback;  ///< push 收到 null 或类型转换失败时写入的兜底值。
        /// @brief 构造持有者：接管 `State<T>` 共享指针并保存兜底值。
        /// @param s 被持有的 `State<T>`（按值移入）。
        /// @param fb 兜底值（按值移入）。
        StateHolder(std::shared_ptr<State<T>> s, T fb) : state(std::move(s)), fallback(std::move(fb)) {}
        void push(const json::Value &j) override {
            if (j.is_null()) {
                state->set(fallback);
                return;
            }
            // T 为整值时原样投递；否则走宽容读：类型不符/域外一律回落 fallback（新容器不抛异常，
            // 原「try + catch(...) 回落」的语义由 as_or 内建）。
            if constexpr (std::is_same_v<T, json::Value>) {
                state->set(j);
            } else {
                state->set(j.as_or<T>(fallback));
            }
        }
    };

    /// @brief 从绑定文件加载到内存：文件不存在视为空配置；打开失败或 JSON 解析失败时记入
    ///        `load_error_` 并重置为空对象；成功后恢复磁盘的版本表/墓碑/清空纪元并经
    ///        `reconcile` 应用已持久化的删除语义。内存模式（无绑定文件）为空操作。
    auto load_from_file() -> void;

    /// @brief 作用域化内部实现约定：`scope` 为空 = 根作用域，复合键 = `scope + "." + key`；
    ///        Group 以自身 `path_` 为 scope 委托这些实现，顶层键（无点号）语义不变。
    /// @tparam T 读取值类型。
    /// @param scope 分组复合前缀，空串表示根作用域。
    /// @param key 作用域内键名。
    /// @param fallback 复合键缺失或类型转换失败时返回的兜底值。
    /// @return 复合键的值转为 T；读不到或类型不符时为 `fallback`。
    template <typename T>
    [[nodiscard]] auto get_impl(const std::string &scope, const std::string &key, T fallback) const -> T;
    /// @brief 写入复合键：更新内存 JSON、记录本进程写入版本并取消既有墓碑，随后在锁外把快照
    ///        推送给该键已注册的 State（若有）。
    /// @note 豁免 performance-unnecessary-value-param：同一模板体对 `T=std::string/json::Value` 这类实例
    ///       靠按值形参 + `json::Value(std::move(value))` 完成移动转换，改 const 引用反而把这些高频实例化
    ///       退化成深拷贝。单一签名的私有模板按最受益形态取形参。
    /// @tparam T 写入值类型。
    /// @param scope 分组复合前缀，空串表示根作用域。
    /// @param key 作用域内键名。
    /// @param value 写入的值（按值移入并转为 `json::Value`）。
    template <typename T>
    auto set_impl(const std::string &scope, const std::string &key, T value)
        -> void;  // NOLINT(performance-unnecessary-value-param)
    template <typename T>
    [[nodiscard]] auto watch_impl(const std::string &scope, const std::string &key, T fallback)
        -> std::shared_ptr<State<T>>;
    /// @brief 在 `watch_impl` 的 State 上构造 `Binding<T>`，注入按作用域删除复合键的墓碑回调。
    /// @tparam T 绑定值类型。
    /// @param scope 分组复合前缀，空串表示根作用域。
    /// @param key 作用域内键名。
    /// @param fallback State 的兜底值。
    /// @return 非拥有 Binding；其 `remove()` 触发 `remove_impl(scope, key)`。
    template <typename T>
    [[nodiscard]] auto binding_impl(const std::string &scope, const std::string &key, T fallback) -> Binding<T>;
    /// @brief 复合键在作用域内是否存在且非 null。
    /// @param scope 分组复合前缀，空串表示根作用域。
    /// @param key 作用域内键名。
    /// @return true 表示复合键存在且值非 null。
    [[nodiscard]] auto contains_impl(const std::string &scope, const std::string &key) const -> bool;
    /// @brief 列出作用域内的直接子键名（不含更深层级）。
    /// @param scope 分组复合前缀，空串列根作用域（跳过 null 值键）。
    /// @return 子键名列表；分组子树缺失或非对象时为空列表。
    [[nodiscard]] auto keys_impl(const std::string &scope) const -> std::vector<std::string>;
    /// @brief 删除作用域内复合键：擦除值与 State 注册、写入墓碑并清除本地版本（需 `flush` 落盘）。
    /// @param scope 分组复合前缀，空串表示根作用域。
    /// @param key 作用域内键名。
    auto remove_impl(const std::string &scope, const std::string &key) -> void;
    /// @brief 清空作用域：根作用域置全局清空纪元并为已知键打墓碑；分组作用域对子树全部复合键打墓碑。
    /// @param scope 分组复合前缀，空串表示根作用域。
    auto clear_impl(const std::string &scope) -> void;

    /// @brief 单例注册表（函数局部静态，线程安全懒构造）：注册名 -> 唯一实例。
    /// @return 全局注册表引用；仅应在持有 `registry_mutex()` 时访问。
    static auto registry() -> std::unordered_map<std::string, std::unique_ptr<Preferences>> &;
    /// @brief 保护单例注册表的互斥锁，供 `instance`/`instance_at` 的懒构造查重串行化。
    /// @return 全局注册表锁引用。
    static auto registry_mutex() -> std::mutex &;

    std::filesystem::path file_;  // 空 = 内存模式
    Options opts_;  // 默认构造即 auto_create_dir=true（Options 为聚合类型，见 Options）
    json::Value root_ = json::Value::object();  // 内存 JSON 存储
    std::unordered_map<std::string, std::shared_ptr<IStateHolder>> states_;  // key -> State
    std::optional<Error> load_error_;
    // 用 std::mutex 而非 std::shared_mutex：MinGW-w64 winpthreads 的 rwlock 在多线程
    // 并发写锁竞争下会间歇性返回非零，触发 libstdc++ `__shared_mutex_pthread::lock()`
    // 的 `__ret == 0` 断言（test_preferences 稳定复现）。本类临界区均为内存操作、
    // 读多写少但单次耗时纳秒级，独占锁无可观测性能差异。
    mutable std::mutex mutex_;  // 保护上述可变状态

    // ----- 多进程可靠删除所需元数据（受 mutex_ 保护） -----
    /// @brief 键 -> 本进程显式 set 的写入版本（时间戳，LWW 依据之一）。不合并磁盘版本。
    std::unordered_map<std::string, double> versions_;
    /// @brief 已删除键的墓碑：键 -> 删除时间戳；随 flush 持久化并跨进程传播，保证删除可靠。
    std::unordered_map<std::string, double> tombstones_;
    /// @brief 全局清空纪元（clear 时置为时间戳）；所有版本早于纪元的键在各进程被删除。
    double cleared_at_ = 0.0;

    /// @brief 当前时间戳（秒，double），用于版本/墓碑/清空纪元的 LWW 排序。
    /// @return 系统时钟距 epoch 的秒数（浮点），单调按 wall-clock 取值。
    [[nodiscard]] static auto now_ts() -> double {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// @brief 依据当前 versions_/tombstones_/cleared_at_ 与磁盘数据重算 root_
    ///         （合并远端新增键、应用墓碑与清空纪元、按版本 LWW 取舍），保证多进程一致。
    auto reconcile(const json::Value &on_disk, const std::unordered_map<std::string, double> &disk_versions) -> void;
};

// ----- Preferences 作用域化实现（模板，供头文件内联公共方法 / Group 委托） -----

template <typename T>
auto Preferences::get_impl(const std::string &scope, const std::string &key, T fallback) const -> T {
    std::unique_lock lock(mutex_);
    const std::string composite = scope.empty() ? key : scope + "." + key;
    const json::Value j = resolve_get(root_, composite);
    if (j.is_null()) {
        return fallback;
    }
    // 恒等分支：T 为整值时直接交出该 Value（嵌套对象/数组原样透出）；其余 T 走宽容读，
    // 类型不符或数值域外一律回落 fallback（语义等同原「try + catch(...) 回落」）。
    if constexpr (std::is_same_v<T, json::Value>) {
        return j;
    } else {
        return j.as_or<T>(fallback);
    }
}

template <typename T>
auto Preferences::set_impl(const std::string &scope, const std::string &key, T value) -> void {
    const std::string composite = scope.empty() ? key : scope + "." + key;
    json::Value snapshot;
    std::shared_ptr<IStateHolder> holder;
    {
        std::unique_lock lock(mutex_);
        // 恒等分支：T 为整值时原样存入（不做任何类型推断）；其余 T 由入向构造落成标量 / 字符串。
        if constexpr (std::is_same_v<T, json::Value>) {
            resolve_set(root_, composite, std::move(value));
        } else {
            resolve_set(root_, composite, json::Value(std::move(value)));
        }
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
    const json::Value j = resolve_get(root_, composite);
    if (!j.is_null()) {
        // 恒等分支：T 为整值时取存储原值；否则宽容读，类型不符回退到默认值。
        if constexpr (std::is_same_v<T, json::Value>) {
            initial = j;
        } else {
            initial = j.as_or<T>(fallback);
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
