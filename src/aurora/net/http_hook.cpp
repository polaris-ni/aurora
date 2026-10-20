// http_hook.cpp —— 交换钩子公共面的取之不尽的定义：视图的构造与四枚读口、收摊问的空缺省、登记句柄的摘除。
//
// 视图与句柄都只有「把字段搬到哪儿、什么时候把登记从链上摘掉」这一条逻辑，没有任何网络或时钟事实；把它们
// 放在头文件里会让每个包含者重付一遍模板与内联展开，也让链内核的私有构造点失去一个明确的调用侧。定义集中在
// 本件，声明处的三条硬约束（同步、短路只在 `send`、保留头不可触碰）由 `detail::ExchangeHookChain` 兑现。
#include "aurora/net/http_hook.h"

#include <utility>

#include "aurora/core/headers.h"
#include "aurora/core/url.h"
#include "aurora/net/detail/http_hook_chain.h"

namespace aurora::net::http {

auto ExchangeRequestView::method() const noexcept -> Method { return method_; }

auto ExchangeRequestView::url() const noexcept -> const std::optional<Url> & { return url_; }

auto ExchangeRequestView::headers() const noexcept -> const Headers & { return headers_; }

auto ExchangeRequestView::leg() const noexcept -> ExchangeLeg { return leg_; }

ExchangeRequestView::ExchangeRequestView(const Method method, std::optional<Url> url, Headers headers,
                                         const ExchangeLeg leg)
    : method_(method), url_(std::move(url)), headers_(std::move(headers)), leg_(leg) {}

auto ExchangeHook::after_response(const ExchangeRequestView & /*request*/, const Response & /*response*/) const
    -> void {
    // 空缺省：只改请求头的钩子（每请求补一组静态字段那一类）不必写第二份空函数体。
    // 这里刻意不记日志——一次交换问两问是常态，为「没实现的可选回调」打一行 WARN 会把真正的异常淹没。
}

ExchangeHookRegistration::ExchangeHookRegistration(std::weak_ptr<detail::ExchangeHookChain> chain,
                                                   const std::uint64_t id)
    : chain_(std::move(chain)), id_(id) {}

ExchangeHookRegistration::~ExchangeHookRegistration() { clear(); }

ExchangeHookRegistration::ExchangeHookRegistration(ExchangeHookRegistration &&other) noexcept
    : chain_(std::move(other.chain_)), id_(std::exchange(other.id_, 0)) {}

auto ExchangeHookRegistration::operator=(ExchangeHookRegistration &&other) noexcept -> ExchangeHookRegistration & {
    if (this != &other) {
        clear();  // 先摘掉自己原来那份登记，再接手对方的
        chain_ = std::move(other.chain_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

auto ExchangeHookRegistration::clear() -> void {
    if (id_ == 0) {
        return;  // 空句柄（默认构造或已被移走）：重复调用与析构并存不双重摘除
    }
    const std::uint64_t id = std::exchange(id_, 0);
    if (const auto chain = chain_.lock()) {
        chain->remove(id);  // 链已随最后一个 `Client` 销毁时，这里就是一次空操作
    }
}

}  // namespace aurora::net::http
