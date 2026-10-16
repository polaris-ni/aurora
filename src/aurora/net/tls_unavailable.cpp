// Aurora net — 无 TLS 后端时的诚实落点。
//
// 本 TU 在「构建没有选出任何 TLS 后端」时编译（提案 §5 的平台矩阵：Linux 未给 provider 即此形态）。
// 它存在的意义是把「没有后端」表达成一个**可判定的运行期事实**，而不是编译失败、静默明文或假能力：
//   - `tls_backend_available()` 为 false ⇒ `Engine::capabilities().tls` 为 false，调用方据此降级；
//   - `make_tls_backend()` 恒落 `net-tls-engine-unavailable` ⇒ 误用有一条带迁移指引的错误。
// 这里不提供任何「退回明文」的通路：把 TLS 请求静默降级成明文是本切片最不可接受的失败形态。

#include <memory>
#include <string_view>

#include "aurora/core/result.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/tls.h"

// 四个后端宏全为假时本 TU 才产出符号：任一为真都由对应后端 TU 定义同名的两枚函数，重复定义即在
// 链接期报错——所以这份条件必须与「后端 TU 的入选集合」逐一对应，漏一枚就是那条腿链不出库。
#if !defined(AURORA_NET_TLS_SCHANNEL) && !defined(AURORA_NET_TLS_SECURE_TRANSPORT) && \
    !defined(AURORA_NET_TLS_OPENSSL) && !defined(AURORA_NET_TLS_MBEDTLS)

namespace aurora::net::detail {

auto make_tls_backend(const TlsContext &, std::string_view) -> Result<std::unique_ptr<TlsBackend>> {
    return make_error(ErrorCode::NetTlsEngineUnavailable,
                      ErrorParams{{"detail", "this build selected no TLS backend"}});
}

auto tls_backend_available() -> bool { return false; }

}  // namespace aurora::net::detail

#endif
