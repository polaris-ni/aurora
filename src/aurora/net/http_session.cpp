#include "aurora/net/http_session.h"

#include <utility>

#include "aurora/net/detail/http_cookie_jar.h"

namespace aurora::net::http {

Session::Session() : jar_{std::make_shared<detail::HttpCookieJar>()} {
    // 身份层只做一件事：把 Jar 装进自持的客户端（friend 接缝，公共面不提供第二个装 Jar 的口）。
    client_.cookie_jar_ = jar_;
}

}  // namespace aurora::net::http
