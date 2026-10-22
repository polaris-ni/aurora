# CHG-032 应用级活动窗口查询：把「最近派发过哪一扇窗」上升为公共面

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-032 |
| 提出日期 | 2026-10-22 |
| 当前状态 | 已提议 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/app/application.h`（新增 `active_window()` 与 `mutable WindowHost *last_dispatch_`）、`src/aurora/app/application.cpp`（应用级六个程序化入口按 #6 过滤后记账、`register_host` 的 key_pre 闭包内于匹配前记账、`reap_closed` 置空指向被回收宿主的指针）、`tests/unit/utest_application.cpp`（并入新增用例，不新建 utest 文件）、一枚新增 integration 用例文件（tests/integration 目录内，落地时定名）、`codespec/specification/06-app-platform.md`、`codespec/ARCHITECTURE.md`、`codespec/SPECIFICATIONS.md`、`codespec/manual-test/10-window.md`；**不动** `include/aurora/app/window_host.h` 与 `src/aurora/app/window_host.cpp`（与 CHG-034 的撞点随之消失）；aurora_api.json 无 classes/methods/functions 段，新增一枚 public 方法不入册 |

## 动机

1. **命中那一刻框架是知道来源宿主的，这个事实在闭包处掉了。** 快捷键派发是「应用级一份注册表 + 逐宿主一枚前置拦截器」：`register_host` 装进 `WindowHost::set_key_pre_handler` 的 lambda 捕获了本宿主指针并把焦点态交给 `ShortcutRegistry::handle`，但 `ShortcutBinding::action` 是无参闭包，来源宿主从捕获处到调用方之间没有通路。
2. **全仓无「当前活动窗口」概念**：active_window / focused_window / current_window 三名在公共头与实现里皆无（实测零命中）。`FocusManager` 是逐窗隔离的，「哪个控件有焦点」是窗内事实，「哪个窗该收这条快捷键」在公共面上无人回答。
3. **消费者只能自建代偿，而代偿与 OS 眼里的活动态天然可分叉**：无头、失焦两档下二者可以不同，于是「命令落到哪一扇」变成只能真机读数、无法判据化的账。
4. **缺的不是能力，是「把已有事实传到调用方」的一次纯增扩展**：只交付「落在哪一扇」，不重做「算不算命中」。

## 变更内容

1. **采纳形态**：`Application::active_window() const -> WindowId` —— **必须 const**，配 `mutable WindowHost *last_dispatch_ = nullptr`（记账一行赋值，查询 O(1)）。`@brief` 首句写死「最近一次用户输入派发的目的窗口，**不等价于 OS 前台窗**」，同一句同步进 `codespec/specification/06-app-platform.md`。命名保留 `active_window()`：消费侧任务书已按此名提需求；曾考虑改名 `last_dispatch_window()`（语义更贴），**否决**——改名收益小于消费侧与任务书的对齐成本。
2. **否决「闭包携带来源」形态**：实测要迁移 **18 处** `ShortcutRegistry::add` 调用点——`tests/unit/utest_shortcuts.cpp` 11 处（78,79,102,117,132,147,158,169,170,180,235）、`tests/unit/utest_application.cpp` 3 处（104,117,147）、`src/aurora/commands.cpp` 2 处（160,251）、`examples/demos/demo_command_palette.cpp` 2 处（91,94）；其中 **17 处**是现有无参闭包（扣掉 180 行的 nullptr 动作），`std::function<void(WindowId)>` 无法由无参 lambda 构造 ⇒ 逐个改签名。破坏面远大于收益。
3. **否决「每宿主派发序号 + 查询取最大」**：`raw->dispatch_seq_ = ++dispatch_clock_` 的时钟若落 Application，其六个应用级派发入口全是 const（application.h:428/435/443/454/459，另 448 的 dispatch_text），成员不声明 mutable 改不动；且 WindowHost 无 Application 反指，宿主级入口够不到该时钟。改为本提案的 `mutable WindowHost *last_dispatch_`：window_host.h **一个成员都不用加**，直接消掉与 CHG-034 在 window_host.h 的撞点。
4. **记账点全集（按可达性分两档，实测复核）**：
   - **记账**：① `WindowHost::dispatch(Event &)` 的 KeyEvent 分支——记账写在 Application 于 `register_host` 注入的 key_pre 闭包内（application.cpp:77），早于 `shortcuts_.handle`；闭包同时捕获 this 与宿主指针，写 mutable 成员，window_host.h/.cpp 一行不动。② 应用级六条程序化入口 `dispatch_click`(application.cpp:397) / `dispatch_pointer`(403) / `dispatch_key`(415) / `dispatch_text`(426) / `dispatch_touch`(430) / `dispatch_system_drop`(application.h:459 内联)，**按 #6 过滤后**才记账——否则 `dispatch_pointer(x, y, MouseAction::Move)` 会让鼠标悬停翻转活动窗，直接违反判据 5。
   - **不记账（够不到）**：宿主级五条直调程序化入口 `WindowHost::dispatch_pointer` / `dispatch_key` / `dispatch_text` / `dispatch_touch` / `dispatch_system_drop`（window_host.cpp:328/335/344/346/352）；宿主无 Application 反指，加反指即给 window_host.h 加成员。`WindowHost::dispatch(Event &)` 的非 Key 分支同理（`key_pre_` 只在 KeyEvent 分支调用）。**text 的两条路径正落在这条分界上**：`Application::dispatch_text` 记账、`WindowHost::dispatch_text` 不记账，故 #6 把 TextInputEvent 列入范围并不自相矛盾，text 走应用级那条路径入账。
5. **记账范围（口径定死）**：**用户输入意图类**事件记一次来源——`KeyEvent`(Down)、`MouseEvent` 且 `action == MouseAction::Press`、`TouchEvent`(Press)、`ScrollEvent`、`TextInputEvent`、`SystemDropEvent`。**不记**：指针 Move / Release（否则鼠标悬停即翻转活动窗）、窗口生命周期回调（可见性 / 几何态 / DPI / 脏区）。范围按事件类型定死，是否真记账还受 #4 的入口可达性约束。
6. **四条兜底口径**：
   - **无记录时**回落表达式取**存活**宿主：`hosts_.empty() ? AURORA_INVALID_WINDOW_ID : (main_host_ != nullptr ? main_host_ : hosts_.front())->id()`；只有 `hosts_` 空才回无效号，`main_window()` 只作该表达式的优先项。（原写法「无记录时回 `main_window()`、不得回 INVALID」与本条 #3 在 `on_window_closed_` 窗口期内给出不同答案，已废。）
   - **主窗易主**：帧末回收「保留最后一个宿主」路径下主窗身份会换，查询不得因此返回已消散的号——只读存活宿主，天然满足。
   - **回收清号**：宿主被 `reap_closed` 回收后，若 `last_dispatch_` 指向它则置空，查询不可能再选中它。
   - **空窗期**：`reap_closed` 内 `main_host_` 先置空、函数尾才回落，`on_window_closed_` 回调正处于该窗口期 ⇒ 走同一表达式，仍有存活宿主时不为无效号。
7. **单窗用法逐位不变**：只有一扇窗时 `active_window()` 与 `main_window()` 恒等；`main_window()` 的兜底语义不得反过来变成新语义。
8. **不改既有匹配语义**：`ShortcutScope::Global` / `Focus` 判定、`KeyCombo::matches` 修饰位口径、命中即消费的后果，全部不变。
9. **性能**：记账 O(1)（一次指针赋值），查询 O(1)，帧循环零新增开销。
10. **与 CHG-031 的互引裁决**：模态选择器（CHG-031：POSIX 文件 / 目录选择器）运行期间的派发**不记账**——其阻塞式运行入口置一枚抑制标志、出口还原（入口符号随 CHG-031 落地定名）；否则选择器自身的输入会把活动窗翻到选择器宿主。
11. **不做**清单：① 不改 `ShortcutBinding::action` 签名；② 不做 `State<WindowId>`（每个事件都会进 Effect 依赖收集，且它在快捷键闭包里被高频读）；③ 不做「下一次事件会落到哪」的预测式查询；④ 不给 os_hotkey 命中塞来源号；⑤ **不承诺 OS 前台 / 聚焦态**语义（本 API 只答「最近派发落在哪一扇」）；⑥ 不给 `active_window` 加显示器 / 虚拟桌面维度；⑦ 不给 `WindowHost` 加 Application 反指，也不改 window_host.h/.cpp；⑧ 不给「最后一次派发的事件种类 / 派发序号」（种类是第二枚真值源，属派生需求）。

## 验收判据

1. **首帧**：两窗构造后 `active_window() == main_window()` 且 `!= AURORA_INVALID_WINDOW_ID`。
2. **键盘记账**：经 `host_b->dispatch(Event &)` 派发 KeyEvent → 回 B（证 key_pre 闭包内记账早于匹配）；再 `app.dispatch_pointer(x, y, MouseAction::Press)` → 回 main。
3. **程序化入口全集探针（参数化）**：遍历「程序化入口全集 × 两窗」——全集为宿主级 5 条 + 应用级 6 条共 11 条，逐条 × {A 窗, B 窗} 断言：应用级 6 条一律回 main（按 #5 过滤后记账），宿主级 5 条直调不改变读数（#4 的够不到声明）。取代原先只探 2 条的写法。
4. **动作读数时序**：`Global` 快捷键闭包内记录 `app.active_window()`，两窗各按一次同一组合，两次记录为两个不同号。
5. **指针口径**：`app.dispatch_pointer(x, y, MouseAction::Move)` → 读数**不变**（新增的应用级探针）；`app.dispatch_pointer(x, y, Press)` → 回 main；`host_b->dispatch(Event &)` 派发 `MouseEvent{Press}` → 不翻转（无挂点，#4 已知边界）。
6. **回收清号**：被记账窗 reap 后不再回它（指针置空、回落存活兜底）。
7. **keep-last 不清号**：若被记账窗正是「全关保留」规则留下的主宿主 → 仍回该号（有效存活号）。从原判据 6 后半句拆出独立成条。
8. **回调窗口期**：`on_window_closed_` 回调内读 `active_window()`，仍有存活宿主时不得为 `AURORA_INVALID_WINDOW_ID`。
9. **单窗恒等**：单无头宿主任意派发后 `active_window() == main_window()`。**标注为回归保护，不占验收名额。**
10. **不回归**：`tests/unit/utest_shortcuts.cpp`、`tests/unit/utest_multi_window.cpp`、`tests/unit/utest_application.cpp`、`tests/unit/utest_commands_shortcut.cpp` 全绿且断言零改写（纯增形态下现有用例无一读新面）。
11. **集成面**：新增一枚 tests/integration 用例（复用 `tests/unit/utest_multi_window.cpp` 的 make_window 底座，`power_saving=false` + `max_frames` 保证确定性），两扇无头窗注册一条 `Global` 快捷键，逐窗派发并断言动作落点随来源移动；关掉一扇后另一扇仍拿得到自己那一格。新文件后须重跑 CMake 以刷新 GLOB。
12. **与关闭闸门的联合判据**：关闭被否决后仍返回该窗，确认回收后不再返回。**需与 CHG-034 合并后验收，不可单独判红**（防两侧生命周期规则漂移）。
13. **门禁**：`check_api_schema_sync`（`--target aurora_api_json` 须跑但**预期零差异**：该文件无 classes/methods/functions 段，新增 public 方法不入册）、`check_api_budget`（只读该文件字节数，不受影响）、`check_naming_conventions`、`check_doc_comments`（`@brief` / `@return` / `@note` 齐备，且 `@brief` 首句写死「不等价于 OS 前台窗」）、`check_no_cjk_literals`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`check_arch_module_map`、`check_test_registry`、`check_no_hardcoded_paths`、`registry_integrity`、`check_version_consistency` 全绿；`lint`（仅改动 TU）与 `format-check` 全绿；按 ARCHITECTURE.md §14.5 补跑 wasm 与 Linux X11/Wayland 两配置；全量 `ctest` 绿。
14. **变异自证**：key_pre 闭包内不记账 → 判据 2 / 4 转红；记账晚于 `shortcuts_.handle` → 判据 4 转红；鼠标 Move 也记账 → 判据 5 转红；`reap_closed` 不置空 → 判据 6 转红；兜底直接回 `AURORA_INVALID_WINDOW_ID` → 判据 1 / 8 转红。

## 回写落点

- `codespec/specification/06-app-platform.md`（§2.4 多窗口：活动窗口查询的「最近一次用户输入派发的目的窗口，不等价于 OS 前台窗」语义、记账入口全集与宿主级直调不记账的边界、记账范围（输入意图类 / Press-only）、四条兜底口径、与 `main_window()` 的分工、os_hotkey 不产生记账的声明；§8.4 快捷键：派发来源与既有匹配语义的边界）
- `codespec/ARCHITECTURE.md`（多窗口与事件链路小节：记账点在派发链上的位置、与 `FocusManager` 逐窗隔离的关系、O(1) 记账口径）
- `codespec/SPECIFICATIONS.md`（§5 特性表 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行：活动窗口寻址一档的现状复核；该行由 CHG-032/033/034 共享，批次末一次合并改写，三份提案互相引用）
- `codespec/manual-test/10-window.md`（取下一个可用序号 TC-WINDOW-017；真机走查项：框架判定的活动窗与 OS 活动态是否一致——无头面读不到，须真机读数一次）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 版本与发布记录：条目类型 Added，落 CHANGELOG.md 的 `[Unreleased]` 节；本批不单独升 alpha 号，四份收口时一次性三处（含 version.h）同步并跑 `check_version_consistency`。该文档不属 codespec 树，按 AGENTS.md 硬规则 5 不列为回写落点。

## 消费侧用法（最小示例）

```cpp
// 命令落点：无记录时回落存活宿主，关闭过的窗不会被返回
const aurora::WindowId target = app.active_window();
if (auto *host = app.window_host(target)) { host->focus_window(); }
// 快捷键闭包内同样可读：记账早于匹配，闭包拿到的是本次派发来源
app.shortcuts().add(combo, [&app] { route_to(app.active_window()); });
```

## 落码次序

本提案与 CHG-034 同改 application.h / application.cpp 两件，双方都触及 `reap_closed`；本提案不再触及 window_host.h，撞点已消。建议次序：CHG-031 → CHG-033 → **CHG-032** → CHG-034。若把 CHG-034 提前，代价是本提案在 `reap_closed` 一处小 rebase（同函数不同段，可接受）；**不建议 CHG-032 与 CHG-034 同批落**。
