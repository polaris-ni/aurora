# CHG-043 虚拟列表的三条公共入口：条目刷新、末尾锚定、偏移变更通知

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-043 |
| 提出日期 | 2026-10-27 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.WIDGET.SCROLL-INTERACTION.001 |
| 影响面 | `include/aurora/widget/lazy_list.h`（三枚公共入口 `invalidate_items()` / `scroll_to_end(bool)` / `set_on_scroll_changed(std::function<void(float)>)` 与两枚私有成员位；另含同文件 `tick_gestures` 的一处既有缺陷修正，见实施记录）、`tests/unit/` 新增一枚测试 TU；`codespec/` 三处回写 |

## 动机

1. **三条都不是「没实现」，而是「实现了但没有公共入口」。** 读源实测（`include/aurora/widget/lazy_list.h`）：live 条目的重建逻辑在 `on_layout` 里就有（按 `visible_range()` 回收滚出项、对窗口内缺失下标跑 `builder_`），但它只对**缺项**动手——`live_` 里已有的下标一律复用，于是换了 builder 也照旧显示旧内容；「落到末尾」的能力藏在 `deserialize_props` 写 `pending_offset_`、`maybe_restore_scroll()` 于首次可滚动布局时夹取应用这条序列化通路里；偏移的 `State<float>` 信号也确实存在，但 `SignalViewBase` 只有 `subscribe(Effect&)` 一条订阅路，`collect_signals` 又返回空表，应用侧不进 `Effect` 作用域就只能自轮询。
2. **持续追加内容的列表是虚拟化的主战场，而它恰好要这三条同时成立。** 日志 / 终端 / 监视卡这类「行只增不减、视图跟尾」的形态，每来一批新行都要：让已实例化的行吃到新内容（刷新）、把视口停在末尾（锚定）、知道用户是否已经手动滚开（跟尾冻结／恢复的判据）。缺任何一条，消费方就得整卡重建或逐帧轮询——本仓库外部消费者今天正是这么代偿的。
3. **代偿形态都有代价且不可持续**：整棵可见子树重建（丢掉条目内部状态、每帧建 tree）、把「首帧落底」写成一条极大偏移的 JSON 属性（借序列化面表达运行期意图，读代码的人看不出那是意图而不是数据）、在 `tick()` 里比较 `scroll_offset()` 与 `max_scroll_offset()` 判「到底了没有」（帧驱动，恢复跟尾最慢晚一帧，且每个使用者都要各写一遍）。
4. **布局前无法表达「落到末尾」是硬事实**：`scroll_to()` / `scroll_to_item()` 的目标一律经 `max_scroll_offset()` 夹取，而它依赖 `viewport_height_`，首次布局前为 0——此时 `max_scroll_offset()` 等于**整个内容高**而不是末尾偏移，直接调用会把偏移停在错误的地方（差一个视口高），或压根无处可去。故「末尾」必须是**意图**、由首次可滚动布局按当时的真实范围夹取，这与既有 `pending_offset_` 的做法完全一致。
5. **不改任何既有语义**：`set_count` 的「存活条目保值」、`restore_key` 恢复、snap / paging、无障碍滚动三量、`visible_range()` 含 `cache_extent` 的窗口口径全部逐位不变；本批只加三枚入口与其实现所需的私有位。
6. **刷新不得在调用栈内销毁控件**：条目刷新常在数据回调里发生，而同一栈里可能正在向被回收的控件派发事件（本仓库既有的一族 use-after-free 缺陷都长这个形状）。因此刷新只置位，真正的回收排在下一帧布局。

## 变更内容

1. **`LazyList::invalidate_items()`——条目内容刷新（甲＝显式失效）**：置 `invalidate_pending_` 并标布局脏；**下一帧** `on_layout` 在计算可见窗口之前清空 `live_`，随后按当前窗口与**当前 builder** 重建。口径：
   - 只在下一帧布局生效，调用栈内不建任何树、不销毁任何控件；
   - `count` / `offset` / `scroll_offset()` / `restore_key` / snap 一律不动（刷新的是内容，不是位置）；
   - 焦点落在被回收条目上的处置与 `set_count` 的回收**同口径**——焦点随之失效（`FocusManager` 持弱引用），宿主若需保留焦点应先移走；
   - 主线程调用（与其余控件写入口一致）；
   - 与 `set_count` 的分工写进头注：那一枚动**总数**（范围内条目保值），本枚动**内容**（全部重建）。
2. **`LazyList::scroll_to_end(bool animate = false)`——末尾锚定，布局前即可表达（甲＝一次性待定偏移）**：
   - **尚未布局**（`viewport_height_ <= 0`）：把「末尾」记成显式待定偏移（复用既有 `pending_offset_` 这一枚单槽，写入一枚命名常量 `AURORA_END_OFFSET_SENTINEL`），由**首次可滚动布局**在 `maybe_restore_scroll()` 里按当时的 `max_scroll_offset()` 夹取落地。走的是与 `deserialize_props` 完全相同的一条路径与一个优先级（显式意图 > `restore_key`），因此不新增第二套恢复时序，也不与既有 `scroll_restored_` 判据打架；
   - **已布局**：等价于 `scroll_to(max_scroll_offset(), animate)`——`animate` 与 reduce-motion 的关系沿用 `scroll_to` 既有分支，不另造滑动；
   - **空表 / 零行高 / 零视口**：夹取结果为 0，不产生任何伪变化，也不触发偏移通知；
   - 与 `restore_key` 同时存在时按既有优先级取显式意图（文档写明，沿用现状不藏优先级）。
3. **`LazyList::set_on_scroll_changed(std::function<void(float)>)`——偏移变更通知（甲＝回调）**：在偏移**实际落位**处调用一次（参数为新偏移），挂点选在既有两条唯一写路径上：`apply_offset()`（滚轮、滑动帧、拖入滚边、`set_scroll_offset`、无障碍滚动、snap 收位）与 `apply_restored_offset()`（恢复与待定偏移落地）。口径：
   - 去重天然成立——两条路径都以「夹取后与当前值相同则直接返回」开头，同值 `set_scroll_offset` 不触发；
   - 滑动期每帧一次（与 `offset_signal` 的发布节奏同源，不额外广播）；静止与空闲节流期零调用；
   - 主线程、随控件销毁自然失效（回调是本控件成员，不需要注销句柄）；置空即注销；
   - 与 `offset_signal()` 并存不冲突：前者是给响应式树的只读信号，后者是给命令式宿主的旁听口，两者都由同一次落位驱动。
4. **三条共同口径**：只置位或只走既有落位路径，不在调用栈内跳转；不改 `collect_signals`（仍返回空表——那是控件树批量采集的面，本批不扩）；不把 `offset_signal()` 改成每帧广播。
5. **分层与预算**：全部落 `include/aurora/widget/lazy_list.h`（头内联，与同文件既有 setter 同形态），不新增源文件、不新增 widget、不引入属性键，`aurora_api.json` 的 schema 面（widgets / enums / layout_rules / state_patterns / error_codes / debug）预期**零差异**——本批只加成员方法与私有位，控件条目与属性表都不变。

## 验收判据

1. **刷新**：100 行、视口可容 10 行；首布局后改数据闭包并调 `invalidate_items()` ⇒ **本帧调用栈内**可见项仍为旧内容（`live_item_count()` 未变、无建树），下一帧布局后可见项文本为新值、`live_item_count()` 仍是窗口量级、`scroll_offset()` 逐位不变。
2. **刷新不动位置**：`invalidate_items()` 不改 `count` / `max_scroll_offset()` / `visible_range()` / `restore_key`；`set_count` 的「范围内存活条目不重建」既有判据零改动全绿（这是两枚入口的分工边界）。
3. **末尾锚定**：建树期（首布局前）调 `scroll_to_end()` 再挂树 ⇒ 首次布局后 `scroll_offset() == max_scroll_offset()`（不是内容高、不是 0）；空表时落 0；布局后再调 ⇒ 立即落到底且只发一次偏移通知；与 `restore_key` 同时存在时取显式意图（按既有优先级）。
4. **通知**：滚轮一次、滑动若干帧（每帧一次）、程序化 `set_scroll_offset`、无障碍 `accessibility_scroll_to`、拖入滚边各触发恰一次；**同值** `set_scroll_offset` 不触发；夹取到同值（已到底再请求更大）不触发；置空回调后不再触发；回调内收到的参数等于回调后 `scroll_offset()` 的读数。
5. **既有行为不动**：`serialize_props` / `deserialize_props` 九键往返、snap / paging 收位、`offset_signal` 的 `Effect` 订阅、`visible_range` 与无障碍三量读数逐位不变，既有用例（`utest_lazy_list` 一族）全绿。
6. **变异自证**：至少三条改坏各自转红——把 `on_layout` 的 `live_.clear()` 摘掉（刷新不生效）、把 `scroll_to_end` 未布局分支改成直接 `scroll_to(max_scroll_offset())`（布局前落到内容高而非末尾）、把通知挂点从 `apply_offset` 摘走（回调空转）。改坏即还原复跑。
7. **回归与门禁**：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移；`check_naming_conventions` / `check_doc_comments` / `check_code_doc_sync` / `check_codespec_xref` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest --preset ninja-test` 无新增红灯。

## 回写落点

- `codespec/specification/04-widget.md`（虚拟化容器一节：`set_count` 段旁补 `invalidate_items` 的「动内容 vs 动总数」分工与时机；滚动恢复段补 `scroll_to_end` 的布局前意图与优先级；辅助 API 表补三枚入口）
- `codespec/SPECIFICATIONS.md`（`SPEC.FEAT.WIDGET.SCROLL-INTERACTION.001` 需求行补「虚拟列表条目刷新 / 末尾锚定 / 偏移变更回调」一句与其落点锚）
- `codespec/specification/05-event-navigation.md` §5.2（自动滚边协议补「滚边态与收位滑动态互不抢占」一条——落码期发现的既有缺陷口径，见实施记录）
- 仓库根的版本记录进 Unreleased 的 Added 节（三条入口）与 Fixed 节（滚边拽回缺陷）；API schema 生成物由生成器重建后应为零差异（本批不新增控件、不新增属性键）

## 实施记录与偏离（落码期回写）

1. **形态与提案一致，三枚入口都落头文件内联**（与同文件既有 setter 同形态），私有位为 `invalidate_pending_` 与 `on_scroll_changed_`，另加一枚命名常量 `AURORA_END_OFFSET_SENTINEL` 表「末尾」在待定偏移单槽里的形态（不用裸字面量，是为了让 `scroll_to_end()` 与 `deserialize_props` 共用同一条路径时读得出意图）。
2. **刷新落地点在 `on_layout` 内、`maybe_restore_scroll()` 之后、算可见窗口之前**：与恢复同帧但不互相打断（恢复定的是位置，刷新定的是内容），随后既有那段「回收滚出项 + 为缺失下标跑 builder」原样把整窗口重建出来，未新增第二条建树路径。
3. **待定偏移单槽复用带来一条附带口径**：`scroll_to_end()` 未布局分支同样把 `scroll_restored_` 置 `false`（与 `deserialize_props` 的显式偏移同处置），否则该意图会被「已就位」挡住、永不落地。
4. **通知只挂在两条落位路径末端，不新增第三条广播**：挂点排在偏移赋值与脏标记之后，回调内读 `scroll_offset()` 与参数同源。这条选择在变异自证里得到正向确认——把挂点从 `apply_offset` 摘走（保留 `apply_restored_offset`）后有 6 例转红，正说明滚轮 / 收位滑动帧 / 程序化跳转 / 无障碍 / 拖入滚边这批来源全部只经 `apply_offset`，而待定偏移落地只经 `apply_restored_offset`：两条都挂才能兑现「来源不限、各恰一次」这句口径。
5. **落码期发现并修正一处既有缺陷（超出提案范围，但为验收判据 4 所迫）**：`tick_gestures` 的早退判据原是「滑动与滚边**都**不活跃」，于是只有滚边在途的那一帧仍会落入下面的滑动块；未激活的 `ScrollGlide::tick` 交回的是**陈旧终点**（从未滑动过即初始 `to == 0`），该值随即经 `apply_offset` 写回，把滚边本帧刚推进的偏移盖回旧值——即「拖文件到列表边缘，列表反而跳回开头」。这条不修，「拖入滚边触发恰一次通知」的判据无法成立（实测为两次且终值归零）。`Scroll::tick_gestures` 早已带这段保护（其注释正写明同一危害），故本控件属同族缺陷漏改，修正形态与 `Scroll` 逐字同形（不引入第三种写法）。既有 25 例 `utest_lazy_list` 当时不覆盖这条路径，故新增回归用例而非改动既有用例。
6. **文档落点在 §3.3 而非提案预计的 §3.4**：虚拟化那三段（`set_count` 运行期入口 / 滚动位置保存恢复 / 滚动增强契约）实际位于 `04-widget.md` §3.3 布局容器，§3.4 列表与虚拟化只有属性表；故三段新文并进 §3.3，§3.4 的 `LazyList` 表行同步补三枚辅助 API，需求行的锚随之写 §3.3、§3.4 两处。
7. **验收判据逐条兑现**：判据 1—5 由 `utest_lazy_list_runtime_entries` 12 例覆盖（全绿）；判据 6 的三处变异各自转红——摘掉 `live_.clear()` → 刷新两例转红、未布局分支改成直接 `scroll_to(max_scroll_offset())` → 锚定四例转红、通知挂点从 `apply_offset` 摘走 → 通知六例转红；另补第四处变异（还原 `tick_gestures` 旧早退判据）→ 滚边两例转红，四处均改坏即还原复跑。判据 7：`aurora_api_json` 重建后 `git status` 对 `aurora_api.json` **零差异**，全量 `ctest --preset ninja-test` 无新增红灯，`lint`（改动相关 TU）与 `format-check` 绿。
8. **一处测试自身的坑已就地修正并留档**：断言里 `log.got.back()` 在变异转红（通知为空）时是越界访问而非干净失败（实测 `RUN_EXIT=139`），改为 `NotifyLog::last()` 空表回不可达哨兵值——判据要能在被改坏时**报出**而不是**崩掉**，这条对后续所有「记次数再取末值」的台账同样适用。
9. **未做的事**：不给 `Scroll` / `LazyRow` / `GridView` 补同名入口（任务书只点虚拟列表，且三者的偏移写路径各不相同，`Scroll` 的滑动帧甚至不走 `apply_offset`）；不把 `offset_signal()` 改成每帧广播；不动 `collect_signals`；不新增序列化键。

