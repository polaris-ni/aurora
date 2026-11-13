# CHG-066 HTTP 协程等待体的完成决议串行化

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-066 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.NET.HTTP-CLIENT.001 |
| 影响面 | `src/aurora/net/detail/http_await.h`（`CallbackAwaiter` 的完成决议）、`tests/unit/utest_net_http_await.cpp`（新增并发决议判据）、`tests/unit/utest_net_http_client.cpp` 与 `tests/unit/utest_net_http_sse.cpp`（TSan 定点复现用例不改）；**不改任何发起器、不改 `Engine::post_after` 与取消令牌语义、不改公共 API 签名**；公共 API 结构无差异 |

## 动机

1. **TSan 已给出稳定可复现的实证报告。** 第二批并发普查在 `await_suspend` 的完成回调里读到 `http_await.h` 旧第 100 行的 `armed_` 读取与旧第 105 行的写入分属引擎线程与主线程、无 happens-before；同一批还命中了 `result_` 的 `std::optional::emplace` 与主线程读取。触发者是现成用例 `sse_subscription_adopts_the_retry_window_the_peer_wrote` 一类，不是构造出来的理论路径；抑制第三方噪声后仍令用例红灯。

2. **实现违反了自身写下的线程契约。** `http_await.h` 文件头把「续体经 `post_to_main` 恢复」写成硬约束，类注释又标 `Thread: single-threaded（每个 awaiter 随一条协程帧串行使用，无共享状态）`。但 `Engine::post_after` 的到期任务按 `include/aurora/net/engine.h` 的契约在**引擎线程**执行，`await_sleep` 与 `await_read_within` 的定时器回调就地调 `deliver` / `fire`，于是完成回调可以在引擎线程直接写 `result_` 并读 `armed_`。引擎的「投递恒回主线程」只管就绪投递，管不到这两条定时器路过来的完成。

3. **后果不止形式 UB，还有双重恢复。** 旧实现用一枚普通 `bool armed_` 在「完成回调」与「`await_suspend` 返回前的发布」之间裁决谁恢复协程：回调写 `result_` 后读 `armed_`，`await_suspend` 写 `armed_` 后读 `result_.has_value()`。无同步时两侧可能各自看到对方尚未发布的旧值，出现「回调判定异步完成而投了一次恢复、`await_suspend` 又判定同步完成而就地前进」的交错——同一协程帧被恢复两次。文件头自陈的 heap-use-after-free 正是这一族；把 `armed_` 简单改成原子只能消除数据竞争，消不掉这个双重恢复窗口。

4. **规格里已有同族先例，不必另立一套并发模型。** `codespec/specification/06-app-platform.md` §8.8 的编排纪律写明「每连接一枚互斥量（引擎定时器与跨线程 `cancel()` 都是真实入口，无锁即无正确性）；锁内只收集待触发的回调、一律在锁外触发」。等待体缺的就是这一层：把「写结果 + 读闸」与「写闸 + 读结果」各自做成临界区，两次决议即成全序，恰好一次恢复。

5. **本提案的边界。** 不改 `await_sleep` / `await_read_within` / `await_coro` 等发起器的「恰好一次」原子闸，不改 `Engine` 定时器丢弃语义，不改取消令牌把失败折成 `net-cancelled` 的解释权，也不把「同步完成就地前进」改成一律投递——文件头已经用 asan 实测说明了后者的危害。`post_to_main` 的打烊取消语义保持原样。

## 变更内容

1. 把 `CallbackAwaiter` 的 `result_` 与 `armed_` 收进一枚 `shared_ptr` 承载的共享决议态，态内含一把互斥量。用共享态而非直接加成员，是为了保住类注释写明的可移动性：`std::mutex` 作成员会让 `= default` 的移动构造被隐式删除，而发起侧把明文 / TLS 两条流的读写经 `std::function` 统一时要移动等待体。
2. 完成回调改为捕获共享态副本与 `std::coroutine_handle` 的值，不再捕获 `this`；在锁内落 `result_` 并读 `armed_`，只在锁外、且确实判定为异步完成时调一次 `post_to_main`。`handle_` 成员随之去掉，消除「成员写入与另一线程读取」的最后一处共享。
3. `await_suspend` 在发起器返回后于同一把锁内发布 `armed_` 并读 `result_.has_value()`；`await_resume` 也在锁内移出结果，使结果落地与取用之间恒有 happens-before，不再只依赖主队列自身的同步。两次决议在全序下只可能出现「回调先入临界区 ⇒ 就地前进」或「发起器先入临界区 ⇒ 投递恢复」之一。
4. 文件头与类注释改写：把「单线程、无共享状态」换成「完成回调可能在引擎线程抵达，决议态是唯一的跨线程共享点，续体恢复仍恒回主线程」；保留同步完成不得投递的长篇说明，并补一句互斥量保证的恰好一次裁决。
5. 在既有 `tests/unit/utest_net_http_await.cpp` 补两组判据，复用该文件现成的排队式值班租约：① 自定义发起器把完成回调交给一条工作线程，断言协程恢复恰好一次、结果原样交回、投递至多一次；② 反复让工作线程的交付与「发起器已返回」的发布在跨线程下交错，每轮断言恢复次数恒为 1。既有用例群继续充当同步完成对照组（`drain()` 恒为 0）。
6. `CHANGELOG.md` 的 `### Fixed` 记一条等待体完成决议收口；规格回写见下节。

## 验收判据

1. TSan 定点实证：以 `ignore_noninstrumented_modules=1` 在插桩构建下重跑 `utest_net_http_sse` 与 `utest_net_http_client`，要求零 `WARNING: ThreadSanitizer: data race` 且 ctest 退出码为 0；修复前同一批报告在这两个用例上稳定红灯，构成独立基准。
2. 变异自证：把决议态的互斥量退回普通成员读写（等价于旧实现）后，上述 TSan 用例必须重新出现报告并令退出码非零；还原后零报告。绿不算证据，转红才算。
3. 并发决议单测在足够多轮交错下「恰好一次恢复、结果不丢」恒成立，且同步完成对照组恒不投递；把决议态的锁退回普通成员读写，断言必须转红。
4. 既有网络用例零改动全绿：退避重试、`Expect: 100-continue` 窗口读、SSE 续传与取消、流式与物化交付路径的完成时序逐位不变。
5. 公共面零签名变更：`aurora_api.json` 的公共 API 结构无差异；改动 TU 的增量 lint、format-check、相关单测与 `check_*` 门禁全绿。

## 回写落点

- `codespec/specification/06-app-platform.md`：§8.8 的协程桥接条目补写「完成回调可能在引擎线程抵达，结果落地与闸发布由同一决议态串行化，恰好一次恢复；续体仍恒回主线程」，并把这条与既有「每连接一枚互斥量」的编排纪律对齐。
- `codespec/SPECIFICATIONS.md`：在 HTTP 客户端特性行的说明里补一句「协程等待体的完成决议跨线程串行、恰好一次恢复」，使特性表与规格正文口径一致。
