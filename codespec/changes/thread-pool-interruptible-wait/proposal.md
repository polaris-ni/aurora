# CHG-030 with_timeout 裸 sleep 致 default_pool 析构卡满超时窗口：改可中断等待

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-030 |
| 提出日期 | 2026-10-21 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `include/aurora/core/thread_pool.h` 新增公共方法 `interruptible_wait_for`；`include/aurora/state/async.h` 的 `Task<T>::with_timeout` 非 deferred 分支改用之；`tests/unit/utest_thread_pool.cpp` 与 `utest_async.cpp` 增测。不改公共 API schema（`ThreadPool` 未进 `aurora_api.json`）、不改 CI 门禁面、不改任何 `AURORA_*` 宏口径 |

## 动机

1. **进程退出卡满超时窗口的实测**。CI 上 `utest_net_http_binding` 单测耗时 ~60s（本地 30s）。实测对照：`--list`（只列名不跑用例）0s、全量 30.01s（21 用例仅 328ms）；单跑用例体 2.6ms 即打出 `[==========] ran` 摘要，之后进程静默 30s 才退出（exit=0 自行恢复，非死锁）。在 `main` 返回前与 `atexit` 处理函数中各打时间戳，二者相差 29999ms ⇒ 30s 卡在 `main` 返回后、atexit 前的静态析构阶段。21 用例逐个计时：仅用 `bind_resource`（直接拿 `Resource`）的 4 例卡 30s，`deliver*` 不卡。
2. **裸 sleep 不可中断是根因**。`ResourceCore::send()` 给作业挂默认 30s 超时（`resource.h:243` 的 `job_->with_timeout(opts_.timeout)`）；`with_timeout` 的非 deferred 分支（`async.h:377-380`）`ThreadPool::default_pool().execute([expire,d]{ std::this_thread::sleep_for(d); expire(); })`。native 构建下 `AURORA_CAP_THREADS!=0` ⇒ `default_pool()` 是真实 worker 线程池（非 deferred）。进程退出时 `default_pool()` 的函数内 `static` 单例析构（`thread_pool.h:80-97`）`stop_=true; cv_.notify_all(); for(w) w.join()` —— 而该超时 worker 正裸睡 30s，必须睡满才能被 join ⇒ 卡 30s。引擎单例与 `EventSourceRegistry` 是 `new` 泄漏、不析构，已排除，故元凶是会被析构的 `default_pool` 单例。
3. **修复对症且零语义变更**。新增 `interruptible_wait_for` 持 `mutex_` 调 `cv_.wait_for(lock, d, [this]{ return stop_; })`；正常运行期 `stop_` 恒 false，语义等同 `sleep_for(d)`，超时判定不受影响；进程退出时 `stop_=true` + `notify_all()` 立即唤醒等待中的超时任务 ⇒ 析构 join 不再卡满。deferred 模式加保护直接 return（`with_timeout` 非 deferred 分支本就不会在 deferred 下调用，保护防误用）。
4. **该延迟是真实进程退出延迟，非测试假象**。任何经 `bind_resource` 挂了默认 30s 超时的资源，在进程退出时都会让 `default_pool` 析构卡 30s；修复后真实应用退出同样受益（不再无条件等待挂起超时的 30s 睡眠窗口）。

## 变更内容

1. `include/aurora/core/thread_pool.h` 新增 public 模板方法 `interruptible_wait_for(d)`：deferred 模式（`AURORA_COMPILE_TIME_DEFERRED` 或 `force_deferred`）直接 return；否则 `std::unique_lock lock(mutex_); cv_.wait_for(lock, d, [this]() -> bool { return stop_; });`。含完整文档注释（`@brief` / `@param` / `@return` / `@note`：说明供 `with_timeout` 等场景使用、进程退出可立即唤醒、deferred 保护）。
2. `include/aurora/state/async.h` 的 `Task<T>::with_timeout` 非 deferred 分支：闭包内 `std::this_thread::sleep_for(d); expire();` 改为 `ThreadPool::default_pool().interruptible_wait_for(d); expire();`；deferred 分支（`register_timeout_guard`）不变。
3. 测试增量：`utest_thread_pool.cpp` 新增用例验证 `interruptible_wait_for` 在 `stop_` 置位（经析构触发）时被 `notify_all` 提前唤醒、且不改变正常等待时长；`utest_async.cpp` 新增用例验证非 deferred 下 `with_timeout` 超时仍正确改道超时错误（语义不变）。

## 验收判据

1. `tools/check/check_change_proposals.py` 全量绿（本提案自身过契约）。
2. clang-format（venv 22.1.8）format-check 绿；clang-tidy 本机 native 对 `thread_pool.h` / `async.h` 绿；`ThreadPool` 未进 `aurora_api.json` ⇒ 无需刷 schema、无需 `check_api_schema_sync`。
3. `utest_thread_pool` / `utest_async` 新增用例绿；重建 `aurora_test_runner` 并对 `utest_net_http_binding` 计时，全量从 30.01s 降至 <1s（30s 退出卡顿消失，21 用例仍 ~328ms 通过）。
4. 先 `cmake --build` 再跑 ctest：`registry_integrity` 不假绿，全量测试绿。

## 回写落点

- `codespec/specification/02-state.md`：with_timeout 超时语义（非 deferred 路径由 worker 裸 sleep 改为可中断等待，进程退出析构不再卡满超时窗口）
- `codespec/changes/thread-pool-interruptible-wait/proposal.md`：状态按 已提议 → 实施中 → 已归档 流转
