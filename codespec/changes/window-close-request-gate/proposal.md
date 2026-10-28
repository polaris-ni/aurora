# CHG-034 窗口关闭请求闸门：销毁前可否决的公共入口

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-034 |
| 提出日期 | 2026-10-22 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/window/window.h`（**新增 `CloseRequestSource` 枚举，紧邻 `ExitPolicy`**——它被 `window_host.h` 消费，而 `application.h` 包含 `window_host.h`，落 `application.h` 会形成反向依赖而编译不过）、`include/aurora/window/surface.h`（**新增虚函数 `cancel_close()`**，默认空实现；`HeadlessSurface` 覆写）、`include/aurora/app/window_host.h` 与 `src/aurora/app/window_host.cpp`（`request_close` 带来源默认参、新增 `cancel_close()`、三个私有记账成员、`friend class Application`）、`include/aurora/app/application.h` 与 `src/aurora/app/application.cpp`（`set_on_close_requested`、私有 `poll_close_requests()` / `ask_gate()`、`step_frame` 内 `on_frame_` 之后插入询问步、`reap_closed` 第①步上移）、后端公共头 `include/aurora/window/{x11,wayland,glfw,macos,wasm}_surface.h` 与 `include/aurora/window/win32_surface.h`（各加一行 `cancel_close` 声明，同源面 `d3d11_surface.h` / `wgpu_win32_surface.h` / `wgpu_x11_surface.h` / `wgpu_wayland_surface.h` 转发）、对应后端 `.cpp`、`tests/unit/utest_multi_window.cpp`（并入用例，不新建文件）、`cmake/AuroraVerify.cmake`（登记真机探针）、`codespec/specification/06-app-platform.md`、`codespec/ARCHITECTURE.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/10-window.md` |

## 动机

1. **关闭是一条单向旗标链**：后端置位 → 宿主聚合（`close_requested_` 或后端 `should_close()`）→ 帧末 `reap_closed` 无条件回收。全仓没有「关闭请求」回调、没有返回 `bool` 的拦截点、没有撤销关闭的公共 API。
2. **应用侧连「问用户一句」的窗口都拿不到**：销毁前唯一可观测点是 `set_on_frame` 里轮询 `WindowHost::should_close()`，而 `reap_closed` 紧随其后——抢在那里弹框的结果是「框还开着、窗已经没了」。`set_on_window_closed` 排在回收**之后**，问什么都来不及。
3. **这是数据损失面**：点窗口关闭钮会静默带走正在跑的子进程 / 会话。
4. **否决不能下放给应用**：后端关闭旗标都在私有 pimpl 里，框架外部连清位入口都拿不到；自定义 Surface 永远无清位路径。
5. **库内已自绘关闭钮**：X11 / Wayland 的 CSD 标题栏关闭钮是库内控件，闸门理应在库内闭合。

## 变更内容

1. **形态：甲＝关闭请求闸门（一次一问、可否决）**。否决乙（只给一枚 `cancel_close()`、闸门由应用自装）：把「只问一次」的负担留给应用，而 `on_frame_` 与 `render_all` 同帧的时序本就脆。否决丙（明确本条不属于框架）：动机第 5 条。
2. **新 API**：
   - `enum class CloseRequestSource : std::uint8_t { OperatingSystem, Programmatic, PolicyCascade }`——**落 `include/aurora/window/window.h`**（与 `WindowRole` / `ExitPolicy` 同址，二者同样是被 `window_host.h` 消费的应用层策略枚举）；不用缩写；不带后端原始原因。
   - `Application::set_on_close_requested(std::function<bool(WindowId, CloseRequestSource)>)`，返回 `false` 即撤销该次关闭。
   - `Surface::cancel_close()` virtual 默认空实现 + `WindowHost::cancel_close()` public（清记账位 + 转发）。
3. **调用点（时序修正）**：插在 `step_frame` 内 **`if (on_frame_)` 之后、`tick_all()` 之前**（不是 `system_drag::pump_reads()` 之后）。三条理由不变（晚于 `pump_all_once` / 早于 `render_all` / 早于 `reap_closed` 收集步），**新增第四条决定性的**：应用最典型的写法是在 `on_frame_` 里调 `close_window()`；若询问步排在 `on_frame_` 之前，该关闭当帧就直接进 `reap_closed` 被回收，**永远不过闸**——「程序化关闭也过闸」被架空。以 `tests/unit/utest_window_lifecycle.cpp` 的 `main_window_closed_policy_closes_every_window`（在 `on_frame_` 首帧关主窗）为照妖镜：位置放错该用例必红。
4. **零回调时逐位不变**：询问步首行判据 `if (on_close_requested_ == nullptr) return;`，且策略连带的**置位**置于该判据之前（仅「询问」受判据控制）⇒ 无回调时净效果与现状逐位等价。这是全部现有用例保持绿的前提。
5. **边沿闩（单遍，无 pass 2）**：每宿主私有 `bool close_asked_` + `CloseRequestSource close_source_`，不进公共 API。询问步单遍索引遍历：
   - 旗标为假 ⇒ 复位 `close_asked_`（边沿记忆随旗标回落）；
   - 已置位且未问过 ⇒ 置闩并询问；回 `false` ⇒ `cancel_close()`（清 `close_requested_` + `close_source_` + 闩 + 后端旗标）；
   - 策略连带已在询问之前置位 ⇒ **单遍即覆盖连带扇，无需第二遍**（第二遍只会全部命中 `close_asked_` 变成死代码）。
   - 四种场景：同帧多次置位 ⇒ bool 幂等，一抬沿只问一次；置位→否决→再置位 ⇒ 清闩后再置即新抬沿，再问一次；后端帧间重复上报 ⇒ 每次上报前旗标已清，各自成为新抬沿；应用长期不答 ⇒ 同步阻塞在 `step_frame` 内，帧循环停摆，不重复询问。
   - 索引遍历 + 每轮重取 `hosts_.size()`（回调内可 `open_window` / `close_window`，同 `pump_all_once` 的重入纪律）。
6. **来源判定式（唯一可行口径）**：`src = h->close_requested_ ? h->close_source_ : CloseRequestSource::OperatingSystem`。不能用「后端旗标 ⇒ OS、宿主旗标 ⇒ 程序化」推导——`WaylandSurface::close()` 把**程序化**关闭写进了同一个后端位，X11 / Win32 则反之；只有宿主层的记账位是可信来源。推论：Win32 `WM_QUIT` 折算出的关闭请求按本式落到 `OperatingSystem`（要标 `Programmatic` 须新增 Surface 原因通道，与不做清单①冲突），此结论写进回写落点。
7. **策略连带**：`reap_closed` 第①步整段上移到询问步首部，**原处不再保留**静默置位；循环**限定 `hosts_[i].get() != main_host_`**（旧代码连带到主窗自身，上移后若照抄会把主窗来源覆写成 `PolicyCascade`，直接违背「按来源放行」的用法示例）；Transient 连带（`reap_closed` 第④步内）一并传 `PolicyCascade`。
8. **清位层次与 fail-open**：
   - `Surface::cancel_close()`（默认空实现，自定义 Surface 零改）契约：把 `should_close()` 清回 `false`；
   - `WindowHost::cancel_close()`：清三枚记账位 + 转发 `window()->surface().cancel_close()`（二者均公有）；
   - **后置复验**：`cancel_close()` 后复验 `should_close()`，仍为真 ⇒ WARN 一次并按**放行**处理（fail-open：宁可完成用户请求的关闭，也不留一扇永远关不掉的窗）。「否决后 `should_close()` 必为假」成文为后置条件。
   - **已销毁路径必须拒绝清位**：X11 `DestroyNotify`、Win32 `handle_destroy`（owned 子窗随 owner 销毁**必然可达**）、Wayland 连接错误三处的窗口已不存在，清位会造出收不掉的僵尸宿主并（X11）触发 BadWindow。各私有 Impl 加一枚 `bool window_gone`（后端私有、零公共面），这些路径置位，`cancel_close()` 遇之拒绝清位 ⇒ 由 fail-open 落到「WARN + 放行、当帧回收」。这让 fail-open 从「自定义 Surface 兜底」升级为「已销毁窗口的必然出口」。
   - 各后端落点：GLFW `glfwSetWindowShouldClose(w, GLFW_FALSE)`（现成、全仓未用）；Win32 清 `should_close`（`handle_close` 本就只置位不 `DestroyWindow`）；X11 / Wayland 清 `close_requested`（ICCCM 与 xdg_toplevel 语义下客户端本就可以不销毁）；无头 `set_should_close(false)`；macOS 为骨架（`Impl` 全 stub），标「后端实写时一并落地」。
9. **否决语义定稿表**：

   | 场景 | 定稿 |
   |:---|:---|
   | 普通 OS 请求被否 | 清旗标；当帧继续 `render_all`；不写几何快照（收集步以 `should_close()` 为条件，天然跳过，**无需守卫**） |
   | `MainWindowClosed` 全否 | 主窗先被否 ⇒ 连带虽预置但同步撤销 ⇒ 无一扇被收 ⇒ `should_exit()` 假 ⇒ `run()` 继续 |
   | `MainWindowClosed` 主放行、部分扇被否 | 主窗照收、`run()` 退出；被否扇作为活宿主留存至 `~Application`——**文档必须明写** |
   | `quit()` | 不走关闭旗标，天然不过闸；文档注明 |
   | `ExplicitOnly` | 关闭照问；退出与否与闸门无关 |
   | Transient 从属连带 | 过闸，来源 `PolicyCascade`；owner 先收、从属次帧被问（现有时序不变） |
   | **孤儿从属（owner 已被收、从属被否存活）** | 连带被否时**一并解绑 owner**：`surface().set_owner(nullptr)`（Win32 `GWLP_HWNDPARENT` 置 NULL 是合法解除）。否则被否从属持悬垂 `owner_` 裸指针，且 Transient 连带与模态输入恢复都失去载体 |
   | 模态从属窗 | 关从属模态窗本身照问；被否则不进回收步、owner 输入保持禁用（恢复输入步不动） |
   | 回调内再 `close_window()` / `open_window()` | 容许（索引遍历）；不承诺同帧必问 |
   | 回调抛异常 | `try/catch`：ERROR 日志 + **按放行处理**（fail-open）；**不**复位闩。绝不穿出 `step_frame` |
   | 回调内阻塞跑应用自有模态循环 | 允许；**禁止重入 `run()` / `step_frame()`**（库不设重入护栏，文档硬约束） |

   > 异常路径原定为「按否决 + 复位闩」，**已改判**：回调若确定性抛异常，该组合会导致每帧一问、每帧一条 ERROR、窗口永久关不掉，直接违反 fail-open 教义。
10. **WASM**：`WasmSurface::cancel_close()` 给**真实实现**——把页面级 static 标志写回 `false`，并在注释与规格中申报降级：「页面级单标志为全部实例共享，否决任一窗口即否决全部；且浏览器可在 beforeunload 后强制卸载，本后端不保证不退到满载档」。不实现（空实现）的后果是每次卸载刷一条 fail-open WARN 且窗必然被收，比真实实现更差。
11. **正确用法模式**：闸门是**同步布尔**，不是「弹框等用户」的地方。正确模式：闸门回 `false`（撤销）→ 应用自行挂确认浮层 → 用户确认后调 `close_window()`（`Programmatic` 源直接放行）。符合 CODING_STANDARDS §4.1「宿主回调只许快、不许阻塞、不许抛」。
12. **不改** `should_exit()` 三策略判定、不改 `reap_closed` 的 ②③④ 步与几何收集条件。**严格模式安全性**：fail-open 走 `AURORA_LOG_WARN`，只有 `Diagnostics::degraded` 会在 `StrictMode::On` 升级为硬失败 ⇒ 安全；**严禁**改经 `Diagnostics::degraded` 上报。
13. **不做**清单：① 不给 `Surface` 加「关闭原因」通道；② 不做异步 / 三态询问；③ 不改 `should_exit()`；④ 不给 `close_asked_` / `close_source_` 开放公共读口（测试经询问计数断言）；⑤ 不修 X11 / Wayland 的 `set_size` / `set_position` 未实现（属 CHG-033 面）；⑥ 不承诺「OS 前台窗」语义（属 CHG-032 面）；⑦ **闸门只覆盖帧循环推进期间的关闭请求**——关闭旗标若在 `run()` 启动前就已置位，`MainWindowClosed` 下 `should_exit()` 立即为真、循环一帧都不推进，闸门不会被询问（实测确认，已写入规格与该判据的用例注释）。

## 验收判据

无头底座：`HeadlessSurface::set_should_close(true)` + `opts.power_saving=false` + `max_frames` 有限帧（范式见 `tests/unit/utest_multi_window.cpp`）。用例**并入**既有 TU，不新建文件（避免 GLOB / `registry_integrity` 双刷）。

1. **否决保活**：闸门恒 `false` → 置旗标 → 跑若干帧 → `window_count()` 不变、`window_host(id)->should_close()==false`、`on_window_closed` 零触发、询问计数 == 1（多帧不重复）。
2. **再置再问**：否决后隔帧再置位 → 询问计数 == 2；仍不被收。
3. **放行即收**：闸门恒 `true` → 置位 → 1 帧 → 宿主进回收、`on_window_closed` 恰一次、几何快照已写。
4. **否决不写快照**：装 `Preferences` + `persist_id`，闸门 `false`，置位跑帧 → 存储内无该键写入。
5. **来源可分**：A 路 `close_window(id)` ⇒ `Programmatic`；B 路 `set_should_close(true)` ⇒ `OperatingSystem`（即第 6 条判定式的双向探针）。
6. **策略连带逐扇问**：`ExitPolicy::MainWindowClosed` + 主窗置位，闸门按 id 记录 → 每个**非主**宿主都收到一次 `PolicyCascade`；主窗自身来源仍为 `OperatingSystem`（不得被覆写）。
7. **全否不退出**：闸门恒 `false` + `MainWindowClosed`，跑满 `max_frames` → `run()` 未提前退、`window_count()` 不变。
8. **主放行扇被否**：主窗放行、某扇 `PolicyCascade` 否决 → 主宿主被收、`run()` 退出；被否扇 `window_host()` 仍非空。
9. **异常不穿出 + fail-open**：闸门抛 `std::runtime_error` → 无异常穿出 `run()`；**窗口被收**；ERROR 日志一条（按第 9 条定稿）。
10. **回调内重入开关窗**：闸门内 `close_window(第三方 id)` → 不崩；第三方次帧前被问。
11. **无回调零行为（等价性铁证）**：不装闸门，跑现有全部多窗口 / 生命周期用例 → 逐位绿。**关键哨兵**：`tests/unit/utest_window_lifecycle.cpp` 的 `main_window_closed_policy_closes_every_window`（在 `on_frame_` 首帧关主窗）——它同时钉住第 3 条的位置决定与第 7 条的连带迁移。
12. **Transient 连带过闸**：owner 放行关闭，从属窗次帧收到一次 `PolicyCascade` 询问；否决则从属存活且 owner 已解绑。
13. **各后端清位（真机走查）**：Win32 关闭消息被否决后窗口未销毁、X11 `WM_DELETE_WINDOW` 被否决后仍存活、Wayland 同、GLFW 用 `glfwSetWindowShouldClose` 清位、无头用 `set_should_close(false)`；并回答「否决后窗口管理器是否重发关闭请求」。探针参照 `tools/verify/win32_csd_live_probe.cpp`，并在 `cmake/AuroraVerify.cmake` 登记。
14. **门禁**：`check_api_schema_sync`（先 `--target aurora_api_json`，预期按仓库口径核对）、`check_naming_conventions`、`check_doc_comments`、`check_api_budget`、`check_platform_macros`、`check_no_cjk_literals`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_test_registry`、`check_arch_module_map`、`registry_integrity`、`check_version_consistency` 全绿；`lint`（后端 TU 入集，标注扩面 lint-windows / lint-macos / lint-wasm）与 `format-check` 全绿；按 ARCHITECTURE.md §14.5 补跑 wasm 与 Linux X11/Wayland 两配置；全量 `ctest` 绿。
15. **变异自证**：闸门恒 `true` ⇒ 判据 1 转红；恒 `false` ⇒ 判据 3 转红；改按电平轮询 ⇒ 判据 1 的「多帧只问一次」转红；把连带置位也放进回调判据内 ⇒ 判据 11 转红；询问步前移到 `on_frame_` 之前 ⇒ 判据 11 的哨兵用例转红；`cancel_close()` 漏清 `close_requested_` ⇒ 判据 1 转红；去掉后置复验 ⇒ 自定义 Surface 上 fail-open 契约失效。

## 回写落点

- `codespec/specification/06-app-platform.md`（§2.4 多窗口与帧循环：闸门的调用点与时序、`CloseRequestSource` 三档语义与判定式、边沿闩口径、清位三层与 fail-open 后置条件、`window_gone` 拒绝清位、否决语义定稿表、与 `set_on_window_closed` 的时点分工、正确用法模式与「回调只许快」纪律、WASM 降级申报、Win32 `WM_QUIT` 落 `OperatingSystem` 的说明；退出策略表补闸门行）
- `codespec/ARCHITECTURE.md`（帧循环小节：询问步在帧序中的位置——`on_frame_` 之后、`tick_all` 之前）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行；该行由 CHG-032/033/034 共享，批次末一次合并改写）
- `codespec/manual-test/10-window.md`（取 `TC-WINDOW-019`——现最高为 016，**017 已由 CHG-032 预留、018 已由 CHG-033 预留**，回写时须注明「经兄弟提案占用后」；须同时补 §3 执行记录表行与 §1.8 覆盖取舍表登记）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 版本与发布记录：条目类型 Added，落仓库根变更记录的 [Unreleased] 节；**本批不单独升 alpha 号**，四份收口时一次性三处同步（该记录顶部 currentVersion、CMakeLists.txt 的 AURORA_VERSION_SUFFIX、核心版本头兜底串）并跑 check_version_consistency。上述文件均不属 codespec 树，按 AGENTS.md 硬规则 5 只在此说明、不列为回写落点。

## 消费侧用法（最小示例）

```cpp
app.set_on_close_requested([](aurora::WindowId id, aurora::CloseRequestSource src) {
    if (src == aurora::CloseRequestSource::PolicyCascade) {
        return true;                       // 策略连带：直接放行
    }
    if (!has_live_session(id)) {
        return true;                       // 无活会话：无需问
    }
    show_confirm_overlay(id, [] { app.close_window(id); });   // 撤销 + 自行挂确认
    return false;                          // 本次关闭撤销
});
```

## 落码次序

blast radius 最大，放最后落。建议次序：CHG-031 → CHG-033 → CHG-032 → **CHG-034**。
实施切片：**S1** 枚举 + `surface.h` 虚函数 + 无头覆写 + `window_host` + `application` 询问步 + 无头用例（此时除无头外全部后端走 fail-open WARN，正是分片价值）；**S2** 后端清位（GLFW → Win32 → X11 → Wayland → macOS 骨架 → WASM）+ 真机探针；**S3** 文档与门禁收口。

## 实施记录与偏离（落码期回写）

1. **macOS 腿已随窗口壳实写落地**（上方「macOS 骨架」只反映提案期状态）：`windowShouldClose:` 置 `should_close = true` 并返回 NO（不让 AppKit 抢先 `orderOut` / 回收窗口），`close()` 只置位、窗口回收仍归宿主；`cancel_close()` 在窗口仍存活时清位，窗口已被 OS 销毁的路径拒绝清位（否则造出永远收不掉的僵尸宿主）。`should_close()` 在窗口壳不存在时返回 true（视同已请求关闭）。
2. **验收**：`tools/verify/macos_surface_live_probe.mm` 覆盖 `close()` 置位 / `cancel_close()` 撤销 / 再次 `close()` 置位三项；`windowShouldClose:` 路径由同一探针的真实 `NSWindow` 间接证明（探针真实建窗并驱动 AppKit）。关闭闸门的跨后端语义由既有 `utest_window_shell` 等用例覆盖，未新增专用真机用例。
