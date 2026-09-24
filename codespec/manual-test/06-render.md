# render 模块人工测试用例

> **文档状态：临时（同批示例文档）**
> 本文件与 `01-core.md` 同属人工测试用例文档集，沿用其确认过的字段范式、格式规范与覆盖取舍方法。
> 按约定，本文件**暂不进入 `AGENTS.md` 的文档导航表，其他文档亦不引用它**；定稿后再决定是否转为常驻文档。
>
> **本文件按固定格式编写，供程序解析**：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。

## 1 适用范围与约定

### 1.1 被测模块

`render/` —— Aurora 的**软件栅格绘制层**（含 `rhi/` 后端抽象与 `detail/` 内部头，公共头合计约 25 个），在实测依赖分层中位于 L2：`layout`、`render`、`state` 三者**互不依赖、平级**（该结论由 `include/aurora/**` 头包含关系实测得出，不是链式顺序）。

`render/` 的可观测行为几乎全部落在**像素**上，人工判定的增益集中在「感知级」质量：

| 载体 | 内容 |
|:---|:---|
| 矢量边缘质量 | 圆角/圆环/折线的抗锯齿，有无台阶、串珠、毛边 |
| 渐变观感 | 色带接缝、过渡平滑度、渐变背景上文字的子像素彩边 |
| 文字渲染 | 锐度与像素对齐（不虚、不跳）、字距/斜体/两端对齐观感、缺字回退（豆腐块） |
| 半透明与效果 | 混合模式变色方向、渐变遮罩淡出、内容模糊的观感 |
| 一致性 | 离屏 PNG 与窗口显示一致、跨后端（软件/GL）观感一致 |
| 脏区重绘 | 局部重绘是否留下残影、错位条带或撕裂 |

### 1.2 执行载体

本模块**复用既有 demo 与 CLI 工具**，不新建载体。各载体的构建目标与用途：

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_radio_spin.cpp` | `demo_radio_spin` | RadioButton 圆环/圆点的抗锯齿（§2.2） |
| `examples/demos/demo_line_chart.cpp` | `demo_line_chart` | 折线/数据点/准线的抗锯齿（§2.2） |
| `examples/demos/demo_canvas.cpp` | `demo_canvas` | 渐变标题横幅、Canvas 自绘仪表条（§2.3） |
| `examples/demos/demo_text.cpp` | `demo_text` | 字号/字重/锐度与排版观感（§2.4） |
| `examples/demos/demo_visual_effects.cpp` | `demo_visual_effects` | 混合模式与渐变遮罩（§2.5） |
| `examples/demos/demo_tooltip_context.cpp` | `demo_tooltip_context` | 内容模糊观感（§2.5） |
| `examples/demos/demo_text_input.cpp` | `demo_text_input` | 选择高亮的局部重绘（§2.6） |
| `examples/demos/demo_scroll.cpp` | `demo_scroll` | 滚动重锚点重绘（§2.6） |
| `examples/demos/demo_glfw_surface.cpp`、`examples/demos/demo_gpu.cpp` | `demo_glfw_surface`、`demo_gpu` | GL 后端对比（§2.6，需 GLFW 构建） |
| `tools/servers/aurora_cli.cpp` | `aurora_cli` | `render` / `preview` 子命令：离屏 PNG 与窗口预览（§2.1、§2.4、§2.6） |

窗口类 demo 经 `examples/demos/demo_common.h` 的 `run_demo` 打开真实平台窗口（Windows 上为 Win32/GDI 软件栅格），**关闭窗口即退出**；demo 启动时自动启用 DPI 感知，高分屏上观感以实际屏幕为准。

离屏渲染用例使用树描述文件 `render_probe.json`（置于 `build/` 下），内容如下：

```json
{
  "type": "Column",
  "props": { "background_color": [245, 247, 250, 255] },
  "children": [
    { "type": "Text", "props": { "content": "Latin 14pt baseline", "font_size": 14, "color": [17, 24, 39, 255] } },
    { "type": "Text", "props": { "content": "中文渲染测试 缺字回退", "font_size": 20, "color": [17, 24, 39, 255] } },
    { "type": "Text", "props": { "content": "日本語テスト العربية", "font_size": 20, "color": [17, 24, 39, 255] } }
  ]
}
```

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja）；有显示后端，demo 可开窗 |
| 构建 | 库与载体均已构建；demo 为按需目标，不在默认构建内 |
| 基线后端 | 基线构建仅启用 `AURORA_BACKEND_WIN32` 与 `AURORA_BACKEND_HEADLESS`（D3D11/GLFW/wgpu 默认 OFF）——影响 TC-RENDER-012 的执行条件 |
| 看图工具 | 任意可打开 PNG 并支持放大（如系统自带照片查看器） |
| 输出物 | 离屏渲染的 PNG 统一写到 `build/` 下，用看图工具打开比对 |

不在本模块范围内：X11、Wayland、Wasm、macOS 后端行为（本机环境不可达）；窗口生命周期与输入派发归 window / event 模块。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `render`，即 `TC-RENDER-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
用例在文档中**按编号升序声明**——该顺序即默认执行顺序，也是依赖列表排序的依据（见 §1.5）。

### 1.5 用例字段清单与格式规范

每条用例由**六个固定字段**组成，排布于同一张 Markdown 表格内。**字段顺序固定、字段名一字不差、每字段独占一行、单元格内不换行**（需要多条内容时用 `<br>` 分隔）：

| 序号 | 字段 | 格式 | 正则约束 | 说明 |
|:--:|:---|:---|:---|:---|
| 1 | 用例编号 | 单行 | `^TC-[A-Z0-9]+-\d{3}$` | 模块内唯一且不复用 |
| 2 | 测试目的 | 单行 | 自由文本 | 一句话，指向**单一**验证目标 |
| 3 | 前置条件 | 单行 | 自由文本 | 只描述**环境状态**；「必须先执行某用例」属依赖用例字段，不在此重复 |
| 4 | 依赖用例 | 单行 | `^无$` 或 `^TC-[A-Z0-9]+-\d{3}(, TC-[A-Z0-9]+-\d{3})*$` | 见下方规范 |
| 5 | 操作步骤 | 条目列表 | 每条目以 `^\d+\. ` 开头，条目间以 `<br>` 分隔 | 自 1 起连续编号 |
| 6 | 预期结果 | 条目列表 | 同操作步骤；编号是步骤号的**子集** | 见 §1.6 |

**依赖用例字段规范（四条硬性要求）：**

1. 无依赖时**必须**写 `无`，不得留空、不得写 `-`、`N/A`、`none` 等变体。
2. 有依赖时写**用例编号全称**（含模块段），不写省略形式；跨模块依赖同样写全称。
3. 多项依赖以**半角逗号加一个空格**（`, `）分隔；逗号前后不得出现空格以外的字符。
4. 多项依赖的**排列顺序即依赖先后**——先决者在前。因用例按编号升序声明，等价于按被依赖用例的**编号升序**排列。

**模块级约定**：本模块载体（§1.2 全表）的构建步骤统一置于 `TC-RENDER-001` 的步骤 1；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。

**为什么必须统一格式**：本文件由 `tools/check/check_manual_test_format.py` 解析守护（CTest 用例 `check_manual_test_format`，校验字段名与顺序、取值域、依赖拓扑、步骤-预期同号映射与执行记录表格式）。上述约定使每个字段可用**单条正则**校验，无需自然语言推断。

### 1.6 操作步骤与预期结果编号规则

**预期结果与操作步骤同号一一对应**：`预期结果 N` 即「步骤 N 的对应结果」。

1. 预期结果的序号**等于**其对应步骤的序号，两者严格相等——不得因省略而重排、压缩或重新连续编号。
2. 纯执行步骤**不产生预期结果**，其序号在预期结果列表中直接跳过。
3. 故预期结果的序号**不要求连续**，条数也无需等于步骤数。
4. 步骤列表中，纯执行步骤在文本末尾标注「（纯执行，无预期结果）」，供与预期结果列表逐号对照。

示例：某用例有步骤 1、2、3、4，其中步骤 3 为纯执行步骤，则该用例的预期结果共 3 条，编号为 1、2、4。

**该规则解决的问题**：执行者按步骤顺序推进时，编号本身即是映射关系，无需自行推断「这条预期结果对应哪一步」；执行记录可借步骤号精确落到失败点。

### 1.7 判定与记录

| 结果 | 含义 |
|:---|:---|
| PASS | 全部非空预期结果均满足 |
| FAIL | 任一非空预期结果不满足；须在记录表登记该预期结果对应的**步骤号**、实际现象与缺陷编号 |
| BLOCKED | 前置条件无法满足（如载体构建失败），未开始执行 |
| SKIP | 主动判定不适用于本环境（如被测后端未编译进本次构建） |

视觉观感类判定受**显示设备与缩放**影响：判定以「缺陷特征是否出现」（台阶、接缝、彩边、残影、豆腐块）为准，不对亚像素级的采样差苛责；放大比对时优先用看图工具的整数倍缩放。
诊断日志中含时间戳、线程 id、行号的字段属可变部分，判定时只核对格式与是否出现，不比对具体值。

**执行记录表格式**（列顺序固定）：

| 列 | 格式 | 允许值 / 正则 |
|:---|:---|:---|
| 用例编号 | 单行 | 与 §2 用例编号一致 |
| 执行日期 | 单行 | `^\d{4}-\d{2}-\d{2}$` |
| 执行人 | 单行 | 自由文本 |
| 结果 | 单行 | `PASS` / `FAIL` / `BLOCKED` / `SKIP` |
| 失败步骤号 | 单行 | 留空（未失败或未执行），或 `^\d+(, \d+)*$`（多个以半角逗号加一个空格分隔） |
| 实际现象 | 单行 | 自由文本 |
| 缺陷编号 | 单行 | 留空或缺陷单号 |
| 备注 | 单行 | 自由文本 |

未执行的取值一律**留空单元格**，不写 `-`、`TBD`、`N/A` 等占位符。

### 1.8 覆盖取舍

本模块按「人工判定是否优于程序判定」取舍，不追求对约 25 个头的全覆盖。

**纳入人工测试的子域**（人工能判得比程序准）：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 矢量抗锯齿边缘 | 台阶感、串珠、断续缺口是人眼敏感特征，容差断言难以覆盖其「观感」维度 |
| 渐变接缝与文字彩边 | 1px 级缝隙与子像素彩边需人眼贴近屏幕或放大比对才能定位 |
| 文字清晰度与排版 | 锐度、字距均匀性、斜体/两端对齐是否符合直觉属感知判断 |
| 半透明叠加与效果 | 变色方向（变深/变亮）、淡出平滑度、模糊无振铃等需整体观感判断 |
| 字体回退与缺字 | 豆腐块、RTL 视觉顺序是否自然，属「一眼即见」的感知特征 |
| 脏区重绘残影 | 残影/撕裂是**时间上**的错误，静态截图与逐位比对都难以触发和断言 |
| 离屏与跨后端一致性 | 需并排对照两条呈现路径的成图，属人因比较 |
| 纯数值与结构 | 见下表——程序判定更精确，人工执行无增益 |

**不纳入人工测试的头及其理由**：

| 头 | 理由 | 已有覆盖 |
|:---|:---|:---|
| `include/aurora/render/blend.h` | 混合公式为纯数值换算，程序判定精确；其视觉结果经 demo 观感间接覆盖 | `tests/unit/utest_blend.cpp` |
| `include/aurora/render/png.h` | 编码器正确性（CRC32/adler32/块结构）是字节级断言，人工无增益 | `tests/unit/utest_png.cpp`、`tests/integration/itest_png_image.cpp` |
| `include/aurora/render/dirty_region.h` | 矩形合并/超限退化是纯几何算法，可断言穷举；成品残影观察见 TC-RENDER-013、TC-RENDER-014 | `tests/unit/utest_dirty_region.cpp` |
| `include/aurora/render/display_list.h` | 录制/回放的等价性由「回放像素与直绘逐位一致」程序判定 | `tests/unit/utest_display_list.cpp`、`tests/integration/itest_perf_display_list.cpp` |
| `include/aurora/render/offscreen.h` | 无头流程与逻辑快照是确定性纯函数，JSON 可逐字段比对 | `tests/unit/utest_offscreen.cpp`；离屏 PNG 产物人工核对见 TC-RENDER-001、TC-RENDER-011 |
| `include/aurora/render/snapshot_diff.h` | 容差判定、区域聚合与归因是数值/算法问题，消费方为 golden 框架 | `tests/unit/utest_snapshot_diff.cpp`、`tests/framework/golden.h`、`tests/integration/itest_gl_golden.cpp`、`tests/integration/itest_wgpu_golden.cpp` |
| `include/aurora/render/bidi.h` | UBA 层级与视觉重排是规格化算法；成品观感见 TC-RENDER-007 | `tests/unit/utest_bidi.cpp` |
| `include/aurora/render/text_aa_mode.h` | 枚举与进程级切换无独立观感载体；ClearType 彩边观感并入 TC-RENDER-004 | `tests/unit/utest_text_aa_mode.cpp` |
| `include/aurora/render/bitmap_font.h` `noto_font_data.h` `freetype_library.h` | 内置资源与第三方库封装，无独立人工观感 | `tests/unit/utest_bitmap_font.cpp`、`tests/unit/utest_noto_font_data.cpp` |
| `include/aurora/render/glyph_atlas.h` `image_cache.h` | 缓存命中正确性程序可断言；失效异常会表现为可见残影，归 §2.6 用例 | `tests/unit/utest_glyph_atlas.cpp`、`tests/unit/utest_image_cache.cpp` |
| `include/aurora/render/font_engine.h` | 度量/命中接口返回值程序判定；绘制观感归 TC-RENDER-005、TC-RENDER-006 | `tests/unit/utest_font_engine.cpp`、`tests/integration/itest_font_pixel_snap.cpp` |
| `include/aurora/render/font_discovery.h` | 候选链解析程序可断言；回退观感归 TC-RENDER-007 | `tests/unit/utest_font_discovery.cpp` |
| `include/aurora/render/rhi/` 各后端头 | 像素一致性由 golden 比对覆盖；跨后端观感一致性归 TC-RENDER-012 | `tests/unit/utest_rhi.cpp`、`tests/unit/utest_gpu_gl_rhi.cpp`、`tests/unit/utest_wgpu_rhi.cpp` |
| `include/aurora/render/detail/` 各内部头 | gamma LUT、SIMD、计时、GPU 层均为内部实现，行为经公共 API 体现 | 既有同名 `utest_*` 与 golden 测试 |
| 投影阴影（`Painter::draw_shadow`）与模糊毛玻璃 | 本机 demo 无现成阴影/毛玻璃载体（仅内容模糊有，见 TC-RENDER-010） | `tests/integration/itest_shadow.cpp`、`tests/integration/itest_blur.cpp` |
| 渐变填充（`draw_linear_gradient` / `draw_radial_gradient`）数值正确性 | 色标插值程序判定精确；接缝观感以渐变标题横幅为载体（TC-RENDER-004） | `tests/integration/itest_gradient.cpp`、`tests/integration/itest_gradient_title_no_fringe.cpp` |

## 2 用例清单

### 2.1 载体与离屏基线

#### TC-RENDER-001 载体构建与离屏渲染管线基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-001 |
| 测试目的 | 确认全部载体可构建，且 `aurora_cli` 的离屏渲染管线能把 UI 树栅格化为可正常打开的 PNG |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；本机工具链可用；当前目录可写 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建载体：`cmake --build build --target aurora_cli demo_canvas demo_text demo_radio_spin demo_line_chart demo_visual_effects demo_tooltip_context demo_text_input demo_scroll demo_glfw_surface demo_gpu`（纯执行，无预期结果）<br>2. 按 §1.2 的清单在 build/ 下创建树描述文件 render_probe.json（纯执行，无预期结果）<br>3. 运行 `./build/aurora_cli.exe` render build/render_probe.json -w 420 -H 160 -o build/render_probe.png<br>4. 用任意看图工具打开 build/render_probe.png 并放大观察 |
| 预期结果 | 3. 构建全部成功无报错；命令 stdout 输出单行 JSON 摘要，其中 "ok" 为 true、"width" 为 420、"height" 为 160<br>4. PNG 正常打开，为 420×160 浅灰底图，自上而下可见三行深色文本（英文 14pt、中文 20pt、日文与阿拉伯文 20pt），无整块黑/白空洞，文字无叠影错位 |

### 2.2 矢量抗锯齿边缘

#### TC-RENDER-002 圆环与圆点的抗锯齿无台阶

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-002 |
| 测试目的 | 观察 RadioButton 圆环与内圆点的抗锯齿质量：曲线平滑、无锯齿台阶与断续缺口 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_radio_spin.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 Size (radio group) 三行的外圈圆环轮廓<br>3. 观察选中项（默认 Medium）与未选中项的圆环粗细及内圆点位置<br>4. 依次点击 Small 与 Large 切换选中，观察新旧选中项的圆环变化 |
| 预期结果 | 2. 圆环曲线平滑连续，贴近屏幕看边缘为约 1px 的渐变过渡，无锯齿台阶、无断续缺口<br>3. 选中项内圆点与外环同心居中；选中与未选中项的外环粗细一致<br>4. 点击后旧选中项内圆点立即消失且无残点，新选中项圆环与内圆点立即完整——不存在半绘制状态 |

#### TC-RENDER-003 折线与数据点无串珠

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-003 |
| 测试目的 | 观察图表折线（抗锯齿多段线）与数据点的描线质量：线宽均匀、拐角无串珠状深浅斑 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_line_chart.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 cpu 与 mem 两条折线的线身<br>3. 观察折线拐角与各数据点圆点<br>4. 将鼠标悬停到图表数据点附近，观察十字准线与值框 |
| 预期结果 | 2. 线宽均匀无粗细跳变，颜色饱满；两线交叉处不出现叠加深色斑点<br>3. 拐角圆滑融合、无顶点处的串珠状深浅点，数据点为边缘平滑的实心小圆<br>4. 十字准线为细线且边缘无锯齿；值框文字清晰，不与曲线糊成一片 |

### 2.3 渐变观感

#### TC-RENDER-004 渐变标题无接缝、文字无子像素彩边

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-004 |
| 测试目的 | 观察蓝→粉渐变标题横幅的色带接缝与多色背景上文字的子像素彩边（banding / fringe） |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_canvas.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察顶部渐变标题横幅的整体过渡（横幅按 24 个色带绘制）<br>3. 用看图工具思路贴近屏幕或截图放大，逐个观察色带交界处<br>4. 观察横幅上白色标题文字的边缘<br>5. 观察下方 Canvas 仪表条：灰色轨道、蓝色进度条与 CPU 文本的相互关系 |
| 预期结果 | 2. 渐变自左向右由蓝过渡到粉，整体平滑、无突兀的明暗断层<br>3. 相邻色带之间无白缝、黑缝或亮线（分带本身是设计使然：带宽含 1dp 重叠防缝，带内颜色一致不属缺陷）<br>4. 文字边缘为中性灰过渡、干净锐利，无红/蓝子像素彩边（默认灰度抗锯齿在多色背景上不应出现彩边）<br>5. 文本与轨道无重叠错位；进度条右边界整齐、与轨道色分界干净 |

### 2.4 文字渲染与回退

#### TC-RENDER-005 多字号多字重的锐度与像素对齐

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-005 |
| 测试目的 | 观察不同字号/字重/颜色文本的渲染锐度与像素对齐：笔画不虚、无重影 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_text.exe` 打开窗口（纯执行，无预期结果）<br>2. 逐行观察 Default 14pt、Bold 22pt blue、Muted 16pt、Accent 18pt 四行文本<br>3. 贴近屏幕观察各行水平与垂直笔画的边缘<br>4. 比较 Bold 22pt 与 14pt Regular 的笔画粗细 |
| 预期结果 | 2. 四行字号与颜色呈明确阶梯（灰色、主蓝、粉色），行内无字符截断或重叠<br>3. 笔画边缘锐利，横竖笔画不发虚、无重影（像素对齐），14pt 小字仍清晰可读<br>4. Bold 行笔画明显粗于同字号 Regular，字形结构完整、无笔画粘连糊死 |

#### TC-RENDER-006 字距、斜体与两端对齐观感

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-006 |
| 测试目的 | 验证字距/词距、斜体与 Justify 两端对齐的排版观感符合直觉且与文本盒一致 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_text.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 Letter spacing 4dp · word spacing 8dp 一行<br>3. 观察 Italic (FontStyle::Italic) 一行<br>4. 观察底部 Justify 两端对齐段落 |
| 预期结果 | 2. 字母间距明显加宽且均匀一致，单词间距进一步大于字母间距，无忽宽忽窄<br>3. 字形整体右倾自然，顶部/底部笔画无被文本盒裁剪的平切口<br>4. 除末行外左右两端对齐成直线，词间空隙被均匀拉伸，无成片空白或字符挤压重叠 |

#### TC-RENDER-007 字体回退无豆腐块

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-007 |
| 测试目的 | 验证字体发现与回退链对 CJK / 日文 / 阿拉伯文的覆盖：缺字不出豆腐块，RTL 呈正确视觉顺序 |
| 前置条件 | `aurora_cli` 已构建成功；TC-RENDER-001 产出的 build/render_probe.png 已保留（或按 §1.2 重建 render_probe.json 后重新执行 TC-RENDER-001 步骤 3） |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 确认 build/render_probe.json 存在且内容与 §1.2 一致（纯执行，无预期结果）<br>2. 用看图工具打开 build/render_probe.png，放大到 200% 以上观察第二行中文<br>3. 观察第三行日文假名与阿拉伯文 |
| 预期结果 | 2. 中文各字均为真实字形、笔画完整（渲染测试缺字回退等字样可辨读），无空心方框/豆腐块占位<br>3. 日文假名为真实字形；阿拉伯文整行按从右到左的视觉顺序呈现、字形连贯，无豆腐块与孤立的断字形 |

### 2.5 半透明叠加与效果

#### TC-RENDER-008 混合模式 Multiply 的变色方向与边缘干净度

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-008 |
| 测试目的 | 观察 blend_mode（Multiply + 蓝 tint）在白底深字上的整体观感：变深不变亮、边缘无色晕 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_visual_effects.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 blend_mode: Multiply + blue tint 文本块<br>3. 与下方两行（shader_mask、cache_layer）横向对比颜色与边缘 |
| 预期结果 | 2. 白底被乘为蓝色（变深方向），块内文字呈更深的蓝黑且仍可读；块为规整矩形、边缘无溢出的颜色晕染<br>3. 混合只作用于该块自身区域，相邻行与卡片背景的颜色和边缘不受影响 |

#### TC-RENDER-009 渐变遮罩淡出平滑无台阶

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-009 |
| 测试目的 | 观察 shader_mask（LinearFade）的顶部不透明→底部淡出效果：渐变平滑、文字随背景同步变淡 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_visual_effects.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 shader_mask: LinearFade 文本块自上而下的颜色变化<br>3. 贴近屏幕观察淡出区间有无分界线 |
| 预期结果 | 2. 块顶部颜色饱满、向下逐渐变淡、底部接近完全消失露出卡片底色；白色文字与背景同步变淡<br>3. 淡出过渡平滑连续，无生硬的水平分界线或台阶感 |

#### TC-RENDER-010 内容模糊无振铃

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-010 |
| 测试目的 | 观察内容模糊（blur 修饰，两遍 box blur ≈ 高斯）的观感：发虚均匀、无振铃与双重轮廓 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_tooltip_context.exe` 打开窗口（纯执行，无预期结果）<br>2. 观察 This text is blurred 模糊块，并与上方两行正常文字对比锐度<br>3. 贴近屏幕观察模糊块的矩形边缘 |
| 预期结果 | 2. 模糊块内文字均匀发虚、不可辨读，背景淡黄色仍在；上方两行文字保持清晰锐利<br>3. 模糊块边缘为平滑过渡，无亮暗相间的振铃条纹，也无原文字的双重轮廓残影 |

### 2.6 一致性与脏区重绘

#### TC-RENDER-011 离屏 PNG 与窗口显示一致

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-011 |
| 测试目的 | 验证同一棵 UI 树经离屏渲染（PNG）与真实窗口呈现的内容一致：颜色、内容与相对布局不因呈现路径漂移 |
| 前置条件 | `aurora_cli` 已构建成功；build/render_probe.json 与 TC-RENDER-001 产出的 build/render_probe.png 已保留 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 用看图工具打开 build/render_probe.png 备查（纯执行，无预期结果）<br>2. 运行 `./build/aurora_cli.exe` preview build/render_probe.json -w 420 -H 160，打开标题为 Aurora Preview 的窗口，观察后关闭<br>3. 逐项对照窗口观感与 PNG：背景色、三行文本的内容与顺序、相对字号与行距 |
| 预期结果 | 2. 窗口正常打开并显示同一棵树；关闭窗口后进程正常退出，stdout 打印 "ok" 为 true 的 JSON<br>3. 两者背景同为浅灰、文本内容与顺序相同、相对字号与行距一致；仅允许边缘抗锯齿约 1px 以内的采样差 |

#### TC-RENDER-012 跨后端观感一致

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-012 |
| 测试目的 | 验证软件窗口（Win32/GDI）与 GL 路径（GLFW 软件纹理上传 / GPU 栅格）对同类 UI 元素的观感一致 |
| 前置条件 | 载体已构建成功；本机有显示后端；**本次构建启用了 AURORA_BACKEND_GLFW**（基线构建未启用时本用例记 SKIP） |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_canvas.exe`（软件 Win32/GDI 路径），记住其渐变标题、文字与卡片边框的观感后关闭（纯执行，无预期结果）<br>2. 运行 `./build/demo_glfw_surface.exe`（软件光栅 + GL 纹理上屏），观察同类元素<br>3. 运行 `./build/demo_gpu.exe`（DisplayList 经 OpenGL 3.3 GPU 栅格），观察同类元素 |
| 预期结果 | 2. GLFW 窗口正常打开：渐变过渡、文字锐度、边框与圆角观感与步骤 1 一致，仅允许约 1px 以内抗锯齿采样差<br>3. GPU 栅格窗口正常打开：上述各项观感仍与软件路径一致，无整块错位、纹理拉花或颜色偏色；若 GPU 初始化失败自动回退软件路径并给出诊断日志，观感仍应正确 |

#### TC-RENDER-013 选择高亮局部重绘无残影

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-013 |
| 测试目的 | 验证选择高亮的脏区局部重绘正确：高亮贴合字形、反选无残留、过程无白屏或撕裂 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_text_input.exe` 打开窗口（纯执行，无预期结果）<br>2. 点击 Pre-filled value example 下方的输入框获得焦点，按 Ctrl+A 全选<br>3. 按住鼠标左键在已选文字上反复拖动，缩小又扩大选区<br>4. 点击输入框内空白处取消选区 |
| 预期结果 | 2. 高亮一次性覆盖 Ada Lovelace 全部字形，色块上下边界与字形行高贴合，无越行或缺行<br>3. 拖动过程中高亮随光标逐字扩展/收缩，离开选区的字形立即恢复原色，无残留高亮块<br>4. 高亮完全消失、无残影；输入框边框与背景保持完整，无整框白屏闪烁或重绘撕裂 |

#### TC-RENDER-014 滚动重绘无错位残影

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-RENDER-014 |
| 测试目的 | 验证滚动容器重绘（含像素重锚点优化）的正确性：内容不错位、无残影条带、边界不破损 |
| 前置条件 | 载体已构建成功；本机有显示后端，demo 可开窗 |
| 依赖用例 | TC-RENDER-001 |
| 操作步骤 | 1. 运行 `./build/demo_scroll.exe` 打开窗口（纯执行，无预期结果）<br>2. 在滚动区域内用滚轮向下滚动直至底部<br>3. 再向上滚动回顶部<br>4. 快速连续大幅滚动后立即停止 |
| 预期结果 | 2. 行内容连续上移、行序正确；滚动区域边框保持完整，区域外的标题与说明纹丝不动<br>3. 回滚后首行 line 0 完整重现，与初始画面无错位<br>4. 停止后无错位条带或半行残影，任意一行文字与其边框对齐如初 |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-RENDER-001 | | | | | | | |
| TC-RENDER-002 | | | | | | | |
| TC-RENDER-003 | | | | | | | |
| TC-RENDER-004 | | | | | | | |
| TC-RENDER-005 | | | | | | | |
| TC-RENDER-006 | | | | | | | |
| TC-RENDER-007 | | | | | | | |
| TC-RENDER-008 | | | | | | | |
| TC-RENDER-009 | | | | | | | |
| TC-RENDER-010 | | | | | | | |
| TC-RENDER-011 | | | | | | | |
| TC-RENDER-012 | | | | | | | |
| TC-RENDER-013 | | | | | | | |
| TC-RENDER-014 | | | | | | | |
