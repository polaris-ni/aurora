#pragma once

/// @file detail/observability_trace.h
/// @brief 发起侧的记账句柄：一次物理往来一枚，析构未收摊即记 `abandoned`。
///
/// 本头只在 `src/` 内使用，不属于公共 API。`AURORA_ENABLE_DEBUG` 未开时整类退化为零成本空壳
/// （构造不复制字符串、各方法为空），故发起侧可以无条件写埋点而不必到处 `#ifdef`。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "aurora/core/url.h"
#include "aurora/net/http_observability.h"

namespace aurora::net::http {

#ifdef AURORA_ENABLE_DEBUG

/// @brief 一次物理往来的记账句柄。
/// @note Thread: 由发起侧的协程帧串行使用；内部发布经进程级锁，不要求同一线程。
/// @note Side-effects: 起手 / 阶段 / 收摊向存储与事件总线发布。
class ExchangeTrace {
  public:
    /// @brief 起手：分配主键、入表并向订阅者发布 begin。
    /// @param method 请求方法文本。
    /// @param request_id 链路标识（未带标识时为空）。
    /// @param target 本跳目标 URL（用于取 origin-form 与主机）。
    /// @param hop 重定向跳序号。
    /// @param pass 物理尝试序号。
    /// @param reused_connection 本跳是否复用池内连接。
    /// @param from_cache 本跳是否新鲜缓存命中（零 mark）。
    ExchangeTrace(std::string_view method, std::string_view request_id, const Url &target, std::uint16_t hop,
                  std::uint16_t pass, bool reused_connection, bool from_cache);

    /// @brief 未收摊即销毁：向订阅者发布 `abandoned = true` 的 end（存储若仍持有也一并更新）。
    ~ExchangeTrace();

    ExchangeTrace(const ExchangeTrace &) = delete;
    auto operator=(const ExchangeTrace &) -> ExchangeTrace & = delete;
    ExchangeTrace(ExchangeTrace &&) = delete;
    auto operator=(ExchangeTrace &&) -> ExchangeTrace & = delete;

    /// @brief 本跳主键。
    /// @return 进程内唯一、递增的编号。
    [[nodiscard]] auto id() const -> std::uint64_t { return exchange_id_; }

    /// @brief 记录一枚已到达阶段。
    /// @param stage 阶段。
    auto mark(HttpStage stage) -> void;

    /// @brief 记录应答状态码（收到应答头即记，体失败也保留）。
    /// @param status 状态码。
    auto set_status(std::uint16_t status) -> void;

    /// @brief 累加交给传输层的字节。
    /// @param bytes 本段字节数。
    auto note_sent(std::uint64_t bytes) -> void;

    /// @brief 累加读回的线上字节。
    /// @param bytes 本段字节数。
    auto note_received(std::uint64_t bytes) -> void;

    /// @brief 更新本跳是否复用池内连接（池在取用后才确定）。
    /// @param reused 复用为 true。
    auto set_reused_connection(bool reused) -> void;

    /// @brief 记一次失败；结局码取错误表的稳定 slug。
    /// @param code 错误码。
    /// @param failed_stage 失败时正在进行的阶段；本地预检拒绝为 `nullopt`。
    auto fail(ErrorCode code, std::optional<HttpStage> failed_stage) -> void;

    /// @brief 收摊：发布 end；重复调用无效果。
    auto finish() -> void;

  private:
    auto publish_begin() -> void;
    auto publish_stage(const StageMark &mark) -> void;
    auto publish_update() -> void;
    auto publish_end(bool abandoned) -> void;

    std::uint64_t exchange_id_{0};  ///< 主键
    std::string request_id_;  ///< 链路标识
    std::string method_;  ///< 请求方法
    std::string path_;  ///< origin-form（含查询串）
    std::string host_;  ///< 主机（含端口，无用户信息）
    std::uint16_t hop_{0};  ///< 重定向跳序号
    std::uint16_t pass_{0};  ///< 物理尝试序号
    std::uint16_t status_{0};  ///< 应答状态码
    std::string error_code_;  ///< 失败码
    std::optional<HttpErrorClass> error_class_;  ///< 失败归因
    std::optional<HttpStage> failed_stage_;  ///< 失败阶段
    std::uint64_t sent_{0};  ///< 已交出字节
    std::uint64_t received_{0};  ///< 已读回字节
    bool reused_connection_{false};  ///< 是否复用连接
    bool from_cache_{false};  ///< 是否新鲜缓存命中
    bool truncated_{false};  ///< 原始目标因上限被替换
    bool finished_{false};  ///< 是否已收摊
    bool any_mark_{false};  ///< 是否已记过阶段（复用连接的首枚 mark 定 offset 0 的依据）
    std::chrono::steady_clock::time_point started_;  ///< 本跳起手时刻
};

#else

/// @brief Release 形态：构造不复制字符串，各方法为空；主键恒为 0。
class ExchangeTrace {
  public:
    ExchangeTrace(std::string_view method, std::string_view request_id, const Url &target, std::uint16_t hop,
                  std::uint16_t pass, bool reused_connection, bool from_cache) {
        (void)method;
        (void)request_id;
        (void)target;
        (void)hop;
        (void)pass;
        (void)reused_connection;
        (void)from_cache;
    }
    ~ExchangeTrace() = default;
    ExchangeTrace(const ExchangeTrace &) = delete;
    auto operator=(const ExchangeTrace &) -> ExchangeTrace & = delete;
    ExchangeTrace(ExchangeTrace &&) = delete;
    auto operator=(ExchangeTrace &&) -> ExchangeTrace & = delete;

    [[nodiscard]] auto id() const -> std::uint64_t { return 0; }
    auto mark(HttpStage stage) -> void { (void)stage; }
    auto set_status(std::uint16_t status) -> void { (void)status; }
    auto note_sent(std::uint64_t bytes) -> void { (void)bytes; }
    auto note_received(std::uint64_t bytes) -> void { (void)bytes; }
    auto set_reused_connection(bool reused) -> void { (void)reused; }
    auto fail(ErrorCode code, std::optional<HttpStage> failed_stage) -> void {
        (void)code;
        (void)failed_stage;
    }
    auto finish() -> void {}
};

#endif

}  // namespace aurora::net::http
