# CHG-051 macOS 原生文件对话框

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-051 |
| 提出日期 | 2026-10-30 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.FILE-DIALOG.001 |
| 影响面 | 新增 `src/aurora/app/file_dialog_macos.cpp`（`NSOpenPanel` / `NSSavePanel` 原生腿）；`include/aurora/app/file_dialog.h`（macOS 原生分支声明与注释）；`src/aurora/app/file_dialog_selfdrawn.cpp`（L3 入口在 AppKit 后端下让位）；`cmake/AuroraBackends.cmake`（ObjC++ TU 登记 + `UniformTypeIdentifiers` 框架）；`tests/unit/utest_file_dialog.cpp`（自绘腿用例按腿门控）；`codespec/specification/06-app-platform.md` §8.1、`codespec/SPECIFICATIONS.md` |

## 动机

1. **macOS 此前只有自绘腿。** Win32 走 COM 原生对话框，macOS 却回落到库内自绘面板——自绘腿在沙盒、iCloud Drive、网络位置与系统侧边栏上天然缺位，用户拿不到系统级的能力（最近位置、标签、云盘挂载）。
2. **AppKit 的面板就是现成答案。** `NSOpenPanel` / `NSSavePanel` 的 `runModal` 与 `Options` 的六个字段一一对得上：`title` → `message`、`initial_dir` → `directoryURL`、`suggested_name` → `nameFieldStringValue`、`filters` → `allowedContentTypes`、`default_extension` → 首选类型与落盘补全。
3. **不能破坏既有语义。** 钩子优先、`interactive=false` 立即取消、无上下文等价取消且不弹窗、取消恒为 `Ok(空)` 而非 `Error`——这四条是四腿共用的契约，原生腿逐条保持。
4. **自绘腿在 AppKit 构建下必须让位但不能整体消失。** 文件选择器的 L0–L2（状态机与视图）仍被单测驱动，只有 L3 入口需要按腿切换，否则同一构建里出现两个同名实现。

## 变更内容

1. **原生腿**：新增 `file_dialog_macos.cpp`，门控 `AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS`。三个入口统一顺序：记录 `headless_last_options` → 钩子命中直接返回 → `interactive` 为假回 `{}` → `NSApp == nil` 记 WARN 回 `{}`（不调用 `sharedApplication`）→ `@autoreleasepool` 内建面板并 `runModal`。
2. **字段折算**：标题走 `setMessage:`；初始目录先做 UTF-8 与 `is_directory` 校验，不可解析时 WARN 并落到系统默认；筛选器把 `*.ext` 的末段扩展名折成 `UTType typeWithFilenameExtension:` 并去重，`default_extension` 排在首位；另存为用 `setNameFieldStringValue:` 播种，确认后按 `default_extension` 补全缺省扩展名。macOS 面板没有命名筛选器的位置，`Filter::name` 在该腿如实不消费。
3. **形态差异**：打开文件允许文件、禁目录、允许多选；另存为单个文件；选择目录禁文件、允许新建目录、无筛选器。取消与失败一律 `Ok(空)`，不产生 `Error`。
4. **入口切换**：`file_dialog_selfdrawn.cpp` 的 L3 入口改为「Linux 或未启用 AppKit 后端的 macOS」编译，其余桌面构建以 `static_assert(true, ...)` 占位；`FilePickerSession`（L0–L2）在所有桌面平台保留，单测照常链接。
5. **头文件契约**：`file_dialog.h` 新增 macOS 原生分支，写清六字段映射、`Filter::name` 不消费、无 `NSApplication` 时 WARN + 空结果；自绘腿注释改为「Linux，或未启用 AppKit 后端的 macOS 构建」。
6. **构建**：新 TU 以 `.cpp` 登记为 ObjC++；`AuroraBackends.cmake` 追加 `-framework UniformTypeIdentifiers`（筛选器折算专用）。
7. **测试门控**：`utest_file_dialog` 的自绘腿用例（含入口路由与会话端到端）按 `AURORA_UTEST_FILE_DIALOG_SELFDRAWN` 门控，AppKit 构建下记 skip 并说明原因；钩子与非交互语义用例四腿共用、照常执行。

## 验收判据

1. macOS AppKit 构建下三个入口解析到原生腿，同一构建中不存在重复定义；非 AppKit 的 macOS 构建仍解析到自绘腿。
2. 钩子非空时原生腿不构造任何面板；`interactive=false` 与无 `NSApplication` 均回空且后者留 WARN，不弹窗、不抛错。
3. 六个 `Options` 字段按上述映射被真实消费；初始目录不可解析时 WARN 并降级；`default_extension` 在另存为确认后补全。
4. 所有腿取消均返回 `Ok(空)`；原生腿不产出 `Error`。
5. `utest_file_dialog` 在 macOS 上全绿：共用用例执行、自绘腿用例记 skip 且跳过原因可读。
6. `check_platform_macros` / `check_api_schema_sync`（公共头新分支）/ `format-check` / 改动 TU 的 `lint` 绿；全量 `ctest` 无新增红灯。
7. AppKit 面板的真机交互（系统侧边栏、iCloud、沙盒授权）留给人工走查；单测不驱动模态循环。

## 回写落点

- `codespec/specification/06-app-platform.md` §8.1（四腿并列表、字段映射、错误语义分腿、`runModal` 说明）。
- `codespec/SPECIFICATIONS.md` 的 `SPEC.FEAT.APP.FILE-DIALOG.001` 行（三真实现腿 + macOS 原生面板的 `Filter::name` 不消费）。
- CHANGELOG.md 的 [Unreleased] Added 记一条。
- 公共 API 面只有 file_dialog.h 的平台分支声明，签名与 `Options` 不变，aurora_api.json 预期零差异。

## 实施记录与偏离（落码期回写）

1. 原生腿按提案落地：三入口统一「记 options → 钩子 → 非交互 → 无 NSApp → `@autoreleasepool` + `runModal`」；标题、初始目录、筛选器、建议名、缺省扩展名逐项折算，初始目录不可解析留 WARN 并降级；取消与失败一律 `Ok(空)`。
2. 自绘腿 L3 入口在 AppKit 后端下让位（`static_assert(true, ...)` 占位），`FilePickerSession` 与 L0–L2 在所有桌面平台继续编译；`utest_file_dialog` 的入口路由与会话 e2e 按腿门控，AppKit 构建下 24 例通过、7 例记 skip（跳过原因指向自绘腿）。
3. 构建侧：新 TU 登记为 ObjC++，macOS 框架补 `UniformTypeIdentifiers`；`cmake/AuroraBackends.cmake` 注释同步。
4. 偏离与未覆盖面：AppKit 模态面板的真实交互（系统侧边栏、iCloud、沙盒授权）无自动化手段，留人工走查；单测不驱动模态循环，只验钩子 / 非交互 / 头文件契约与自绘腿行为。
5. 门禁：初始目录的 `NSString` 先绑定再判空（静态分析不再把可空返回值直接送进 `fileURLWithPath:`）；macOS 平台定向 clang-tidy 与 clang-format 对改动 TU 复扫 0 命中；全量 `ctest` 450/450。
