# CHG-065 命中链派发统一按生命周期守卫取用控件

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-065 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.WIDGET.FLAT-COMPONENTS.001 |
| 影响面 | `src/aurora/event/dispatcher.cpp`（滚轮 / 悬停 / 光标解析三处取用口径）、`include/aurora/event/dispatcher.h`（悬停链注释口径）、`tests/unit/utest_dispatcher.cpp`（回收态与回调内销毁判据）；**不改 `HitNode` 公共契约、不改派发入口签名、不改滚动余量上冒与悬停 diff 语义、不动 `hit_test` 兜底路径**；公共 API 结构无差异 |

## 动机

1. **规格已经把口径写死，代码没有兑现。** `codespec/specification/04-widget.md` §6.1 的 `HitNode` 生命周期契约明确区分两种取用：`get()` 只可用于「不解引用」的用途，`lock(out_keepalive)` 才把强引用延长至调用方作用域结束；同节验收标准写的是「命中链的解引用路径全部经 `lock()` 持有强引用」。`src/aurora/event/dispatcher.cpp` 里指针与触控的五条链路径（`deliver_chain`、`focus_target_of`、`broadcast_touch`、`deliver_synthesized`）已按此实现，滚轮、悬停、光标解析三条链路径没有。

2. **滚轮主路径完全不判活。** `dispatch(Widget&, ScrollEvent&)` 沿命中链反向遍历时直接读 `HitNode::ptr` 再调 `wants_scroll()` / `on_scroll()`。链上由 `shared_ptr` 持有的控件被虚拟列表回收后，`ptr` 仍是非空裸指针而 `guard` 已失效，派发器会对已释放对象做虚调用。只读探针在同一回收态下同时读到 `ptr != null` 与 `get() == null` / `lock() == null`，把取用换成判活读数判定立刻翻转——这是可确定性区分的证据，不是「多读几次看崩不崩」。

3. **另两处判了活却没持住强引用。** `update_hover` 与 `resolve_cursor` 用 `get()`：`get()` 对受守卫的控件确会判活，但其文档明确限定只用于不解引用的比较用途，且 `guard.lock()` 产生的临时 `shared_ptr` 在函数返回时即析构。两处随后分别调 `on_hover_change()` 与 `modifier()` / `cursor_shape()`，并继续读 `modifier()` 返回的引用与 `mod.has_clickable()`；回调内若丢掉该控件最后一个强引用（页面重建、列表复用），回调返回后基类仍要写自身成员，即 use-after-free。`update_hover` 的文件头注释还自称「`lock` 返回空即跳过，绝不解引用悬垂指针」，与实现自相矛盾。

4. **现有动态检测驱动不到这三条路径。** 真实窗口 `etest_*` 用例带 `LABELS e2e`，被 ASan 作业的 `-LE e2e` 排除；一次 454 例 ASan+UBSan 全量 ctest 零内存报告，恰恰说明这些面没有被插桩跑到。`tests/unit/utest_dispatcher.cpp` 只有良性树上的悬停 diff、光标优先级、滚轮兜底与余量上冒用例，既没有回收态节点，也没有「回调内销毁自身」的判据。

5. **本提案的边界。** `hit_test` 兜底路径返回的是即时命中的裸指针，手上没有 `HitNode` 守卫可用，本提案不动它、也不改其「无可滚动者则回落点命中目标」语义；`contains` 一类只比较指针、不解引用的读法继续用 `get()`，符合其文档限定。`HitNode::get()` / `lock()` 的契约与全部派发入口签名保持原样，只把三条链路径的取用方式对齐到已有契约。

## 变更内容

1. 滚轮主路径把 `it.ptr` 换成 `lock(keepalive)`：在循环体内声明 `std::shared_ptr<Widget> keepalive`，`lock()` 返回空即 `continue`，强引用覆盖 `wants_scroll()`、`on_scroll(e)` 与其后的余量判定；`has_scrollable` 只在确实取到存活控件后置位，链上全为已回收节点时仍按既有语义落到兜底路径。
2. `update_hover` 的旧链、新链两轮遍历逐个节点改用 `lock(keepalive)`，强引用覆盖 `on_hover_change()` 调用；`contains` 内「是否同一控件」的比较仍用 `get()`，不改「先离开再进入」与「只在形状变化时回调」的顺序语义。
3. `resolve_cursor` 改用 `lock(keepalive)`，强引用覆盖 `modifier()` 返回引用的整段读取、`cursor_shape()` 虚钩子与 `has_clickable()`；优先级「修饰链声明 > 控件虚钩子 > 含 Clickable」与链空回落 `Arrow` 均不变。
4. 更新 `src/aurora/event/dispatcher.cpp` 的悬停段注释与 `include/aurora/event/dispatcher.h` 的 `hover_chain_` 注释，使「经 `lock()` 判活并持住强引用」的表述与三条路径一致；不改 `include/aurora/widget/widget.h` 里 `HitNode` 的契约文字。
5. `tests/unit/utest_dispatcher.cpp` 补三组判据：① 经可覆写的命中链钩子注入一条「`guard` 已失效、但所指控件对象仍存活」的节点，滚轮派发必须跳过它并把事件交给更浅的存活可滚动祖先；② 悬停离开回调内丢掉控件最后一个强引用，回调内观察到的析构标志必须为假；③ 光标解析虚钩子内同样销毁自身，回调内观察到的析构标志必须为假。每组都带存活态对照组，证明判据不是恒真断言。
6. `CHANGELOG.md` 的 `### Fixed` 记一条命中链取用口径收口；规格回写见下节。

## 验收判据

1. 回收态滚轮判据可证伪：失效节点所指控件的 `on_scroll` 调用数为 0、更浅存活祖先为 1，返回值仍为「命中可滚动目标」；把该处 `lock()` 改回 `it.ptr` 后断言必须转红。
2. 悬停判据：离开通知确实送达（通知计数 +1），且 `on_hover_change(false)` 回调内观察到的析构标志为假；把该处 `lock()` 改回 `get()` 后断言必须转红。
3. 光标判据：光标形状仍按链上声明正常解析，且 `cursor_shape()` 虚钩子内观察到的析构标志为假；把该处 `lock()` 改回 `get()` 后断言必须转红。
4. 三处真注入变异分别令对应判据转红、还原源码后复跑全绿；三条存活态对照组全部通过，证明判据不是恒真断言。
5. 既有 `tests/unit/utest_dispatcher.cpp` 全部用例零改动全绿：悬停进入 / 离开 diff、光标形状优先级、滚轮兜底与余量上冒、指针捕获、连击判定等语义逐位不变。
6. 公共面零签名变更：`HitNode::get()` / `lock()`、`EventDispatcher` 各 `dispatch` 重载、`aurora_api.json` 的公共 API 结构均无差异；改动 TU 的增量 lint、format-check 与相关单测全绿。

## 回写落点

- `codespec/specification/04-widget.md`：§6.1 的 `HitNode` 生命周期契约段补写「滚轮 / 悬停 / 光标解析三条链路径同样一律经 `lock()` 持强引用」，并把该段验收标准与派发器实现逐处对齐。
- `codespec/specification/05-event-navigation.md`：§3.3 的滚轮派发条目补写「遍历先按守卫判活、已回收节点跳过」，同时声明余量上冒与无可滚动者时的兜底语义不变。
- `codespec/SPECIFICATIONS.md`：在扁平组合模型行的说明里补一句「命中链解引用一律经 `lock()` 持强引用」，使特性表与规格正文口径一致。
