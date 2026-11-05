# CHG-063 流式解压上限的公共契约对齐：把「缺省跟随」限定到物化路径

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-063 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.HTTP-CLIENT.001 |
| 影响面 | `include/aurora/net/http_client.h`（`max_body_bytes` / `max_decompressed_bytes` 的文档口径与 `open_stream` 段）、`include/aurora/net/http_stream.h`、`include/aurora/net/http.h`、`src/aurora/net/detail/http_encoding.h` 的注释口径；`codespec/errors.toml` 的 `http-body-too-large` 提示及其生成物 `include/aurora/core/error_codes.gen.h`、`codespec/ERROR_CATALOG.md`；`tests/unit/utest_net_http_client.cpp` 与 `tests/unit/utest_net_http_stream.cpp` 补两路径对照用例。**不改任何函数签名、不改流式实现语义、不新增错误码、不动 `DecompressOptions`**；`aurora_api.json` 仅错误码段的提示文案随真源刷新，公共 API 结构无差异 |

## 动机

1. **公共头对同一枚旋钮给出了两条互斥的口径。** `include/aurora/net/http_client.h` 的类级说明与 `max_decompressed_bytes` 的 setter / getter 白纸黑字写「`std::nullopt` = 跟随 `max_body_bytes()`」「`nullopt` 表示跟随，不表示不限」，且没有限定适用路径；而同一头文件的 `open_stream` 段、`include/aurora/net/http_stream.h` 与 `codespec/specification/06-app-platform.md` §8.8.21 写的是「线上字节不设限，解压产物只取 `max_decompressed_bytes()`，未设即逐层不限」。调用方按 setter 的字面承诺只设 `max_body_bytes(64 MiB)` 便以为解压炸弹已挡住：走 `send` 确实受限，改走 `open_stream` 或 `open_sse` 则 `DecompressOptions::max_output_bytes` 为 `nullopt`，`GuardedDecompressor` 的 `has_value()` 恒假、`produced_` 只累加不判定，产物完全不设限。同一份配置在两条交付路上给出相反的安全结局，这是契约说谎，不是实现回归。

2. **规格里的「流式不跟随」是有完整理由的产品裁决，本提案不推翻。** `codespec/specification/06-app-platform.md` §8.8.21 明确：`max_body_bytes()` 是物化路径的内存闸，流式不物化、没有内存可闸；把 `send` 的「未显式收紧即跟随」搬到流式，会让「流式下载一个大压缩包」必须调大一枚与内存无关的旋钮，等于把「不设限」的承诺又收回去。物化路径 `src/aurora/net/http_client.cpp` 用三元式跟随，流式路径只取 `max_decompressed_bytes_`，两者都按规格实现。因此该缺陷的正确修法是让公共文档与规格说同一句话，而不是改实现语义。

3. **现有错误提示把调用方往无防护的路上引。** `codespec/errors.toml` 的 `http-body-too-large` 提示结尾建议「或切到 `open_stream`，以有界分块交付替代物化」；在只设了 `max_body_bytes` 的配置下，这条建议恰好把用户从有解压闸的路径导向解压不限的路径。生成物 `include/aurora/core/error_codes.gen.h` 继承同一句话。

4. **覆盖空白让分歧长期不被发现。** `tests/unit/utest_net_http_client.cpp` 的 `send_enforces_the_decompressed_ceiling` 与 `tests/unit/utest_net_http_stream.cpp` 的 `stream_enforces_the_decompressed_ceiling` 都只显式设 `max_decompressed_bytes` 再断言生效；`stream_ignores_the_materialized_body_ceiling` 只设 `max_body_bytes` 并断言线上字节不受约束，却从不检查解压产物。全仓没有一条用例把「同一 `Client`、同一份只设 `max_body_bytes` 的配置」放在 `send` 与 `open_stream` 两侧对照，公共头的两处口径因而无人对账。

5. **本提案的边界。** 给流式路径加默认解压上限（库侧兜底常量）会改变 §8.8.21 的产品语义，也会让「显式不限」在现有 `std::optional` 类型下无从表达（`nullopt` 已被「跟随」占用）；该产品决策另立提案。本轮只承担「契约说真话 + 消除误导引导 + 用单测把两条路径的差异钉成可证伪的事实」。

## 变更内容

1. `include/aurora/net/http_client.h` 的类级说明、`max_body_bytes` 与 `max_decompressed_bytes` 的 setter / getter 文档统一改成路径感知口径：`nullopt` 的「缺省跟随 `max_body_bytes()`」只对物化入口 `send` 成立；流式入口 `open_stream` / `open_sse` 不看 `max_body_bytes()`，`max_decompressed_bytes()` 未设即解压产物不限。getter 文档删去「`nullopt` 表示跟随，不表示不限」这类无路径限定的绝对句，改为分别点名两条路径的读法。
2. `open_stream` 段与 `include/aurora/net/http_stream.h` 的口径补一句显式安全须知：处理不可信对端或压缩应答时必须显式设定 `max_decompressed_bytes()`，否则本入口的解压产物没有上限；同时保留「线上字节不设限」的既有事实，不把两枚旋钮混为一谈。
3. `codespec/errors.toml` 的 `http-body-too-large` 提示删去「切 `open_stream` 规避物化上限」的建议，改为按越限的那枚旋钮分流：线上字节越限才可考虑流式交付，解压产物越限则两条路都要调 `max_decompressed_bytes()`。经 `generate_error_codes` 刷新 `include/aurora/core/error_codes.gen.h` 与 `codespec/ERROR_CATALOG.md`，不手改生成物。
4. `include/aurora/net/http.h` 中引用两枚旋钮的注释按本提案的文档口径同步；`src/aurora/net/detail/http_encoding.h` 的 `StreamingDecoder::create` 参数注释把 `std::nullopt` 的含义写全（逐层不限），避免内部实现侧再留一处歧义。
5. **两路径对照单测**：在 `tests/unit/utest_net_http_client.cpp` 补一例「只设 `max_body_bytes`、不设 `max_decompressed_bytes` 的 gzip 膨胀体走 `send`，落 `http-body-too-large` 且 detail 点名跟随值」；在 `tests/unit/utest_net_http_stream.cpp` 补一例「同一份配置走 `open_stream` 时解压产物不受该上限约束，`StreamOutcome` 成功且 `delivered_bytes` 为膨胀后的真实长度」，以及一例「显式设 `max_decompressed_bytes` 后同一流式载荷落 `http-body-too-large`」。依赖内置解码器的用例按 `tests/unit/utest_net_http_stream.cpp` 既有的 TEST-R6 纪律，在 `AURORA_ENABLE_COMPRESSION=OFF` 腿登记同名 `AURORA_TEST_SKIP`，两腿名册逐名可比。
6. `CHANGELOG.md` 的 `### Fixed` 记一条文档契约修正；规格回写见下节。

## 验收判据

1. `include/aurora/net/http_client.h`、`include/aurora/net/http_stream.h`、`include/aurora/net/http.h`、`src/aurora/net/detail/http_encoding.h` 与 `codespec/specification/06-app-platform.md` §8.8.20 / §8.8.21 对 `max_decompressed_bytes()` 未设时的行为描述逐处一致：物化跟随、流式不限，且都写明调用方须在流式路径显式设限。
2. `codespec/errors.toml` 的 `http-body-too-large` 提示不再建议「切 `open_stream`」规避上限；`generate_error_codes` 后生成物与真源逐字一致，错误码同步门禁无新增红灯。
3. 两路径对照单测在 `AURORA_ENABLE_COMPRESSION=ON` 腿全绿，三条用例分别断言：物化侧越限、流式侧不越限、流式侧显式设限后越限；`=OFF` 腿同名登记、名册逐名相同。
4. 既有回归不转红：`send_enforces_the_decompressed_ceiling`、`stream_enforces_the_decompressed_ceiling`、`stream_ignores_the_materialized_body_ceiling` 三条用例的断言与用例名不变，行为读数不变。
5. **判据可辨别力（真注入变异）**：把 `src/aurora/net/http_client.cpp` 物化路径的三元跟随临时改回「只取 `max_decompressed_bytes_`」，两路径对照用例中的物化侧必须转红；把流式路径的 `decompressed_limit` 临时改成跟随 `max_body_bytes_`，两路径对照用例中的流式侧必须转红。两处变异各注入一次、还原后复跑全绿，判据不靠「多读几次」或恒真断言成立。
6. 公共面零签名改动：`--target aurora_api_json` 后 schema 结构无差异，仅 `http-body-too-large` 的提示文案随 `codespec/errors.toml` 刷新；不新增错误码、不改函数签名、不动 `DecompressOptions`；改动 TU 的增量 `lint`、`format-check` 与相关单测全绿。

## 回写落点

- `codespec/specification/06-app-platform.md`：§8.8.20 的「解压产物另有独立上限」句补上「仅物化路径」限定；§8.8.21 的「线上字节不设限，解压产物照 `max_decompressed_bytes()` 限」段补一句公共头按路径申报的口径与显式设限须知，保留原产品裁决不动。
- `codespec/errors.toml`：`http-body-too-large` 的提示去掉误导性的流式切换建议，改按越限旋钮分流。
- `codespec/ERROR_CATALOG.md`：随生成器刷新该错误码的提示行，保持与真源一致。
- `codespec/specification/01-core.md`：§12.2 的「解压炸弹防护双层」段补一句「限额是否注入由各消费面决定，HTTP 流式入口在未显式设限时逐层不限」，使 core 侧不与 HTTP 侧口径冲突。
