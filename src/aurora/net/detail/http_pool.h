// HTTP 连接池内核（§12「分段超时与连接池治理」条、提案 §16 第 36 项）。
//
// 只回答发起侧的四个问题：取（空闲复用还是新建还是排队）、还（回池还是关闭）、按什么键、超时怎么淘汰。
// 锁只护表体，判定全部由调用方（http_client.cpp 的两条协程）在锁外完成——池不认识请求、不认识分帧，
// 「这条连接还干不干净」是装配器与应答头的事，这里只存「有人保证过它干净」的连接。
//
// 线程口径与 `detail::HttpResponseCache` 同格局：`Client` 可拷贝、可并发发起，拷贝共享同一张池，故互斥量
// 在本类内、出口拿不到引用（交出的连接是 `shared_ptr`，所有权随交割转移）。

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/net/http.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"

namespace aurora::net::detail {

/// @brief 一条池化连接的键：一次建连的完整口径。
///
/// 五枚在线事实 + 一枚配置代次。前四枚来自每跳现算的路由决策（`plan_route()` 的产物）——「这条连接当初
/// 是怎么建起来的」决定了「下一个请求能不能坐上来」：请求行形态（绝对 / 相对）、对端是谁（目标本身还是
/// 转发代理）、有没有先建隧道，任何一枚不同，线上会话的口径就不同。`secure` 取自目标 scheme（隧道之上
/// 再握手的口径见 `net/http_client.h`）。配置代次由 `Client::tls_context()` 每次改写推进（拷贝共享一枚
/// 计数器），池条目记下入池时的值、不匹配即不复用——改了信任根还复用旧连接，等于新配置静默不生效。
struct HttpPoolKey {
    bool secure{false};  ///< 目标是否 `https://`（连接上有没有 TLS 会话）
    std::string peer_host;  ///< 建连对端（直连 = 目标主机；转发 = 代理主机；隧道 = 代理主机）
    std::uint16_t peer_port{0};  ///< 建连对端端口
    bool absolute_form{false};  ///< 请求行是否绝对形态（明文经 HTTP 代理转发那一腿）
    bool tunneled{false};  ///< 是否先经 `CONNECT` 建了隧道
    std::uint64_t tls_generation{0};  ///< 建连时的 TLS 配置代次（`Client::tls_context()` 每次改写自增）

    /// @brief 严格弱序比较（供 `std::map` 计数用）；字段次序即比较次序。
    ///
    /// 布尔字段按「假 < 真」折成整数序（`readability-implicit-bool-conversion` 要求显式转换）。
    auto operator<(const HttpPoolKey &other) const -> bool {
        if (secure != other.secure) {
            return static_cast<int>(secure) < static_cast<int>(other.secure);
        }
        if (peer_host != other.peer_host) {
            return peer_host < other.peer_host;
        }
        if (peer_port != other.peer_port) {
            return peer_port < other.peer_port;
        }
        if (absolute_form != other.absolute_form) {
            return static_cast<int>(absolute_form) < static_cast<int>(other.absolute_form);
        }
        if (tunneled != other.tunneled) {
            return static_cast<int>(tunneled) < static_cast<int>(other.tunneled);
        }
        return tls_generation < other.tls_generation;
    }

    /// @brief 相等比较（同字段全等）。
    auto operator==(const HttpPoolKey &other) const -> bool {
        return secure == other.secure && peer_host == other.peer_host && peer_port == other.peer_port &&
               absolute_form == other.absolute_form && tunneled == other.tunneled &&
               tls_generation == other.tls_generation;
    }
};

/// @brief 一条池化连接的持有物：明文与 TLS 两型恰一型非空。
///
/// 两条流的读写口在传输层逐字同构（`net/transport.h` 的分工边界），发起侧以类型擦除闭包收敛；池只负责
/// 持有与交割，不碰读写。
struct HttpPoolConnection {
    std::shared_ptr<net::StreamTransport> plain;  ///< 明文流（secure 为假时有意义）
    std::shared_ptr<net::TlsTransport> secure;  ///< TLS 流（secure 为真时有意义）

    /// @brief 是否持有任何一条流。
    [[nodiscard]] auto empty() const -> bool { return plain == nullptr && secure == nullptr; }
};

/// @brief 取用判词：交出一条空闲连接 / 放手新建（名额已记上）/ 排队（名额已满）。
enum class HttpPoolAcquire : std::uint8_t {
    Ready,  ///< 命中空闲连接（载荷在连接里）
    OpenNew,  ///< 无空闲但名额有余：调用方自行建连，名额已在本判词里记上
    Queue,  ///< 名额已满：调用方排队等待（等待时间计入 `Timeouts::connect` 档）
};

/// @brief 一次取用的结果：判词 + 连接 + 这条连接的出生时刻。
///
/// 出生时刻要随连接走：`max_age` 管的是「这条连接活了多久」，复用不能把寿命钟重置，故出生点在
/// `OpenNew`（建连时刻）记一次、经空闲表随连接循环传递，归还时原样交回。
struct HttpPoolAcquired {
    HttpPoolAcquire verdict{HttpPoolAcquire::Queue};  ///< 判词
    HttpPoolConnection connection;  ///< 仅 `Ready` 时非空
    std::chrono::steady_clock::time_point born;  ///< 连接出生时刻（`Ready` = 空闲表记的；`OpenNew` = 现在）
};

/// @brief 连接池本体：键、取还、空闲与最长寿命淘汰、每键并发上限。
///
/// 计数口径：`live_` 数的是「这条键上还存在几条连接」（在途 + 空闲合计），`max_connections_per_host`
/// 对着它判。`acquire` 判 `OpenNew` 时即 `+1`（名额在建连之前就占住，否则并发的两次交换会同时越过上限），
/// `release` 收到空连接（建连失败或判不干净要关）时 `-1`，收到可回池连接时入空闲表、计数不动。
class HttpConnectionPool {
  public:
    /// @brief 取一条连接：命中空闲即交出；无空闲且名额有余则记上名额放手新建；否则判排队。
    /// @param key 池键（含配置代次；代次不匹配的空闲条目按过期处理）。
    /// @param options 本客户端的池口径（淘汰阈值与上限都从它取）。
    /// @param now 现在的时刻（注入便于判据观测；调用方用 `std::chrono::steady_clock::now()`）。
    /// @return 判词、连接与出生时刻（连接仅 `Ready` 时非空）。
    [[nodiscard]] auto acquire(const HttpPoolKey &key, const http::PoolOptions &options,
                               std::chrono::steady_clock::time_point now) -> HttpPoolAcquired;

    /// @brief 归还一条连接：判干净就回空闲表，否则关闭并让出名额。
    /// @param key 池键（与取用时同一枚）。
    /// @param connection 交回的连接；空 = 这条连接已死或不干净，只让名额。
    /// @param born 连接的出生时刻（随连接循环传递，见 `HttpPoolAcquired`）。
    /// @param options 本客户端的池口径（与取用时同源）。
    /// @param now 现在的时刻（空闲起算的基准）。
    auto release(const HttpPoolKey &key, std::optional<HttpPoolConnection> connection,
                 std::chrono::steady_clock::time_point born, const http::PoolOptions &options,
                 std::chrono::steady_clock::time_point now) -> void;

    /// @brief 清空全部空闲连接并让出全部计数（关掉开关时调用；在途连接由各自的归还路径收尾）。
    auto clear() -> void;

  private:
    /// @brief 一条躺在空闲表里的连接。
    struct IdleEntry {
        HttpPoolKey key;  ///< 入池时的键（代次含在内）
        HttpPoolConnection connection;  ///< 持有物
        std::chrono::steady_clock::time_point born;  ///< 入池时刻（最长寿命的出生点）
        std::chrono::steady_clock::time_point idle_since;  ///< 空闲起算时刻
    };

    /// @brief 把过期的空闲条目清出去（空闲超时、最长寿命、代次不匹配三算一并）；计数随之让出。
    /// @param key 调用方当前的池键（代次对照基准）。
    /// @param options 本客户端的池口径。
    /// @param now 现在的时刻。
    auto sweep_expired(const HttpPoolKey &key, const http::PoolOptions &options,
                       std::chrono::steady_clock::time_point now) -> void;

    std::mutex mutex_;  ///< 护下面两件；出口不交引用，锁外只拿到值
    std::vector<IdleEntry> idle_;  ///< 空闲连接（量级 = 每键上限之和，线性扫描足矣）
    std::map<HttpPoolKey, std::size_t> live_;  ///< 每键现存连接数（在途 + 空闲合计）
};

/// @brief 一条连接在交换期间的占用卫：析构即归还（判干净回池、否则关闭并让出名额）。
///
/// 存在的理由是发起侧两条协程体的跳循环里失败出口太多（写出失败、读失败、协议错位、体越上限……），逐一
/// 补还池调用迟早漏一处；把「占着一条连接」收成作用域事实后，任何出口都恰好归还一次。`arm()` 是唯一的
/// 判定动作：装配器与应答头在锁外判完五判据，全过才武装——未武装的析构一律按「这条连接不再可信」关闭。
class HttpPoolLease {
  public:
    /// @brief 空卫（池未启用时的占位，析构零作为）。
    HttpPoolLease() = default;

    /// @brief 建卫并占住一个名额位：此后任何出口都恰好归还一次。
    /// @param pool 池本体（须比本卫活得久：池挂在 `Client` 上，交换期间恒在）。
    /// @param key 池键。
    /// @param options 本客户端的池口径（归还时用）。
    HttpPoolLease(HttpConnectionPool &pool, HttpPoolKey key, http::PoolOptions options)
        : pool_{&pool}, key_{std::move(key)}, options_{options} {}

    /// @brief 只可移动：卫代表「恰好一次归还」这件事，拷贝会制造第二次归还。
    HttpPoolLease(const HttpPoolLease &) = delete;
    auto operator=(const HttpPoolLease &) -> HttpPoolLease & = delete;
    HttpPoolLease(HttpPoolLease &&other) noexcept
        : pool_{other.pool_}, key_{std::move(other.key_)}, options_{other.options_},
          connection_{std::move(other.connection_)}, born_{other.born_}, adopted_{other.adopted_},
          armed_{other.armed_} {
        other.pool_ = nullptr;
    }
    // 移动赋值先 `settle()` 放手旧租约：名义抛点只有池内空闲表的插入（bad_alloc），而本函数已声明
    // noexcept——改 try/catch 会把「归还失败」静默吞掉，与「恰好一次归还」的契约正面冲突，故就地豁免。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    auto operator=(HttpPoolLease &&other) noexcept -> HttpPoolLease & {
        if (this != &other) {
            settle();
            pool_ = std::exchange(other.pool_, nullptr);
            key_ = std::move(other.key_);
            options_ = other.options_;
            connection_ = std::move(other.connection_);
            born_ = other.born_;
            adopted_ = other.adopted_;
            armed_ = other.armed_;
        }
        return *this;
    }

    // 同上：析构期没有上报通道，且 `settle()` 的名义抛点只有池内表插入。
    // NOLINTBEGIN(bugprone-exception-escape)
    /// @brief 析构即归还：武装过且回池判据成立交回连接，否则关闭并让出名额。
    ~HttpPoolLease() { settle(); }
    // NOLINTEND(bugprone-exception-escape)

    /// @brief 认领一条连接（取用所得或新建握手所得），并记下它的出生时刻。
    ///
    /// 「认领」同时是析构归还的开关：排队失败而没认领任何东西的卫，析构零作为（名额没占过就不得让出）。
    /// @param connection 认领的连接（`OpenNew` 路径在建连之前先认领一枚空连接——名额已记上，建连失败时
    ///                   析构据此让出名额）。
    /// @param born 连接的出生时刻。
    auto adopt(HttpPoolConnection connection, std::chrono::steady_clock::time_point born) -> void {
        connection_ = std::move(connection);
        born_ = born;
        adopted_ = true;
    }

    /// @brief 武装归还：析构时把认领的连接交回空闲表（而非关闭）。
    auto arm() -> void { armed_ = true; }

    /// @brief 已认领的连接（尚未认领时两枚指针皆空）。
    [[nodiscard]] auto connection() -> HttpPoolConnection & { return connection_; }

    /// @brief 这条连接的出生时刻。
    [[nodiscard]] auto born() const -> std::chrono::steady_clock::time_point { return born_; }

  private:
    /// @brief 归还动作（析构与移动赋值共用）；幂等。未认领过任何东西的卫零作为。
    ///
    /// 武装与否在这里兑现成两种归还：武装 = 交回连接（回空闲表，名额不动）；未武装 = 交空（连接关闭、
    /// 名额让出）——「判干净」的决定权在装配器与应答头，卫只负责把判定不带私心地翻译成池的动作。
    auto settle() -> void {
        if (pool_ == nullptr || !adopted_) {
            pool_ = nullptr;
            return;
        }
        std::optional<HttpPoolConnection> handed_back = std::nullopt;
        if (armed_) {
            handed_back = std::move(connection_);
        }
        pool_->release(key_, std::move(handed_back), born_, options_, std::chrono::steady_clock::now());
        pool_ = nullptr;
        adopted_ = false;
        armed_ = false;
    }

    HttpConnectionPool *pool_{nullptr};  ///< 池本体（非拥有）
    HttpPoolKey key_;  ///< 取用时的键
    http::PoolOptions options_;  ///< 归还时用的口径快照
    HttpPoolConnection connection_;  ///< 已认领的连接（空 = 还没认领或已归还）
    std::chrono::steady_clock::time_point born_;  ///< 连接出生时刻
    bool adopted_{false};  ///< 是否认领过（析构归还的开关）
    bool armed_{false};  ///< 是否判干净（武装后析构回池，否则关闭）
};

}  // namespace aurora::net::detail
