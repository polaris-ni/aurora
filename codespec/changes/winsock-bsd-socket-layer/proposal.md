# CHG-006 统一 socket 操作层（aurora::net 公共模块）与五处裸 socket 调用收敛

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-006 |
| 提出日期 | 2026-10-12 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.ZERO-IFDEF.001 |
| 影响面 | 新增公共模块 `net/`（`socket.h` + `src/aurora/net/socket.cpp`，伞头接入、`aurora_api.json` 刷新、`errors.toml` 扩 `net-*` 码）；核心库 Windows 链接面无条件引入 `ws2_32`；替换 `inspector_server.cpp`、`tools/servers/inspector_client.h`、`utest_inspector_server.cpp`、`utest_inspector_client.cpp`、`itest_inspector_robustness.cpp` 五处裸 socket 实现并删除其平台适配块；不改 Inspector REST 协议语义、不改 X11/Wayland 事件循环 fd 通道 |

## 动机

全仓 socket 使用面存在 **4 份互相独立、各自漂移的 Winsock/POSIX 适配实现**（外加核心内 1 份词汇 shim），跨平台行为靠逐处复制维持，已出现真实口径分叉风险：

1. `src/aurora/inspector/inspector_server.cpp`：在 POSIX 侧反向伪装 Winsock 词汇（`SOCKET`/`INVALID_SOCKET`/`WSAStartup`/`WSAGetLastError`/`closesocket`/`MAKEWORD` 等 7+ 名字），并为此在 `check_naming_conventions`/lint 上挂了本仓唯一的「词汇绑定型」豁免（见 `widened-lint-coverage` 提案）。主体逻辑靠「假装在写 Winsock」实现跨平台，语义倒挂。
2. `tools/servers/inspector_client.h`：第二份适配（`detail::SocketHandle`/`close_socket`），且就地记录了 `SO_RCVTIMEO` 两平台**不同形**（Winsock 收 DWORD 毫秒、POSIX 收 `struct timeval`；误传即 5 秒变 5 毫秒）的踩坑注释——该知识只活在这一个文件里。
3. `tests/unit/utest_inspector_server.cpp`、`tests/unit/utest_inspector_client.cpp`、`tests/integration/itest_inspector_robustness.cpp`：各自再写 `WinsockSession`、`closesocket` shim、`select` nfds 差异（Windows 忽略、POSIX 须 `fd+1`）、`send`/`recv` 返回值窄化承接等第三～五份适配副本。
4. 链接面同样分散：`ws2_32` 在 `AuroraTools.cmake`（mcp / e2e / inspector_server）与 `AuroraTests.cmake`（runner 直挂）四处手工挂载，注释里明文解释「借不了传导」的原因正是核心库不碰 socket。

本库定位 AI-first、跨平台一致行为（SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 / ZERO-IFDEF.001）：socket 这类「读一次 Winsock 文档才知道两平台哪里不同形」的操作不应让每个消费点自行踩坑。封装为公共模块（而非内部层）经用户裁示——Inspector 只是当前消费者，AI 驱动的网络能力（后续 MCP/远程交互）以 `aurora::net` 为底座，受 schema/预算/文档门禁统一约束。

## 变更内容

1. **新增公共模块 `net/`**：`socket.h`（命名空间 `aurora::net`，**不暴露任何平台 socket 头**：句柄以 `std::intptr_t` 承载、`-1` 恒为无效值，字节序/`sockaddr`/`socklen_t` 全部封闭在实现侧）+ `src/aurora/net/socket.cpp`（全仓唯一的 Winsock/POSIX 适配落点）。伞头 `aurora/aurora.h` 与 `umbrella_manifest.txt` 同步收录。
2. **API 面（三个类型，失败一律 `Result<T>` + 表驱动 `Error`）**：
   - `Session`：进程级 socket 运行时 RAII（引用计数；Windows 腿 `WSAStartup/WSACleanup`，POSIX 腿零成本占位），`ok()` 表初始化成败。取代各文件自写的 `WinsockSession`/逐点 `WSAStartup` 调用。
   - `Listener`：`listen_loopback(port, backlog) -> Result<Listener>`（port=0 即系统临时端口，`port()` 回报实际值；只绑 `127.0.0.1`）、`wait_readable(timeout) -> Result<bool>`（select 可中断等待，两平台 nfds 差异内部消化）、`accept() -> Result<StreamSocket>`、`close()`（先 `shutdown(SD_BOTH)` 再关，保证唤醒对端线程的等待，语义同现 server stop）。
   - `StreamSocket`：`connect_loopback(port) -> Result<StreamSocket>`（不经 DNS、恒连回环，沿用 client 信任模型）、`set_io_timeout(timeout) -> Result<void>`（毫秒统一口径：Windows 侧转 DWORD、POSIX 侧转 `timeval`，杜绝「5 秒变 5 毫秒」形态错配）、`read_some(span<char>) -> Result<size_t>`（返回 0 = 对端关闭）、`write_all(span<const char>) -> Result<void>`（内部 ≤1MiB 分块循环写满，两平台返回值窄化在实现侧一次解决）、`is_loopback_peer()`、`close()`。
3. **错误码**：`codespec/errors.toml` 新增 `net-*` 组（runtime/open/bind/listen/accept/connect/timeout-option/wait/send/recv 十枚，category=io），`Error.message` 携带 OS 原生码（`WSAGetLastError`/`errno` 数值）；跑 `gen_error_codes` 刷新 `error_codes.gen.h`/`ERROR_CATALOG.md`/`aurora_api.json`。
4. **链接收敛**：顶层 `CMakeLists.txt` 对 `aurora` 目标 Windows 无条件 `PUBLIC ws2_32`（socket 成为核心自带能力）；删除 `AuroraTools.cmake`（mcp / e2e_client / inspector_server 三处）与 `AuroraTests.cmake`（runner）的散挂 `ws2_32`，`shcore`/`oleacc` 等无关库保留原样。Emscripten 走 POSIX 腿编译，无需目标级排除。
5. **替换五处调用方**：`inspector_server.cpp` 删除 POSIX Winsock 词汇 shim（连带其命名豁免，豁免清单同步注记失效）并改用 `net::Session/Listener/StreamSocket`；`inspector_client.h` 的 `http_request` 函数签名与响应语义逐值不变、内部改经 `aurora::net`（该头从「自带平台适配」退化为纯 HTTP 组帧/解析）；三个测试文件删除自写 `WinsockSession`/shim/select 差异块，测试客户端与回环桩改走 `net` 类型。
6. **不变量**：Inspector HTTP 协议（路由、`Connection: close`、4MiB 上限、localhost 拒连语义）、各测试断言口径零变化——本次只搬运输层，不改协议面。X11/Wayland 的 `poll` fd 通道不纳入（纯 Linux 路径、无 Winsock 对应面，经用户确认排除）。

## 验收判据

1. 新增 `tests/unit/utest_net_socket.cpp`（无 GUI、进程内回环）覆盖：`listen_loopback(0)` 实端口回报、`wait_readable` 超时返回 false 且不报错、`connect`→`accept` 配对成功、`write_all`/`read_some` 数据逐字节往返、对端关闭后 `read_some` 返回 0、无监听者时 `connect_loopback` 失败且 `code` 为 `net-connect-failed`、`is_loopback_peer()` 为真、`set_io_timeout` 生效（慢对端下 `read_some` 报超时而非挂死）、`Session` 嵌套构造不重复初始化且不提前清理。
2. 全仓 grep 判据：平台 socket 头（`winsock2.h` / sys/socket.h）的 `#include` 仅出现在 `src/aurora/net/socket.cpp`（生成器宿主与平台无关文件除外）；五处调用方文件中 `socket|bind|listen|accept|connect|send|recv|select|WSA|closesocket` 零裸调用。
3. 既有 Inspector 回归（`utest_inspector_server`、`utest_inspector_client`、`itest_inspector_robustness`、相关 demo）全绿，REST 行为逐值不变。
4. `cmake --build build --target aurora_api_json` + `gen_error_codes` 刷新后，`check_api_schema_sync`、`check_naming_conventions`、`check_api_budget`（75k token 预算内）、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`lint`、`format-check` 全绿；`ctest --preset ninja-test` 全量绿。
5. 文档回写完成：`ARCHITECTURE.md` §4 模块映射新增 `net` 行、`06-app-platform.md` 增补 socket 层契约节、`08-tooling.md` 传输实现口径更新、`BUILD_OPTIONS.md` 链接面改写、`CHANGELOG.md` Added 记新增公共模块（MINOR 语义）。

## 回写落点

- `codespec/ARCHITECTURE.md`（§4 模块映射：`net/` 头/源文件行）
- `codespec/specification/06-app-platform.md`（跨平台适配层新增 socket 层契约：句柄屏蔽形态、毫秒超时统一、`Session` 计数语义）
- `codespec/specification/08-tooling.md`（InspectorServer / inspector_client 传输层改述为 `aurora::net` 消费者）
- `codespec/BUILD_OPTIONS.md`（Windows `ws2_32` 链接口径：核心无条件 PUBLIC，散挂删除）
- `codespec/ERROR_CATALOG.md`（生成物：`net-*` 错误码）
- `codespec/changes/winsock-bsd-socket-layer/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）
