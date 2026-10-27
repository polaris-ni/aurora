# CHG-044 文件对话框的另存为意图：建议文件名与缺省扩展名（附初始目录的接口与实现分叉修正）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-044 |
| 提出日期 | 2026-10-27 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.FILE-DIALOG.001 |
| 影响面 | `include/aurora/app/file_dialog.h`（`Options` 两枚新格 + 一枚 headless 记录用全局 + 三分支注释段）、新增折算层 `src/aurora/app/detail/file_dialog_plan.{h,cpp}` 与同目录的 Win32 内部接缝头 file_dialog_win32_apply.h、`src/aurora/app/file_dialog_win32.cpp`（三枚 OS  setter 接线 + SDK 版本旋钮）、`src/aurora/app/file_dialog_selfdrawn.cpp`（状态机播种 / 输入框初值 / 确认时补扩展名）、两枚测试 TU 与一枚真机探针；`codespec/` 四处回写 |

## 动机

1. **另存为缺「建议文件名」这一格，而两条真实现腿手上都有原生接缝。** 读源实测：`Options` 只有 `title` / `initial_dir` / `filters` 三格；Win32 腿的 `save_file` 走 `IFileSaveDialog`，`IFileDialog::SetFileName` 就在它的继承面上、一次没调；桌面自绘腿本来就画了一枚文件名输入框（`name_input_`），初值恒空。缺的不是能力，是**接口的一个字段**。
2. **消费方今天的代偿是把命名完全交给人。** 每次导出都要手打一个有意义的名字，且打错了只有 OS 会拦。名字其实与内容同源（导出的正是那一份会话），框架不给这一格就是让应用把已知信息丢掉。
3. **非 ASCII 名是一条会被静默折坏的通道，必须在本库钉住。** 本机活动码页为 936：窄串若经 ANSI 码页解，`E4 B8 AD` 会被折成 GBK 花码。库内既有转换器 `aurora::internal::utf8_to_wstr` 显式用 `CP_UTF8`，本仓外部消费者在这条通道上已踩过一次，故新增字段必须以「逐字节到达」为验收判据而不是「看起来能用」。
4. **相邻的那条分叉更糟：接口说有能力、实现不做、注释还说工具链给不了。** `Options::initial_dir` 的头注只写「空串 = 平台默认位置」，读起来像两腿同能力；而 Win32 腿三个入口一律不消费它，`file_dialog_win32.cpp` 里那句「预选目录需要 Vista+ 的 `SHCreateItemFromParsingName`，当前 MinGW 工具链未提供」在同函数体内与「已经调了 `SetTitle`」并存。本次实测该句已过期：声明的门禁是 `_WIN32_IE >= _WIN32_IE_IE70`（本文件现取 `0x0600`），把旋钮抬到 `0x0700` 即声明可见、链接命中随库 PUBLIC 链接的 `shell32`；真机建出 `IFileSaveDialog` / `IFileOpenDialog` 后 `SetFolder` 返回 `S_OK` 且 `GetFolder` 读回 `C:\Windows\System32`。故本批按「补供给 + 纠口径」处理，而不是把头注改成限制——描述与实现分叉是最难查的那类缺陷。
5. **`suggested_name` 只在 `save_file` 生效是语义判断而不是省事。** 打开态预选一个名字与「选择已存在的文件」这个动作相互冲突（多选列表里凭空出现一个填了字的框）；目录形态根本没有名字概念。做不到的一致比不一致更糟，故另外两枚入口如实不消费并写进头注。
6. **今天没有任何一条面能看到传进去的 `Options`，判据无处可写。** 三枚 `headless_*` 钩子是**预设返回值**，`interactive == false` 只是回空——「建议名有没有被消费」这类命题没有自动化落点。故观察面先行，否则新增字段只能靠真机走查。

## 变更内容

1. **`Options` 增两格（甲＝新增字段，落点在 `initial_dir` 之后、`filters` 之前）**：`suggested_name`（建议文件名）与 `default_extension`（缺省扩展名），均为 **UTF-8 窄串**、空串 = 不表达意图，与既有 `title` 同一编码口径。次序理由：这两格是「名字」不是「类型」，与 `filters` 相邻易误读成过滤器的一部分。
2. **`suggested_name` 的口径（三腿同读一份）**：预选值而不是答案——用户可以改、可以清空、可以换目录；清空后的归因维持现状（取消 = `Ok(空)`，不是 `Error`）。⑴ 只取**末段名字**：传入含目录分隔符（`/` 或 `\`）时按文件名部分处置并留一条可归因 WARN，不静默丢弃也不整串塞进名字栏；⑵ 空串与不传该字段同形；⑶ 仅 `save_file` 消费，`open_file` / `open_folder` 如实不消费。
3. **`default_extension` 的口径（两腿形态不同、结果同一句）**：Win32 交 `IFileDialog::SetDefaultExtension`（名字栏里由 OS 显示与补全）；自绘腿在**确认时**补——用户没打点则追加 `.ext`，已带点则原样。两腿的共同结果都是「选定路径以该扩展名结尾」。空串 = 不补全。
4. **`initial_dir` 在 Win32 腿接上**：新增一处 `apply_initial_dir()`，把窄串转宽串后经 `GetFullPathNameW` 归一（实测解析名**不接受正斜杠**：`C:/Windows/system32` 直传 `SHCreateItemFromParsingName` 回 `E_INVALIDARG`，归一后即 `S_OK`），再 `SHCreateItemFromParsingName` + `IFileDialog::SetFolder`。任一步失败即留一条 `AURORA_LOG_WARN`（tag `file_dialog`，含原因与 hr）并按 OS 默认位置打开——与自绘腿「降级到根 + WARN」同形，不谎报成功、不返回 `Error`。三个入口一律接（`open_file` / `save_file` / `open_folder`）。SDK 旋钮随批抬升：`_WIN32_IE` `0x0600` → `0x0700`（该 TU 自带，不动其余四个取 `0x0600` 的 Win32 TU）。
5. **headless 观察面**：`inline std::optional<Options> headless_last_options;`，与三枚既有钩子同族、同处那段 `NOLINT` 块内。三腿的六个（含内联回退三支）**入口第一行**统一记录本次收到的 `Options` 原文——排在钩子判据之前，故钩子路径与非交互路径同样可观察。`nullopt` = 本进程从未调用过，空 `Options` = 调过但没表达意图，两者不混为一谈。
6. **折算层零平台头**（`src/aurora/app/detail/file_dialog_plan.{h,cpp}`，不进伞头、不占公共 API 预算，只引 `app/file_dialog.h` 与标准库）：`last_path_segment()`（同时认 `/` 与 `\`，尾部连续分隔符不吃掉一个名字）、`suggested_file_name()`（末段提取 + 截断时留 WARN）、`normalize_extension()`（去前导点、去空白）、`with_default_extension()`（名为空不补、名已带扩展名不补）。两座腿共用同一份换算，规则只有一份、可在全部 CI 作业上逐值单测。
7. **不改的部分**：三枚入口的签名与返回类型、钩子优先 → 非交互回空 → 真实/自绘的次序、取消不是失败、`Error` 只在系统级异常、自绘腿四层结构与 `run_modal` 执行模型、`filters` 语义、WASM / Android / BSD 内联回退「仅保钩子语义」——全部逐位不变；本批不新增错误码（`errors.toml` 零变化）。

## 验收判据

1. **观察面**：`interactive = false` 下依次调三枚入口，`headless_last_options` 逐格等于传入值（含四格全空的那一次），且 `nullopt` 只出现在首次调用之前；钩子置值时同样记录（证记录排在钩子判据之前）。
2. **零行为变化**：`Options{}` 在三枚入口的返回值与取消语义与今日逐字节相同；既有用例（`utest_file_dialog` 全部）零改动全绿。
3. **末段提取与截断留痕**：`suggested_name` 取 `"dir/sub/report.txt"`、`"C:\\logs\\report.txt"`、`"report.txt"` 三形 ⇒ 折算结果同为 `report.txt`；前两形各留一条可归因 WARN，末形不留；空串 ⇒ 空结果、不留 WARN。
4. **扩展名口径**：`with_default_extension("report", "txt")` = `report.txt`；`("report.md", "txt")` 原样；`("report", ".txt")` 去前导点后同结果；名为空 ⇒ 空（取消归因不变）；`default_extension` 为空 ⇒ 逐字节原样。
5. **非 ASCII 逐字到达**：`suggested_name` 传入转义写死的 UTF-8 中文字节串 ⇒ `headless_last_options` 读回**逐字节相等**；末段提取与扩展名补全对多字节串按字节不做任何拆断；真机探针另证 `SetFileName` → `GetFileName` 的 UTF-8 往返逐字节相等。
6. **自绘腿吃到建议名**（Linux / macOS 腿，其余记 skip）：`FilePickerModel` 在 `SaveFile` 形态构造后 `save_name()` 即等于末段名，`FilePickerView` 的输入框初值同值（model 与 widget 两处都喂，只喂其一不算闭合）；用户随后改写仍以改写后的值回传；`OpenFile` / `OpenFolder` 形态不受该字段影响。
7. **Win32 腿真机取证**（探针 `aurora_verify_win32_file_dialog`，`AURORA_BUILD_VERIFY_TOOLS` 口径、不进 CTest）：真实 COM 对话框对象上逐项证 `SetFileName` / `SetDefaultExtension` / `SetFolder` 的**读回值**（`GetFileName` / `GetDefaultExtension` / `GetFolder`）与折算计划一致，且不弹框、不等用户；不存在的目录走 WARN 降级分支不返回 `Error`。
8. **回归与门禁**：`--target aurora_api_json` 后按生成器口径复核 schema 同步；`check_naming_conventions` / `check_doc_comments` / `check_code_doc_sync` / `check_codespec_xref` / `check_platform_macros` / `check_umbrella_header` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest --preset ninja-test` 无新增红灯。

## 回写落点

- `codespec/specification/06-app-platform.md` §8.1（`Options` 字段表补两格、§8.1.3 消费口径逐格改写成「哪条腿消费哪几格」的真值表并含 `initial_dir` 的 Win32 接线、§8.1.2 观察面一句、三分支注释段同步）
- `codespec/SPECIFICATIONS.md`（新需求行 `SPEC.FEAT.APP.FILE-DIALOG.001`）
- `codespec/BUILD_OPTIONS.md` §2.4（真机探针登记）
- 仓库根的版本记录进 Unreleased 的 Added 节（两枚字段与观察面）与 Fixed 节（`initial_dir` 的「注释与实现相反」那条分叉）
- API schema 生成物由生成器重建后按门禁复核；`codespec/errors.toml` 零变化（不新增错误码）
