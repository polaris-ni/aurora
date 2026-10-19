#pragma once

// Aurora net — TLS 后端接缝的内部一侧。
//
// 公共契约（`TlsBackend`、`TlsDataState`、注册表四组入口）在 `aurora/net/tls_backend.h`，本头只放
// **库内**才需要的四样东西：
//   - 内置后端的唯一入口声明：每枚后端一个 TU，入口名带各自的后端标识（`make_tls_backend_schannel`
//     等），因此多条腿可以同时编进库里。声明按各自的构建宏门控——没编进来的腿连声明都不存在，
//     注册表也就无从误调一枚不存在的工厂（链接期缺符号比运行期落错误更早、也更贵）。
//   - 候选链解析入口：握手起手用一次，产出的后端实例与生效的后端 id 一并交给会话状态。
//   - 能力位探测口：`Engine::capabilities().tls` 的唯一真值源，现由注册表回答（注册表非空即真）。
//   - 注册表的整表快照与还原：注册表全部入口都是可写的进程级状态，同进程里先后跑的测试用例必须
//     能把现场原样还回去，否则「前一用例注册的桩」会决定后一用例选到哪条腿。
//
// 回落纪律（规范性，实现落 `net/tls_backend.cpp`）：只有「这一枚不可用」才换下一枚，即候选未注册、
// 或工厂落 `net-tls-engine-unavailable` / `net-platform-unavailable`；其余失败终止整条链并原样上报。
// 后端一旦造出并开始握手就不再换腿——信任根在各腿各自实现里，换腿即换一次证书校验的口径。

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/tls_backend.h"

namespace aurora::net::detail {

#ifdef AURORA_ENABLE_NET_TLS_SCHANNEL
/// @brief Schannel 腿工厂（`tls_schannel.cpp` 定义）。
/// @param context 安全口径。
/// @param server_name SNI 与主机名校验的目标名。
/// @return 成功携后端；口径不可表达为 `net-platform-unavailable`。
auto make_tls_backend_schannel(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>>;
#endif

#ifdef AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT
/// @brief SecureTransport 腿工厂（`tls_securetransport.cpp` 定义）。
/// @param context 安全口径。
/// @param server_name SNI 与主机名校验的目标名。
/// @return 成功携后端；口径不可表达为 `net-platform-unavailable`。
auto make_tls_backend_securetransport(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>>;
#endif

#ifdef AURORA_ENABLE_NET_TLS_OPENSSL
/// @brief OpenSSL 腿工厂（`tls_openssl.cpp` 定义）。
/// @param context 安全口径。
/// @param server_name SNI 与主机名校验的目标名。
/// @return 成功携后端；口径不可表达为 `net-platform-unavailable`。
auto make_tls_backend_openssl(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>>;
#endif

#ifdef AURORA_ENABLE_NET_TLS_MBEDTLS
/// @brief mbedTLS 腿工厂（`tls_mbedtls.cpp` 定义）。
/// @param context 安全口径。
/// @param server_name SNI 与主机名校验的目标名。
/// @return 成功携后端；口径不可表达为 `net-platform-unavailable`。
auto make_tls_backend_mbedtls(const TlsContext &context, std::string_view server_name)
    -> Result<std::unique_ptr<TlsBackend>>;
#endif

/// @brief 按候选链造出本次握手用的后端。
/// @param context 安全口径；`context.backend` 置值即只用那一枚且不回落。
/// @param server_name SNI 与主机名校验的目标名（透传给工厂）。
/// @return 成功携「后端 + 生效的后端 id」；链里没有任何可用后端为 `net-tls-engine-unavailable`
///         （detail 点名链内容与在案 id），工厂的非跳过类失败原样上报。
/// @note 只在握手起手调用一次：结果连同 id 固化进会话状态，此后不再有第二次选择的机会。
auto resolve_tls_backend(const TlsContext &context, std::string_view server_name)
    -> Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>;

/// @brief 本进程是否有可用的 TLS 后端（`Engine::capabilities().tls` 的唯一真值来源）。
///
/// 由注册表回答，因此注入自研后端后能力位随之为真——能力位与「调用真的能成功」仍然同源。
[[nodiscard]] auto tls_backend_available() -> bool;

/// @brief 把注册表的整表状态（在案后端与其工厂、优先级、缺省候选链与「链是否被显式置过」）存进
///        库内的唯一快照槽。
///
/// 注册表是进程级状态，而它的全部入口都可写，故同一 runner 进程里跨用例的判据必须先存后还。
/// 快照槽只此一枚、按调用次序覆盖，因此调用方必须是串行跑的测试用例。
/// @note Side-effects: 写入库内静态快照槽（不影响任何在途会话）
auto snapshot_tls_backend_registry() -> void;

/// @brief 把快照槽里的状态原样装回注册表；槽未填过即空操作。
///
/// 还原的是注册表本身，已在途的会话不受影响——它们持有自己的后端实例，那正是注册表与实例分开的理由。
/// @note Side-effects: 覆盖注册表的在案集合与缺省候选链
auto restore_tls_backend_registry() -> void;

}  // namespace aurora::net::detail
