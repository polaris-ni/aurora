# CHG-037 原生菜单栏平台渲染桥（Win32 `HMENU` / macOS `NSMenu`，Linux 能力位申报与自绘回落）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-037 |
| 提出日期 | 2026-10-26 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.APP.NATIVE-MENU-BAR.001 |
| 影响面 | `include/aurora/window/platform.h`（`PlatformCapabilities` 增 `native_menu_bar` 位）；`include/aurora/app/` 新增原生菜单栏门面一枚；`src/aurora/window/detail/` 新增零平台头折算层与 Win32 / macOS 两枚平台桥；`codespec/errors.toml` 及生成物扩两枚码；`tests/unit/` 新增折算层与能力位用例；`tools/verify/` 新增两枚真机探针；`codespec/` 三处回写 |

## 动机

1. **需求只兑现了前半截，且这不是一句场面话。** 菜单数据模型的单一真源（`MenuModel`）与四枚投影已立，「自绘与原生二显一存——同一份模型，任一时刻只有一条渲染路径生效」也已写进规格，但**第二条渲染路径一行都没有**。全仓实证：`HMENU` / `CreateMenu` / `AppendMenu` 的唯一真实调用点在系统托盘的 Win32 实现里，那是托盘右键菜单，与顶层菜单栏无关；`NSMenu` / `NSMenuItem` 在 `src/` 与 `tools/` 下零命中；Linux 的 appmenu / DBusMenu 同样零命中。需求原文「AI 声明一次菜单即获原生渲染」今天只能落到自绘那一条。
2. **自绘菜单栏有四件它做不了的事，且都落在体验与可达性的硬处。** 一是位置不对：macOS 的应用菜单位于屏幕顶部、Windows 的窗口菜单位于标题栏之下，自绘控件画在客户区里，用户找不到。二是无障碍语义缺失：`MenuBar` 是自绘控件，读屏拿到的是绘制产物而非原生菜单角色，而本库无障碍桥覆盖的正是 Win32 UIA 与 Linux AT-SPI2 这类原生语义面——菜单语义只有走原生菜单对象才能被正确朗读。三是系统菜单项无法集成：macOS 的 About / Services / 隐藏与 Windows 的系统菜单都由 OS 提供，自绘路径里它们没有落点。四是 macOS 上不接 `NSMenu` 就没有标准应用菜单，Cmd+Q / Cmd+, 这类用户肌肉记忆直接断裂。
3. **Linux 的三端并列是过时假设，必须就地收窄而不是硬凑。** 需求文本写的是「Win32 / macOS / Linux 原生渲染桥」，这个并列来自 Unity 全局菜单仍在的年代。今天的事实是：GNOME 自 3.x 起取消全局菜单，GTK4 的 `GtkPopoverMenuBar` 本身就是窗口内自绘；KDE 的 DBusMenu 要求一个 DBus 客户端，而本库没有——文件对话框的自绘实现里明确记着「本库无 DBus 客户端」，通知模块走的是运行时 `dlsym` 取 `libdbus-1` 后手写报文的先例。那条路能走，但为一条正在退场的桌面约定引入手写 DBus 报文不划算。定案：**Linux 侧不落原生桥**，能力位申报 `false` 并回落自绘 `MenuBar`（在 Linux 上，窗口内自绘就是 GTK 同款形态），与既有「Linux 无 OS 级机制 ⇒ 能力位 `false` + 文档申报」口径同款。需求行文字随之修订，不再并列三端。
4. **macOS 端有前置阻塞，判据必须两档写。** macOS 后端窗口壳未实写（建窗 / 销毁 / 上屏三处待办、事件泵未接），而 `NSMenu` 需要 `NSApplication` 与窗口对象才能挂上。桥代码本身可以先落并被无头机制层证到，但端到端验收依赖 macOS 后端实写，故判据拆「机制层无头可证」与「真机段留白」两档——不在没有真机的地方把推断写成事实。
5. **不建平行菜单 API。** 桥只消费 `MenuModel` 投影，应用侧声明菜单仍然只有 `Menu` / `MenuItem` / `CommandRegistry` 一套，不新增面向使用者的第二套菜单类型。这是需求原文「不建平行菜单 API」的硬约束，也是本提案全部取舍的前提。

## 变更内容

1. **能力位**：`PlatformCapabilities` 增 `native_menu_bar`，取机制存在性语义（与 `system_drag_in` / `system_drag_out` 同款：只答「机制在不在」，运行期不可用走结构化错误，二者不互相替代）。按方向独立判定的同款纪律——`Win32` 与 `D3D11` 为 `true`（同一宿主）、`MacOS` 为 `true`、`X11` / `Wayland` / `Glfw` / `Wasm` / `Headless` 一律 `false`；未落地的方向不预先声称支持。
2. **公共门面**：`include/aurora/app/` 新增 `native_menu_bar.h`，声明进程级单槽门面 `NativeMenuBar`——`install(const std::vector<MenuModel> &)`（返回 `Result<void>`，同一进程只认一份菜单栏，重复安装替换旧树）、`uninstall()`、`is_installed()`，以及点击回投所需的登记口。**安装的是 `MenuModel` 而不是 `Menu`**：桥只认纯数据，行为一律经 `MenuModel::id` 反查 `CommandRegistry::invoke`，与既有投影的自绘路径共用同一个出口。
3. **`id` 是硬前提，且空 `id` 的处置不许走两条捷径。** 原生路径跨菜单重建只能靠稳定标识路由点击（自绘路径靠指针身份即可），`MenuModel::id` 为空的项在原生路径上无法路由。本批口径：空 `id` 的项照常建出、照常显示，但点击时不回投并留一条 WARN 诊断。不静默丢弃（菜单看着建好了却点不动，是最难排查的一种失败），也不伪造 id（同一项在重建后会错位）。
4. **折算层（零平台头）**：`src/aurora/window/detail/` 新增 `menu_bar_translate.{h,cpp}`，把 `MenuModel` 树摊成平台无关的菜单项扁平表（深度、父索引、分隔符、勾选态、禁用态、快捷键文本、稳定 id）。这一层不引任何平台头、可无头逐值单测，是「无头 CI 证明不了的只剩平台接缝」那条纪律在菜单域的落点；两枚平台桥共用同一份换算，不存在两套次序。
5. **Win32 桥**：同目录新增 `win32_menu_bar.{h,cpp}`——`CreateMenu` / `AppendMenu` / `SetMenu` 建树，`WM_COMMAND` 由 `Win32Host` 现有消息泵收下并按 `id` 回投。**桥实例由 `Win32Host::Impl` 唯一持有**，各 Win32 系 Surface 转发同一实例（与无障碍桥、输入法桥、系统拖放桥同款纪律，防多份窗口映射分裂）；窗口销毁期先拆桥再放窗口。
6. **macOS 桥**：同目录新增 `macos_menu_bar.{h,cpp}`（沿用 `macos_drag` 的混编形态）——`NSMenu` / `NSMenuItem` 建树并挂到 `NSApp` 的 `mainMenu`，点击经 target-action 按 `id` 回投。窗口壳未实写期间 `install()` 因拿不到应用与窗口对象而失败并记 WARN 降级，**不谎报成功**。
7. **Linux**：零代码。能力位 `false`，规格申报「无 OS 级菜单栏机制，回落自绘 `MenuBar`」。
8. **错误码**：`codespec/errors.toml` 新增 `native-menu-unsupported`（平台无此机制，属能力缺位而非运行期故障）与 `native-menu-install-failed`（机制在但装不上），均 `retryable = false`。空 `id` 的点击走 WARN 诊断而非错误码——它是声明缺失，不是菜单故障。
9. **测试与真机接缝**：`tests/unit/` 新增 `utest_menu_bar_translate.cpp`（折算层，无头）与能力位矩阵用例；`tools/verify/` 新增 `win32_menu_bar_live_probe.cpp` 与 `macos_menu_bar_live_probe.mm`（`AURORA_BUILD_VERIFY_TOOLS` 口径、`EXCLUDE_FROM_ALL`、不进 CTest，与既有探针同纪律），并显式登记到 `cmake/AuroraVerify.cmake`。
10. **不落本批**：上下文菜单的原生化、macOS 的 Services 菜单与 Windows 系统菜单的扩展、菜单图标（`MenuItem::icon` 在自绘路径亦尚未渲染）、Linux 的 DBusMenu 导出。

## 验收判据

1. **折算层纯逻辑单测**：嵌套子菜单摊平后深度与父索引逐值正确；分隔符、勾选态、禁用态、快捷键文本逐字段保真；稳定 `id` 透传。**变异自证**：调换父子索引或漏传 `id`，相关用例必须转红。
2. **空 `id` 口径单测**：空 `id` 的项照常出现在扁平表里（不丢弃）且安装成功；其点击不回投、留 WARN，其余项照常工作。**变异自证**：把「空 id 照常建出」改成「跳过空 id 项」，用例必须转红。
3. **能力位矩阵单测**：逐 `SurfaceKind` 断言 `native_menu_bar` 的期望值（Win32 / D3D11 / MacOS 为 true，其余为 false）。**变异自证**：为未落地方向翻位，对应用例转红。
4. **安装与卸载契约**：重复 `install` 替换旧树、不叠加、不泄漏；`uninstall` 后 `is_installed()` 为假且可再次安装；未安装时点击回投是 no-op 而非崩溃。
5. **Win32 真机探针**：真实窗口上建出菜单栏，逐级展开与点击被回投，回投 `id` 与声明逐值一致；窗口销毁后桥已拆除（不残留菜单项）。
6. **macOS 机制层**：`NSMenu` 对象树构造与 `id` → 回投的映射在无窗口环境下逐值可证；真机段在窗口壳实写前按留白登记，不写推断当事实。
7. **回归与门禁**：既有五份菜单用例（`utest_menu` / `utest_menu_bar` / `utest_modifier_input` / `utest_system_tray` / `utest_commands`）零改动全绿；`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移、`check_api_budget` 增量通过；`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` / `check_no_hardcoded_paths` 全绿；`lint`（仅改动相关 TU）与 `format-check` 绿；全量 `ctest -j16` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md`（§8.4 菜单、快捷键、命令与显示：原生菜单栏桥的能力位、门面契约、折算层、两枚平台桥、`id` 硬前提与空 `id` 口径、Linux 回落自绘的申报）
- `codespec/specification/04-widget.md`（MenuBar 控件节：自绘与原生二显一存的落点、Linux 侧回落说明）
- `codespec/SPECIFICATIONS.md`（需求行文字修订：不再并列三端，改为「Win32 / macOS 原生渲染桥 + Linux 能力位申报并回落自绘」）
