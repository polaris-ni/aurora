# CHG-068 TLS 主机名校验：兑现 SAN 优先口径并补跨后端对账

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-068 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.NET.TLS-BACKEND.001 |
| 影响面 | `src/aurora/net/tls_schannel.cpp`（SAN 存在性判定与回落条件）、`src/aurora/net/tls_openssl.cpp` / `src/aurora/net/tls_securetransport.cpp` / `src/aurora/net/tls_mbedtls.cpp` 的头部注释、`tests/framework/tls_peer.h`（证书名字形态夹具）、`tests/unit/utest_net_tls.cpp`（SAN 优先与跨后端对账判据）；**不改四后端「链校验交平台 / 库」的产品裁决、不新增错误码、不改 `TlsContext` 与任何公共签名**；公共 API 结构无差异 |

## 动机

1. **自家实现与自家注释不一致。** `src/aurora/net/tls_schannel.cpp::verify_hostname()` 的注释写「只有 SAN 完全没给出 dNSName 才回落到 CN」，但 `certificate_dns_names()` 在「SAN 扩展存在、其中只有 IP / URI / email 等条目」时同样返回空集，调用方据此回落 CN。也就是说一张明确声明了可核对名字不在这张证书里的现代证书，仍会被 CN 兜底接受——比注释承诺的宽松。RFC 6125 的 SAN 优先口径在此被绕开。

2. **三份头部注释互相矛盾，调用方拿不到确定契约。** `src/aurora/net/tls_openssl.cpp` 头部写 `SSL_set1_host` 的「有 SAN 就不再回看 CN、通配只认最左标签且与 Schannel 腿手写的 `name_matches` 是同一套规则」；`src/aurora/net/tls_securetransport.cpp` 头部第 19–21 行却自陈「与 Schannel 有一处已知不重合：那腿对只有 CN、没有 SAN 的证书回落到 CN，而本平台按自己的现行策略裁决」；`src/aurora/net/tls_mbedtls.cpp` 头部又写「信任判定交回库内，与 Schannel 相反」。一侧是库内校验器、一侧是手写匹配、两侧是系统策略，「同一套规则」这句没有依据。

3. **夹具把差异藏起来了。** `tests/framework/tls_peer.h::openssl_issue()` 的 `openssl req` 恒同时写 `/CN=<name>` 与 `-addext subjectAltName=DNS:<name>` 且两者同名，因此 `tests/unit/utest_net_tls.cpp` 既有的自签 / 追加根 / 过期 / 错主机名四类用例从未造出「只有 CN、没有 SAN」或「SAN 里没有 dNSName」的证书，扫描确认的跨后端差异零驱动。`CA:TRUE` / `serverAuth` 两项是为 SecureTransport 的系统策略补的，改夹具时必须保留。

4. **对账自动化仍是空白。** `cmake/AuroraNetTls.cmake` 按平台装配：Windows 走 Schannel（可并存 OpenSSL provider）、macOS 走 SecureTransport、Linux 由 mbedTLS 或 OpenSSL provider 提供；任何单一 CI 作业只看得到其中一到两套。清单要求的「四类证书 × 四后端」逐值对账没有自动化载体，差异目前只靠注释自陈，无法防止后续改动悄悄扩大它。

5. **本提案的边界。** 不推翻「链校验与名字判定交平台 / 库」的产品裁决，不把四后端统一成一份手写匹配规则，不新增错误码与公共枚举，不改 `TlsContext`、不改 `verify_hostname()` 以外的 Schannel 校验流程；`name_matches()` 已收紧的通配符与大小写口径原样保留。CN-only 证书上确实存在的平台策略分叉予以保留，但改成可观测、可断言、可归因的对账表。

## 变更内容

1. `tls_schannel.cpp` 把「SAN 扩展是否存在」与「其中有无 dNSName」拆成两件事：`certificate_dns_names()` 增一个出参回报扩展存在性（含扩展存在但解码失败时按「存在」计，绝不静默回落到 CN）；`verify_hostname()` 只在扩展完全不存在时才回落 CN，扩展存在而 dNSName 列表为空即以 `net-tls-cert-rejected` 拒绝并带上目标名。`name_matches()` 与大小写归一逻辑不动。
2. 订正三份头部注释：`tls_openssl.cpp` 删去「与 Schannel 手写规则是同一套」的无依据断言，改为「同一条把名字判定交库内 RFC 6125 通路的取舍」；`tls_securetransport.cpp` 把自陈差异限定为「CN-only 证书上的平台策略分叉」并指向新增对账用例；`tls_mbedtls.cpp` 的表述与 `mbedtls_ssl_set_hostname()` 的实际语义对齐；`tls_schannel.cpp` 的注释与收紧后的实现一致。
3. `tests/framework/tls_peer.h` 给 `openssl_issue()` 增加证书名字形态参数：默认恒带同名 CN + SAN（既有全部用例零改动），另可产出「只有 CN」与「SAN 里只有非 dNSName 条目」两张证书；`CA:TRUE` / `serverAuth` / `basicConstraints` 等既有扩展逐项保留，避免系统策略腿无故转红。
4. `tests/unit/utest_net_tls.cpp` 新增两组判据：① SAN 优先——SAN 里无 dNSName 恒拒、dNSName 与目标名不匹配恒拒、只有 CN 且链可信时按本后端既有裁决落地，每条都带同名 SAN 的对照组；② 跨后端对账——对当前构建注册到的每一套后端逐一记录四类证书的结论，断言「每格都有确定结论且错误可归因到 `net-tls-cert-rejected`」，不强制四腿同色，但把实际差异写进用例输出。平台与能力分支一律写在**用例体内**，沿用该文件既有的运行期 `AURORA_TEST_SKIP` 纪律。
5. `CHANGELOG.md` 的 `### Fixed` 记一条主机名校验口径收口；规格回写见下节。

## 验收判据

1. SAN 优先判据可证伪：把「SAN 扩展存在」的判定短路回「dNSName 列表为空即回落 CN」，新增的拒绝断言必须转红；还原后全绿。
2. 同名 SAN 对照组全部通过，证明新增判据不是恒真断言；既有 `utest_net_tls.cpp` 四类证书用例零改动全绿。
3. 对账表覆盖当前构建注册到的每一套后端，每格结论由成功态或 `net-tls-cert-rejected` 表达，不出现「无结论 / 静默跳过以外的空档」；缺 `openssl` 可执行文件时按既有口径 `AURORA_TEST_SKIP`，并在用例说明里写明这是环境前提而非逻辑豁免。
4. 四份后端头注释互相不再矛盾，CN-only 的平台策略分叉有且只有一处权威表述。
5. 公共面零签名变更：`aurora_api.json` 的公共 API 结构无差异；改动 TU 的增量 lint、format-check、相关单测与 `check_*` 门禁全绿。

## 回写落点

- `codespec/specification/06-app-platform.md`：§8.8.18 的 TLS 后端条目补写「SAN 扩展存在即不再回看 CN」的手写侧口径，并把「CN-only 证书上的平台策略分叉」写成唯一权威表述。
- `codespec/SPECIFICATIONS.md`：在 TLS 后端特性行的说明里补一句「名字判定按 SAN 优先，CN-only 分叉由跨后端对账用例记录」，使特性表与规格正文一致。
