# CHG-013 剪贴板富格式立骨（MIME 词汇表 + uri-list 编解码 + 测试后端）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-013 |
| 提出日期 | 2026-10-13 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.CLIPBOARD-RICH-FORMATS.001 |
| 影响面 | `app/clipboard.h`（同址扩充富格式 API 与 memory 测试后端）、新增 include/aurora/app/mime_types.h（MIME 常量表单一真源）、`src/aurora/app/`（uri-list 编解码纯逻辑）、`codespec/errors.toml` 与生成物、`tests/unit/utest_clipboard.cpp`（扩充，既有用例不动） |

## 动机

1. **现状只有两档且平台残缺**：`Clipboard`（`app/clipboard.h`）仅 text / image 两族——text 三平台（SetClipboardData / xclip·xsel / pbcopy），image 仅 Win32（`CF_DIB`），其它平台报 `GeneralNotSupported`。files / uri-list / HTML / 自定义 MIME 全缺；「从资源管理器复制文件 → 粘贴进应用」这类基本交互在框架上没有路径。
2. **对标全平台内置**：Qt（`QMimeData` 含 urls / html / image / 自定义）、GTK4（`GdkContentProvider`）、Electron（`clipboard.write` 多格式）均把富格式做进剪贴板 API；MIME 是互操作货币。
3. **与系统拖放共用词汇**：CHG-012 的 `SystemDropEvent.formats` 以 MIME 清单表达可用格式；剪贴板若另用一套格式词汇，「拖进来的内容能粘贴、复制来的内容能拖出」的对称性断裂。两者须共用同一张 MIME 常量表（单一真源），但实现分离——剪贴板是按需拉取、拖放是会话期推送后读取，不共用传输代码。
4. **立骨定位**：本提案只交付「类型 + 纯逻辑 + 测试后端」；平台后端（Win32 `CF_HDROP` / "HTML Format" / `RegisterClipboardFormat`、X11 / Wayland Selection 目标协商、macOS `NSPasteboard`）由接线程提案落地。立骨期非测试路径下富格式在无机制平台按错误口径返回，Win32 既有 text / image 路径零回归。

## 变更内容

1. 新增 include/aurora/app/mime_types.h：MIME 常量表单一真源（命名空间作用域 `constexpr std::string_view`，按项目常量命名规范取 `AURORA_MIME_*` 前缀），覆盖 `text/plain`、`text/uri-list`、`image/png`、`text/html` 及 `aurora/widget`（应用内拖拽既有词汇），附 `is_known_mime()` 辅助。`Clipboard` 与拖放后端（接线程）同引此表，禁各处硬编码字符串。CHG-012 的 `SystemDropEvent.formats` 继续用裸 `std::string`（`event/` 不依赖 `app/`），两提案可各自独立合并，本提案不依赖 CHG-012 先行。
2. `app/clipboard.h` 同址扩充（不新开门面）：`available_formats() -> Result<std::vector<std::string>>`（当前剪贴板持有的 MIME 清单；立骨期按既有实现如实返回——`text/plain` 三平台恒在列、`image/png` 仅 Win32 在列，富格式在接线程落地后入列，**不因「新 API 已声明」就谎报可用格式**）；`set_payload(mime, bytes) -> Result<bool>` / `get_payload(mime) -> Result<std::vector<std::uint8_t>>`（自定义 MIME 通用读写）；`set_files(paths) -> Result<bool>` / `get_files() -> Result<std::vector<std::string>>`（`text/uri-list` 便利封装）；`set_html(html) -> Result<bool>` / `get_html() -> Result<std::string>`（映射 `text/html`）。
3. `text/uri-list` 编解码纯逻辑（`src/aurora/app/`，无平台依赖）：编码 = 本地路径逐条转 `file://` URI（百分号编码空格等保留字符）；解码 = 按行切分（CRLF / LF / CR 三种分隔符容错、忽略空行）、`file://` 行百分号解码为本地路径、**非 file scheme 行（`http://` 等）不映射为路径**——`get_files()` 只返回可映射为本地路径的条目、静默跳过其余 scheme（全被跳过时为 `Ok` 空清单，空内容不是失败），完整原始行经 `get_payload(AURORA_MIME_URI_LIST)` 取得。`set_files` 只接受本地路径入参（URL 类内容走 `set_payload`），文档注明该边界。编码 / 解码互为逆变换（对本地路径而言）。
4. 空口径沿旧约并写入 `clipboard.h` 的失败口径表：格式不存在 = `Ok` 空值（空内容不是失败）；本平台无该格式机制 = 新增错误码 `clipboard-format-unsupported`（category = platform、severity = error、retryable = **false**——机制缺失是稳态）；空串 / 空清单写入为 no-op，保留既有内容不触碰剪贴板。
5. memory 测试后端扩充至富格式：`set_payload` / `get_payload` / `available_formats` / `set_files` / `get_files` / `set_html` / `get_html` 全部在内存后端往返；`install_test_backend` / `reset_test_backend` / `remove_test_backend` 三件套与双宏裁切策略不变。
6. 平台实现不落本提案：立骨期富格式 API 在未接平台经测试后端或返回 `clipboard-format-unsupported` / `GeneralNotSupported`（Win32 既有 text / image 实现零改动）；接线程落 Win32 富格式与 X11 / Wayland Selection 协商、macOS `NSPasteboard`（规格随 stub 后端交付）。
7. `codespec/errors.toml` 追加 `clipboard-format-unsupported` 并重生成 `include/aurora/core/error_codes.gen.h` 与 `codespec/ERROR_CATALOG.md`。

## 验收判据

1. uri-list 编解码单测：本地路径 ↔ `file://` URI 往返保真（覆盖空格、中文、`%`、`#`、`?` 等须编码字符的编解码）；混合 `file://` 与 `http://` 行的分类正确；三种分隔符（CRLF / LF / CR）与空行容错；空清单编解码为空的往返。**变异自证**：替换分隔符判读或百分号编解码逻辑，往返用例必须转红。
2. memory 后端富格式往返单测：自定义 MIME payload 往返、files / html 便利封装、`available_formats` 如实反映已写格式；`reset` / `remove` 生命周期与安装态查询。
3. 空口径单测：读不存在格式 = `Ok` 空值（非 `Error`）；写空串 / 空清单 = no-op 且既有内容保留。
4. 既有路径零回归：`utest_clipboard` 既有 text / image 用例全绿且不改动；Win32 image 的 `GeneralNotSupported` 非 Win32 口径不变。
5. MIME 常量表治理：单测断言常量表取值与互操作预期一致（`text/uri-list` 等小写规范形），并以编解码输出对照常量取值，间接断言 `set_files` / `get_files` 走常量而非字面量。**实测项目无「禁硬编码 MIME」的门禁脚本**（`tools/check/` 内无对应检查）⇒ 代码侧无字面量的约束改由评审 + 上述对照断言守护，不声称为门禁能力。
6. API 面治理：`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移；`check_api_budget` 增量评估通过（同址扩充、无新门面）。
7. 门禁与测试：`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md`（§8.2 剪贴板节：富格式 API 契约、MIME 词汇表单一真源、空口径与失败口径表扩充）
- `codespec/errors.toml`（新 slug）与 `codespec/ERROR_CATALOG.md`（生成物同步）
- `codespec/SPECIFICATIONS.md`（需求行已于本提案同批落定）
