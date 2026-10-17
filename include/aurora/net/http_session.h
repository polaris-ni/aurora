/// @brief HTTP 会话面（身份层）：Cookie Jar + 缺省配置（§12「Cookie」条、提案 §16 第 36 项第①款）。
/// @file net/http_session.h
///
/// 本头回答一个问题：**「一组请求共享同一次登录态」**。`Session` 收窄为身份层——持一枚内存 Cookie Jar
/// 并把它装进自己的 `Client`，`client()` 交出的引用照常配置（连接池、重试、代理……都在 `Client` 上）：
/// 经它发出的交换自动收发 Cookie，`Client` 的拷贝共享同一张池但不共享身份（`shared_ptr` 共享表是刻意的
/// 设计，但对身份而言「两份配置另一只手」会变成「两个调用方共用一个登录态」，那是看不见的副作用，故
/// Jar 只住 `Session`、裸 `Client` 不收不发 Cookie）。
///
/// Cookie 的口径是 `RFC 6265` 的最小集（name / value / domain / path / `Secure` / `HttpOnly` 加过期，
/// `Expires` 与 `Max-Age` 并给时 `Max-Age` 优先）：内存、不落盘、不做 `SameSite`；`Set-Cookie` 恒不折叠；
/// `Secure` cookie 遇明文目标不发送也不存储——这一条独立于 `NetPolicy` 的明文闸（策略管「这次交换允不
/// 允许发生」，Jar 管「这份凭据上不上这条信道」）。附着点在发起侧：起手把 Jar 按 `§5.4` 的选择算法算出的
/// `Cookie` 头附到原始请求副本上（内存缓存的建键因此自动看见它——凭据字段恒参与建键），重定向换 origin
/// 时剥掉旧值、按新目标的域重新取；应答侧把 `Set-Cookie` 逐条存回。Jar 不引入新失败面：畸形的
/// `Set-Cookie` 丢弃、不报错——一枚坏 cookie 不该让一次本可成功的交换失败。
///
/// @code
/// aurora::net::http::Session session;
/// session.client().pool({.enabled = true});           // 连接池挂在 Client 上（缺省关）
/// auto task = session.client().send(aurora::net::http::Request::get("https://example.com/me"));
/// @endcode
/// @note Thread: thread-safe（值语义；Jar 与池皆为共享状态，出海口只交值）
/// @note Side-effects: 经 `client()` 发起的交换收发 Cookie；构造无副作用

#pragma once

#include <memory>

#include "aurora/net/http_client.h"

namespace aurora::net::detail {
class HttpCookieJar;  ///< 实现侧 Jar（字段全部留在 `net/detail/http_cookie_jar.h`）
}

namespace aurora::net::http {

/// @brief HTTP 会话面：持一枚 Cookie Jar 与一只自己的 `Client`，让一组请求共享同一次登录态。
/// @note Thread: thread-safe（`client()` 交出的引用照常可拷贝、可并发发起）
/// @note Side-effects: 经 `client()` 发起的交换自动收发 Cookie
class Session {
  public:
    /// @brief 以缺省口径构造：自持一只 `Client` 并给它装上本会话的 Cookie Jar。
    ///
    /// 定义在实现侧：Jar 是一只指向库内类型的共享指针，公共头只知道它的名字，构造点必须看得见全貌。
    Session();

    /// @brief 查看本会话的客户端（连接池、重试、代理等配置都在它身上）。
    /// @return 客户端引用；本会话存续期间恒有效。
    [[nodiscard]] auto client() -> Client & { return client_; }

    /// @brief 查看本会话的客户端（只读）。
    /// @return 客户端常量引用。
    [[nodiscard]] auto client() const -> const Client & { return client_; }

  private:
    Client client_;  ///< 本会话的发起侧客户端（配置照常在它身上）
    std::shared_ptr<aurora::net::detail::HttpCookieJar> jar_;  ///< 本会话的身份存储（装进 `client_`；随会话生灭）
};

}  // namespace aurora::net::http
