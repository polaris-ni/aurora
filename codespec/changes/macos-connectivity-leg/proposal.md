# CHG-050 macOS 链路态采集腿（SystemConfiguration）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-050 |
| 提出日期 | 2026-10-30 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.CONNECTIVITY.001 |
| 影响面 | `src/aurora/net/connectivity_probe.cpp`（`apple_global_default_route()` 采集、`compiled_source()` 的 macOS 分支、`probe_default_route()` 分派、`has_evidence_path()` 取值）；`cmake/AuroraBackends.cmake`（`SystemConfiguration` 框架链接）；`tests/unit/utest_net_connectivity.cpp`（无腿平台口径改写）；`codespec/specification/06-app-platform.md` §8.8.41、`codespec/SPECIFICATIONS.md`、`codespec/ARCHITECTURE.md` 的 connectivity 元数据 |

## 动机

1. **macOS 是采集腿里唯一靠旁证顶替的平台。** Win32 读路由 / 接口表、Linux 读路由伪文件与 `sysfs operstate`；macOS 此前落到「有旁证通路但不采集」，`compiled_source()` 报不出具体源，读屏与诊断只能看到笼统的旁证态。
2. **系统 API 现成且语义对得上。** `SCDynamicStore` 的 `State:/Network/Global/IPv4`、`State:/Network/Global/IPv6` 两个键就是「系统当前是否给出全局默认路由」的权威读数——键存在即在线、键缺失即离线，正是 `probe_default_route()` 需要的三态输入。
3. **必须保住三态纪律。** 既有契约：读不到 ≠ 离线、无源 ≠ 离线。macOS 腿在动态存储创建失败或键义不可判时回 `nullopt`（`Unknown`），绝不折算为离线；同样也不把「有键」提升成 `Online` 之外的推断。
4. **源标识要如实。** `ConnectivitySource` 需把 macOS 标成自己的源，测试与诊断才能区分「采集腿」与「旁证腿」，否则文档里的能力宣称无法被观察面验证。

## 变更内容

1. **采集实现**：新增文件内 `apple_global_default_route()`——`SCDynamicStoreCreate` 建会话，依次查 `State:/Network/Global/IPv4` 与 `.../IPv6`，任一键存在即返回 `true`；两者都缺失返回 `false`；会话创建失败或查询返回不可判结果时返回 `nullopt`。会话在函数内释放，不跨帧持有。
2. **源标识**：`compiled_source()` 在 `AURORA_PLATFORM_MACOS` 下返回 `ConnectivitySource::DynamicStore`；其余平台分支不变。
3. **分派**：`probe_default_route()` 在 macOS 上走 `apple_global_default_route()`；`has_evidence_path()` 仅 WASM 为假，macOS 因已有采集腿保持为真。
4. **链接**：`cmake/AuroraBackends.cmake` 的 macOS 框架列表追加 `SystemConfiguration`，注释说明只服务链路态采集腿。
5. **测试口径**：`utest_net_connectivity` 中三处「macOS 腿未落地」的过期表述改写为「无采集腿的构建（wasm / BSD / Android 一类）」与 `DynamicStore` 已列名，断言本身不变（脚本化钩子驱动，不触真机网络栈）。
6. **文档回写**：§8.8.41 的 macOS 段写明 `SCDynamicStore` 全局键口径与三态处置；§1707 附近「Apple 腿未落地」的旧句改写。

## 验收判据

1. macOS AppKit 构建下 `compiled_source()` 为 `DynamicStore`，`probe_default_route()` 有真实读数路径；非 macOS 构建分支逐字节不变。
2. 动态存储不可用时返回 `Unknown`（非离线、非在线）；键缺失时为离线；键存在时为在线。
3. `has_evidence_path()` 在 macOS 上为真、WASM 上为假，其余平台取值不变。
4. `utest_net_connectivity` 全绿，脚本化钩子覆盖三态；无网络环境不产生红灯。
5. 真机探针 `aurora_verify_net_connectivity --expect auto` 在 macOS 上给出 `source=dynamic_store` 且结果与系统实际联网状态一致。
6. `check_platform_macros` / `format-check` / 改动 TU 的 `lint` 绿；全量 `ctest` 无新增红灯。

## 回写落点

- `codespec/specification/06-app-platform.md` §8.8.41（macOS 采集腿与三态处置、旧「未落地」句改写）。
- `codespec/SPECIFICATIONS.md` 的 `SPEC.FEAT.NET.CONNECTIVITY.001` 行（源列表含 macOS `SystemConfiguration` 动态存储）。
- `codespec/ARCHITECTURE.md` 的 connectivity 头文件元数据行。
- CHANGELOG.md 的 [Unreleased] Added 记一条。

## 实施记录与偏离（落码期回写）

1. 采集腿按提案落地：`apple_global_default_route()` 建 `SCDynamicStore` 会话，查 `State:/Network/Global/IPv4` 与 `.../IPv6`，任一存在为在线、皆缺为离线、会话或读数不可用回 `nullopt`；`compiled_source()` 在 macOS 报 `DynamicStore`，`has_evidence_path()` 仍只在 WASM 为假。
2. macOS 框架列表补 `SystemConfiguration`（原本已随 TLS 拓扑链入，此处显式登记避免依赖链接顺序）。
3. 真机取证：`aurora_verify_net_connectivity --expect auto` 在本机 macOS 上 `result: PASS`，首拍 `source=dynamic_store`、订阅首拍 `state=online`，三态与源标识均被真实读数驱动。
4. 偏离：无。`utest_net_connectivity` 的脚本化用例不触真机网络栈，`connectivity_facade_without_source_contract` 在有采集腿的构建上按预期 skip。
5. 门禁：macOS 平台定向 clang-tidy 与 clang-format 对改动 TU 复扫 0 命中；全量 `ctest` 450/450。
