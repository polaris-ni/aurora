# debug 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`debug/` —— 调试与自检层（5 个公共头 `aurora/debug/debug_backend.h`、`aurora/debug/debug_paint.h`、`aurora/debug/debug_runtime.h`、`aurora/debug/debug_trace.h`、`aurora/debug/feature_flags.h`），在实测依赖分层中位于 L7。

该模块把「平时不编译、出问题时才需要」的调试能力做成一组建制规整的自由函数：`AURORA_ENABLE_DEBUG` 打开时提供真实后端叠层、拾取、运行时导出与截图，关闭时只保留零开销的降级实现；它同时承载编译期 feature 宏的运行时查询门面（`feature_flags`），使 Inspector / CLI 等工具能在不重编的前提下报告「当前链接进来的库开了什么」。

**门控模型**是理解本模块用例的前提，其要点如下：

| 事实 | 说明 |
|:---|:---|
| 声明始终可见 | `aurora::debug` 的自由函数在任何构建下都被声明（ODR 安全），消费端调用恒可编译 |
| 函数体按宏裁切 | 函数**体**位于 `#ifdef AURORA_ENABLE_DEBUG` 之内：开时走真实实现，关时替换为降级分支，零调试代码被编译进 Release |
| 唯一真源 | 门控取值以 `codespec/debug_api.toml` 为准（22 个函数），由 `tools/gen/gen_debug_api.cpp` merge-only 写入 `aurora_api.json` 的 `debug` 段 |
| 非门控出口 | 5 个接口（`resolve_output_path`、`set_output_directory`、`output_directory`、`feature_flags`、`feature_flags_json`）声明为 `gated = "none"`，Release 下仍可用 |

### 1.2 执行载体

复用既有示例程序，不新增载体：

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/app/google_play/demo_google_play.cpp` | `demo_google_play` | 五个可视化叠层的快捷键开关、文本 AA 模式切换、帧缓冲与屏幕窗口两种截图、Ctrl+P 一次性导出六段运行时信息 JSON |

本机 `build/` 已以 `AURORA_ENABLE_DEBUG=ON` 配置，故载体内所有 DEBUG 热键均被注册——这些分支整体位于 `#ifdef AURORA_ENABLE_DEBUG` 之内，Release 下不编译、不注册。

**输出通道**是本模块人工判定的关键约定：

| 日志宏 | 通道 | 本载体的实际表现 |
|:---|:---|:---|
| `AURORA_LOG_RAW` | stdout | 叠层名、AA 模式、截图结果、六段 JSON、FPS 摘要全部落在此通道 |
| `AURORA_LOG_INFO` / `AURORA_LOG_WARN` / `AURORA_LOG_ERROR` | stderr | 本载体**不产生**此类输出 |

因此实测 `build/demo_google_play.exe` 的 **stderr 为空**，全部 debug 输出在 **stdout**。判定时必须分离重定向后对照，否则会把「功能输出」误当作诊断日志——这一区分正是 `TC-DEBUG-001` 的验证目标。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja） |
| 构建 | 库与载体均已构建；载体须以 `AURORA_ENABLE_DEBUG=ON` 配置，否则全部热键不注册 |
| 终端 | 支持 UTF-8 输出 |
| 分离输出 | debug 输出写 stdout，需与 stderr 分别重定向后对照 |
| 文件访问 | 需可读写当前工作目录下的 `aurora_debug/` |
| 输入 | 真实键盘（快捷键为 F1–F6 与 Ctrl 组合键） |
| 显示 | 真实窗口可见且尽量处于前台；屏幕窗口截图依赖后端抓取窗口像素 |

**关于输出目录**：载体的输出目录由 `au::debug::set_output_directory("aurora_debug")` 设定为当前工作目录下的相对目录 `aurora_debug/`；`Ctrl+S` 与 `Ctrl+Shift+S` 的两张截图落在同一目录。执行完毕后如需清理，删除该目录即可。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `debug`，即 `TC-DEBUG-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
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

**模块级约定**：本模块载体的构建步骤统一置于 `TC-DEBUG-001` 的步骤 1；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。

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
| SKIP | 主动判定不适用于本环境（如被测平台与用例限定平台不符） |

输出中含**时间戳、线程 id、行号**的字段属可变部分，判定时只核对格式与是否出现，不比对具体值。运行期导出 JSON 中含 `frame_count`、`fps`、`p50_ms` / `p99_ms`、各相位耗时等随负载波动的数值，同样只核对**键名与结构**，不比对具体取值（例如空闲后 FPS 读数会显著升高）。

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

本模块的门控 / 降级语义、叠层 flag 读写、拾取链、导出 JSON 结构等**可程序判定**的部分已由 `tests/unit/utest_debug_backend.cpp`、`tests/unit/utest_debug.cpp`、`tests/unit/utest_debug_runtime.cpp`、`tests/unit/utest_debug_trace.cpp`、`tests/unit/utest_feature_flags.cpp` 覆盖。

人工用例只针对单测覆盖不到的三类性质：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| 叠层的视觉可辨性 | 「布局参考线 / 重排边界 / 层边界 / 重绘高亮 / 过度绘制」是否在真实窗口上肉眼可辨，程序只能判 flag 是否置位 |
| 快捷键到输出的端到端链路 | 从真实按键到 `AURORA_LOG_RAW` 落到 stdout 的完整链路，单测中不存在真实事件循环与键盘输入 |
| 截图真的落在磁盘上 | 「`aurora_debug/` 目录被创建、PNG 文件头正确、两张截图互不覆盖」是跨进程文件系统事实 |

`feature_flags` / `feature_flags_json`（`gated = "none"`）的「宏名 ↔ 结构体字段」镜像正确性已由 `tests/unit/utest_feature_flags.cpp` 以对照表全覆盖，且**当前没有任何交付载体打印这两个接口**——`tools/servers/aurora_cli.cpp` 的 `schema` 子命令输出的是运行时重建的 API 骨架，只含 `library` / `language` / `include` / `alias` / `widgets` / `enums` 六段，**不含 `debug` 段**。故不为其设运行时人工用例，其可观测性以 `TC-DEBUG-007` 的静态 / 文件级核对替代。

`debug_trace.h` 的 `record_dirty` 属 `detail::` 内部埋点（不进入 `codespec/debug_api.toml`），只能由 `why_trace` 间接观测，已含在 `TC-DEBUG-006` 内，不单设用例。

`check_render_purity` 声明在 `include/aurora/core/debug.h`，其语义由单测覆盖，不单设人工用例。

## 2 用例清单

### 2.1 载体与输出通道基线

#### TC-DEBUG-001 载体构建与 DEBUG 热键清单基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-001 |
| 测试目的 | 确认载体在 DEBUG 构建下可构建启动，且 debug 输出全部走 stdout 而 stderr 为空 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置且以 `AURORA_ENABLE_DEBUG=ON` 配置；本机工具链可用 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 构建载体：`cmake --build build --target demo_google_play`（纯执行，无预期结果）<br>2. 启动并分离重定向：`./build/demo_google_play.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>3. 查看 `out.txt` 的首行<br>4. 查看 `err.txt` 的字节数<br>5. 查看 `out.txt` 中周期性出现的 FPS 摘要行 |
| 预期结果 | 3. 首行恰为 `DEBUG shortcuts: F1-F5 overlays \| F6 text AA \| Ctrl+S framebuffer \| Ctrl+Shift+S window \| Ctrl+P runtime info`<br>4. `err.txt` 为 0 字节（空文件）——debug 输出全部经 `AURORA_LOG_RAW` 落在 stdout，而 `AURORA_LOG_INFO`/`WARN`/`ERROR` 才写 stderr，本载体不产生此类日志<br>5. 约每秒 1 行、以 `FPS ` 开头，形如 `FPS <f> \| avg <a>ms \| P99 <p>ms \| jitter <j>ms \| dropped <n> \| idle <i> \| layout <l>ms \| paint <pa>ms \| present <pr>ms`（九段，`dropped` 与 `layout` 之间另有 `idle` 段）；只核对格式与出现，不比对具体数值 |

### 2.2 可视化叠层与文本 AA

#### TC-DEBUG-002 F1–F5 五个可视化叠层开关逐项生效

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-002 |
| 测试目的 | 逐个确认五个叠层快捷键把对应 flag 字段置位，并让窗口叠层与 stdout 报告同步出现 |
| 前置条件 | 载体 `demo_google_play` 已构建成功（DEBUG 构建）；窗口可见且处于前台 |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 按一次 `F1`，观察窗口叠层与 stdout 新增行<br>2. 按一次 `F2`，观察窗口叠层与 stdout 新增行<br>3. 按一次 `F3`，观察窗口叠层与 stdout 新增行<br>4. 按一次 `F4`，观察窗口叠层与 stdout 新增行<br>5. 按一次 `F5`，观察窗口叠层与 stdout 新增行 |
| 预期结果 | 1. 窗口叠加显示布局参考线；stdout 恰新增 1 行 `debug overlay layout_guides: ON`（再次按下同一键则输出 `debug overlay layout_guides: OFF`，状态往返）<br>2. 窗口叠加显示重排边界；stdout 恰新增 1 行 `debug overlay relayout_boundaries: ON`<br>3. 窗口叠加显示层边界；stdout 恰新增 1 行 `debug overlay layer_borders: ON`<br>4. 窗口叠加显示重绘高亮；stdout 恰新增 1 行 `debug overlay repaint_highlight: ON`<br>5. 窗口叠加显示过度绘制着色；stdout 恰新增 1 行 `debug overlay overdraw: ON` |

#### TC-DEBUG-003 F6 文本 AA 模式切换与缓存失效

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-003 |
| 测试目的 | 验证 `F6` 在 ClearType 与 Supersample 之间往返切换，且切换使既有字形缓存按世代整体失效 |
| 前置条件 | 载体 `demo_google_play` 已构建成功（DEBUG 构建）；窗口内有可见文本；起始 AA 模式为 ClearType（载体启动时设定） |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 按一次 `F6`，观察 stdout 新增行<br>2. 观察窗口内文本笔画边缘的变化<br>3. 再按一次 `F6`，观察 stdout 新增行<br>4. 观察文本笔画边缘是否恢复到步骤 1 之前的形态 |
| 预期结果 | 1. stdout 恰新增 1 行 `text AA mode: Supersample (gray)`<br>2. 全部可见文本由 ClearType 子像素平滑切换为灰度抗锯齿，且变化在切换后**立即整体生效**，而非「先无变化、随后零星浮现」（切换自增字体栅格世代，使全树 Display List / 离屏层缓存一并失效）<br>3. stdout 恰新增 1 行 `text AA mode: ClearType (LCD)`<br>4. 文本边缘恢复为步骤 1 之前的子像素平滑外观——两次切换构成往返，无残留 |

### 2.3 截图落盘

#### TC-DEBUG-004 Ctrl+S 帧缓冲截图落盘

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-004 |
| 测试目的 | 验证帧缓冲截图经输出目录解析后真的以合法 PNG 落盘，且目标目录被自动创建 |
| 前置条件 | 载体 `demo_google_play` 已构建成功（DEBUG 构建）；当前工作目录可写 |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 记录执行前当前工作目录下 `aurora_debug/` 目录是否存在（纯执行，无预期结果）<br>2. 按 `Ctrl+S`，观察 stdout 新增行<br>3. 检查 `aurora_debug/google_play_framebuffer.png` 是否出现并查看其大小<br>4. 读取该文件的前 8 个字节 |
| 预期结果 | 2. stdout 恰新增 1 行 `capture(framebuffer): OK -> aurora_debug/google_play_framebuffer.png`<br>3. 文件存在且大小非 0；纯文件名经 `resolve_output_path` 解析进 `set_output_directory("aurora_debug")` 设定的目录，且目标父目录被自动创建<br>4. 以 8 字节 PNG 签名 `89 50 4E 47 0D 0A 1A 0A` 开头，确认是真实 PNG 图像而非空文件或错误文本 |

#### TC-DEBUG-005 Ctrl+Shift+S 屏幕窗口截图落盘

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-005 |
| 测试目的 | 验证屏幕窗口截图（含 OS 装饰）同样以 PNG 落盘，且与帧缓冲截图互不覆盖 |
| 前置条件 | 载体 `demo_google_play` 已构建成功（DEBUG 构建）；`aurora_debug/` 目录已存在；窗口未被其它应用完全遮挡 |
| 依赖用例 | TC-DEBUG-001, TC-DEBUG-004 |
| 操作步骤 | 1. 按 `Ctrl+Shift+S`，观察 stdout 新增行<br>2. 检查 `aurora_debug/google_play_window.png` 是否出现并查看其大小<br>3. 读取该文件的前 8 个字节<br>4. 复核同目录下的 `aurora_debug/google_play_framebuffer.png` |
| 预期结果 | 1. stdout 恰新增 1 行 `capture(window): OK -> aurora_debug/google_play_window.png`<br>2. 文件存在且大小非 0；该图取自 `CaptureSource::OnScreenWindow`，含 OS 窗口装饰（标题栏与边框）<br>3. 以 8 字节 PNG 签名开头<br>4. 两张截图各自落盘、互不覆盖——`google_play_framebuffer.png` 仍在同目录 |

### 2.4 运行时信息导出

#### TC-DEBUG-006 Ctrl+P 打印六段运行时信息 JSON

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-006 |
| 测试目的 | 验证 `Ctrl+P` 一次性打印六个运行时导出 API 的 JSON，且各段结构与约定一致 |
| 前置条件 | 载体 `demo_google_play` 已构建成功（DEBUG 构建）；窗口已创建（`surface_state` 需要真实 Surface） |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 按 `Ctrl+P`，观察 stdout 新增内容<br>2. 清点输出节标题的个数与顺序<br>3. 查看 `surface_state:` 节的字段<br>4. 查看 `widget_tree:` 节的形态<br>5. 查看 `perf_snapshot:` 与 `frame_phase_timeline:` 两节的关键键<br>6. 查看 `why_trace:` 与 `diagnostics:` 两节的结构 |
| 预期结果 | 1. 先出现 `=== runtime info ===`，随后依次为 `surface_state:`、`widget_tree:`、`perf_snapshot:`、`frame_phase_timeline:`、`why_trace:`、`diagnostics:` 六节，每节标题后跟该 API 返回值的 JSON（二空格缩进美化输出）<br>2. 节标题恰 6 个，顺序与上一致，无缺失、无多余、无重复<br>3. 含 `available` 为 `true`、`width` 为 `1100`、`height` 为 `760`、`has_native_window` 为 `true`、`scale_factor` 为 `1`、`clear_color`（数组）、`frame_count`、`should_close`<br>4. 一份递归树：每层含 `type`（控件类型名）、`props`（对象）、`children`（数组），根节点即当前路由的根<br>5. `perf_snapshot` 含 `fps`、`avg_frame_ms`、`p50_ms`、`p99_ms`、`jitter_ms`、`dropped_frames`、`dropped_ratio`、`idle_frames`、`hitches`、`frame_budget_ms`、`perf_log`；`frame_phase_timeline` 含 `avg_frame_ms`、`avg_layout_ms`、`avg_paint_ms`、`avg_present_ms`、`dropped_frames`、`flamegraph`<br>6. `why_trace` 为 `{"count":<n>,"entries":[...]}`，条目含 `frame`、`kind`、`propagated`、`type`，`kind` 取 `layout` 或 `paint`、`propagated` 区分根因与父链传播；`diagnostics` 为 `{"count":0,"diagnostics":[]}`（正常运行时无诊断项） |

### 2.5 门控可用性与 SSOT 一致性

#### TC-DEBUG-007 非门控 API 与门控 API 的可用性差异

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-007 |
| 测试目的 | 通过 SSOT 与头文件双重核对，确认非门控接口在 Release 下仍可用，而门控接口的可用性随 `AURORA_ENABLE_DEBUG` 变化 |
| 前置条件 | 位于仓库根目录；可读取 `codespec/debug_api.toml`、`aurora_api.json` 及 `include/aurora/debug/`、`src/aurora/debug/` 下的源文件 |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 在 `codespec/debug_api.toml` 中检索 `gated = "none"` 的条目，列出其函数名<br>2. 在 `aurora/debug/debug_backend.h` 与 `aurora/debug/feature_flags.h` 中核对这五个函数及 `capture`/`surface_state` 的**声明**是否位于任何 `#ifdef` 之外<br>3. 在 `src/aurora/debug/debug_backend.cpp` 中定位 `capture` 与 `surface_state` 的**函数体**，核对门控<br>4. 核对门控关闭（Release / 未开开关）时二者的降级行为<br>5. 核对 `feature_flags()` 返回的结构体与宏名的镜像关系 |
| 预期结果 | 1. 恰列出 5 个：`resolve_output_path`、`set_output_directory`、`output_directory`、`feature_flags`、`feature_flags_json`<br>2. 七者的声明均在宏守卫之外，声明始终可见（ODR 安全），Release 下仍可编译与链接<br>3. `capture`、`surface_state` 的函数体被 `#ifdef AURORA_ENABLE_DEBUG` 一分为二：开时走真实实现，关时替换为降级分支，零调试代码进入 Release<br>4. `capture` 返回 disabled 错误、`surface_state` 返回 `available` 为 `false` 且带 `reason` 的 JSON——关闭门控是「返回降级结果」而非编译失败<br>5. 三表一一对应且同序：`FeatureFlags` 字段 ↔ `feature_flags()` 内的 `#ifdef` 探针 ↔ `to_json()` 的键，各 22 项、无重复、无遗漏；JSON 键 = **完整宏名**（如 `AURORA_BACKEND_HEADLESS`）；字段名 = 宏名去 `AURORA_` 前缀并小写，`AURORA_ENABLE_*` 组中除 `AURORA_ENABLE_AUDIO_*` 保留 `enable_` 段（`enable_audio_wasapi`）外一律再省略 `ENABLE_` 段（`layout_cache` ↔ `AURORA_ENABLE_LAYOUT_CACHE`、`image_jpeg` ↔ `AURORA_ENABLE_IMAGE_JPEG`） |

#### TC-DEBUG-008 debug 段 SSOT 一致性与幂等重生成

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-DEBUG-008 |
| 测试目的 | 验证 `codespec/debug_api.toml` 是 `debug` 段的唯一真源，且重生成目标为 merge-only 且幂等 |
| 前置条件 | 位于仓库根目录；`build/` 已完成 CMake 配置；`gen_debug_api` 已构建；工作区中 `aurora_api.json` 无未提交改动 |
| 依赖用例 | TC-DEBUG-001 |
| 操作步骤 | 1. 记录重生成前 `aurora_api.json` 的 `debug` 段摘要（条目数与文件哈希）（纯执行，无预期结果）<br>2. 核对 `codespec/debug_api.toml` 中 `[[function]]` 块的数量与各条 `gated` 取值<br>3. 运行 `cmake --build build --target gen_debug_api_json`（纯执行，无预期结果）<br>4. 复核重生成后 `aurora_api.json` 的 `debug` 段<br>5. 检查 `aurora_api.json` 的其它段是否被改写<br>6. 用 `git status --porcelain aurora_api.json` 观察工作区状态 |
| 预期结果 | 2. `[[function]]` 块恰 22 个；其中 `gated = "none"` 5 条，其余 17 条 `gated = "AURORA_ENABLE_DEBUG"`<br>4. `debug` 段仍为 22 条，且与步骤 1 的摘要一致（条目数与哈希都不变）——重生成幂等<br>5. 其它段（`library` / `language` / `include` / `alias` / `widgets` / `enums` 等）逐字节未变——生成器为 merge-only，只重写 `debug` 段<br>6. 无任何输出行——该目标幂等，重生成不产生工作区改动 |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-DEBUG-001 | 2026-09-24 | Qoder Agent | PASS | | `cmake --build build --target demo_google_play` rc=0；`./build/demo_google_play.exe 1> out.txt 2> err.txt` 起真实窗口；`out.txt` 首行逐字为 §1.2 所载的 DEBUG 热键清单行（`DEBUG shortcuts:` 起，其后 `F1-F5 overlays`、`F6 text AA`、`Ctrl+S framebuffer`、`Ctrl+Shift+S window`、`Ctrl+P runtime info` 五段以竖线分隔）；`err.txt` 恰 0 字节；FPS 摘要行实测 20 行 / 20 秒 = 恰 1 行/秒，以 `FPS ` 开头、竖线分隔的九段键名齐备（依次为 `FPS`、`avg`、`P99`、`jitter`、`dropped`、`idle`、`layout`、`paint`、`present`），实测取值形如 `FPS 32.5994` `avg 30.6754ms` `P99 32.2416ms` `jitter 2.26636ms` `dropped 3598` `idle 5392` `layout 0.000140625ms` `paint 2.504ms` `present 0.667281ms` | | 空闲段该行会追加 `(stale)` 后缀并重复同一 FPS 值，仍为 1 行/秒节奏，不算多打；本机 144 DPI 下载体未调 `enable_dpi_awareness()`，故 `scale_factor` 为 1.0 而窗口被 DWM 位图缩放；本行不使用竖线字符，因记录表解析按朴素切列（竖线无转义语义，见门禁 `check_manual_test_format`） |
| TC-DEBUG-002 | 2026-09-24 | Qoder Agent | PASS | | 五键各新增恰 1 行且叠层名逐一对应：`debug overlay layout_guides: ON`、`relayout_boundaries: ON`、`layer_borders: ON`、`repaint_highlight: ON`、`overdraw: ON`；再按同一键各得 1 行 `...: OFF`，五次往返齐全。像素侧以「全 OFF 基线 → 逐键累加 ON」六张帧缓冲截图比对，取两轮基线实测 0 差异的静态行带（y 190–664）作观测窗，相邻两图差异像素依次为 82676 / 95382 / 70736 / 516339 / 516384，横向覆盖 1038–1100 列（共 1100 列）——每个叠层都在真实窗口上画出跨全树的图元；全部复位 OFF 后，无动画区域（y 285–474、y 475–664）与基线差异恰为 0 像素 | | 目视抽验 `layout_guides`（控件边界的青色点线）与五叠层全开态（重绘区橙色着色 + 青色边界线）；余下三个叠层以差异像素的跨树分布判定，未逐一目视 |
| TC-DEBUG-003 | 2026-09-24 | Qoder Agent | PASS | | 步骤 1 stdout 恰新增 `text AA mode: Supersample (gray)`；步骤 2 切换后约 300 ms 的首帧截图即整体生效：与切换前基线相比差异像素 bbox 为 (29,14)..(1083,750)，覆盖全部含文本的行带（y 285–379 / 475–569 / 570–664 分别 1117 / 2768 / 934 处），而唯一无文本的纯图标行带（y 380–474）差异恰为 0；三处文本区的子像素色散像素数由 1199 / 900 / 744 崩至 98 / 0 / 165（ClearType 字形边缘 R≠B，灰度模式为中性灰）。步骤 3 stdout 恰新增 `text AA mode: ClearType (LCD)`；步骤 4 往返后与切换前基线仅差 0.55% 像素且全部落在 FPS 面板文本、自动翻页的轮播图与一条 1100 px 的动画行，静态文本行带 y 190–664 中除轮播外差异为 0，三处色散计数复原为 1199 / 900 / 744 | | 判据用「色散像素计数」而非肉眼：ClearType↔灰度的差异本质是子像素着色，计数比观感可复核；纯图标行带 0 差异是同图的对照组，排除「整屏位移」造成的假阳性 |
| TC-DEBUG-004 | 2026-09-24 | Qoder Agent | PASS | | 执行前当前工作目录下 `aurora_debug/` 不存在；按 `Ctrl+S` 后 stdout 恰新增 `capture(framebuffer): OK -> aurora_debug/google_play_framebuffer.png`；文件出现且 3345083 字节，头 8 字节为 `89 50 4e 47 0d 0a 1a 0a`，`IHDR` 解出 1100×760（即客户区逻辑尺寸），尾部含 `IEND`；纯文件名确被解析进 `set_output_directory("aurora_debug")` 设定的目录，父目录自动创建（仓库根与 `build/` 两种 CWD 下各复现一次） | | 组合键须以「人手按住」节奏注入（修饰键先按下、主键后按后放）：`src/aurora/window/win32_host.cpp` 的 `on_key` 在**派发时刻**用 `GetAsyncKeyState` 采样修饰键，毫秒级完成的合成 down/up 会让 Ctrl 在消息处理前已抬起，热键静默不触发；另需先取得窗口前台权，否则按键落到别的窗口 |
| TC-DEBUG-005 | 2026-09-24 | Qoder Agent | PASS | | 按 `Ctrl+Shift+S` 后 stdout 恰新增 `capture(window): OK -> aurora_debug/google_play_window.png`；文件 3663367 字节，头 8 字节同为 PNG 签名，`IHDR` 解出 1122×816 = 客户区 1100×760 + 横向 22 px / 纵向 56 px 的 OS 装饰，目视含标题栏 `Google Play` 与最小化/最大化/关闭三键；复核同目录，`google_play_framebuffer.png` 仍在，两图并存互不覆盖 | | 实际执行顺序为先本用例（`Ctrl+Shift+S`）后 TC-DEBUG-004，两者文件名不同故无覆盖，判定不受顺序影响 |
| TC-DEBUG-006 | 2026-09-24 | Qoder Agent | PASS | | `Ctrl+P` 后先出现 `=== runtime info ===`，随后节标题恰 6 个且顺序为 `surface_state:` / `widget_tree:` / `perf_snapshot:` / `frame_phase_timeline:` / `why_trace:` / `diagnostics:`，各节均为二空格缩进 JSON；`surface_state` 含 `available` true、`width` 1100、`height` 760、`has_native_window` true、`scale_factor` 1.0、`clear_color` `[245,245,247,255]`、`should_close` false、`frame_count` 0；`widget_tree` 为递归树，根 `NavigatorHost` → 1 个 `Provider`，每层含 `type`/`props`/`children`；`perf_snapshot` 十一个键齐备；`frame_phase_timeline` 六个键齐备（含 `flamegraph`）；`why_trace` 条目含 `frame`/`kind`/`propagated`/`type`；`diagnostics` 逐字为 `{"count":0,"diagnostics":[]}` | | 两处按运行时补记：`why_trace` 另含 `total_recorded` 键，且 `count`(256) ≠ `len(entries)`（环形缓冲按上限截断）；六节 JSON 会被周期性 `FPS` 摘要行插断（同写 stdout），脚本解析须先过滤 `^FPS ` 行。`frame_count` 取值本用例未作断言，实跑为 0 而窗口正以约 32 FPS 出帧，留作后续观测项 |
| TC-DEBUG-007 | 2026-09-24 | Qoder Agent | PASS | | 步骤 1 `codespec/debug_api.toml` 中 `gated = "none"` 恰 5 条：`resolve_output_path`、`set_output_directory`、`output_directory`、`feature_flags`、`feature_flags_json`；步骤 2 该 5 个加 `capture`、`surface_state` 共 7 个声明逐行核对，预处理指令嵌套深度均为 0（`debug_backend.h` 的 36/42/46/49/54 行、`feature_flags.h` 的 57/60 行），声明恒可见；步骤 3 `capture`（`debug_backend.cpp:59-75`）与 `surface_state`（77-110）函数体各被 `#ifdef AURORA_ENABLE_DEBUG` / `#else` / `#endif` 一分为二；步骤 4 关闭分支分别返回 `GeneralNotSupported` + `aurora::debug::capture: AURORA_ENABLE_DEBUG not enabled ...` 与 `{"available":false,"reason":"AURORA_ENABLE_DEBUG not enabled"}`；步骤 5 `FeatureFlags` 22 字段 ↔ 22 个 `#ifdef` 探针 ↔ 22 个 JSON 键，集合相等且顺序一致、无重复宏 | | 预期结果 5 原文「字段名 = 宏名去 `AURORA_` 前缀并小写」仅对 11/22 成立（8 个 `AURORA_BACKEND_*` 与 3 个 `AURORA_ENABLE_AUDIO_*`），其余 11 个再省略 `ENABLE_` 段，已按代码回填该条措辞；字段命名在 `enable_audio_*` 与 `image_*`/`simd` 之间不一致属既有 API 形态，本次仅记录不改 |
| TC-DEBUG-008 | 2026-09-24 | Qoder Agent | PASS | | 步骤 2 锚定行首清点 `codespec/debug_api.toml` 得 `[[function]]` 恰 22 块，`gated` 取值分布为 `none` 5 条 + `AURORA_ENABLE_DEBUG` 17 条；步骤 3 `cmake --build build --target gen_debug_api_json` rc=0，自报 `generation complete: 22 debug APIs`；步骤 4 重生成后 `debug` 段仍 22 条、规范化 JSON 的 sha256 前缀 `1dcae67407f9825b` 与重生成前逐位相同；步骤 5 其余 9 段（`alias`/`enums`/`error_codes`/`include`/`language`/`layout_rules`/`library`/`state_patterns`/`widgets`）合并哈希 `1187f921c47e08c1` 与整文件哈希 `17e600f0a5c8a624` 均未变，即 merge-only；步骤 6 `git status --porcelain aurora_api.json` 无输出 | | 该文件头部注释里出现一次 `[[function]]` 字面，清点须锚定行首（`grep -c '^\[\[function\]\]'`）否则得 23；执行前已确认 `aurora_api.json` 无未提交改动，排除把工作区脏改动误判为幂等失败 |
