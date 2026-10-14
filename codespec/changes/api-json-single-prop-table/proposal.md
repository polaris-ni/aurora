# CHG-016 aurora_api.json 属性单一权威表 + error/布尔去冗余

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-016 |
| 提出日期 | 2026-10-14 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PERF.API.TOKEN-EFFICIENCY.001 |
| 影响面 | 库内 describe 发射层（src/aurora/widget/serialization.cpp）与五个库内消费端（src/aurora/app/validate_ui.cpp、include/aurora/app/ui_prompt.h、include/aurora/app/generate_ui.h、include/aurora/widget/codegen.h 的 descriptors_for()、tools/servers/aurora_lsp.cpp）、error_codes 生成器（tools/gen/gen_error_codes.cpp）、两个门禁（tools/check/check_api_schema_sync.py、tools/check/check_naming_conventions.py）、五个测试文件（tests/integration/itest_cli.cpp、itest_mcp.cpp、itest_known_enums.cpp、itest_to_code.cpp、tests/unit/utest_ui_prompt.cpp）、codespec 文档与 manual-test 22-inspector；aurora_api.json 由生成器重建，不手改 |

## 动机

1. **预算濒临裁决点**：aurora_api.json 现为 277,098 B ≈ 69,274 tokens（check_api_budget 口径 4 B/token），距 TOKEN_BUDGET 75,000 仅剩 7.6% 余量。按当前增速，下一次大特性就要靠上调预算裁决，与 SPEC.PERF.API.TOKEN-EFFICIENCY.001「Token 效率」的初衷相悖。
2. **同一批属性在磁盘上存了四份**：prop_descriptors（433 条）/ props_schema（433 条）/ default_props（535 条）/ props（535 条名字列表）。实测 props_schema 是 prop_descriptors 的严格子集重编码（唯一独有子键 description 即 note，type/minimum/default/enum/maximum 零不一致），除 check_naming_conventions 读键名外无消费端；props 与 default_props 的键逐 widget 含顺序完全一致，是纯投影。四块合计约 175 KB，占全文件 63%。
3. **四份并非严格 1:1，而是互补投影**：并集 610 条——177 条布局公共属性（overflow×68 / width×34 / height×33 / show×35 / offset×2 及 5 个单例）只存在于 default_props / props，75 条只存在于 prop_descriptors；props_schema 在 4 个 widget 整体缺失（其属性数本就为 0，无损失）。单一权威表必须覆盖并集；已评估并否决「只留 props_schema」的有损方案——那会丢 11 个 required 标记与 cpp 类型名，令 validate_ui 的必填与类型检查、修复回填的保真度退化。
4. **error_codes 段一个可推导字段**：index 恒等于数组下标（91/91、零消费端），删除。slug **保留**：它不是机械可推导的——除 5 处 `IO*` 缩写族（缩写感知驼峰转 kebab 可覆盖）外，`RuntimeAsyncTimeout`→`async-timeout`（丢 `Runtime` 前缀）与 `GenerateUiEmpty`→`generation-ui-empty`（`Generate`→`generation` 换词）是语义改写，任何字符级规则都推不出；slug 又是 ERROR_CATALOG 明文的冻结外部契约，删之等于迫使每个消费者各背一张例外表。fix_category 与 category 正交（io 类对应 5 种、validation 类对应 7 种），不删；category / severity / auto_fixable / retryable 均携真实信息，不删。
5. **widget 段布尔/常量噪声**：thread 恒为 "main"（73/73，全域零消费端）；is_layout 与 container / is_container 为同一表达式（73/73 三键同值）；is_clickable（1/73）、dynamic_children（2/73）、container（7/73）为稀疏事实，显式输出 73 次是浪费。
6. **键名压缩不做**：键名共现约 7,025 次，压缩虽省得更多，但 schema 的直读性本身就是 AI-first 定位的价值；note / description 同义字段对在单一权威表统一后自然消除，无需单独处理。
7. **default 的两种表示必须分清**： PropDescriptor::default_value 是**显示串**（433 条里 158 条是 C++ 风格表达式：`auto`/`Color::white()`/`{8,8,8,8}`/`theme.primary`/`nullptr`…，并非 JSON 字面量），default_props 是**原生 JSON 值**（serialize_props 实测输出）。75 条描述符属性（CHG-011 后的主题角色色、Timer.on_tick 等运行态项）只存在于描述符、无原生值。

## 变更内容

1. **单一权威属性表**：component_schema() 停止分别发射 prop_descriptors / props_schema / default_props / props 四块，改发射**一个** props 数组。取数组而非对象：itest_to_code 与 codespec/specification/08-tooling.md 的代码生成依赖 *Props 声明序，对象键序不可依赖。每条形如 {name, type, default?, default_expr?, required?, note?, enum?, minimum?, maximum?, pattern?, constraint?, requires_props?, conflicts_with?}：name / type(cpp) / note / enum / minimum / maximum / pattern / constraint / requires_props / conflicts_with 全部来自既有 PropDescriptor 字段集（descriptor_to_json 已定义，发射层复用）。**default 一律为原生 JSON 值**（serialize_props 实测输出；修复回填与 prompt 投影的可信源，见 ui_prompt.h 注释口径）；**default_expr** 为描述符显示串，仅 75 条无原生默认值的属性输出（其默认信息只以该形态存在；有原生值时显示串与原生值同指一件事，按等价判据不再保留第二表示）。required 仅 true 时输出（11 处）；json_type 删除（379 处、除命名门禁读键名外无语义消费端）。
2. **布局公共属性入表**：177 条只存在于 serialize_props 的布局公共属性以同一数组补入权威表，type 取真实 C++ 类型（width / height→Length、show→bool、overflow→string、offset→float、snap_action_count / visible_count→int、menu_titles / segments / tab_labels→std::vector<std::string>——取真实成员类型而非泛化 "array"，itest_known_enums 的 primitive 过滤依赖 `std::` 前缀）。每 widget 的属性集合 = descriptors ∪ serialize_props 键（610 条并集）：序取 descriptors 声明序，布局公共条目依 serialize_props 序追加。
3. **error_codes 段**：仅删除 index 字段（91/91 恒等于数组下标）；slug 按动机第 4 条保留。codespec/errors.toml、error_codes.gen.h、ERROR_CATALOG.md 三个既有产物同源生成、不受影响。
4. **widget 段布尔键**：删除 is_container（与 container 重复，ui_prompt.h 一行改读 container）、is_layout（与 container 同表达式同值）、thread（常量无消费端）；container / is_clickable / dynamic_children 改稀疏输出（仅 true 时出现，读取侧 as_or 默认 false 语义不变）。消费端同批同步：validate_ui.cpp 改读单一 props 表取 required 与 type；ui_prompt.h 改读 container 与单一 props 表（default 原生值直接用于修复回填；prompt 投影只对带 default 的条目带出取值）；generate_ui.h 的文案候选键存在性判据改查 props 表键集（三个候选键均不在「仅描述符收录」集合内，两口径等价）；codegen.h 的 descriptors_for() 改读单一 props 表（声明序 = 数组下标，type / enum 字段同名，一行键名修改）；aurora_lsp.cpp 的 PropSchema 投影改读单一 props 表（default 非字符串时 dump 成字符串，该分支已存在）。
5. **门禁同步**：check_api_schema_sync.py 的 prop_keys() 改读单一 props 表（两侧同源，零漂移判据不变）；check_naming_conventions.py 的属性键收集改读 props[].name；check_api_budget 口径不变（全文件字节 / 4）。
6. aurora_api.json 由 --target aurora_api_json 重建，不手改；error_codes 段由 gen_error_codes 同批重建。

## 验收判据

1. **信息无损等价**：一次性等价校验（不入库）：旧版每个 (widget, prop) 对及其全部字段值（name / type / required / note / enum / minimum / maximum / pattern / constraint / requires_props / conflicts_with，default 以原生值口径）必须能从新版单一 props 表逐条恢复；610 条并集一条不少（75 条 descriptors 独有条目、177 条布局公共条目全部在表）；75 条无原生值属性的显示串经 default_expr 原样恢复。**变异自证**：从新版表删去一个布局公共条目（如 Badge 的 overflow）或改错一个 default，等价校验必须转红。
2. **预算判据（独立基准）**：重建后 check_api_budget 实测 ≤ 45,000 tokens（预期约 39,000，余量 ≥ 40%）；73 个 widget、25 个 enum、91 条 error_codes 的计数不变。
3. **零漂移**：check_api_schema_sync 绿（代码视角与提交物同源）；check_gen_api_merge 绿（error_codes / debug 段合并不截断）；check_change_proposals 绿。
4. **消费端行为不回退**：validate_ui 对缺必填属性与类型错配的检出用例全绿；utest_ui_prompt 的投影与修复回填用例全绿；itest_to_code 的声明序用例全绿（证 array 保序）；itest_cli / itest_mcp / itest_known_enums 全绿。
5. **error_codes 字段判据**：新版 error_codes 条目数组序与旧版 index 序列逐一对应（91/91，证下标可派生）；slug 字段逐条保留（含 RuntimeAsyncTimeout / GenerateUiEmpty 两处语义改写），与 errors.toml 声明一致。
6. **手工用例实测回写**：codespec/manual-test/22-inspector.md 的 TC-INSPECTOR-008 及其预期行按 inspector /api/components 实测输出重写（新键集、新字节数，逐字对齐实测）；既有编号连续不变。
7. **门禁与全量**：check_naming_conventions / check_doc_comments / check_platform_macros / check_core_layer_boundary / check_no_hardcoded_paths 全绿；lint（仅改动相关 TU）与 format-check 绿；全量 ctest -j16 绿（含 wasm 与 Linux 两腿——公共 describe 格式跨平台同源）。

## 回写落点

- `codespec/specification/08-tooling.md`（§2.5.1 发射管线的自描述数据源、§2.6 自修复环回填口径、§6 自描述发现 API 表、§10.3 示例 JSON 与字段口径：四块属性副本改单一 props 表的字段定义与声明序口径）
- `codespec/CODING_STANDARDS.md`（§6.12 描述 component_schema() 字段增强的段落改单一权威表口径）
- `codespec/manual-test/22-inspector.md`（TC-INSPECTOR-008 及预期行按新键集与实测字节数重写）
