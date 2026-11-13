# CHG-058 移除 TLS provider 旧单值别名 `AURORA_NET_TLS_PROVIDER`

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-058 |
| 提出日期 | 2026-11-04 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.NET.TLS-BACKEND.001 |
| 影响面 | 构建面旧单值缓存变量 `AURORA_NET_TLS_PROVIDER` 删除（替代写法 `AURORA_NET_TLS_PROVIDERS` 逐字不变）；`cmake/AuroraNetTls.cmake` 的别名读取、新旧同设冲突诊断与回落赋值三段删除；[`codespec/BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md) §1.2 别名条删除；[`codespec/specification/06-app-platform.md`](../../specification/06-app-platform.md) §8.8.18 别名句删除；[`codespec/manual-test/23-net.md`](../../manual-test/23-net.md) `TC-NET-001` 步骤 11 与预期结果 11 的别名两半句删除；同批清理零引用遗留模块 cmake/AuroraCheckTestRegistry.cmake 并删 [`codespec/BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md) §1.1 对应行。不触及任何 feature 宏、CMake 目标、链接面与运行期行为；仍用旧单值拼写的既有命令行将在 configure 期因变量未知而失配，迁移路径见根目录 CHANGELOG.md |

## 动机

1. **迁移期已经结束，别名失去存在理由。** 别名由 CHG-022 引入时的口径是「现 CI 与 manual-test 的历史写法语义不变」；如今 CI 十处引用全部是 `AURORA_NET_TLS_PROVIDERS` 新写法（`.github/` 全仓检索零旧写法命中），mbedTLS 也已 vendor 进 `third_party/mbedtls`、mbedTLS-as-provider 路径已在 `cmake/AuroraNetTls.cmake` 改为 configure 期硬失败——CHG-022 与 [`BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md) §1.2 预告的两项删除前提均已满足，别名只剩 [`manual-test/23-net.md`](../../manual-test/23-net.md) 一处「别名仍可用」的自证消费。
2. **同一旋钮两种拼法是持续存在的第二真相源。** 别名要求装配面永久保留「新旧同设即硬失败」的诊断分支与回落读取逻辑；任何后来者读到这段都要重新理解一遍「为什么有两枚」。拼法归一后该分支整体消失，装配面回到「一旋钮一拼法」。
3. **遗留模块 cmake/AuroraCheckTestRegistry.cmake 是零引用死文件。** `registry_integrity` 门禁已改由 [`AuroraTests.cmake`](../../../cmake/AuroraTests.cmake) 直接注册 [`tools/check/check_test_registry.py`](../../../tools/check/check_test_registry.py)，该模块自标注「保留仅供手工 / 历史参考」且全仓检索无任何 `include()` 引用；[`BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md) §1.1 的表行同样失去指涉对象。

## 变更内容

1. `cmake/AuroraNetTls.cmake`：删除 `AURORA_NET_TLS_PROVIDER` 缓存变量声明、「新旧同设即 FATAL_ERROR」冲突诊断块、`_aurora_tls_entries` 的旧单值回落赋值三段；provider 腿改为只读 `AURORA_NET_TLS_PROVIDERS`。
2. 删除 cmake/AuroraCheckTestRegistry.cmake（整文件）。
3. [`codespec/BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md)：§1.2 删除「旧单值写法保留为别名」整条；§1.1 删除 `AuroraCheckTestRegistry.cmake` 表行。
4. [`codespec/specification/06-app-platform.md`](../../specification/06-app-platform.md) §8.8.18：删除 OpenSSL provider 逐项规则末句的别名半句。
5. [`codespec/manual-test/23-net.md`](../../manual-test/23-net.md) `TC-NET-001`：测试目的去掉「新旧旋钮同设」分支；操作步骤 11 与预期结果 11 各删别名两半句（同种类重复条目的失败形态判据保留）。2026-10-17 的历史执行记录不改写——它如实记录当时代码形态下的观测值。
6. 根目录 CHANGELOG.md：[Unreleased] Breaking 补一条（旧拼写删除 + 迁移路径 `AURORA_NET_TLS_PROVIDERS`），同条附记遗留模块删除。
7. 历史提案（CHG-018 / CHG-022）内的旧写法表述不改写：提案是提出时点的设计史，现行口径以 [`BUILD_OPTIONS.md`](../../BUILD_OPTIONS.md) 为准；本条即是对 CHG-022 验收判据 10 的废止。

## 验收判据

1. 全仓 `AURORA_NET_TLS_PROVIDER` 旧写法检索：`build*/`、`CHANGELOG.md`、`codespec/changes/`（历史提案）与 [`manual-test/23-net.md`](../../manual-test/23-net.md) 历史执行记录之外零命中。
2. 只给 `AURORA_NET_TLS_PROVIDERS` 的 configure 行为逐字不变：Win32 缺省 configure 日志仍为 `TLS backend: system stack (AURORA_ENABLE_NET_TLS_SCHANNEL)`，provider / 内置 mbedTLS / 共存 / 各失败形态的日志与诊断与删除前一致（本机 Windows 腿以 `cmake --preset ninja` 重新 configure 验证日志行）。
3. cmake/AuroraCheckTestRegistry.cmake 文件不存在，全仓 `AuroraCheckTestRegistry` 零命中。
4. 门禁全绿：`check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_no_cjk_literals`；configure 期无新告警。

## 回写落点

1. `codespec/BUILD_OPTIONS.md`：§1.2 删别名条、§1.1 删遗留模块行。
2. `codespec/specification/06-app-platform.md`：§8.8.18 OpenSSL provider 逐项规则删别名句。
3. `codespec/manual-test/23-net.md`：`TC-NET-001` 测试目的、操作步骤 11、预期结果 11 随别名删除收窄。
4. `codespec/changes/remove-tls-provider-alias/proposal.md`：实施完成后状态流转至已归档。
