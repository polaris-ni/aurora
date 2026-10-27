# CHG-041 应用级「系统明暗偏好变更」通知与当前档读点

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-041 |
| 提出日期 | 2026-10-27 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `include/aurora/app/window_host.h`（新增 OS 明暗上报表 `set_os_scheme_sink`）；`src/aurora/app/window_host.cpp`（`on_os_color_scheme` 转发 + `teardown` 解绑）；`include/aurora/app/application.h`（`os_color_scheme_state()` / `set_on_os_color_scheme()` 公共读点与聚合成员）；`src/aurora/app/application.cpp`（`refresh_os_color_scheme()` 定义 + 逐宿主装表）；`aurora_api.json` 重建；`tests/unit/` 新增一枚测试 TU；`codespec/` 三处回写 |

## 动机

1. **框架已经检测到了，但应用看不见。** Win32 后端在 `WM_SETTINGCHANGE` 里重读注册表、与宿主自己的档位基线比对后逐宿主上报（`win32_host.cpp` 的 `handle_setting_change`），上报槽是 `Surface::set_os_color_scheme_handler`。这条链路目前只有框架自己消费：`WindowHost::on_os_color_scheme` 推 `Window::set_os_color_scheme` 并整帧重绘，参与默认根主题的三级优先链。应用侧既拿不到「现在是亮还是暗」的公共读点，也挂不上通知——`Surface` 那一枚 handler 是**独占单槽、赋值即替换**，`WindowHost` 在 `attach_surface` 已占，应用再装就顶掉框架的根主题跟随。
2. **消费方被迫自轮询注册表。** 实测到的代偿形态是装配层用 `Scheduler` 挂 1 s 间隔轮询自由函数 `aurora::os_color_scheme()`：代价是 Windows 上每秒一次注册表读、最多 1 s 的换档延迟，而且轮询读到的值与框架此刻正在用的值不是同一份真值源（前者每次现读、后者是宿主推入的快照）。框架已有事件源，本不该让应用去猜。
3. **缺的是「一条腿」，不是新机制。** 检测、去重、上报、根主题解析全部已落；本批只加「应用级会看到」这一层：既不改 Win32 的逐宿主基线去重，也不改 `Window` 的优先链与重绘口径。落地形态取「应用级 `State<ColorScheme>`」，与既有 `window_state()` / `window_mode()` 同构（响应式读点 + 命令式回调并存），因为这条同构在仓库里已经有两份先例、也有现成的观察者体系可接。
4. **聚合范围与既有两枚 State 有意不同，必须写清。** `window_state()` / `window_mode()` 只投影主窗口（`register_host` 里 sink 判 `raw == main_host_`）；OS 偏好是**进程级事实**，N 扇窗各自收到同一次系统切换应当只算一次应用级通知。故本批的 sink 对**全部宿主**安装，去重靠转发处的先比后设——不需要额外的跨窗记账位。把「只投影主窗」的旧口径照搬过来会是错的：非主窗先收到切换时应用级读点不动。
5. **`State::set` 对相等值照样发信号**，去重不能指望 State 本身（`state.h` 的 `set` 无条件 `notify()`），必须在聚合出口比。既有 `on_window_state_changed` 已是同款先比后设，本批沿用同一形态。
6. **初值不给隐式 seed。** `platform()` 的既有口径是「显式查询，跳过探测」（读偏好有注册表/文件副作用，`platform()` 不代做）。同理，`Application` 构造与宿主登记不为应用级 State 隐式读注册表：初值恒为 `Unspecified`，启动期解算由应用显式调 `refresh_os_color_scheme()`。宿主登记时框架自己那一读（推给 `Window` 的既有行为）不进应用级 State，两条口径由此分开。
7. **框架只报 OS 原值。** 上报含 `Unspecified`，不替应用回落明/暗——「系统不表态回哪一档」是应用策略（消费方的三档外观设置属于它自己的主题层）。框架若在此处回落，应用就永久失去区分「系统说亮」与「系统没说」的能力。

## 变更内容

1. **宿主侧上报表**：`WindowHost` 新增 `set_os_scheme_sink(std::function<void(ColorScheme)>)`，与同类的 `set_state_sink` / `set_mode_sink` 同款（声明在公开接线区、成员在私有区）。`on_os_color_scheme` 的既有两半（推 `Window` 快照 + `force_full_redraw`）原样保留，在其后追加一次转发；`teardown()` 把新表与另两枚一并置空（回调捕获 `this`，消息泵在宿主回收后仍可收到 `WM_SETTINGCHANGE`）。
2. **应用级读点**：`Application` 新增响应式读点 `os_color_scheme_state()`（返回 `State<ColorScheme> &`，初值 `Unspecified`）与命令式回调注册 `set_on_os_color_scheme(std::function<void(ColorScheme)>)`，二者并存的关系与 `window_state()` / `set_on_window_state()` 一致。命名带 `_state` 后缀而不直写 `os_color_scheme()`：同名自由函数 `aurora::os_color_scheme()`（`window/platform.h`，显式查询腿）已在同一命名空间存在，成员与它重名会让「读快照」与「现读注册表」两件不同的事在调用点无法区分。
3. **聚合出口**：私有 `on_os_color_scheme_changed(ColorScheme)` 先比后设，值变化才 `set` 并触发回调；由 `register_host` 对**每个**宿主安装的 sink 调用。跨宿主去重因此是「同值第二次数值相等 ⇒ 静默」的自然结果。
4. **显式 seed**：`Application::refresh_os_color_scheme()` 定义在 `application.cpp`（公共头不引 `window/platform.h`），现读 `aurora::os_color_scheme()` 后走**同一条**聚合出口——幂等与「变化才通知」两条口径由复用出口自动成立，不在 seed 腿里另写一遍比较逻辑。
5. **不动的既有面**（明确列出以防被当成顺带改动）：`Window` 的三级优先链与 `resolved_root_theme()`、`WindowHost::attach_surface` 创建窗口时的那一次 `set_os_color_scheme(os_color_scheme())` 推送、`force_full_redraw`、Win32 `handle_setting_change` 的逐宿主基线去重、`Surface::set_os_color_scheme_handler` 的独占单槽语义（本批不改成链式或多播——应用侧有了旁听入口，无需抢那一枚槽）。
6. **不引入**：平台线程或后台探测线程（通知仍在消息泵线程同步转发）；chrome 色值（属应用主题层）；「系统明暗」与「应用明暗」的合并概念（应用仍可 `Window::set_theme` 显式覆盖）。

## 验收判据

1. **跨宿主聚合**：两扇无头窗，`emit_os_color_scheme_change(Dark)` 打在任一窗上 ⇒ 应用级观察者恰一次、State 值 `Dark`；另一窗再打同值 ⇒ 仍恰一次。反向补一条：非主窗上报不同值照常通知（证明聚合不是「只投影主窗」的旧口径）。
2. **同值去重与顺序**：同窗重复 emit 同值不产生第二次通知；`Light` 再 `Dark` 得两次且顺序正确；`Unspecified` 作为上报值在未 seed 时被同值去重吸收（不产生噪声通知）。
3. **seed 与无头**：`refresh_os_color_scheme()` 后 State 等于自由函数实值；连调三次至多一次通知（通知次数由「实值是否区别于 `Unspecified`」推得，判据不随真机档位漂移）。未 seed 且未上报时读 `Unspecified`，含带窗宿主登记与混合宿主（无头 + 带窗）场景。
4. **主线程**：回调在 emit 的调用线程被调（用例主线程 emit、断言线程 id 相等），不引入新线程。
5. **框架行为不动**：emit 仍把档位推入 `Window` 并整帧置脏（基线自证：先渲染消费残脏，再断言 emit 后置脏为真）；`teardown()` 后框架腿与应用级上报表同时解绑（断言「无脏」与「无通知」两条，否则与「从未接线」不可区分）。既有根注入用例 `itest_theme_root_injection` 逐位不变、全绿。
6. **变异自证**：把 `register_host` 的 sink 改回「只装主窗」⇒ 跨宿主用例转红；删掉聚合出口的先比后设 ⇒ 同值去重用例转红；删掉 `on_os_color_scheme` 的转发 ⇒ 聚合与顺序用例转红。改坏即还原复跑。
7. **回归与门禁**：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移、`check_api_budget` 增量通过；`check_naming_conventions` / `check_doc_comments` / `check_code_doc_sync` / `check_codespec_xref` / `check_core_layer_boundary` / `check_platform_macros` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest --preset ninja-test` 无新增红灯。

## 回写落点

- `codespec/specification/06-app-platform.md`（§2.2 接口表补一行 + §6.2「上层聚合」段写清 OS 偏好与可见性/几何态的聚合范围差异、初值与 seed 口径；§5「OS 明暗偏好查询」段申报「运行期跟随不要轮询现读函数」）
- `codespec/specification/07-environment-modifier.md`（§5.1「OS 偏好的推入路径」段补应用级转发：谁上报、在哪去重、原值透传不回落）
- `codespec/SPECIFICATIONS.md`（`SPEC.FEAT.APP.MULTI-WINDOW.001` 需求行补「应用级 OS 明暗聚合读点」一句）
- 仓库根的版本记录与 API schema 生成物同批刷新：版本记录进 Unreleased 的 Added 节，schema 由生成器重建后应为**零差异**（其表面向 widgets / enums / layout_rules / state_patterns / error_codes / debug，`Application` 与 `WindowHost` 的成员方法不入表，`ColorScheme` 枚举条目未变）

## 实施记录与偏离（落码期回写）

1. **形态选甲并叠加乙的回调**：应用级 `State<ColorScheme>` 读点与命令式回调并存（与 `window_state()` / `set_on_window_state()` 的既有并存关系同款），`refresh_os_color_scheme()` 两条形态共用。派发侧给的甲 / 乙在此不是二选一——并存的成本只是一枚 `std::function` 成员，而只给 State 会让非响应式装配层（直接在帧外用命令式回调的既有消费者）多绕一层 `Effect`。
2. **读点命名带 `_state` 后缀**：`os_color_scheme_state()` 而非 `os_color_scheme()`——同一命名空间已有自由函数 `aurora::os_color_scheme()`（`window/platform.h`，无缓存现读），成员与它重名会让「读聚合快照」与「现读注册表」在调用点无法区分，而这两件事正是本条要分开的两半。
3. **聚合出口复用**：`refresh_os_color_scheme()` 不自己比较与通知，改调与后端上报同一条 `on_os_color_scheme_changed`——幂等与「变化才通知」由复用自动成立，两处口径不会漂移。真机档位随环境而变，故 seed 用例的判据写成「State 等于自由函数实值 + 通知次数由实值是否区别于 `Unspecified` 推得」，任何 runner 上都确定。
4. **验收判据兑现情况**：第 1—5 条由新增 `utest_app_os_color_scheme` 的 8 例兑现（未 seed 读 `Unspecified` 并含上报 `Unspecified` 被吸收、跨宿主恰一次 + 非主窗反向补证、同值去重与 `Light → Dark` 序、seed 幂等、主线程、`Effect` 订阅重跑、框架腿置脏 + `teardown` 双解绑、无头与带窗混合宿主）；第 6 条变异自证见下条实测；第 7 条回归与门禁全绿（全量 ctest 447/447、`check_api_schema_sync` 零漂移、`format-check` 1220 文件 0 非合规）。既有 `itest_theme_root_injection`（6 例）零改动全绿。
5. **变异自证三条均已实跑**：把 `register_host` 的 sink 改成「只装主窗」⇒ `emit_on_either_window_notifies_once` 与 `headless_host_without_window_still_aggregates_nothing` 转红（2 红）；摘掉聚合出口的先比后设 ⇒ 去重相关 5 例转红（含 seed 幂等与 `Effect` 重跑）；删掉 `WindowHost::on_os_color_scheme` 的转发 ⇒ 除「未 seed」外的 7 例全转红。三条改坏后均还原本文件并复跑回绿（8 通过），用例非空转。
6. **同批修掉一处 HEAD 编译阻塞**：`src/aurora/window/detail/menu_bar_translate.cpp` 用了 `std::optional` 却未含 `<optional>`（上一批原生菜单栏桥的漏项，MinGW 腿直接编译失败、挡住本批全部构建）。与本条无关，只补该 include，不动其它。
