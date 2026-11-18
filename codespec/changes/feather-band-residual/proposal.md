# CHG-085 羽化带逐像素残余成本与录制态快照收敛

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-085 |
| 提出日期 | 2026-11-18 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PERF.RUNTIME.FRAME-BUDGET.001 |
| 影响面 | `src/aurora/render/painter.cpp`（`fill_sector` 羽化带、圆角子像素过渡列、`draw_linear_gradient` / `draw_radial_gradient` 过渡带、`draw_shadow` 边缘环、`draw_line` / `stroke_polyline` 羽化带、`composite` 录制态快照）；`tests/unit/utest_painter.cpp` / `utest_painter_primitives.cpp` / `utest_offscreen.cpp` / `utest_display_list.cpp` / `utest_simd_parity.cpp` 与 `tests/support/gpu_golden_scenes.h`；`tests/integration/itest_shadow.cpp`、`tests/integration/itest_gradient.cpp`、`tests/integration/itest_gradient_title_no_fringe.cpp` 的语义覆盖须继续成立；`tools/bench/bench_render.cpp`（须增补扇形与宽描边场景）；`CHANGELOG.md`；`include/aurora/render/display_list.h` 与显示列表回放侧、`tests/unit/utest_rhi.cpp` 及两个 GPU 后端用例**仅在快照方案评审选择扩展命令数据时**纳入，届时按公共头变更处理 |

## 动机

1. **批量核心已经铺开，成本集中到边界带。** `fill_rect` 圆角全覆写区、扇形径向/角向核心、渐变的规范化分段、描边的平方判据核心、阴影的内部矩形都已走整段写入；未收敛的是同一批原语的 1px 羽化带。阴影模糊、宽线、圆弧与大面积渐变下，残留逐像素计算量随覆盖面积线性放大，是当前渲染分项里最大的可确定剩余成本。

2. **可消除的是「不必求值的像素」，不是「过渡带内的精确求值」。** 这些边界带的逐像素循环里，真正需要完整公式的只是跨越边界的少数像素，其余是零覆盖（写不进去）或全覆写（结果恒等于源色）像素。当前 `fill_sector` 过渡带对整带逐像素跑 `sqrt + atan2 + fmod`；圆角裁剪下的文本过渡列对整列求 SDF 后 `blend_subpixel`；线性渐变左右圆弧过渡带与径向渐变全行跑 `sample_gradient + sqrt` 且径向没有分段推进；`draw_shadow` 边缘环在 `dy` 按行提升后仍逐像素 `sqrt`；`draw_line` / `stroke_polyline` 核心带已用平方判据批量，羽化带仍 `sqrt`。本案的收益全部来自把前两类像素判出去，跨界像素保留原表达式。

3. **前一轮热路径治理已明确把三项几何回退移出，需要独立承接。** 当时否决的理由是「几何重写无法由现有用例自证逐位一致」。本案把逐位自证升为硬判据，并要求每项给出可转红的注入变异，而不是把等价性当作不证自明。

4. **录制态还留一次整帧拷贝。** `Painter::composite(const Painter &)` 已通过移动入池消掉图像的第二次拷贝，但 `to_image()` 仍复制源 `Painter` 的整个像素缓冲。滚动滑窗与离屏缓存每帧都可能付这份成本。它牵涉显示列表的像素所有权、回放与 GPU 后端命令数据，不能当作普通小修处理，故与羽化带一并纳入本案，但实现边界须评审后确定。

5. **边界。** 不改双色标全不透明且无裁剪的 SIMD 渐变快路径；不做 `draw_line` 轴对齐特判（调用点分布未实测）；不改 `AURORA_ENABLE_SIMD` 默认值；不借本案放宽 golden 容差或重生成基线来掩盖差异。

## 变更内容

### 共同口径

1. 各项统一按「先判该不该求值、再按原式求值」推进：几何快路径只负责判定全覆写区（整段写入）与零覆盖区（跳过），跨界像素继续走现有 `sqrt` / `atan2` / `sample_gradient` 表达式与同一 alpha 整数化口径。**不承诺消除过渡带内的精确求值**；若某项还想按覆盖度分档批量写入，须先单独证明与逐像素口径逐位一致，否则不纳入该项。

2. 每项改动随同落地一条「快路径 ↔ 逐像素正交路径」的对照用例：同一画布、同一底色上分别走两条路径，用 TU 内自带的 `count_diff_pixels` 辅助判等，沿用 `rounded_fill_row_fast_path_matches_per_pixel_coverage` 的既有范式，不引入生产侧测试钩子。

### 过渡带逐位收敛

3. `fill_sector`：保留现有径向/角向核心的解析求交，把羽化带改为按行可界定的角向区间加径向边界收缩，先剔除零覆盖与全覆写像素，带内仍用原 `sqrt` / `atan2` / `fmod` 求精确覆盖度。须写明分段规则：`sweep` 达到整圆时走整环分支、`sweep ≥ π` 与跨 ±π 的角向区间按象限切开、内外半径退化（`inner ≥ outer`）与 `radius` 为零时回落原逐像素路径，不得因分段不全静默少画。

4. 圆角文本：保留 `blend_subpixel_span` 的 safe 区批量，把圆弧过渡列改多列小批量；`coverage` 恒为 1 的判定继续复用与逐像素回退相同的复核，避免批量结果与逐像素结果分叉。过渡列批量只是减少函数调用与列内重复的 SDF 求解，不改单像素覆盖度算法。

5. 渐变：线性渐变的左右圆弧过渡带在保持 `sample_gradient` 语义的前提下复用已规范化的色标推进，剔除带外零覆盖列；径向渐变按半径分段推进，去掉全覆写区的逐像素色标线性搜索与重复 `sqrt`，带内仍按原采样口径求值。SIMD 快路径与回退路径的分工保持现状，不改触发条件。

6. 阴影与描边：`draw_shadow` 继续按行解析羽化半宽，把边缘环内的逐像素 `sqrt` 限制到真正跨界的像素，环内全覆写段直接整段写入、环外直接跳过；`draw_line` / `stroke_polyline` 保留圆帽与连接语义，羽化带只在像素跨出核心带时求距离。轴对齐特判不纳入，除非先有基准证据。

### 录制态快照

7. `Painter::composite(const Painter &)` 录制态的一次 `to_image()` 整帧拷贝作为本案后续收敛项。实现边界在实施前评审：可选内部共享像素池，或 `Image` 共享句柄加写时复制；两者分别牵涉 `include/aurora/render/display_list.h` 的命令数据扩展与公共结构/序列化迁移。评审前不改 `DrawCmd` / `Image`。选定方案后须保证回放、GPU 后端读回、离屏快照与直绘逐位一致。

8. 若评审确认需要扩展命令数据或公共结构，按公共头变更处理：在 `CHANGELOG.md` 记版本与迁移路径，刷新 `aurora_api.json`，并说明既有调用方的迁移方式；「`aurora_api.json` 无差异」不能当作未改公共契约的证据（命令数据与结构布局不在该 schema 覆盖范围内）。若确认不改公共结构，则在回写文档中写明共享句柄的生命周期与失效规则。

### 共同纪律

9. 每项可独立移出本案：某一项若在实施中发现无法满足逐位一致或无法给出可转红变异，允许单独移出并归档为遗留项，移出时在本提案实施记录中写明理由、已产生的中间结论与独立承接方式，不因此阻塞其余项。

10. 每项改动落地时同步更新 `CHANGELOG.md` 的 `### Performance` 或 `### Fixed` 段落；像素口径、几何边界与所有权契约按「回写落点」更新对应规格。

11. **实施记录**：落地阶段按第 9 条移出三项，剩余两项已实施并自证，快照项留待评审。移出的三项与理由：① `draw_shadow` 边缘环——现有实现已用 `half = sqrt(blur²−dy²)+1` 把每行迭代收进羽化带，余量仅 1px，环内每个像素的 dist 不同、alpha 连续变化，逐像素 `sqrt` 是几何必需，逐位一致下无可批量空间；② `fill_sector` 角向带——径向核心已按行解析批量、角向核心已用径向边垂距批量，剩余 `atan2` 全在真正的角向羽化带内，覆盖度由精确角度决定，无法逐位一致地批量；③ 圆角文本过渡列——safe 区已批量且经 `covers_fully` 复核，过渡列每列 SDF coverage 不同且走 gamma-correct 混合，多列批量会改变量化结果。三项继续硬改只能靠近似公式或放宽容差，违反第 5 条边界，故按第 9 条移出，保留现状描述。已实施两项：`draw_line` / `stroke_polyline` 的平方上界提前剔除（golden 变异转红 10px 漂移、max delta 49 自证）、径向渐变多色标行内区分段推进（新增逐位对照用例；变异索引推进转红 57/45px）。快照项的实现边界仍待评审，未落码。性能实测（同机 Debug/AUTO、`bench_render` 1280×720 scale=1.0，暂存实现取基线后恢复）：`stroke_wide_slanted` 6.770 → 4.821 ms（−28.8%），`radial_gradient_multistop` 8.951 → 7.416 ms（−17.1%）；两场景为本轮新增，分别覆盖双斜线羽化外沿与五段非 SIMD 径向渐变。

## 验收判据

1. **像素逐位（已有基线的部分）**：`tests/unit/utest_painter_primitives.cpp` 的 `golden_polyline_matches_baseline`、`golden_sector_matches_baseline` 与 `tests/unit/utest_offscreen.cpp` 的像素快照基线，以及 `tests/support/gpu_golden_scenes.h` 覆盖的折线、扇形、文本列、柱状图场景，改动前后逐位 0 差异；`tests/unit/utest_simd_parity.cpp` 的标量 / SSE2 / AVX2 三实现继续逐位一致。若某项确实改变基线，须在实施记录中列出差异像素数、分布与上限，并说明为何属于语义修正；禁止只重生成基线。

2. **像素逐位（无既有基线的部分）**：阴影、线性/径向渐变、圆角文本过渡列、描边羽化带目前没有逐位 golden——`tests/integration/itest_shadow.cpp`、`itest_gradient.cpp`、`itest_gradient_title_no_fringe.cpp` 是粗粒度颜色与无彩边断言，只能作语义不退化判据。这四项须在对应单测里自建独立基准：同画布上一条路径走快路径，另一条走「逐像素 `blend_pixel` / `push_clip_rounded` + `pop_clip`」的正交路径，`count_diff_pixels` 必须为 0，覆盖边界形态（整数与非整数锚点、半宽等于半径、`sweep` 临界、粗线圆帽）。为阴影与两个渐变方向补 golden 场景是可选的后续强化，新增基线属独立决定，不在本案静默生成。

3. **反向自证**：每项至少注入一次**会改变输出**的变异，绿不算证据、转红才算；实施时先验证该变异确实让现有断言失败，不成立就换成别的。可用变异示例：扇形解析角向区间放宽一个量化步、或径向边界外扩 1px；圆角文本把 `coverage < 1` 的过渡列判进 safe 批量；渐变分段索引不推进（复用上一段色标端点）；阴影去掉边界余量或把环内 alpha 提前截断；描边核心带半径放宽 1px。明确排除两类无效变异——「过渡带退回逐像素」是等价路径、输出不变；「阴影半宽扩到整扩展区」只多算零覆盖像素、同样不改变输出。

4. **性能基准**：同机、同构建配置、`AURORA_ENABLE_PROFILING=ON`（三态开关，AUTO / ON / OFF，默认 AUTO）。原语场景用 `tools/bench/bench_render.cpp` 的 `linear_gradient_full`、`radial_gradient_full`、`shadow_card`，按固定 warmup 与迭代次数记录改动前后的均值与绘制分项，只接受实测改善且改善幅度大于同码重复测量波动。扇形与宽描边当前没有基准场景，实施时先在 `bench_render` 增补 `sector_ring` 与 `stroke_polyline_wide` 两项作为前置，否则这两项不申报性能收益、只申报逐位一致与变异自证。整帧成本用该文件已有的 scroll 场景（`ScrollBenchHarness`，300 帧）看 p99 / jitter / full_redraw_frames。`tools/check/perf_gates.json` 的阈值仅在连续多次实跑稳定通过后才收紧，禁止先改门槛再补数据。

5. **快照专项**：若录制态快照方案落地，`tests/unit/utest_display_list.cpp` 与 `tests/unit/utest_offscreen.cpp` 须覆盖「录制 → 回放 → 多帧复用 → 内容失效 → 清池」链路（仓库内没有显示列表序列化路径，不以序列化往返作判据）；录制态整帧拷贝字节数或整帧分配次数须在基准中下降；回放与直绘逐位一致。方案若触及 RHI 命令数据，另跑 `tests/unit/utest_rhi.cpp`、`utest_gpu_gl_rhi.cpp`、`utest_wgpu_rhi.cpp` 与 GPU golden 两路。公共头若有变更，`check_api_schema_sync` 与 `check_gen_api_merge` 必须同批绿，并按破坏性变更给迁移说明。

6. **门禁与跨平台**：`ctest --preset ninja-test` 全绿（含全部 `check_*`、`lint`、`format-check`）；`check_code_doc_sync` / `check_codespec_xref` / `check_change_proposals` 绿。触及公共头时另跑 `build-wasm` 与 WSL 的 `build-x11`；只改库实现时至少完成增量构建与相关用例。

7. **范围红线**：SIMD 双色标渐变路径、轴对齐特判、golden 容差与基线生成规则均不在本案授权范围内，需另案。

## 回写落点

- `codespec/ARCHITECTURE.md` §8.1 渲染核心：补「批量全覆写区 + 羽化过渡带」双路径不变量与逐位一致要求；§8.3 Scroll 离屏内容缓冲（滑窗）：补录制态合成快照的复用与失效边界；§10 性能检测体系：补原语均值与整帧分位数两套读数口径的分工；§14.6 按改动类型的门禁矩阵：补触及渲染原语时的验证组合（含无 golden 项的自建基准要求）。
- `codespec/specification/03-layout-render.md` §8.1 Painter：补扇形角向边界（含整圆、`sweep ≥ π` 与跨 ±π 分段）、圆角子像素过渡列、渐变过渡带、阴影边缘环、描边羽化带的边界与取整口径；§8.4 离屏渲染与快照、§8.6 显示列表与 RHI 后端：补录制态合成快照的所有权、生命周期与回放契约。
- `codespec/CODING_STANDARDS.md` §3.5 golden 基线测试规范：补「无 golden 的原语以测试内独立逐像素基准对照」的写法与适用边界；§6.14 热路径分配纪律：补「几何快路径须可注入变异自证」与「像素基线不得靠重生成掩盖差异」。
