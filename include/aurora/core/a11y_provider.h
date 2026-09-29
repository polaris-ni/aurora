#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "aurora/core/accessibility.h"

/// @brief Aurora 根命名空间：库的全部公共类型与自由函数均在此命名空间下（推荐别名 `au`）。
namespace aurora {
/// @brief 控件基类（定义于 `widget/widget.h`）：本头仅以**指针**持有（桥的根指针 / 销毁通知 /
/// 播报目标），不触碰任何成员，故前置声明即可。
class Widget;
}  // namespace aurora

/// @brief 无障碍（a11y）命名空间：平台桥抽象与注册表、桥侧广播入口。
namespace aurora::a11y {

/// @brief 平台桥抽象（平台中立；各平台一个实现，见设计 §5.1）。
/// 生命周期：**惰性激活**—— 无读屏在线时零开销（不构建语义树、不发平台事件）；
/// 首个平台查询到达时才 `activate()` 并回填 `screen_reader_active`。
/// 同步模型：**拉取式**—— 事件到达只置 dirty，`sync_if_dirty()` 在平台查询
/// 到达时才重建快照 + diff + 发平台事件，避免高频变更下的重建风暴。
/// @note Thread: main-thread only（in-proc provider 由平台在 UI 线程回调）
class Provider {
  public:
    /// @brief 默认构造；桥的派生类据此在栈/堆上正常构造。
    Provider() = default;
    /// @brief 虚析构：允许经 `Provider*` 多态销毁各平台实现。
    virtual ~Provider() = default;

    /// @brief 禁用拷贝构造（桥是进程级单例资源，禁止复制）。
    Provider(const Provider &) = delete;
    /// @brief 禁用拷贝赋值（桥是进程级单例资源，禁止复制）。
    /// @return 无返回值语义（恒 delete）。
    auto operator=(const Provider &) -> Provider & = delete;
    /// @brief 禁用移动构造（桥身份与平台侧注册绑定，禁止搬移）。
    Provider(Provider &&) = delete;
    /// @brief 禁用移动赋值（桥身份与平台侧注册绑定，禁止搬移）。
    /// @return 无返回值语义（恒 delete）。
    auto operator=(Provider &&) -> Provider & = delete;

    /// @brief 激活：安装事件广播、建首个快照、回填 `screen_reader_active`。
    virtual auto activate() -> void = 0;
    /// @brief 去激活：注销注册表、断连平台侧对象（窗口关闭必经）。
    virtual auto deactivate() -> void = 0;
    /// @brief 平台查询到达且 dirty 时的同步点：重建树 + diff + 批量发平台事件。
    virtual auto sync_if_dirty() -> void = 0;
    /// @brief 标记为「下次查询需重投影」（事件通道只置脏，不即时重建）。
    virtual auto mark_dirty() -> void = 0;
    /// @brief 桥是否已激活（已安装广播、可响应平台查询）。
    /// @return 已激活为 true。
    [[nodiscard]] virtual auto is_active() const -> bool = 0;

    /// @brief 注入语义树根（`Window::present_root` 每帧调用；桥无根即无法投影）。
    /// 幂等：同一根重复注入只置脏一次。窗口切换根（导航栈换页）时桥据此重投影。
    /// @param root 新根的控件指针（非拥有，仅在本调用与后续查询期间有效）。
    virtual auto set_root([[maybe_unused]] Widget *root) -> void {}
    /// @brief 设置 RTL 标志（默认 no-op；Win32 UIA 桥覆写为桥内 `rtl_`）。
    /// 由 `Surface::set_accessibility_rtl` 转发；见设计 §16.2 #5。
    /// @param rtl 文本方向是否为从右到左。
    virtual auto set_rtl([[maybe_unused]] bool rtl) -> void {}

    /// @brief 事件到达（已置脏）：桥可在此做平台侧的即时处理（如焦点事件优先路由）。
    /// 默认 no-op：拉取式模型下事件只需置脏，重建与事件派生推迟到
    /// `sync_if_dirty()`，避免高频变更下的重建风暴。
    /// @param e 到达的语义事件（播报事件不经此通道）。
    virtual auto on_event([[maybe_unused]] const AccessibilityEvent &e) -> void {}

    /// @brief 动态播报：不经 diff，桥直译平台「立即朗读」信号。
    /// @param text 待朗读的文本。
    /// @param target 播报目标的控件指针（非拥有，可为空）。
    virtual auto on_announcement([[maybe_unused]] const std::string &text, [[maybe_unused]] const Widget *target)
        -> void {}

    /// @brief 某控件实例即将销毁（`Node::~Node()` 单源上报，指针此刻仍有效）。
    /// 桥按设计只持**裸根指针**（子节点每次查询重投影，无需感知生死）；因此只有一种情形
    /// 需要本通知：**被销毁的正是当前根**。此时桥必须立刻切断根与缓存 —— 否则宿主「先拆
    /// UI 树、后拆窗口」的常规顺序下，窗口存活期间的平台查询会拿悬垂根重建语义树（实测
    /// SIGSEGV）。
    /// @param w 即将析构的控件实例（仅在本次调用期间有效，接收方不得保存）。
    /// @note 接收方**不得保存**该指针；返回后即失效。
    virtual auto on_widget_destroying([[maybe_unused]] const Widget *w) -> void {}
    /// @brief 桥名称（诊断 / 真机探针用；如 "win32-uia"）。
    /// @return 桥实现自报的名称字符串。
    [[nodiscard]] virtual auto name() const -> std::string = 0;
};

/// @brief 实现细节命名空间：桥注册表与钩子装配，不属于面向消费者的 API 面。
namespace detail {

/// @brief 进程级桥注册表（事件广播路由）。仅 activate/deactivate 时增删。
/// 事件到达时广播给全部已激活桥；桥内按 `id → Widget*` 映射判定归属、无关即忽略
/// （多窗口规模下广播成本可忽略，不为桥把单槽改多播）。
class ProviderRegistry {
  public:
    /// @brief 进程级单例访问点（函数局部静态，首次调用时构造）。
    /// @return 全局唯一的注册表引用。
    [[nodiscard]] static auto instance() -> ProviderRegistry & {
        static ProviderRegistry registry;  // NOLINT
        return registry;
    }

    /// @brief 注册桥：加入表尾并装入进程级事件广播钩子（重复注册幂等）。
    /// @param p 待注册的桥实例（引用语义，注册期间须保持存活）。
    auto register_provider(Provider &p) -> void {
        if (std::ranges::find(providers_, &p) != providers_.end()) {
            return;
        }
        providers_.push_back(&p);
        install_hook();
    }

    /// @brief 注销桥：从表移除；表空时卸下广播钩子（未注册时幂等 no-op）。
    /// @param p 待注销的桥实例。
    auto unregister_provider(Provider &p) -> void {
        const auto it = std::ranges::find(providers_, &p);
        if (it == providers_.end()) {
            return;
        }
        providers_.erase(it);
        if (providers_.empty()) {
            uninstall_hook();
        }
    }

    /// @brief 广播一条事件给全部已激活桥（宿主处理器由事件通道独立调用，与本表无关）。
    /// @param e 到达的语义事件（每个目标桥先置脏再交即时处理）。
    auto broadcast(const AccessibilityEvent &e) -> void {
        for (Provider *p : providers_) {
            if (p != nullptr && p->is_active()) {
                p->mark_dirty();
                on_event(*p, e);
            }
        }
    }

    /// @brief 广播「某控件实例即将销毁」给全部已激活桥（与事件广播并列的独立通道）。
    /// 与 `broadcast()` 的差别：**不置脏**。销毁通知的语义是「切断，不是重建」——桥收到后
    /// 应立即失效其根，任何随后由结构事件置起的脏标记都会在 `rebuild()` 里被空根短路。
    /// @param w 即将析构的控件实例（各桥不得保存该指针）。
    auto broadcast_widget_destroying(const Widget *w) -> void {
        for (Provider *p : providers_) {
            if (p != nullptr && p->is_active()) {
                p->on_widget_destroying(w);
            }
        }
    }

    /// @brief 已注册桥的数量（测试与诊断用）。
    /// @return providers_ 的元素个数。
    [[nodiscard]] auto count() const -> std::size_t { return providers_.size(); }
    /// @brief 已注册桥的裸指针列表（只读快照，顺序即注册顺序）。
    /// @return 指向 providers_ 内部向量的常引用。
    [[nodiscard]] auto providers() const -> const std::vector<Provider *> & { return providers_; }

  private:
    /// @brief 事件到达的桥侧入口：置脏 + 交给桥做平台侧即时处理（如焦点事件优先路由）。
    /// @param p 目标桥。
    /// @param e 到达的事件。播报事件在此短路，直上抛平台「立即朗读」信号：
    /// p.on_announcement(e.announcement_text, e.target);
    static auto on_event(Provider &p, const AccessibilityEvent &e) -> void {
        // 播报不经 diff：桥直接上抛平台「立即朗读」信号，不依赖 dirty 拉取。
        if (e.kind == AccessibilityEventKind::Announcement) {
            p.on_announcement(e.announcement_text, e.target);
            return;
        }
        p.on_event(e);  // 常规事件交桥的 Provider::on_event 做平台侧即时处理（已置脏）。
    }

    /// @brief 装入进程级广播/销毁钩子（首个桥注册时；重复调用幂等）。
    /// 钩子体即转给单例注册表的 broadcast 与 broadcast_widget_destroying;
    auto install_hook() -> void {
        if (hook_installed_) {
            return;
        }
        ::aurora::detail::set_a11y_broadcast_hook(
            [](const AccessibilityEvent &e) -> void { ProviderRegistry::instance().broadcast(e); });
        ::aurora::detail::set_a11y_widget_destroy_hook(
            [](const Widget *w) -> void { ProviderRegistry::instance().broadcast_widget_destroying(w); });
        hook_installed_ = true;
    }
    /// @brief 卸下进程级广播/销毁钩子（最后一个桥注销时；未装入时幂等 no-op）。
    /// 把两个钩子置回空 callable：set_a11y_broadcast_hook({});
    auto uninstall_hook() -> void {
        if (!hook_installed_) {
            return;
        }
        ::aurora::detail::set_a11y_broadcast_hook({});
        ::aurora::detail::set_a11y_widget_destroy_hook({});
        hook_installed_ = false;
    }

    std::vector<Provider *> providers_;  ///< 已注册桥的裸指针表（非拥有；顺序即注册顺序）。
    bool hook_installed_ = false;  ///< 进程级广播/销毁钩子是否已装入（首个注册装入、最后一个注销卸下）。
};

}  // namespace detail

/// @brief 注册一个平台桥（activate 时调用）。
/// @param p 待注册的桥实例（注册期间须保持存活）。
inline auto register_provider(Provider &p) -> void { detail::ProviderRegistry::instance().register_provider(p); }

/// @brief 注销一个平台桥（deactivate 时调用）。
/// @param p 待注销的桥实例。
inline auto unregister_provider(Provider &p) -> void { detail::ProviderRegistry::instance().unregister_provider(p); }

/// @brief 事件通道的桥侧入口：广播给全部已激活桥。
/// @param e 到达的语义事件。
inline auto broadcast_to_providers(const AccessibilityEvent &e) -> void {
    detail::ProviderRegistry::instance().broadcast(e);  // 单点转发：注册表广播给全部已激活桥。
}

/// @brief 控件销毁通道的桥侧入口：广播给全部已激活桥（由 `Node::~Node` 单源调用）。
/// @param w 即将析构的控件实例（接收方不得保存该指针）。
inline auto broadcast_widget_destroying(const Widget *w) -> void {
    detail::ProviderRegistry::instance().broadcast_widget_destroying(w);
}

/// @brief 当前已激活（已注册）桥数量（测试与诊断用）。
/// @return 注册表中的桥个数。
[[nodiscard]] inline auto registered_provider_count() -> std::size_t {
    return detail::ProviderRegistry::instance().count();
}

// ============================================================================
// 公共无障碍钩子（#9：由内部 `aurora::detail` 钩子升为公共 API）
// ============================================================================

/// @brief 安装无障碍事件广播钩子（公共 API）。
/// 允许宿主/应用在语义树事件广播到平台桥之前插入自定义逻辑（录制、断言、第三方转发）。
/// 传空 callable 即卸载。桥自身在 `activate()` 时已自动安装其内部钩子，本接口用于**额外**的
/// 消费者钩子，二者并存（链式调用）。
/// @param h 自定义广播钩子 callable；传空 callable 即卸载。
inline auto set_accessibility_broadcast_hook(::aurora::detail::AccessibilityBroadcastHook h) -> void {
    ::aurora::detail::set_a11y_broadcast_hook(std::move(h));
}

/// @brief 安装控件销毁广播钩子（公共 API）。
/// 控件销毁经 `Node::~Node` 单源广播给已激活桥；本钩子允许消费者在桥之外也感知销毁
/// （如资源清理、引用释放）。传空 callable 即卸载。
/// @param h 自定义销毁钩子 callable；传空 callable 即卸载。
inline auto set_accessibility_widget_destroy_hook(::aurora::detail::AccessibilityWidgetDestroyHook h) -> void {
    ::aurora::detail::set_a11y_widget_destroy_hook(std::move(h));
}

}  // namespace aurora::a11y
