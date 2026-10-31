# CHG-053 main_poster 空槽两义与四文件单槽抢占：收敛为进程级唯一队列与单一值班租约

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-053 |
| 提出日期 | 2026-10-29 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.CORE.ASYNC-CONCURRENCY.001 |
| 影响面 | 新增 `include/aurora/core/main_queue.h`；`include/aurora/state/async.h` 删除进程级槽与公共安装口 `Task<T>::set_main_poster`、新增 `blocking_wait`；`include/aurora/state/coroutine.h` resume 世代化；net 发起面（`include/aurora/net/engine.h`、`include/aurora/net/connectivity.h`、`include/aurora/net/http_binding.h`、`include/aurora/net/transport.h`、`include/aurora/net/datagram.h`、`src/aurora/net/resolver.cpp` 及 `src/aurora/net/detail/http_await.h`、`src/aurora/net/detail/http_fetch.h`）增发起时校验；`include/aurora/window/window.h` 删除 `Window::run` 自循环；`include/aurora/app/application.h` 与 `src/aurora/app/application.cpp` 的 `loop_begin/loop_end/run_modal` 装卸改值班租约；边界读点 `src/aurora/inspector/inspector_server.cpp`、`src/aurora/window/detail/win32_ua.cpp` 收敛为 `try_post`；`TestController` 补排水；测试 `utest_window.cpp` 与 async 三件套假 poster 退场。属破坏性公共 API 变更，按 `SPECIFICATIONS.md` §12 记入 CHANGELOG 并给迁移路径；被删符号未见于 `aurora_api.json`（验收时复核，若有条目须刷 schema）。**订正（实施期发现）**：`Window::run` 的直接调用者非一处而是六处——`examples/demos/demo_common.h:291`、`demo_gpu.cpp:83`、`demo_glfw_surface.cpp:84`、`demo_title_bar.cpp:80`、`tests/integration/itest_nav_win.cpp:77`、`tests/unit/utest_window.cpp:388`（初稿漏扫 examples）；裁定为严格删除，六处全部迁移到 `Application` 驱动 |

## 动机

1. **空槽两义是机制级暧昧，注释已在三处各自裁定**。`async.h:55-66` 的 `post_to_main` 在槽为空时无条件就地执行：同一个空槽同时表达「headless/测试合法」与「循环已打烊」。`window.h:952`、`win32_ua.h:178`、`inspector_server.cpp:519-521` 三处注释各写各的回退口径，正是两义需要打补丁的症状。
2. **静默跨线程写 State 由此产生**。打烊后（`application.h:794` 置空、或 `Window::run` 还槽后）worker 上的迟到结果命中空槽 ⇒ 在 worker 线程就地执行 ⇒ 跨线程写 State，无任何告警。`src/aurora/net/detail/http_await.h:17` 已因此踩坑，靠局部标志绕行而未修根。
3. **单槽抢占：一枚槽同时承载「投进谁家队列」与「叫谁起床」**。复杂度来自能力集允许「Window 独立自跑 + Application 嵌套」并存：`loop_end` 不比对安装者即置空（可打死他人循环）、`run_modal` 需要 prev/restore 快照（`application.cpp:596-631`）、`Window::run` 自带 RunQueue 与还槽逻辑（`window.h:922-986`）、`loop_end` 后无人装槽时 inspector/win32_ua 又各自复刻「锁内读槽+空则就地」（`inspector_server.cpp:524-528`、`win32_ua.cpp:1747-1756`）。
4. **strict 在本应拦截的路径上失效**。`strict_mode` 是 thread_local（`strict_mode.h:24`），worker 线程恒读到 Off，「strict 下把静默回退升级为失败」在缺陷 2 的现场（worker 线程）落不了地——拦截必须不依赖线程局部的开关。

## 变更内容

1. **新增 `include/aurora/core/main_queue.h`**：全静态纯门面 `MainQueue`（与 `thread_pool.h`、`strict_mode.h` 同层；`core` 不认识 `Application`，分层不反转）。命名取「队列」而非已随旧机制消亡的「poster」：类型名指向本设计唯一结构性保证的事实——全进程恒存在、只此一家；投递拒绝本就是队列类型的常规语义（对照 Java `ThreadPoolExecutor` 队列+准入+`RejectedExecutionException`、Android `MessageQueue` quit 后拒收、Chromium `PostTask` 对死亡 runner 返回 false）。内部单一 Meyers struct：锁、队列、值班位、epoch 计数、wake 同址**同锁**。
   - **两相失败形态**（本设计的中枢，取代单点硬失败）：
      - **发起时**无值班，按时间相二分——
        - 非打烊窗口 ⇒ 纯编程错误 ⇒ `on_strict_failure`（FATAL 日志 + 默认 `std::terminate`，与 strict 开关无关、不依赖 NDEBUG，同函数文档 `strict_mode.h:51-53`）——**不新增注入面**，测试经既有 `set_strict_failure_handler`（`strict_mode.h:45-49`）注入捕获 ⇒ 栈即元凶（调用 `async().then()` 的那一行），归因准确、fail-fast。
        - **打烊窗口**（`closing` 位，见时序纪律）⇒ 合法竞态：收尾排水中的续体链式再发起异步（长轮询/重试链接力形态），正常关窗不得被打成崩溃 ⇒ **出生即取消**：`async()` 不进线程池，立即返回 `cancelled`+`delivered` 已置位的 Task——`then` 恒不执行（复用既有 `take_for_delivery` 双标志短路，零新增状态）、`is_cancelled()==true` 诚实、`on_cancel` 观察者登记时同步触发（复用「登记时已取消即触发」既有契约 `async.h:329-351`）、记 WARN 级打烊取消计数（与落地相同族同计数）。
     - **落地时**无值班或世代失配 ⇒ 合法生命周期事实（「打烊取消」）⇒ 不投递、不 terminate，记 ERROR 级诊断与计数（可观测，非静默）：`post` 返回 void、`try_post` 返回 false。若在落地时 terminate，「HTTP 在途时用户关窗」会偶发打死退出路径——不可接受。
   - `post(task)` / `post(task, expected_epoch)`：锁内判值班（带 epoch 形另比世代）后入队或按打烊取消处理，锁外调 wake。值班判定与入队同锁 ⇒ 无孤儿项。**返回 void 系刻意设计**：落地相调用方（投递代码）对失败无任何可行动作，可忽略的 bool/Result 返回值只会催生恒被忽略的分支或「那就就地执行」的新静默回退；需要失败知识的一方走 `try_post`（`[[nodiscard]]`），「想知道吗」的分岔编进函数名而非留给调用纪律。
   - `try_post(task)` / `try_post(task, expected_epoch) -> bool`：契约边界在外读点（UIA/Inspector）与协程 resume 的无失败形。
   - `take_batch() -> std::vector<std::function<void()>>`：主线程捞批次，不要求值班在手（`loop_end` 收尾捞用）。
   - `current_epoch() -> std::uint64_t`：值班世代的**全函数**，取值钉死如下——
     - **世代号**由内部单调计数器在每次 `on_duty` 成功时 +1 分配，自 1 起、**永不重置/复用/回退**（`Lease` 内持有的 epoch 即此号，一枚计数器两用：防冒卸 + 投递比对；uint64 回绕需 2^64 次重入，不可达）。
     - **在岗**时返回当前在岗世代号（≥1）；**不在岗**时（从未上岗、打烊后、重入 run 的 release→重领之间）恒返回 **0**。
     - 于是钉死不变量 `has_on_duty() ⟺ current_epoch() != 0`——`has_on_duty()` 降为此式的可读别名，非独立事实，杜绝两处判据漂移。
      - `expected_epoch` 定义域：**0 = 代际无关**（发起时不捕世代的 net/协程站点传此哨兵）；**≥1 = 发起时捕获的世代**。发起本要求在岗（无值班的非打烊发起 = 编程错误已硬失败；打烊发起 = 出生即取消、根本不产生投递），故被追踪任务的 `expected_epoch` 恒 ≥1，与"打烊返回 0"永不混淆。
     - **接受判据**（`post`/`try_post` 锁内、与入队原子）：`current_epoch() != 0 && (expected_epoch == 0 || expected_epoch == current_epoch())`。推论三条，各钉一处竞态：①打烊 ⇒ `current_epoch()==0` ⇒ 一切投递（含代际无关的 net/协程）拒绝，杜绝"打烊窗口恰好等于某任务世代"的误放行；②重入 run 的 `loop_end` release、下次 `loop_begin` 重新 `on_duty` 领到**更大的新世代** ⇒ 上一世的在途任务 `expected_epoch < current_epoch` ⇒ 拒绝（这正是要防的"重入 run 的世代与上一世相同而错误放行"，靠单调 +1 消灭）；③同世在途任务 ⇒ 相等 ⇒ 放行，正常路径。
     - 投递闭包携带发起时捕获的本世 epoch（async 侧读 `current_epoch()`；net/协程用默认形 0）。不暴露 `Lease *`（epoch 整数判等对跨层足够，且具值语义、无解引用与时序风险）。
   - `is_closing() -> bool`：**仅区分 `current_epoch()==0` 的两种内蕴**——`true` = `loop_end` 正在 `closing_scope()` 守卫内收尾（打烊在途），`false` = 从未上岗或早已收尾完毕。不变量：`is_closing() ⇒ current_epoch()==0`（收尾期无在岗），反之不成立。**发起分岔据此三分**：`current_epoch()!=0` ⇒ 正常发起（捕本世 epoch）；`is_closing()` ⇒ 出生即取消（不投、不 terminate）；否则 ⇒ 编程错误硬失败。投递侧不受 `closing` 影响——收尾窗口 `post` 因 `current_epoch()==0` 本就拒绝，与 `expected` 无关。
   - `on_duty(wake) -> std::optional<Lease>`：独占上岗，**此刻**已被占返回 `nullopt`；`Lease` 为嵌套 RAII 值对象（**epoch 比对即上文世代号，一枚计数器兼防冒卸与投递比对两用**、**只有持有人能卸任**、幂等；析构兜底防循环抛出泄漏值班位）。**租约归还后槽位可再次领取**：WASM 的 `run()` 在 `loop_begin` 后即把帧环移交浏览器 rAF（`application.h:462,469-470`），末帧 `raf_tick` 调 `loop_end`（`application.h:819`），JS 侧随后再次 `run()` 会重新 `loop_begin`；native 循环重入同理——CAS 只拒并发，不拒复用。`wake` 在投递者线程被调，仅负责打断值班者的 `wait_events` 睡眠（Win32 `WM_NULL` / X11·Wayland self-pipe / GLFW `glfwPostEmptyEvent` / macOS 合成 `NSEvent` / headless 空操作）；不决定任务归属。
   - **持牌者身份是不变量而非白名单**：任何自排空循环的主体合法领牌，唯一性由 CAS 结构性强制。合法持有者的穷举是能力集塌缩的结果，不是许可清单——`Application::loop_begin`、`TestController`、领空 wake 租约自行 `take_batch` 排空（或经 `blocking_wait` 同步等）的 CLI 宿主。
   - 时序纪律：`loop_end` = 先 `Lease::release()`（epoch→0）、再以**作用域守卫** `MainQueue::closing_scope()`（RAII，析构即清 `closing`，异常安全）罩住 `take_batch()` 收尾执行、守卫出作用域自动清 `closing`。release 前入队项必被收尾捞兜住；收尾中续体若链式发起 `async()` 命中「出生即取消」；守卫一析构，其后任何发起回到「编程错误硬失败」。**`closing` 必须有界、绝不延到下次 `on_duty` 才清**——否则「循环早已结束后再发起异步」这一真实编程错误会被弱化成静默取消，丢掉 fail-fast。
   - **`closing` 的置/清是 `loop_end` 私有机制，不暴露公共 setter**：内部由作用域守卫 `MainQueue::closing_scope()`（返回嵌套 RAII 值对象，构造置位、析构清位）实现，仅 `loop_end` 收尾时取用。`is_closing()` 供发起侧读；守卫本身不上公共面（外部无法、也不该自行进入打烊态）。
 2. **`async.h`**：删槽与安装口（`39-51`、`385-389`），`detail::post_to_main` 退化为 `MainQueue::post` 的内联转发；每次 post 的 `std::function` 锁内拷贝（`async.h:59`）随槽一并消失。**发起时检查**（三分，见变更 1 `is_closing`）：`async()` 与 `Task<T>::then()` 发起时——`current_epoch()!=0` ⇒ 正常、捕获本世 epoch；`is_closing()` ⇒ 返回出生即取消的 Task、不排队；否则（不在岗且非收尾窗口，即从未上岗/已收尾完毕）⇒ `on_strict_failure`。循环内发起则值班必在，故该检查仅在违规现场触发；投递闭包携带发起时捕获的 epoch。投递点保持既有 cancelled 短路（`take_for_delivery` / `expire_timeout` / 帧尾 sweep 先查 `is_cancelled`/`delivered`），且 post 闭包不得引入与 `state->mutex` 的新锁序。类文档 `259-260` 删「未装投递器时在承接 worker 线程同步执行」，改为「发起需循环（值班者）在场；打烊窗口内发起 = 出生即取消；落地晚于打烊 = 取消（计数诊断），均不静默、不 terminate」。
3. **新增 `Task<T>::blocking_wait() -> Result<T>`**：future 形同步等待**结果本身**（`AsyncState` 就绪条件），不经投递通道、与值班/epoch 无关——**打烊取消的是投递、不是结果**（worker 置 `AsyncState` 结果与值班无关），故持有句柄的等待者即便在打烊后仍照常取到 `Result<T>`，「then 没跑」对其不构成知识黑洞。关键性质：即便在循环内阻塞也不会死锁（`then` 回调经 post 入队、只有排水才执行，而 `blocking_wait` 不等它们；worker 置结果不取队列/值班锁，主线程持锁入队与 worker 置结果并发安全）——代价是停摆一帧。文档写明：勿在循环内用它等 `then` 回调（那是投递，要排水才走）。**出生即取消的 Task 上 `blocking_wait` 立即返回取消类错误**（`cancelled` 已置位即短路，绝不空等永不到来的结果）。
4. **`coroutine.h`**：`co_spawn` 首次 resume 时捕获 `MainQueue::current_epoch()`，后续每跳 resume 改走 `try_post(epoch)`——false = 协程随循环打烊终结（与「打烊取消」同形态）；`post_to_main` 转发保留（默认世代 0）。`co_await` 内部 awaiter 的「调用线程上禁 post」不变量（`async.h`/`http_await.h` 已载）不变。
5. **net 发起校验**：`net::start/engine/connect/bind/http 发起 API` 调用线程为 app 线程 ⇒ 发起时 `current_epoch()==0` 即拒绝（返回错误/走发起硬失败形态，按各 API 既有错误契约就近选择），杜绝「发起时无人值班、静默丢弃续体」。投递侧（`transport.cpp:192`、`resolver.cpp:59`、`datagram.cpp:160`、`engine.cpp:246`、`http_binding.h:153`）经 `post_to_main` 自动获得新语义，零改动。`connectivity` 的「无订阅者直接丢弃」事件与 poster 无关，不改。
6. **`application.h/cpp`**：`loop_begin` 持 `on_duty([this]{ request_wake_all(); })` 的 `Lease`，`nullopt` ⇒ 拒绝并发 run（多实例允许、并发 run 不允许，`on_strict_failure` 并携前值班者 epoch）；`loop_end` 按上述时序收尾——「无条件置空」缺陷（`application.h:794`）根除；`run_modal` 的 prev_poster 存/取与 `SingletonRestore::poster` 成员整段删除（模态与主循环共享同一全局队列，`step_frame` 经 `take_batch` 排水照常）；`posted_mutex_/posted_/drain_posted` 移除。
7. **`window.h`**：删 `Window::run` 自循环与槽操作（`918-987`，含 RunQueue；直接调用者六处见影响面订正，全部迁移到 `Application` 驱动）。`pump_events/wait_events/next_wait_ms_/set_next_wait` 中，`next_wait_ms_/set_next_wait` 随唯一消费者 `Window::run` 消亡（`decide_wait` 走宿主侧计算，不读它），一并删除；`pump_events/wait_events` 保留，`Application` 帧序为唯一驱动。
8. **边界读点归一**：`win32_ua::wait_on_main` 就地路径② 改 `try_post`（false ⇒ 零值 `R{}` ⇒ `UIA_E_ELEMENTNOTAVAILABLE`；主人线程路径① 与拆除门闩保留）；`inspector marshal_get` 改 `try_post`（无值班 ⇒ HTTP 错误响应）。两处自锁读槽代码删除。
9. **测试与 TestController**：`utest_async/utest_coroutine/utest_state_resource` 的 PosterQueue + `set_main_poster` 改为持 `Lease` + `take_batch`；`TestController` Impl 领 `wake` 为空的 `Lease`，`pump` 帧头 `take_batch` 排水（删除 `test_controller.h:92` 自述的「唯一差异：无跨线程回投排水」）；`utest_window.cpp:382-391` 删除，帧语义由 Application 层用例覆盖。
10. **文档口径回写**：`resource.h:24`「必须先安装主线程投递器」、`engine.h:19/75`、`event_source_registry.h:18-19` 等「未装则就地执行」表述改为两相失败形态与租约语义。

## 验收判据

1. `tools/check/check_change_proposals.py` 全量绿；clang-format（venv 22.1.8）绿；clang-tidy native 口径绿（浏览器口径的 dynamic-static 类告警按 `CODING_STANDARDS.md` §5.2 处理）。
2. 新增 `utest_main_queue`：①非打烊无值班发起（`async()`/`then()`）⇒ 经 `set_strict_failure_handler` 注入的捕获器命中且不 terminate；②租约互不冒卸——A 持牌时 B `on_duty` 得 `nullopt`，B 的 `release`/析构不影响 A 在岗；**A release 后 C 重新领取成功**（覆盖 WASM raf_tick 后重入形态）；③收尾竞态钉死——`release()` 前入队项必被其后 `take_batch()` 捞全；④世代失配落地 ⇒ 丢弃且计数诊断、**进程不终止**（退出竞态用例：在途任务发起 → 循环关闭 → post 返还，断言无 terminate）；⑤`try_post` 无值班/false 世代均返回 false 且**不触发 `StrictFailureHandler`**；⑥**epoch 全函数钉死**——断言 `has_on_duty() ⟺ current_epoch() != 0` 恒成立；打烊后 `current_epoch()==0` 使在途追踪任务（expected≥1）与代际无关任务（expected==0）**双双拒绝**；release 后重新 `on_duty` 领到严格更大的世代，注入上一世在途任务投递 ⇒ 拒绝（钉死「重入 run 误放行」）；同世在途任务 ⇒ 放行；⑦**打烊窗口发起二分**——在 `closing_scope()` 守卫内调 `async().then()` ⇒ 返回的 Task `is_cancelled()==true`、`then` 恒不执行、`on_cancel` 登记即触发、WARN 计数命中、**不 terminate**；守卫析构后同场景 ⇒ 回到 `StrictFailureHandler` 捕获（钉死 `closing` 有界、绝不延到下次 `on_duty`）。
3. 边界降级用例：无值班时 `win32_ua` 路径返回 `R{}`（映射 `UIA_E_ELEMENTNOTAVAILABLE`）、inspector marshal 回 HTTP 错误，进程存活。
4. `co_spawn` 世代捕获用例：跨打烊的 resume 被丢弃、协程终结不崩；net 发起校验用例：无值班时 `connect`/`start` 即拒。async 三件套迁移后绿；`utest_net_http_binding` 无退出卡顿回归（CHG-030 不回退）；WASM 口径编译绿；`blocking_wait` 循环内/外两用例 + 出生即取消 Task 上立即返回取消类错误（不空等）。
5. 全量 ctest 绿。`aurora_api.json` 复核：`set_main_poster`、`Window::run` 若有条目则刷 schema 并过 `check_api_schema_sync`。

## 验收证据

执行面：Windows（Ninja + g++ 16.1.0，Release 构建树 `build/`，16 逻辑核，全量 ctest 并发度 16）、Emscripten（`build-wasm`）。日期 2026-10-31。

| 判据 | 实测证据 |
|:---|:---|
| 1 提案 / 格式 / 静态检查 | `check_change_proposals` 绿（48 篇提案元数据/章节/回写落点一致）；`format-check` 检查 1262 文件、合规 1262、非合规 0；`check_nolint_layout` 1871 条指令全落在代码行；clang-tidy native 口径全量 32 TU `unique findings: 0`（`blocking(>=warning): 0`），本轮改动的三个 TU（`utest_async.cpp`、`utest_strict_mode.cpp`、`utest_main_queue.cpp`）定向复跑 0 finding；浏览器口径（`--emscripten`）对同三 TU 亦 0 finding——该口径首跑浮出两处并已修：`main_queue.h` 的 `bugprone-dynamic-static-initializers`（原 `NOLINTNEXTLINE` 挂在函数签名行、罩不到函数体内的声明，已移到声明行）与 `async.h` 的 `bugprone-unchecked-optional-access`（`condition_variable::wait` 的谓词静态检查看不进，按 `CODING_STANDARDS.md` §5.2 就地 NOLINT 并写明理由）。全量与定向复跑之后仅剩注释文本整理（去掉代码注释里的提案编号），语义面未再变动。 |
| 2 utest_main_queue 七面 | 7/7 绿（1.46 ms）。①硬失败一侧落在 `utest_async.initiation_without_a_duty_loop_hard_fails_through_the_handler`（发起语义属 `state/async.h`，故归 async 套件）：注入处理器抛哨兵 ⇒ 命中 1 次、进程不 terminate，stderr 见 `[FTL][contract]`；②`duty_is_exclusive_and_reusable_after_release`（A 持牌时 B 得 `nullopt`，A 卸任后 C 领取成功且世代严格更大）；③`close_race_release_then_drain_catches_all`（`release()` 前入队的 1 项被其后 `take_batch()` 捞全并执行）；④`post_acceptance_matrix`（旧世 `post` 被拒、`dropped_count` +1、不抛出、进程存活）；⑤同测例——全程注入的处理器命中数恒 0，`try_post` 与 `post` 的拒绝均不进失败处理器；⑥`has_on_duty_iff_epoch_nonzero` 与 `post_acceptance_matrix`（打烊后 `current_epoch()==0`，代际无关（`expected==0`）与旧世（`expected≥1`）双双拒绝）；⑦`closing_scope_is_bounded_flag`（守卫内 `is_closing()` 真、守卫析构即假）；打烊窗口内的发起侧二分见 `utest_async.cancel_at_birth_task_short_circuits_every_observable`（`is_cancelled()` 真、`then` 恒不执行、`on_cancel` 登记即触发、不 terminate）。 |
| 3 边界降级 | `utest_win32_ua_marshal` 6/6（`missing_duty_degrades_to_zero_value_without_inline`：`try_post` 返回 false ⇒ `R{}` ⇒ `UIA_E_ELEMENTNOTAVAILABLE`）；`utest_inspector_server` 34/34（`tree_routes_degrade_to_503_without_a_loop_on_duty`：无值班 ⇒ HTTP 503）；两套件均正常退出、进程存活。 |
| 4 协程世代 · net · WASM · blocking_wait | `utest_coroutine` 14/14（跨打烊 resume 丢弃、协程终结不崩）；`utest_async` 21 例（20 通过 + 1 按 feature 宏 SKIP）；`utest_state_resource` 13/13；`blocking_wait` 三例——`blocking_wait_returns_result_without_drain`（循环内不等排水即取到结果）、`blocking_wait_after_closure_still_yields_result`（打烊后仍取到结果）、出生即取消的 Task 上立即返回取消类错误（不空等）；`utest_net_http_binding` 21/21、1.75 s 退出无卡顿；WASM `--target aurora` 167 步 EXIT=0。**订正（实施期复核）**：net 发起面未新增校验分支——`git diff` 确认 `include/aurora/net/engine.h`、`src/aurora/net/engine.cpp`、`src/aurora/net/resolver.cpp`、`src/aurora/net/detail/http_await.h` 本轮只改注释；net 各入口为 `CoroTask`（首跳 resume 由调用方推进），无值班时的拒绝由投递侧两相失败（打烊取消）承担，故判据原文的「发起即拒」用例未落地。 |
| 5 全量 ctest · schema | 全量 `ctest --preset ninja-test` **451/451 通过（32.43 s，EXIT=0）**，含全部 `check_*` 静态门禁；`aurora_api.json` 复核：`set_main_poster` / `main_poster` 命中 0，`"run"` 方法命中 0（`Window::run` 无残留条目）；`check_api_schema_sync` 76 widgets / 46 enums in sync。 |

首轮全量曾有两例红：`utest_net_http_auth` 的 `a_derived_provider_can_await_the_credential_exchange`（3006 ms）与 `a_failed_exchange_propagates_the_provider_error`（3010 ms），stderr 为 `posted task cancelled by loop closure (no on-duty main loop)`。定性为 16 路满负荷下 worker 世代回投晚于 3 s 等待预算（`AURORA_WAIT_BUDGET` 未改），单跑 27 ms、后续两轮全量复跑均绿，非本改动引入。

## 回写落点

- `codespec/specification/01-core.md` §8.2：MainQueue 队列与值班租约、两相失败契约（新节）
- `codespec/specification/02-state.md` §7.2：异步回投契约——发起需循环在场、落地晚于打烊 = 取消、就地回退废除、`blocking_wait` 出口
- `codespec/specification/06-app-platform.md`：帧循环与值班——`loop_begin/loop_end` 领取/归还/重领时序、并发 run 拒绝、`Window` 不再自跑帧循环、模态共享全局队列
- `codespec/SPECIFICATIONS.md`：需求表 `SPEC.FEAT.CORE.ASYNC-CONCURRENCY.001` 行与 §「单线程 UI vs 异步」处置表述改值班口径
- 本提案状态流转：已提议 → 实施中 → 已归档
