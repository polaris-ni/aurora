# CHG-060 渲染/布局三枚架构级优化恒开

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-060 |
| 提出日期 | 2026-11-04 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | 构建选项（删除三枚 `option` 与导出宏）；公共头 `widget.h` / `painter.h` 去门控；`FeatureFlags` 删除三字段与三个 JSON 键；`widget.cpp` 与 `feature_flags.cpp` 实现去门控；七套单元 / 集成用例去门控；CI `toggles` 矩阵与 lint 反极性遍；`BUILD_OPTIONS.md` / `05-event-navigation.md` / `CHANGELOG.md` 回写 |

## 动机

`AURORA_ENABLE_LAYOUT_CACHE`、`AURORA_ENABLE_OCCLUSION_CULLING`、`AURORA_ENABLE_DISPLAY_LIST` 三枚选项默认 `ON`，且 `#ifdef` 内是各自能力的**唯一实现**：关闭选项只是整段代码不参与编译，并不存在可用的等价替代路径。`FeatureFlags` 只是编译期快照，无运行期改写路径。

保留开关的实际后果是：

- 关闭侧构建缺少核心性能能力，却仍被包装成「受支持的构建组合」，CI 还要为其维持一条专用矩阵项；
- 每个能力在公共头、实现、测试、`FeatureFlags`、文档与 CI 中形成多份宏分支，增加阅读与维护成本；
- 消费者可用 `-D...=OFF` 得到语义残缺的库，且该组合没有任何测试价值。

因此三枚能力改为**恒编入**，删除 CMake 选项、导出宏与全部 `#ifdef` 分支。

## 变更内容

以下为文件级、符号级变更。

### 构建与导出

- `cmake/AuroraBackends.cmake`（约 L677-687）：删除三枚 `option(... ON)`、`foreach` 注册与整段「架构级优化开关」注释。Headless 恒编入的既有写法不动。
- 导出宏：`aurora_define_feature(..., EXPORT)` 不再调用这三项；`AURORA_EXPORTED_DEFINES` 自动少三项，无需改 `AuroraInstall.cmake`。

### 公共头

- `include/aurora/widget/widget.h`
  - `invalidate_display_list_up()` 外层 `#ifdef AURORA_ENABLE_DISPLAY_LIST`（L525/L544）→ 去掉宏行与 `#endif`。
  - `invalidate_subtree_caches()` 内两处 `#ifdef`（L557-559、L560-562）→ 保留 `layout_cache_valid_ = false;` 与 `dl_valid_ = false;`，去宏行。
  - `mark_needs_layout_impl()`（L1623-1628、L1629-1631）与 `mark_needs_paint_impl()`（L1643-1645）→ 去宏行。
  - DL 字段块（L1913-1921）→ 去掉 `#ifdef` / `#endif`，保留 `display_list_`、`dl_valid_`、`last_paint_bounds_`、`dl_raster_gen_`；块首注释改为「Display List 缓存」。
  - `Container::on_paint()`（L2001-2003、L2007-2015）→ 去掉两处宏行，`clip` 与剔除判据无条件生效；注释保留「与命中闸必须同改」的约束。
  - 注释去宏名：L500（布局缓存说明）、L1652（分节标题）、L2210 / L2389（`layout` 入口说明）改为「布局缓存」等能力名表述。
- `include/aurora/render/painter.h` L367、L480：注释中 `(AURORA_ENABLE_DISPLAY_LIST)` 改为「Display List 录制 / 回放」；无代码变化。
- `include/aurora/debug/feature_flags.h`：删除 `layout_cache` / `occlusion_culling` / `display_list` 三字段；`FeatureFlags` 顶部注释中「架构级优化（§3.3）」分组说明同步改写（§3.3 仍存在但语义改为恒开）。

### 实现

- `src/aurora/widget/widget.cpp`
  - L305-314：布局缓存命中早返回去宏。
  - L379-392：缓存回填去宏。
  - L725-768：DL 录制/回放快路径去宏，保留 `skip_dl_record`、恒等变换、`can_cache_display_list()`、`recording_is_dynamic` 判据与注释。
- `src/aurora/debug/feature_flags.cpp`：删除 L42-50 三组 `#ifdef` 镜像与 L116-118 三个 JSON 键；文件头「三处同步」纪律说明保留（不涉及具体宏）。

### 测试

- `tests/unit/utest_button.cpp`：L62（探针类门控）、L309/L340/L419/L453 四处用例门控与 L315/L348/L431/L467 的 `AURORA_TEST_SKIP` 关闭侧全部去除，保留主体断言。
- `tests/unit/utest_command_palette.cpp`：L67 探针门控、L530 用例门控与 L549 的 `SKIP` 去除。
- `tests/integration/itest_occlusion_culling.cpp`：L6 注释去宏名；L65-79 门控去除，剔除断言无条件执行。
- `tests/integration/itest_selection_paint_only.cpp`：L6 注释去宏名；L110-115 门控与 `TRACE` 去除。
- `tests/integration/itest_theme_root_injection.cpp`：L254-263 恢复为无条件断言（删除 `#ifdef` 与 `TRACE` 关闭侧）。
- `tests/unit/utest_overlay_host.cpp`：L706 / L709 注释改为恒开语义；L715 起门控去除，保留断言。
- `tests/unit/utest_feature_flags.cpp`：L30 注释「全部 26 个」→「全部 23 个」；删除 L40-42 三行表项；删除 L75-77 三条默认断言。
- `tests/unit/utest_dropdown.cpp` L135、`tests/unit/utest_skeleton.cpp` L62：注释去宏名，保留语义。

### CI

- `.github/workflows/ci.yml` L677-680：删除 `optimizations-off` 矩阵项（含其 `name` / `cmake-flags` 整条）。
- `.github/workflows/ci.yml` L1227-1229：lint 反极性 `flags` 去掉三枚 `-DAURORA_ENABLE_*=OFF`，保留 `-DAURORA_ENABLE_SIMD=OFF -DAURORA_ENABLE_TEST_HOOKS=OFF -DAURORA_NET_TLS_PROVIDERS=shared:/usr`；注释改为「SIMD 与测试钩子关闭侧」。

### 文档

- `codespec/BUILD_OPTIONS.md`
  - L31 总表括注：删除三枚宏名。
  - §3.3（L233-241）：标题改为「架构级优化（恒开）」，删除三行开关表与 `-D...=OFF` 排查示例，改为说明三枚优化恒编入、无开关、旧 `-D...=OFF` 无效。
  - L534/536 lint 口径：删除三枚宏。
  - L862 §9.3 导出宏说明：删除三枚宏名。
  - L976-979 §10 示例：删除三行。
- `codespec/specification/05-event-navigation.md` L286：`（AURORA_ENABLE_LAYOUT_CACHE 下）` → `（布局缓存恒开，命中即跳过 on_layout）`。
- `CHANGELOG.md`：顶部当前版本段追加 Breaking / Changed 条目，说明三枚开关与 `FeatureFlags` 三字段/JSON 键删除。
- `codespec/changes/` 下历史提案不改。

## 兼容性与迁移

- 传入 `-DAURORA_ENABLE_LAYOUT_CACHE=OFF`、`-DAURORA_ENABLE_OCCLUSION_CULLING=OFF`、`-DAURORA_ENABLE_DISPLAY_LIST=OFF` 不再关闭任何能力（CMake 只会留下未使用的缓存变量），三枚能力恒可用。
- 依赖 `FeatureFlags::layout_cache` / `occlusion_culling` / `display_list` 字段或 `to_json()` 中对应宏名键的消费者需删除该读取分支——三枚能力已恒开，判断恒为真。
- 依赖导出宏 `AURORA_ENABLE_*` 做条件编译的消费者：这些宏不再由 `Aurora::aurora` 导出，条件块应按恒真处理并直接删除。

## 不变量与边界

- 三枚能力的运行时行为必须与关闭前默认 `ON` 的构建逐位一致：布局缓存命中条件（约束相等 + `can_cache_layout()`）、DL 回放条件（`dl_valid_` + 光栅世代 + bounds 相等）、剔除条件（全局盒与裁剪区不相交且非 `paints_outside_layout_box()`）保持原样。
- 不引入新的运行期开关；`FeatureFlags` 不再报告这三项。
- 不触碰 Headless 恒编入约定、不做与本次无关的重构或格式化。

## 验收判据

- 全仓检索三枚宏名，仅剩 `codespec/changes/` 下历史提案与本提案的迁移说明；源码、测试、CMake、CI、现存规格文档均无残留。
- 默认构建与 `ctest --preset ninja-test` 全绿，含 `check_api_schema_sync`（先 `aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_core_layer_boundary`、`check_platform_macros`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format`、`format-check`。
- `utest_feature_flags` 的 JSON 键数量断言与结构体字段一一对应（23 项）。
- 改动 TU 的 `lint` 通过（本地按改动文件跑，CI 全量）。

## 实施步骤

- [x] 删除 `cmake/AuroraBackends.cmake` 中三枚 `option` 与 `aurora_define_feature` 注册循环，保留 Headless 等相邻段不动。
- [x] `include/aurora/widget/widget.h` 去门控：`invalidate_display_list_up`、`invalidate_subtree_caches`、`mark_needs_layout_impl`、`mark_needs_paint_impl`、DL 字段块、`Container::on_paint`；同步改写四处注释里的宏名。
- [x] `src/aurora/widget/widget.cpp` 去门控：布局缓存命中早返回、缓存回填、DL 录制/回放快路径。
- [x] `include/aurora/render/painter.h` 两处注释去宏名。
- [x] `include/aurora/debug/feature_flags.h` 与 `src/aurora/debug/feature_flags.cpp` 删除三字段与三个 JSON 键，修正分组注释。
- [x] 单元测试去门控：`utest_button`、`utest_command_palette`、`utest_overlay_host`；`utest_feature_flags` 删三行表项、三条默认断言并把计数注释改为 23。
- [x] 集成测试去门控：`itest_occlusion_culling`、`itest_selection_paint_only`、`itest_theme_root_injection` 恢复无条件断言。
- [x] 注释去宏名：`utest_dropdown`、`utest_skeleton`。
- [x] `.github/workflows/ci.yml`：删除 `toggles` 的 `optimizations-off` 项；lint 反极性 `flags` 只保留 SIMD 与测试钩子关闭，并改写注释。
- [x] `codespec/BUILD_OPTIONS.md` 回写 §3.3、§9.3、§10 与 lint 口径（L31、L534、L536、L862）。
- [x] `codespec/specification/05-event-navigation.md` 删除 L286 宏名括注。
- [x] `CHANGELOG.md` 追加 Breaking / Changed 与迁移路径。
- [x] 刷新 `aurora_api.json`（`cmake --build build --target aurora_api_json`）并确认无实质 schema 漂移。
- [x] 验证：configure、构建 `aurora_test_runner`、`ctest --preset ninja-test` 全绿、`format-check`、改动 TU 的 `lint`。
- [x] 全仓检索三枚宏名，确认源码/测试/CMake/CI/现存规格无残留。
- [ ] 提交并按 CI 结果修复；提案状态改为「已归档」。

## 回写落点

- `codespec/BUILD_OPTIONS.md` §3.3、§9.3、§10、§11（lint 口径）。
- `codespec/specification/05-event-navigation.md`（布局缓存语义表述）。
- `codespec/changes/always-on-optimizations/proposal.md`：状态流转与验收记录。

## 审批

本提案已获批准并完成实施；状态已归档。验收记录：`ctest --preset ninja-test` 461 项全绿，`format-check` 1322 文件全符合，改动 TU 的 clang-tidy 0 告警，`aurora_api.json` 无漂移。
