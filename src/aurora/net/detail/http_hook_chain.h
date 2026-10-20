// http_hook_chain.h —— 交换钩子链的库内内核：登记表、快照、递归闸、保留头差分与触点集。
//
// 为何单列：链的并发口径（快照与锁的边界）、递归闸（线程局部栈）与两套字段比较（保留头 / 触点差异）都是
// 纯逻辑，与某条腿的建连、重定向、缓存无关；写进 `http_client.cpp` 就只能靠真对端才能断言。落在这里，
// 零 socket、零时钟，四条腿共用同一份判据。公共面只经 `net/http_hook.h` 的抽象类与视图表达，本头不占公共
// API、不进伞头。
//
// 六条口径（规范性，与 `net/http_hook.h` 的注释同源）：
// - **快照**：一次交换起手拷出一份 `shared_ptr` 数组，此后登记与注销都不影响在途交换；回调期间注销另一枚
//   钩子不会把它的实现对象在本次交换中途析构。
// - **锁只在增删与取快照时持有**：调用钩子时不持锁——钩子在回调里注销自己或另一枚是合法形态。
// - **空链快路径**：只读一枚原子计数即返回，不加锁、不分配、不构造视图。
// - **递归闸**：线程局部的「本枚在栈上」集合护住两问；自己已在栈上即跳过该枚、记一行 WARN，链上其余各枚
//   照常跑。闸只管同一线程的同步重入，推迟到回调返回之后或另一条线程的嵌套交换看不到。
// - **保留头逐钩子差分**：每枚跑完按大小写不敏感的「名 → 值序列」比较前后两份字段表；保留头被新增、删除或
//   改值即拒，detail 点名钩子序号、腿与规范拼写的那枚头名，不带值。链末另有一次终检（结构不变量，逐钩子
//   检查已使它不可独立观测）。
// - **触点差异集**：同一套差分里所有被改过的名字（不只保留头）取并集，供重定向跨 origin 剥离与缓存建键。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "aurora/net/http_hook.h"

namespace aurora::net::detail {

/// @brief 交换钩子链：登记表 + 快照 + 两问的编排与差分。
///
/// 由 `Client` 以 `shared_ptr` 持有（与缓存表、连接池同格局），拷贝的 `Client` 共用同一条链。本类不持有
/// 请求或应答，两问的入参都由调用方在栈上给出。
/// @note Thread: 线程安全（登记表由互斥量护住；两问在调用线程上同步跑）
/// @note Side-effects: 调用已登记钩子的两问
class ExchangeHookChain final : public std::enable_shared_from_this<ExchangeHookChain> {
  public:
    /// @brief 一次 `before_request` 链的产物：谁跑过、跑完之后的逻辑头、触点集。
    ///
    /// `asked` 只收**实际跑过**的钩子（被递归闸跳过的不在收摊前缀里）；`logical_headers` 是链跑完那一刻的
    /// 请求头快照，`after_response` 的视图据此构造——它不含库内稍后补的条件头与 `Accept-Encoding`。
    struct Run {
        std::vector<std::shared_ptr<http::ExchangeHook>> asked;  ///< 实际跑过 `before_request` 的钩子（按次序）
        Headers logical_headers;  ///< 链跑完那一刻的请求头快照
        std::vector<std::string> touched;  ///< 被钩子改过的字段名（ASCII 小写，链上并集）
        bool engaged{false};  ///< 是否取到过非空快照（假 = 空链快路径，收摊问与终检一概跳过）
    };

    /// @brief 链的结局：继续、短路或拒绝。
    enum class BeforeStatus : std::uint8_t {
        Continue,  ///< 链跑完，照常发起
        ShortCircuit,  ///< 某枚钩子交出短路值（只有 `send` 会走到这里）
        Rejected,  ///< 保留头 / 非 `send` 短路 / `from_cache` 伪命中 / 异常：以 `rejection` 收场
    };

    /// @brief `run_before` 的完整产物。
    struct BeforeResult {
        BeforeStatus status{BeforeStatus::Continue};  ///< 结局
        std::optional<Result<http::Response>> delivery;  ///< 短路值（`ShortCircuit` 时有值）
        std::optional<Error> rejection;  ///< 拒绝原因（`Rejected` 时有值）
        Run run;  ///< 链的记账事实
    };

    ExchangeHookChain() = default;
    ExchangeHookChain(const ExchangeHookChain &) = delete;
    auto operator=(const ExchangeHookChain &) -> ExchangeHookChain & = delete;
    ExchangeHookChain(ExchangeHookChain &&) = delete;
    auto operator=(ExchangeHookChain &&) -> ExchangeHookChain & = delete;
    ~ExchangeHookChain() = default;

    /// @brief 登记一枚钩子并返回句柄；空指针即一次空登记（链上无条目）。
    /// @param hook 钩子。
    /// @return 登记句柄；链上无条目时为空句柄。
    [[nodiscard]] auto use(std::shared_ptr<http::ExchangeHook> hook) -> http::ExchangeHookRegistration;

    /// @brief 从链上摘除一枚登记（幂等）。
    /// @param id 登记编号；`0` 为空操作。
    auto remove(std::uint64_t id) -> void;

    /// @brief 链上当前是否没有任何登记（空链快路径的判据）。
    /// @return 无登记为 true。
    [[nodiscard]] auto absent() const noexcept -> bool;

    /// @brief 跑一遍 `before_request` 链：逐枚取视图快照、检查保留头与触点差异、处理短路。
    /// @param method 请求方法（视图用）。
    /// @param url 目标（视图用；四条腿都在目标闸之后问，故解析成功时恒有值）。
    /// @param headers 要上线的请求头（就地改写）。
    /// @param leg 本腿。
    /// @return 结局与记账事实。
    [[nodiscard]] auto run_before(http::Method method, const std::optional<Url> &url, Headers &headers,
                                  http::ExchangeLeg leg) const -> BeforeResult;

    /// @brief 跑一遍 `after_response`（只问实际跑过 `before_request` 的前缀）。
    ///
    /// 异常在这里被捕获并记一行 WARN；本函数不抛。
    /// @param run `run_before` 的记账产物。
    /// @param method 请求方法。
    /// @param url 原始逻辑目标。
    /// @param leg 本腿。
    /// @param response 要交出的应答。
    static auto run_after(const Run &run, http::Method method, const std::optional<Url> &url, http::ExchangeLeg leg,
                          const http::Response &response) noexcept -> void;

  private:
    /// @brief 链上的一条登记。
    struct Entry {
        std::uint64_t id{0};  ///< 登记编号
        std::shared_ptr<http::ExchangeHook> hook;  ///< 钩子（链持有，登记期间不析构）
    };

    [[nodiscard]] auto add(std::shared_ptr<http::ExchangeHook> hook) -> std::uint64_t;

    mutable std::mutex mutex_;  ///< 护登记表；调用钩子期间不持有
    std::vector<Entry> entries_;  ///< 登记表（按登记次序）
    std::uint64_t next_id_{1};  ///< 下一枚登记编号
    std::atomic<std::size_t> registered_{0};  ///< 登记数（空链快路径只读它）
};

}  // namespace aurora::net::detail
