# CHG-029 移除 google_play 应用演示：滚动基准负载改为自包含应用形态场景

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-029 |
| 提出日期 | 2026-10-21 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `examples/app/google_play/` 三文件与集成测试 `itest_play_repository.cpp` 删除、`examples/app/` 目录消失；`tools/bench/bench_scroll.cpp` 场景表（删 `google_play`、新增 `app`）、`tools/check/perf_gates.json` 的 scene 与 note（G-1 至 G-4 阈值不变）；`cmake/AuroraDemos.cmake` / `cmake/AuroraTools.cmake` / `cmake/AuroraTests.cmake` 三处；`codespec` 六份文档（含 manual-test 的 `21-debug.md` 整模块作废）。不改公共 API、不改任何已导出 `AURORA_*` 宏口径、不改 CI 门禁面（G-5 至 G-8 计数器门锁在固定确定性场景，与本场景无关） |

## 动机

1. **双重身份耦合，删除决策被基准口径绑架**。`examples/app/google_play/` 三个文件（合计约 112 KB）对外是「应用级 demo 可执行」，对内同时是滚动基准的真实业务树与集成测试的数据层。demo 结构已收敛为 `examples/demos/` 每组件一可执行，`examples/app/` 是目录外仅剩的残留；但树被 `bench_scroll` 依赖，导致「删 demo」不能只删可执行——留树则 `examples/` 下出现没有 demo 的 fixture，连树删则推翻 §7 基线口径。两难的本质是**基准负载寄生在演示资产里**。
2. **资产维护成本高于演示边际价值**。`google_play_ui.h`（81 KB）与 `google_play_data.h`（24 KB）是全库最大的消费者 TU：lint 选集证据（`keycode.h` 的 `readability-string-compare` 从该 TU 报出的实例）、MinGW `-mbig-obj` 决策（该 TU 实测 33614 段）、hybrid-debug 帧率证据（~15 抬至 ~50 FPS 的实测对象）三处工程决策都以它为证据源——任何公共头改动都会重新编译它。
3. **基准方法论去耦合**。自包含的应用形态场景（标准控件 + 两个小型自定义叶，数据本地确定性生成）复刻旧树的页面级负载特征：纵向 `Scroll` 包裹异构 Section、文本密集单元格（吃 freetype 光栅）、`Canvas` 矢量图标、层缓存单元格、自驱动轮播叶（永不停更的落定路径考验）。与旧树刻意不同的是**页面 body 内不再嵌任何可滚动控件**：harness 把每次滚轮派发给视口中心之下最深的 `wants_scroll()` 控件，而 `ScrollProbe` 跟踪的是页面 `Scroll`——旧树内嵌 `GridView` + `LazyRow` 抢走页面滚轮，p99 0.899 ms 大概率测的是内层网格而非页面（内层行程仅 ~84 dp 级，来回弹跳，读数必然虚低）；这一新旧错位正是旧基准失真的根源。新场景让页面 `Scroll` 成为树中唯一滚动器，虚拟化滚动路径改由既有的 `lazy` / `grid` 隔离场景直接驱动根控件覆盖——职责单一、读数干净。
4. **阈值重标定不可避免，且必须诚实标注口径**。G-1 至 G-4 时间类门槛与 `08-tooling.md` §7 的滚动基线历来在 google_play 树上、Win32 软件后端口径实测（p99 best-of 0.899 ms 等）；换场景后在本机无头环境重测。时间类门槛仅本地趋势对照、不进 CI，重标定不改变 CI 判定面；新旧数字不可直接比对将在文档与 JSON note 中注明。

## 变更内容

1. **删除** `examples/app/google_play/` 整个目录（`demo_google_play.cpp` / `google_play_ui.h` / `google_play_data.h`），`examples/app/` 随之消失；`cmake/AuroraDemos.cmake` 的 glob 收回 `examples/demos/*.cpp`，头部注释与 Inspector 接线段同步改写。
2. **删除** 集成测试文件 `itest_play_repository.cpp`——其目标单元即被删数据层，仓库过滤 / 排序的测试覆盖随资产一并退场（无库侧逻辑损失：该文件测的是演示数据的组织，不是库行为）。
3. `tools/bench/bench_scroll.cpp`：删除 `build_google_play_tree` 与 `google_play` 场景分支；新增自包含 `build_app_tree()`——顶栏（标题 + 搜索输入）→ Section 头 → 自驱动轮播叶 `AppBannerCarousel`（墙钟驱动、过渡期间每帧 `mark_needs_paint`，0.35s 入场淡入 + 0.45s 滑移，卡片内容一次性渲染进离屏层、禁 Display List 缓存、每帧仅 composite）→ chip 行 → **静态网格**（72 格 = 18 行 × 4 列等分，子格 `Modifier{}.expand()` 分宽，取代旧 `GridView`）→ **静态推荐行**（4 条目，取代旧 `LazyRow`）→ 评论卡 → 底部导航；自定义叶两个（轮播叶、整格层缓存单元格 `AppCellLeaf`），数据本地确定性生成。**刻意不在页面 body 放任何可滚动控件**（否则内层滚动器抢页面滚轮、重蹈旧树读数失真覆辙），`LazyList` / `GridView` 的自驱动滚动路径由 `lazy` / `grid` 隔离场景覆盖。场景 choices 改 `app/synthetic/lazy/grid/all`；viewport 1100x760 改述为「固定约定」。
4. `cmake/AuroraTools.cmake` 删 `bench_scroll` 的 google_play include 与相关注释；`cmake/AuroraTests.cmake` 删 runner 的 `examples/app/google_play` include 与注释。
5. `tools/check/perf_gates.json`：`scene` 字段改 `app`，note 中记明场景替换的原因（旧树内嵌滚动器抢轮致读数失真）与新无头基线（best-of-3：p99 4.040 / worst 4.171 / jitter 0.703 / long tasks 0，extent 3174dp、6.45 屏、读数 trustworthy），并注明与旧 Win32 实测数字不可直接比对。**G-1 至 G-4 阈值保持不动**——16.67 / 33.3 / 2.0 / 3 是 60fps 帧预算推导值，对新基线有约 4 倍余量，重标定只会把趋势对照的灵敏度削平。
6. codespec 回写见回写落点；manual-test 的 `21-debug.md` 整模块作废删除（唯一载体即被删 demo，DEBUG 热键 F1-F6 / Ctrl+S / Ctrl+P 为该 demo 私有实现，其余 demo 与共享启动器均无）；`manual-test/17-perf.md` 删 TC-PERF-008 及其记录行（载体消亡，叠加层刷新语义由 `itest_perf_overlay_refresh` 自动化覆盖保留；§7.4.1 的叠加层实测数字作为历史记录保留并注明载体已移除）；`manual-test/22-inspector.md` 删 `demo_google_play` 载体行（主载体 `demo_inspector_server` 不受影响）。
7. 「本机实测」证据注释改写：`CMakeLists.txt` hybrid-debug 段、`cmake/AuroraUtils.cmake` MinGW 大 TU 段、`tools/check/lint_db.py`、`tools/check/select_lint_tus.py`、`codespec/BUILD_OPTIONS.md` 选集段、`examples/demos/demo_inspector_server.cpp` 头注释分工段、`src/aurora/perf/scroll_bench.cpp` 落定判据段（改为指向 app 场景轮播叶）。
8. `CHANGELOG.md` 记录本次变更（按惯例不列入回写落点）。

## 验收判据

1. `tools/check/check_change_proposals.py` 全量绿（本提案自身过契约）。
2. 全仓检索 `google_play`：仅余 `CHANGELOG.md` 历史条目、manual-test 历史执行记录行（记录不改写）与本提案——构建系统 / 测试 / 门禁脚本 / codespec 正文零残留。
3. cmake configure 通过；`demo_google_play` 目标不存在；`--target demos` 可配置；`bench_scroll` 构建通过。
4. `bench_scroll --scene app` 在 Release 下运行、读数 trustworthy（moved_frames == frames、reversals ≤ 10% 等自证字段全过），p99 / worst / jitter / long_task_count 四项有效产出并写入 `perf_gates.json` note；G-1 至 G-4 阈值不动（帧预算推导值，对新基线余量约 4 倍），note 注明口径差异。
5. `utest_scroll` 的 `scroll_regression_counter_gates`（G-5 至 G-8，固定确定性场景）全绿——证场景替换不触动 CI 计数器门禁面。
6. `check_manual_test_format` 绿（21-debug 删除、TC-PERF-008 删除后：编号升序、记录行序=声明序、跨模块依赖无悬空）；`check_code_doc_sync` 与 `check_codespec_xref` 绿。
7. 先 `cmake --build` 再跑 ctest：`registry_integrity` 不假绿（`itest_play_repository` 删除后套件登记同步），全量测试绿。
8. `--target demos` 抽验至少三个 demo 目标可编译——证 AuroraDemos glob 收敛后其余 demo 不受影响。

## 回写落点

- `codespec/specification/08-tooling.md`：§7 基线口径、§7.4.1 载具表与复现步骤、工具表 bench_scroll 行
- `codespec/GUIDELINE.md`：bench_scroll 示例命令
- `codespec/manual-test/17-perf.md`：删 TC-PERF-008 及记录行
- `codespec/manual-test/22-inspector.md`：删 demo_google_play 载体行与分工表述
- `codespec/BUILD_OPTIONS.md`：lint 选集证据段
- `codespec/changes/scroll-bench-app-scene/proposal.md`：状态按 已提议 → 实施中 → 已归档 流转
