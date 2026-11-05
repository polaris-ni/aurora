# CHG-064 `text/uri-list` 解码出口的不可信输入闸门

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-064 |
| 提出日期 | 2026-11-05 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.CLIPBOARD-RICH-FORMATS.001 |
| 影响面 | `src/aurora/app/uri_list.cpp`（解码出口闸门）与 `src/aurora/app/uri_list.h`（契约注释）、`include/aurora/app/clipboard.h`、`include/aurora/event/event.h`（三处文件读取面的文档契约）、`tests/unit/utest_uri_list.cpp`（恶意载荷表驱动用例）；**不改 `decode` / `encode` 签名与返回类型、不新增错误码、不改 `encode` 的逐字节逆变换、不动任何平台消费面**；公共 API 结构无差异 |

## 动机

1. **解码出口对任意百分号序列无条件还原。** `src/aurora/app/uri_list.cpp` 的 `decode` 在 `hex_nibble` 只校验十六进制合法性，就把 `%XX` 直接还原成任意字节；行切分发生在解码之前，`%0D%0A` 解出的 CRLF 不会被重新切分而原样进入路径；`has_file_scheme` 只比前七字节、不校验 authority；出口除剥一个盘符前导斜杠外零校验。于是 `text/uri-list` 载荷可产出内嵌 NUL、控制字节、`..` 遍历段、空条目与非空 authority 形态，而返回类型没有错误通道，所有条目一律当作「可映射为本地路径」交付。

2. **这些条目经两条路离开库，库内却不校验。** `uri_list::decode` 全仓七处调用：五处汇入 `Clipboard::get_files()` 与 `DragPayload::files()` / `SystemDropEvent::files_async()` 交应用（应用通常直接打开），一处进 Win32 拖出方向的 `make_hdrop_global`，经宽字符换算写进 `DROPFILES` 宽串清单交外部进程——内嵌 NUL 会破坏双 NUL 收尾结构与长度计算。现有 ASan / UBSan 全量动态检测对此零报告，正是因为「解析正确但语义危险」，静态与动态检测都报不出。

3. **公共文档没有把边界责任说清。** `src/aurora/app/uri_list.h`、`include/aurora/app/clipboard.h` 的 `get_files()`、`include/aurora/event/event.h` 的三处文件读取面都只写「只返回可映射为本地路径的条目」，既没声明库侧过滤了什么，也没声明调用方还须负责什么；`codespec/specification/06-app-platform.md` §8.2 同样只写编解码规则与「非 `file` scheme 静默跳过」。契约缺位使「危险路径算不算合法输出」无从判定。

4. **净化不能破坏既有往返契约。** `src/aurora/app/uri_list.h` 自陈编码侧与解码侧构成逐字节逆变换，`tests/unit/utest_uri_list.cpp` 把 Windows 反斜杠路径、`%5C` 字面形与 UTF-8 多字节路径的往返钉成既有断言；`tests/unit/utest_clipboard.cpp` 与 `tests/unit/utest_drag_payload.cpp` 各有良性文件清单往返。因此反斜杠作为合法 Windows 分隔符、盘符形态、UTF-8 字节序列必须原样保留，闸门只能针对「结构性非法」，不能按「出现了平台分隔符」一刀切。

5. **本提案的边界。** 库不打开这些路径，也不做真实文件系统的边界校验（符号链接、允许目录、权限）；闸门过滤的是载荷层面可判定为非法或注入的结构，剩余「路径是否落在应用允许范围内」的责任须在文档里明确交给调用方。不改为 `Result<std::vector<std::string>>`：那会牵动七处消费面与两处公共读取面，超出缺陷收口范围，且「不可映射条目静默跳过」是既有统一口径，危险条目按同一口径跳过即可。

## 变更内容

1. `decode` 出口加净化闸门，沿用「不可映射条目静默跳过」的既有口径：单条解码结果出现下列任一情形即整条丢弃，不进入返回清单——内嵌 NUL 或 `0x01`–`0x1F` / `0x7F` 控制字节（含 `%0D%0A` 解码出的 CRLF 注入）、按 `/` 与 `\` 切段后存在独立的 `..` 段、空串条目、`file://` 之后既不以 `/` 开头也不是 `X:` 盘符形态的非空 authority 条目。判定一律作用于**解码之后**的字节，使 `%2E%2E`、`%00`、`%0D%0A` 等编码变体与字面形式同等处理。
2. 保留集不变：字母数字与 `-._~/:`、反斜杠、UTF-8 多字节序列、盘符前导斜杠剥离、三种行分隔符与空行忽略、scheme 大小写不敏感、非 `file` scheme 静默跳过，全部维持原样；`encode` 一字不改，逐字节逆变换与既有往返用例不被破坏。
3. `src/aurora/app/uri_list.h` 的 `decode` 文档写明两件事：库侧已在出口过滤结构性非法条目并静默跳过；调用方仍须负责真实路径边界（允许目录、符号链接、权限、打开前是否复用为 URI）。写清「过滤不等于授权」。
4. `include/aurora/app/clipboard.h` 的 `get_files()` 与 `include/aurora/event/event.h` 的 `DragPayload::files` / `SystemDropEvent::files` / `files_async` 文档同步补同一句边界声明，措辞与 `src/aurora/app/uri_list.h` 一致，避免三处公共面各说各话。
5. `tests/unit/utest_uri_list.cpp` 补恶意载荷表驱动用例：逐组断言遍历段（`..` 与编码变体）、内嵌 NUL、CRLF 注入、控制字节、空条目、非空 authority 被过滤；另加良性对照组（普通 POSIX 绝对路径、Windows 反斜杠路径、UTF-8 路径、盘符形态、混合 scheme 载荷）断言全部保留且顺序不变；并补一例「危险条目与良性条目混排时只丢危险条目、良性条目相对次序不变」。编码分隔符载荷按保留集口径判为合法保留，用例须显式写明「解码后仍是合法路径字符」的理由，不笼统标成危险。
6. `CHANGELOG.md` 的 `### Fixed` 记一条解码出口加固；规格回写见下节。

## 验收判据

1. 恶意载荷逐组可证伪：NUL / 控制字节 / CRLF 注入 / `..` 与编码变体 / 空条目 / 非空 authority 的条目全部不出现在 `decode` 结果中；编码分隔符载荷按「解码后仍是合法路径字符」的口径保留，用例逐字钉住该结论。
2. 良性对照组不误杀：普通 POSIX 绝对路径、Windows 反斜杠路径、UTF-8 多字节路径与 `file:///C:/x` 盘符形态全部原样返回；既有八条 `tests/unit/utest_uri_list.cpp` 用例（含往返保真）零改动全绿，`tests/unit/utest_clipboard.cpp` 与 `tests/unit/utest_drag_payload.cpp` 的文件清单往返用例零改动全绿。
3. 混排用例断言「只丢危险条目且良性相对次序不变」，证明闸门作用于条目粒度而非整份载荷。
4. **判据可辨别力（真注入变异）**：把 `decode` 出口的净化闸门临时短路一次，恶意载荷断言必须成片转红；还原后复跑全绿。良性对照组同时证明判据不是恒真。
5. 公共契约逐处一致：`src/aurora/app/uri_list.h`、`include/aurora/app/clipboard.h`、`include/aurora/event/event.h`、`codespec/specification/06-app-platform.md` §8.2、`codespec/specification/05-event-navigation.md` §5.2 都写明「库侧过滤结构性非法条目」与「真实路径边界归调用方」两层，无一处沿用「只返回可映射为本地路径的条目」这种可被误读为已授权的表述。
6. 公共面零签名变更：`decode` / `encode` 签名、返回类型、错误码集合、伞头清单与公共 API 结构均无差异；改动 TU 的增量 `lint`、`format-check` 与相关单测全绿。

## 回写落点

- `codespec/specification/06-app-platform.md`：§8.2 的 `text/uri-list` 编解码条目补写解码出口闸门过滤集合、沿用静默跳过口径，以及「调用方仍须做真实路径边界校验」的责任声明；保留原有逐字节逆变换与平台分隔符口径。
- `codespec/specification/05-event-navigation.md`：§5.2 的 `files()` 载荷读取条目同步同一口径，避免拖放侧与剪贴板侧文档分叉。
- `codespec/SPECIFICATIONS.md`：在剪贴板富格式与系统拖放两行的说明里补一句「解码出口有结构性非法条目闸门」，使特性表与规格正文口径一致。
