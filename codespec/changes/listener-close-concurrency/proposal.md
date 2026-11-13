# CHG-067 监听口关闭唤醒的并发契约收口

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-067 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.TRANSPORT-SUITE.001 |
| 影响面 | `include/aurora/net/blocking.h`（`Listener` 的句柄与端口成员及线程契约）、`src/aurora/net/socket.cpp`（移动、关闭与两处读侧）、`tests/unit/utest_net_socket.cpp`（并发关闭与读侧判据）；**不改 `Listener` 任何成员函数签名、不改 `StreamSocket`、不改 `InspectorServer` 与 `ScriptedPeer` 的停机顺序**；公共 API 结构无差异 |

## 动机

1. **TSan 已确认这是本库代码的缺陷，不是第三方噪声。** 第二批并发普查在 `include/aurora/net/blocking.h` 所声明的 `Listener` 上记下 104 条报告，落点是 `close()` 写句柄与 `wait_readable()` / `accept()` 读句柄；抑制第三方噪声后 `utest_net_http_client` 等用例仍红灯。对侧栈来自 `tests/framework/scripted_peer.h` 的脚本线程，即真实并发停机。

2. **公共头把数据竞争写成了设计用途。** `Listener` 的类注释写「不 thread-safe（单所有权句柄）；`close()` 允许与另一线程的 `wait_readable` / `accept` 竞态唤醒（设计用途：宿主以 close 中断 accept 线程）」，实现侧 `src/aurora/net/socket.cpp` 的 `close()` 注释又自陈「先 shutdown 再 close：唤醒另一线程可能阻塞其中的 `wait_readable` / `accept`（InspectorServer 停机协议）」。明知另一侧有线程在读 `handle_`，却用普通 `std::intptr_t` 成员、无锁无原子——「允许竞态」在有内存模型的 C++ 里不是可声明的设计，是未定义行为。

3. **后果不止形式 UB。** 句柄被另一线程关闭并置无效后可能被内核复用；读侧若在此之后才取 `handle_`，`select` / `accept` 会落到一个已不属于该监听口的描述符上。`close()` 现有写法还在 `is_valid()` 判定之后分三次读同一个普通成员（shutdown、native_close、置无效），两个线程同时进入时既不是幂等也可能重复关闭同一个数值。

4. **停机协议本身应保留。** `src/aurora/inspector/inspector_server.cpp` 的 `accept_loop` 以 200ms 可中断 `select` 轮询停止位，`stop()` 依赖 `close()` 的 `shutdown(both)` 立刻打断阻塞中的等待与收发；`tests/framework/scripted_peer.h` 的夹具同理。若按「先 join 工作线程再 close」改造，停机最坏要等满一个等待窗口，且卡在连接处理里的线程不再有唤醒手段，属功能退化。因此本提案不推翻这条产品裁决，而是把它兑现成有定义的并发语义。

5. **本提案的边界。** 只处理 `Listener` 自身被并发读写的两个标量成员；`StreamSocket` 是 move-only 单所有权句柄、不参与这条停机协议，不改。`native_handle()` 借出的仍是瞬时快照，其「调用方不得自行关闭、仅在未 `close()` 期间有效」的既有契约不变；不新增错误码、不新增公共枚举、不改 `wait_readable` 的「纯超时返回 false、失败返回 `net-wait-failed`」口径。

## 变更内容

1. `Listener` 的 `handle_` 与 `port_` 改为 `std::atomic`，公共头补 `<atomic>`；`is_valid()` / `port()` / `native_handle()` 改为一次 `load()`，类注释把「允许竞态唤醒」改写为「`close()` 可与另一线程的 `wait_readable` / `accept` 并发：句柄按原子交换裁决，读侧取一次快照，停机唤醒按既有协议生效」。
2. `close()` 用一次 `exchange` 取出待关闭的句柄并立即发布无效值：取到无效值即返回，保证任意数量的并发调用中恰有一个执行 `shutdown(both)` 与 `native_close()`；`shutdown` 与 `native_close` 全程使用这次取出的局部快照，不再重读成员；`port_` 随之原子归零。
3. `wait_readable()` 与 `accept()` 各自在入口 `load()` 一次句柄快照，整段系统调用只用该快照；读到无效快照时直接按既有失败口径回报——`FD_SET` 不接受 -1，而停机路径上另一线程读到的正是这个值，此前会构成越界写。超时与错误语义逐位不变。
4. 移动构造与移动赋值改用 `exchange` 从源对象摘出句柄与端口，保持「源对象转无效态」的既有语义；`listen_loopback` 与 `bind` 的成功路径改用 `store()` 落值。
5. `tests/unit/utest_net_socket.cpp` 补三组判据：① 两个工作线程与用例线程并发 `close()` 之后监听器恒为无效态、端口恒为 0、原生句柄恒为 -1，且后续新建的回环监听口仍能完成一次真实接受（证明并发关闭没有误伤系统后续分配的句柄）；② 关闭后 `wait_readable()` / `accept()` 按既有失败口径回报而非落到野句柄；③ 一条工作线程阻塞在 `wait_readable()` 时由另一线程 `close()` 唤醒，墙钟上界把它与「等满自己的窗口」确定性区分开。
6. `CHANGELOG.md` 的 `### Fixed` 记一条监听口关闭唤醒并发收口；规格回写见下节。

## 验收判据

1. TSan 定点实证：以 `ignore_noninstrumented_modules=1` 在插桩构建下重跑 `utest_net_http_client` 与 `utest_net_http_sse`，要求零 `WARNING: ThreadSanitizer: data race` 且 ctest 退出码为 0；修复前同一批报告在 `Listener::close()` 落点稳定命中，构成独立基准。
2. 变异自证：把 `handle_` / `port_` 退回普通标量、`close()` 退回「先判有效再分次读写」后，上述 TSan 用例必须重新出现报告并令退出码非零；还原后零报告。
3. 并发关闭单测中结果态恒定：无效、端口为 0、原生句柄为 -1，且新建监听口仍可用；把 `exchange` 换成「先 `load()` 再 `store()`」的非原子两步，断言必须转红。存活态对照组全部通过。
4. 既有 `tests/unit/utest_net_socket.cpp` 全部用例零改动全绿：绑定、回环连接、移动后源对象无效、纯超时返回 false 等语义逐位不变；`InspectorServer` 与脚本化对端的停机路径不改一行仍可用。
5. 公共面零签名变更：`Listener` 全部成员函数签名不变、`aurora_api.json` 的公共 API 结构无差异；改动 TU 的增量 lint、format-check、相关单测与 `check_*` 门禁全绿。

## 回写落点

- `codespec/specification/06-app-platform.md`：§8.8 的 `blocking::Listener` 条目把「`close()` 唤醒另一线程」补写为「句柄经原子交换裁决，读侧取一次快照，并发停机不构成未定义行为」，并明确 `native_handle()` 借出的仍是未关闭期间的瞬时值。
- `codespec/SPECIFICATIONS.md`：在传输族特性行的说明里补一句「监听口关闭唤醒为有定义的并发操作」，使特性表与规格正文口径一致。
