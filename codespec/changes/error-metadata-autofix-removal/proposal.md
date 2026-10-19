# CHG-021 错误码移除自动修复层：删去修复元数据列与运行时修复回调

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-021 |
| 提出日期 | 2026-10-17 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.QUALITY.CORE.STRUCTURED-ERROR.001 |
| 影响面 | 数据源 `codespec/errors.toml`（136 条目各删 `auto_fixable` / `fix_category` 两行，头部声明清单与字段释义两处注释同步）；生成器 `tools/gen/gen_error_codes.cpp`（`ErrorEntry` 两字段、两条解析分支、`fix_category` 兜底校验、`ErrorMeta` 结构两行、结构化绑定两变量、表初始化两行、`is_auto_fixable()` 与 `fix_category_of()` 两枚生成函数、目录表头与单元格、`aurora_api.json` 两字段）；三件生成物 `include/aurora/core/error_codes.gen.h` / `codespec/ERROR_CATALOG.md` / `aurora_api.json`（error_codes 段对象键由 9 降为 7、目录表列由 10 降为 8，随生成器重建）；`include/aurora/core/result.h` 与 `src/aurora/core/result.cpp`（`Error` 删 `auto_fixable` / `fix_category` / `fix_params` 三字段，`make_error_from_table` 初始化同步，`to_json()` 恒输出字段由 7 降为 6）；`include/aurora/core/diagnostics.h` 与 `src/aurora/core/diagnostics.cpp`（删 `FixSuggestion::auto_fix` 回调、`has_auto_fix()`、`apply_fix()` 两重载、`register_fix()`、`auto_fix_all()`、匿名命名空间 `fix_registry()`；保留 `FixSuggestion{code, description}`、`Diagnostic::fix`、`report(..., fix)`、`collect_fixes()`）；测试 `tests/unit/utest_error_codes.cpp` / `tests/unit/utest_result.cpp` / `tests/unit/utest_diagnostics.cpp`；工具文案 `tools/servers/aurora_mcp.cpp` 一处；无新增文件，全部落点为既有文件原地改写。**不在本次范围**：`retryable` 列与其查表函数 `retryable()`——它在 `src/aurora/net/detail/http_retry.cpp` 的 `should_retry_error()` 中有真实消费端，与 `error_class_of()` 归因档取交集决定重放资格，删之属重试策略变更而非清理。 |

## 动机

1. **三列元数据恒无消费者**：`auto_fixable` 除生成头的 `is_auto_fixable()` 与两条单元测试断言外，库内外零消费端；`fix_category` 连一条断言都不服务于语义（仅 `utest_error_codes.cpp` 把它当「非空」的完整性闸）；`fix_params` 从不写入（`make_error_from_table` 恒填 `{}`），故 JSON 中从不出现。三者是「为将来某个自动修复器预留」而立的契约，而那个修复器从未出现。
2. **位置不对**：错误码表描述的是「发生了什么」——域、严重级、文案、修复提示、能否重试。而「能不能被机器改写」是**修复器自身的能力判断**，依赖修复器的形态与现场上下文，不该由一枚静态表列的布尔来回答。把能力判断钉进声明表，等于让错误码替一个不存在的组件背书。
3. **运行时修复回调是死代码且带生命周期隐患**：`Diagnostics` 的自动修复 API（`auto_fix` 回调 / `apply_fix` / `register_fix` / `auto_fix_all`）除 `utest_diagnostics.cpp` 自测外零调用。更糟的是注册表为进程级全局且无注销接口，回调若捕获栈局部变量，用例结束后被后续用例执行即为 UB——现有测试文件为此专门写了文件级静态槽位注释与 `fix_hits(slot)` 规避设施。**测试为绕开设计缺陷而加的脚手架，本身就是该设计该退场的证据。**
4. **收敛两处口径**：`Diagnostic::to_json_line()` 现以「是否带修复回调」决定输出 `fix_code` / `fix_desc`，而 `src/aurora/debug/debug_runtime.cpp` 用的是 `if (d.fix)`。同一件事两处判据。回调删除后，两者统一为「携带修复建议即输出」。
5. **边界先划清**：`retryable` 看着与「修复」同类，实则是**活着**的判据（见「影响面」末尾），本次不动——把「删悬空字段」和「改重试策略」耦成一批，出问题时难以归口。
6. **业界对照一致**：提供 FixIt / QuickFix 的是**编译器与语言服务**（rustc `--fix`、clang、TypeScript / Swift 语言服务），前提是其持有源码与改写授权；而 Rust `std::error::Error`、Go 的 error 包装、Swift `Error` / `LocalizedError` 都不在错误类型上携带「能否被工具自动改写」这类字段——它们只区分「是什么错」（类型 / 码 / 域 / 严重度）与「怎么修」（`recoverySuggestion` / `source()` 链）。本库是应用框架，只产出描述性建议、执行权恒在应用侧；把执行能力挂进错误码与诊断层，既越权又无人消费。删除后错误码的三层分工更清晰：机器契约层（slug）、事实层（category / severity / retryable）、人与 AI 可读层（message / hint / suggestion / docs）。

## 变更内容

1. **`codespec/errors.toml` 删两列**：136 条目的 `auto_fixable = ...` 与 `fix_category = ...` 行全部删除（机械删除，按行首精确匹配，避开与 `category` 的前缀重叠）；头部注释中「声明字段清单」去掉这两项，`fix_category` 的取值枚举说明整段删除。条目顺序、`enum` / `slug` / `category` / `severity` / `retryable` / `message` / `hint` 全部不动。
2. **生成器同步**：`ErrorEntry` 删 `auto_fixable` / `fix_category`；`apply_field` 删两条分支；删 `fix_category` 空值时兜底为 `"unknown"` 的校验；生成的 `ErrorMeta` 结构删两行（含 `fix_category` 注释里对 `auto_fixable` 的引用）；表遍历的结构化绑定由 9 元降为 7 元；表初始化删两行；不再生成 `is_auto_fixable()` 与 `fix_category_of()`；目录表头与单元格删两列；`aurora_api.json` 输出删两字段。`retryable()` 与其生成逻辑原样保留。
3. **三件生成物重建**：经 `generate_error_codes` 目标重刷，不手改。
4. **`Error` 结构体删三字段**：`auto_fixable`、`fix_category`、`fix_params` 及其在 `make_error_from_table` 中的初始化一并删除；字段说明注释与 `to_json()` 的字段清单注释同步改写。
5. **`to_json()` 输出口径**：恒输出字段收敛为 `code` / `message` / `code_enum` / `severity` / `category` / `retryable`；条件输出字段保留 `suggestion` / `docs` / `where` / `hint`；`fix_category` 与 `fix_params` 两个条件输出分支删除。
6. **`Diagnostics` 删回调机制**：删 `FixSuggestion::auto_fix`、`has_auto_fix()`、`apply_fix(std::string_view)`、`apply_fix(ErrorCode)`、`register_fix()`、`auto_fix_all()` 与 `fix_registry()`。`FixSuggestion` 保留为「机器可读 `code` + 人类可读 `description`」的纯展示结构，`Diagnostic::fix`、`Diagnostics::report` 的 `fix` 形参、`collect_fixes()` 全部保留。
7. **诊断 JSON 口径统一**：`Diagnostic::to_json_line()` 的 `fix_code` / `fix_desc` 输出条件由「带修复回调」改为「携带 `fix`」——与 `debug_runtime.cpp` 现有判据一致，也让「上报了修复建议就一定能被看到」成立（此前无回调的建议会被静默丢弃）。
8. **挂载与收集条件改写**：`report()` 的挂载条件去掉回调项，改为「`code` 或 `description` 非空」；`collect_fixes()` 的过滤条件去掉回调项，改为「`code` 非空」——收集口径与原先对「可按码定位的建议」的定义一致，只是不再要求可执行。
9. **测试迁移**：`utest_error_codes.cpp` 删查表一致性里两行、编译期断言里一条；`utest_result.cpp` 删三处字段断言、一条 JSON 形态断言；`utest_diagnostics.cpp` 删 `register_fix` / `auto_fix_all` / 按码应用三例，`fix_suggestion` 一例改为只断言收集与描述，并删除为规避悬垂而设的静态槽位设施，同步改文件头注释。
10. **工具文案**：`tools/servers/aurora_mcp.cpp` 中一处工具描述去掉「供 AI 自动修复」字样，保留「含修复建议」表述。
11. **`FixSuggestion` 语义收口**：删回调后该结构退化为纯描述载荷——只含机器可读 `code` 与人类可读 `description`，不再持有 `std::function`，`diagnostics.h` 相应不再需要 `<functional>`。注释须写明它**不含执行能力**，避免后来者把「修复建议」读成「可执行修复」。
12. **`retryable` 语义钉死**：删除修复元数据后 `Error` 只剩这一枚布尔元数据，其口径在 `01-core.md` §3.1 与本档 §12.3 写为「声明性」——只回答「同一输入重跑一次是否可能换到不同结果」，不替代调用方的幂等性与退避决策。库内唯一按此合成的判定是 `net::detail::should_retry_error()`（`retryable` 与 `error_class_of()` 归因档取交集），其口径不动。
13. **废弃通道取舍（显式记账）**：本批按「alpha 预览版不构成稳定性承诺」（`SPECIFICATIONS.md` §1）**直接移除**这些公共符号，而非先 `[[deprecated]]` 保留一个 MINOR 周期。取舍理由：被删字段零消费端、且随生成器重建，先标废弃再删只让两批变更各背一半状态，收益为零。代价与替代路径记 CHANGELOG.md 的 `Breaking` / `Migration`。
14. **已知缺项（不在本次范围）**：`Error` 尚无错误链 / 根因字段（对标 Rust `Error::source()`、Go `%w` + `errors.Is` / `errors.As`、Python `raise ... from`），下层错误无法携带原始成因，跨层归因只能靠 `where` 文本。这属**新增能力**而非清理，另案提出，不混入本批。
15. **无新增文件**：不新增提案以外的任何源码或文档。

## 验收判据

1. **全仓零残留**：排除 `build*/` 与 `cmake-build*/` 后，检索 `auto_fixable` / `fix_category` / `fix_params` / `auto_fix` / `has_auto_fix` / `apply_fix` / `register_fix` / `auto_fix_all` 零命中；CHANGELOG.md 的历史条目与已归档提案正文豁免（历史档案不改）。
2. **`to_json()` 字段集合**：对一枚 `make_error` 产生的 `Error`，输出恰含 `code` / `message` / `code_enum` / `severity` / `category` / `retryable` 六键，且不含另两者；补齐 `suggestion` / `docs` / `where` / `hint` 后按非空条件出现。变异自证：在 `to_json()` 里补回一个 `auto_fixable` 常真输出 ⇒ 该例转红。
3. **生成器幂等**：连续两次运行 `generate_error_codes`，三件产物零 diff；`check_gen_api_merge` 绿。
4. **诊断 JSON 口径**：携带 `fix`（`code` 与 `description` 皆非空）的诊断，`to_json_line()` 同时输出 `fix_code` 与 `fix_desc`；未携带 `fix` 的诊断两者皆不出现。变异自证：把条件改回常量 `false` ⇒ 该例转红。
5. **重试路径零回归**：`utest_net_http_retry` / `utest_net_http` / `utest_net_http_client` / `utest_clipboard` 涉及 `retryable` 的用例全绿且零改动——这是「保留列未被误伤」的直接判据。
6. **门禁**：`check_api_schema_sync`（先 `aurora_api_json`）/ `check_api_budget` / `check_doc_comments` / `check_naming_conventions` / `check_code_doc_sync` / `check_codespec_xref` / `check_change_proposals` / `check_core_layer_boundary` / `check_umbrella_header` / `format-check` 全绿；`lint` 仅改动 TU；全量 ctest 最高并行度在 Windows LLVM 构建下绿，并另跑 WSL X11 与 wasm 构建验证（改公共头）。

## 回写落点

- `codespec/errors.toml`（删两列 ×136；头部声明清单与字段释义注释同步）
- `codespec/ARCHITECTURE.md`（§12.1 设计目标、§12.2 类型模型：去掉正文字段枚举里的三项；§12.3 分类维度：删 `auto_fixable` 与 `fix_category` / `fix_params` 两行表格，并把 `retryable` 行的用途改写为声明性口径）
- `codespec/specification/01-core.md`（§3.1 Error 字段清单：删三项并补 `retryable` 声明性说明；§4.1 Diagnostic 的 `FixSuggestion` 描述：删回调与 `has_auto_fix()`，写明不含执行能力；§4.2 Diagnostics 收集器方法表：删 `apply_fix` / `register_fix` / `auto_fix_all` 三条，`collect_fixes` 口径收敛为「`code` 非空者」）
- `codespec/specification/06-app-platform.md`（§8.8.x 若干小节中「新增错误码」描述里的 `auto_fixable` / `fix_category` 片段删除，`retryable` 与 `severity` / `category` 保留）
- `codespec/specification/08-tooling.md`（§10.1 需求规格的 `Error` 字段清单同步）
- `codespec/specification/09-cli.md`（§6.2 建议文本中引用自动修复表述的措辞改写）
- `codespec/ERROR_CATALOG.md`（生成物，随生成器重建，不手改）
- 发布记录 CHANGELOG.md（破坏性条目：`Error::auto_fixable` / `Error::fix_category` / `Error::fix_params` 三字段移除及其 JSON 输出键移除；`Diagnostics::apply_fix` / `register_fix` / `auto_fix_all` 与 `FixSuggestion::auto_fix` / `has_auto_fix()` 移除；迁移路径：消费端改读 `hint` 或 `Diagnostics::collect_fixes()` 的建议文案自行决策）——该文档不属 codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步。
