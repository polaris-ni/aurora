# CHG-035 i18n 构建期消息管线与运行期目录重构

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-035 |
| 提出日期 | 2026-10-22 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `include/aurora/i18n/localized_string.h`、`string_table.h`、`locale.h`、`plural.h`、`format.h`（值模型与目录改型，**破坏性**，迁移表见变更内容第 2 条）、新增 include/aurora/i18n/ 下 message_id.h、message_catalog.h、builtin_messages.h（+ builtin_messages.gen.h）与 src/aurora/i18n/ 实现 TU（含库内 detail/message_engine.h、detail/plural_rules.h 与生成件 plural_rules.gen.cpp，均不进公共面）；新增 tools/gen/gen_i18n.cpp、tools/gen/gen_plural_rules.py、tools/check/check_i18n_gen_golden.py；`cmake/AuroraTools.cmake`、`cmake/AuroraTests.cmake`、新增 cmake/AuroraI18n.cmake；新增数据源 codespec/i18n_builtin.json（CLDR 规则源数据照 UCD 先例不入库，见变更内容第 12 条）；`tests/unit/` 相关 utest 扩展、新增 `examples/demos/demo_i18n.cpp`；`codespec/specification/07-environment-modifier.md`、`codespec/specification/08-tooling.md`、`codespec/ARCHITECTURE.md`、`codespec/GUIDELINE.md`、`codespec/manual-test/02-i18n.md`、`codespec/errors.toml`、`codespec/ERROR_CATALOG.md`；`aurora_api.json` 重建（公共 API 变更） |

## 动机

1. **运行时半边已有，构建期是空白。** `LocalizedString` / `StringTable`（含 CLDR 六类复数与两级回退）/ `LocaleProvider` 响应式接线均已落地（`include/aurora/i18n/` 全套 + `codespec/specification/07-environment-modifier.md` §6），但词条只能运行期 `add()` 逐条注册：key 以字符串字面量散落调用点，拼错即查表 miss 静默回退（`string_table.h:76-77`），没有任何编译期或生成期信号。
2. **位置占位在译文调序时静默错位。** `{0}/{1}` 的下标语义随语言漂移是位置模型的固有缺陷；译者重排语序后占位含义改变，运行期无从发现。
3. **复数分支内打印未分组裸数字。** 选择用裸值与展示用格式化值共用同一字符串（`string_table.h:141-149`），`en` 下 1234 条显示为 "1234" 而非 "1,234"，这不是缺功能，是选择器与展示两种职责挤在一个占位上。
4. **内置文案 key 双端字面量。** 控件端与宿主注册端各写一份 `command_palette.placeholder`，靠单测钉字符串防对不上（`07-environment-modifier.md` §6.2 自述该脆弱性）。
5. **props JSON 压扁丢失本地化信息。** 序列化只落 `text`，`key/args` 丢弃（`src/aurora/widget/text.cpp:157`，Button 同口径），JSON 驱动 UI 无法引用词条。
6. **CLDR 手写表覆盖不全。** 现规则表仅 10 个语系族（`plural.h:127-143`），小数操作数按 double 截断处理，ordinal 类别（`1st/2nd/3rd`）完全缺失。

## 变更内容

1. **裁决总表（已按「不计工作量、只取最优设计」口径收口，破坏性候选全数采纳）**：

   | 项 | 收口结论 |
   |:--|:--|
   | 产出形态 | bin 数据外置 + 薄生成头（翻译迭代不触碰 C++ 构建） |
   | 消息语法 | **ICU MessageFormat 1 子集**（命名占位 / `plural`+`selectordinal`+`select` / `offset:N` / `=N` 精确分支 / 分支内 `#` / `:number`·`:currency`·`:date` 函数 / 引号转义），`format()` 由自研消息引擎替换；MF2 不做（C++ 实现尚在 tech preview），留演进 |
   | 查表模型 | **封闭词表强类型 `MessageId`** + 单缓冲冻结表 O(1) 直排；字符串 key 降为派生规范名（生成表反查，供序列化/诊断） |
   | 生成 API | 每 key 一枚 inline 函数，`.` 分段映射嵌套 namespace；形参名与类型来自**必填显式元数据** |
   | 回退链 | 载入期物化六档指针表：`language-Script-Region → language-Script → language-Region → language → default tag → default language`（先丢 region 后丢 script，同 CLDR 右删次序，子标签缺失的档跳过）；`Locale` 增 `script` 子标签 |
   | bin 打包 | 全语言合一；locale 切换 = 当前视图指针切换，零重载 |
   | props JSON | 并入本切片：`{"$msg": {"id", "args"}}` 保真往返 |
   | 新增设计件 | 自研消息引擎（解析→选择→格式化三段式）；内置词表同管线（狗粮）；`Window::set_locale` 帧级根注入；CLDR 全量 cardinal+ordinal 规则表构建期生成入库；缺译聚合诊断 + 查询 API |

2. **破坏性迁移表（CHANGELOG 按 alpha 口径记 breaking + 迁移路径）**：

   | 旧（作废） | 新 | 迁移 |
   |:--|:--|:--|
   | `LocalizedString::tr("key", 位置 args)` | `tr(MessageId, 具名 args)` / 生成函数 | 机械替换：key 字面量 → 生成 Id 或函数调用 |
   | `StringTable` / `default_string_table()` | `MessageCatalog` / `default_catalog()` | 类名替换；批量走 bin，稀疏走保留的名字入口 `add()` |
   | `{0}` 位置模板 | MF1 命名模板 | 翻译侧一次性改写（`{0}` → 具名占位；分支内 `{0}` → `#`） |
   | `args` 可嵌套 `LocalizedString` | `MessageArg` 纯值（string / int64 / double） | 嵌套处先 `resolve` 再传串 |
   | `Locale{lang, region}` 位置初始化 | 增 `script` 字段 | 指定初始化器形态零改，位置初始化调用点逐处迁移 |
   | 内置 key 字面量查表 | `aurora::msg` 词表常量解析 | 规范名拼写不变 |
   | 库内既有调用点（`button.h` / `command_palette.h` / `reorderable_list.h` / `rich_text.h` / `text.h` 与五个相关 utest） | Id 解析 + 具名参数 | 随批机械迁移，规范名不变 |

3. **词表与 ID 空间**：`MessageId` 为 message_id.h 中的**唯一强类型**（struct 包 uint32_t，各词表共用一型，生成侧产 `inline constexpr` 常量——跨词表可进同一目录直排，且无 int 隐转；若每词表各造一枚 enum class，`LocalizedString::tr` 的单一签名反而无法同时收两个词表的类型）；`[0, 1024)` 为内置词表保留区，应用词表缺省基址 1024（`--id-base` 显式可调）；`canonical_name(MessageId)` 生成表反查。`LocalizedString` 改型为 `{text, id, args, localize}`，`MessageArg = {name, variant<string, int64, double>}`，**放弃嵌套本地化参数**（需要嵌套先 `resolve` 再传串）；`operator==` 比较 text 与 id（args 不参与，同现行口径 `localized_string.h:56-59`），无效 Id 的 resolve 走字面 text。
4. **冻结目录 `MessageCatalog`（取代 `StringTable`）**：`load_binary(span<const std::byte>)` 缓冲驻留、同 locale 后到覆盖并重建视图；id 越界 / 词表区间重叠 / magic / version / 截断 → 结构化错误（`errors.toml` 新增 i18n 组，经 `generate_error_codes` 重建三产物）；模板解析缓存挂视图槽；`resolve` 纯 O(1)。文件读取留应用侧，Aurora 只收内存字节——不扩 IO 面，wasm 同样成立；`add()` 名字入口对未知规范名返回结构化错误（名字索引为载入期排序数组，二分查找）。
5. **bin 格式（小端，magic `AUM1` version 1）**：header（id_base / id_count / locale_count）→ locale 目录（tag）→ 每 locale 的 id 直排偏移表（`0xFFFFFFFF` 缺译哨兵）→ UTF-8 字符串池。仅承诺小端并在载入时校验（非小端 → 结构化错误；现役目标 x64 / ARM64 / wasm 均小端）；`id_base` 承载词表区间 `[id_base, id_base+id_count)`，目录据此检测多 bin 区间重叠。
6. **消息引擎**：自研三段式（解析→选择→格式化），`plural`/`selectordinal` 按 CLDR 类别求值（`=N` 精确优先、`offset:N` 参与判定），`select` 精确串匹配，分支内 `#` = selector 值按类型格式化后的展示串；占位格式化复用 `format_number` / `format_currency` / `format_date`，新增 `CivilDate{year, month, day}` 值类型承接日期形参；`format_date` 追加缺省 `style` 形参承接 short/medium/long（源兼容，现行签名零迁移）。引擎实现私有，公共面只有 `resolve`。
7. **生成器 `gen_i18n`**：CLI 全显式（`<out.hpp> <out.bin> <cpp-namespace> [--id-base N] <base.json> [<locale.json>…]`）；基准文件 ARB 布局（词条 + `@key` 元数据），译文文件纯词条；校验分 error / warn 两级（元数据覆盖、函数与类型匹配、selector 类型约束、跨语言结构树比对、CLDR 分支覆盖对照、`#` 作用域、UTF-8、key 形态、嵌套深度上限）；输出确定性（同输入逐字节同输出）。工具链 `aurora_json`，Emscripten 交叉走宿主原生子配置。
8. **CMake**：库内 `gen_i18n` target；下游助手 `aurora_i18n_generate(TARGET … NAMESPACE … BASE … LOCALES … OUT_DIR … [ID_BASE n])`（新 cmake/AuroraI18n.cmake，随本提案落地）：头入 target 源、bin `copy_if_different` 到可执行目录；golden check 注册进 `AuroraTests.cmake`。生成头经 `#include` 进入 demo 翻译单元，而 `lint` 目标不走编译链，故 `lint` / `lint-fix` 在 configure 期自动依赖词表生成目标（全局属性登记，demos 关闭时清单为空），避免「只配不编」的构建目录里生成头缺席、门禁只报出一条 file-not-found。
9. **locale 帧级注入**：`Window::set_locale(loc)` 存值、下一帧 `prepare_context` 根注入 `set<Locale>`（与 Theme/WindowState 同构）→ 帧内语言一致；`LocaleProvider` 子树覆写保留（优先级高于窗口级）；未注入回落缺省档口径不变。
10. **内置词表狗粮**：源数据 codespec/i18n_builtin.json（起步即 `command_palette.placeholder` / `command_palette.no_results` 两条 key，规范名拼写不变）→ 生成保留区词表常量与规范名表；gen_i18n 内嵌同一份词表并自动并入每个生成 catalog（应用译文直接覆盖内置 key，同一通道）；控件端按 Id 解析、miss 回控件内英文字面量，三档优先级保持；**「库不预置任何语言的词条」原则不动**。控件端迁移面不止 command_palette：`button.h`（label 与无障碍）、`command_palette.h`、`reorderable_list.h`（公告串 `announce_text` 位置形参随 `MessageArg` 改具名）、`rich_text.h` / `text_span.h`（片段文本与 props 反序列化）、`text.h`。
11. **props JSON 保真**：`localize` 时输出 `{"$msg": {"id": "<规范名>", "args": {…}}}`，纯文本仍为 string，读侧双形态兼容；`PropDescriptor.json_type` 扩展；修复 `text.cpp:157` 与 Button 同口径的压扁缺口。
12. **CLDR 全量复数表（照 UCD 先例：源数据不入库）**：转换脚本 tools/gen/gen_plural_rules.py 一次性消费 CLDR 48 的 plurals.json / ordinals.json（稀疏拉取 cldr-json，源文件**不 vendored**），导出 src/aurora/i18n/plural_rules.gen.cpp（首行 `AUTO-GENERATED`，头部注明 CLDR/Unicode 版本、来源文件与再生成命令；lint 豁免、format 对齐——UCD 区间表与字体数据同款先例）；cardinal + ordinal 双表、全语言；复数操作数按十进制串推导；`plural_category()` 公共签名不变，实现换表驱动；生成器校验与运行时选择同表同源；许可索引补 `THIRD_PARTY_LICENSES.md`（不属 codespec 树，按硬规则 5 只在此说明）。
13. **无头可测性**：引擎、目录、生成器校验、回退物化、序列化全部无头可证（utest / golden）；`demo_i18n` 仅做真机走查，不引入 GUI 交互测试。
14. **不做清单（独立申报）**：mmap / 零拷贝（wasm 无 mmap，量级收益趋零）；MF2（生态未起，bin 带版本号留演进）；OS locale 自动检测；网络热更（bin 是纯数据文件，应用自取）；feature 宏（i18n 头常编，生成器随常建工具走）；新增 IO / 三方运行期依赖；`:time` 函数 / `percent` 样式 / 复数范围（plural ranges，即「1–3 个文件」区间选型）。

## 验收判据

1. **golden 门禁绿**：fixture 覆盖 `plural`/`offset`/`=N`/`selectordinal`/`select`/`:number`/`:currency`/`#`/引号转义/嵌套 → 生成头独立可编译、bin 经 `load_binary` 后按 locale 逐条命中；同输入重复生成逐字节一致。
2. **单测绿**：bin 截断 / 错 magic / 错 version / id 越界 / 词表区间重叠 / 非小端 → 结构化错误码；六档回退逐档命中（含 zh-Hans 档）；`=N` 优先于类别；`offset:N` 语义；`#` 输出已分组数字（en `1,234`）；`select` 精确匹配与 other 兜底；ordinal 表（en `1st/2nd/3rd/4th`）；缺译诊断计数与 `missing_translations` 一致；`add()` 稀疏路径与 bin 覆盖次序、未知规范名报错；`CivilDate` 与 `format_date`（含 style 形参）对齐。
3. **内置词表同源锁**：`aurora::msg::Id` 与 gen_i18n 内嵌词表出自同一源数据（golden 双向比对）；两条 `command_palette.*` 规范名回归钉死（钉规范名↔Id 映射，不自比自）。
4. **帧级注入**：`set_locale` 当帧离屏快照不变、下一帧切换；`LocaleProvider` 子树覆写优先于窗口级。
5. **props 往返**：`{"$msg"}` serialize → from_json 逐字段还原；旧 string 形态读侧兼容不回归。
6. **demo 实测**：`demo_i18n` 运行期切 locale 文本即时切换、帧内无混排（manual-test 走查）。
7. **门禁**：`check_api_schema_sync`（先 `--target aurora_api_json`）、`check_naming_conventions`、`check_doc_comments`、`check_api_budget`、`check_umbrella_header`（两个新公共头入清单）、`check_core_layer_boundary`、`check_arch_module_map`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_change_proposals`、`check_version_consistency`、`check_no_cjk_literals`（demo 词条全走外部数据）、`check_no_hardcoded_paths`、`check_gen_api_merge`、`lint`（`--print-tus` 确认新 TU 入集）、`format-check` 全绿；wasm 配置补跑；全量 ctest 绿。
8. **变异自证**：删 `=N` 精确比对 → 判据 2 转红；摘掉结构变更后的视图失效（`MessageCatalog::invalidate()` 不清物化快照）→ 判据 2 的「bin 后到覆盖稀疏值」读到旧值转红（`utest_message_catalog` 的 `binary_reload_overwrites_sparse_and_rejects_conflicts`；口径说明：物化本身是形状不变量，逐次查与物化在可观测行为上等价、无判据可钉，故自证落在失效一致性上）；回退阶梯丢 zh-Hans 档 → zh-Hans-CN 用例转红；删 `$msg` 写侧 → 判据 5 转红；内置词表双端各自维护 → 判据 3 转红；去掉确定性排序 → 判据 1 转红；**去掉换档时的向下缓存失效**（`Provider<T>::on_signal_dirty` 不再调 `invalidate_subtree_caches`，或 `Window::set_locale` 的待处理标记不消费）→ 判据 4 与判据 6 的换档用例转红（`itest_locale_injection` 第三条：`State<Locale>` 换档后文本仍是旧语言）。

## 回写落点

- `codespec/specification/07-environment-modifier.md`（§6 全面重写：值模型 / MF1 语法 / 目录模型 / 帧级注入 / 内置词表契约；§6.2 key 外部契约升级为「规范名↔Id」口径）
- `codespec/specification/08-tooling.md`（gen_i18n CLI 契约、校验清单、golden check）
- `codespec/ARCHITECTURE.md`（§14.6 门禁矩阵加一行：strings JSON 变更 → `check_i18n_gen_golden`）
- `codespec/GUIDELINE.md`（生成 API 与目录载入用法片段）
- `codespec/manual-test/02-i18n.md`（取下一个可用 `TC-I18N-NNN` 序号增补：真机切 locale、缺译诊断走查）
- `codespec/errors.toml` 与 `codespec/ERROR_CATALOG.md`（i18n 组错误码，经 `generate_error_codes` 重建）
- 收尾：本提案状态随批次流转（已提议 → 实施中 → 已归档）。
- 版本与发布记录：仓库根变更记录 [Unreleased] 节 Added / Changed（breaking）分条 + 迁移表；是否随本批升 alpha 号由实施批次定，收口时一次性同步版本三处并跑 check_version_consistency。上述变更记录不属 codespec 树，按硬规则 5 只在此说明、不列为回写落点。

## 消费侧用法（最小示例）

```cpp
// 构建期（CMakeLists.txt）：aurora_i18n_generate(TARGET myapp NAMESPACE myapp::msg
//         BASE messages_en.json LOCALES messages_zh-Hans-CN.json OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/i18n)
// 产出两件：messages_en.hpp（Id 常量 + 具名形参构造函数）与 messages_en.bin（AUM1，全语言合一）。

// 启动：载入 bin（文件读取在应用侧，库不扩 IO 面），设缺省语言档
const std::vector<std::byte> bin = read_whole_file("messages_en.bin");
if (auto loaded = au::default_catalog().load_binary(std::span<const std::byte>{bin}); !loaded) {
    /* 结构化错误：magic / version / 字节序 / 截断 / Id 越界 / 词表区间重叠 */
}
au::default_catalog().set_default_locale(au::Locale{.language = "en"});

// 组树：生成 API 具名强类型（形参名与类型取自 @key.placeholders，误用编译期即拦下）
au::Text{au::TextProps{.content = myapp::msg::demo::greeting(3, "Ada")}};
// 等价的手写形态：Id 常量 + 具名参数
au::Text{au::TextProps{.content = au::LocalizedString::tr(
    myapp::msg::id::AURORA_DEMO_GREETING,
    {{"count", std::int64_t{3}}, {"name", std::string{"Ada"}}})}};

// 切语言：两条注入通道——子树 `LocaleProvider{State<Locale>}` 优先，窗口级 `set_locale` 作默认；
// 两者换值时都向下清子树缓存，故自下一帧起整屏重解析、帧内不混排。
window.set_locale(au::Locale{.language = "zh", .script = "Hans", .region = "CN"});
```
