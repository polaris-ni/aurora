# CHG-048 macOS 运行期窗口显隐与置顶接线

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-048 |
| 提出日期 | 2026-10-30 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.MULTI-WINDOW.001 |
| 影响面 | `src/aurora/window/macos_surface.cpp`（四槽落地 + 可见性并入状态推导 + 构造期播种 + `raise` / `focus_window` 已隐藏短路）、`include/aurora/window/macos_surface.h`（四枚覆写与类注释）、`include/aurora/window/platform.h`（两枚能力位对 `MacOS` 翻位、字段注释改写）、`tests/unit/utest_window_runtime_control.cpp`（能力位矩阵）、`tests/unit/utest_macos_surface.cpp`（覆写签名静态钉住）、`tools/verify/macos_surface_live_probe.mm`（真机探针补一段） |

## 动机

1. **同一套公共面在 macOS 上是空的。** CHG-042 把 `Surface::set_visible` / `set_always_on_top` 四槽追加进虚段表，Win32 / D3D11 已接线；macOS 仍是基类默认（setter 空、`is_visible()` 恒 true、`is_always_on_top()` 恒 false），能力位也如实为 `false`。macOS 后端并不缺机制：`orderOut:` / `orderFrontRegardless` 与 `setLevel:` 是现成 AppKit 动作，本批只补桥。
2. **`MacOSSurface` 已有可见性状态推导，但第三输入恒 `true`。** `compute_window_state(minimized, active, on_screen)` 的 `on_screen` 形参在 macOS 侧未接原生 `isVisible`，隐藏后会被 AppKit 随后的失焦通知改写成 `Occluded`——「隐藏却报遮挡」正是 CHG-042 在 Win32 侧修过的缺陷形态。
3. **`raise()` / `focus_window()` 会把隐藏窗口重新排回屏上。** `orderFront:` / `makeKeyAndOrderFront:` 对 `orderOut:` 之后的窗口就是「显示」，等于绕过运行期显隐开关。这两个方法必须与 `set_visible(false)` 的语义共存：已隐藏的窗口不因提层或聚焦而出现。
4. **能力位必须只在桥落地后翻位。** 与 `system_drag_in` / `native_menu_bar` 同口径：本批把 `runtime_visibility` / `runtime_always_on_top` 对 `MacOS` 翻为 `true`，同时删掉「macOS 无窗口壳 / 桥未接线」的过期说法，避免文档与实现相反。

## 变更内容

1. **四槽落地**：`set_visible(on)` 走 `orderFrontRegardless`（显示不激活，与建窗 `NoActivate` 档同语义）/ `orderOut:`（隐藏）；`set_always_on_top(on)` 走 `setLevel:(NSFloatingWindowLevel / NSNormalWindowLevel)`。两枚 setter 先读原生实值（`isVisible` / `level`）判等，同值不重发 AppKit 调用。
2. **读回取 AppKit 实值**：`is_visible()` 走 `[window isVisible]`；`is_always_on_top()` 以 `[window level] > NSNormalWindowLevel` 为真（AppKit 没有 Win32 那样的布尔置顶位，外部设得更高时同样如实为真）。
3. **可见性并入状态推导**：`Impl::sync_state()` 把 `[window isVisible] != 0` 作为 `compute_window_state` 第三输入；构造尾部按可见性档播种 `last_state`（`Hidden` 档 ⇒ `Hidden`，`NoActivate` ⇒ `Occluded`，`Normal` ⇒ `Visible`），只写成员值不触发 handler。
4. **提层 / 聚焦不唤回隐藏窗口**：`raise()` 与 `focus_window()` 在读原生 `isVisible` 为假时直接返回；`focus_window()` 仍保留激活 App 与 `makeFirstResponder:` 原逻辑。
5. **能力位翻位**：`Platform::capabilities()` 的两枚位在 `SurfaceKind::MacOS` 上为 `true`；`platform.h` 两段字段注释改写为 macOS 已接线，并删除「macOS 窗口壳未接线」的旧说法。
6. **测试**：`utest_window_runtime_control` 的能力位矩阵把 `MacOS` 移入真档、假档集合收为 `{X11, Wayland, Glfw, Wasm, Headless}`；`utest_macos_surface` 用成员指针类型静态钉住四枚覆写的签名。

## 验收判据

1. `MacOSSurface` 在 `AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS` 下 `static_assert` 四枚覆写签名与基类一致，且仍不可拷贝 / 移动 / 默认构造。
2. 能力位逐 `SurfaceKind` 矩阵：`MacOS` 两枚为真；其余五档为假。
3. 真机探针：`set_visible(false)` 后 `[window isVisible] == 0`、`WindowState` 报 `Hidden`、frame / position 不变；`raise()` / `focus_window()` 不唤回；隐藏期间 `begin_frame` / `present` 正常；`set_visible(true)` 后可见且不抢键盘焦点、frame 不变；两枚 setter 同值幂等；`set_always_on_top` 读回与 `[window level]` 同源且切换不动 frame。
4. 既有 macOS 用例与全量 `ctest` 无新增红灯；`check_platform_macros` / `format-check` / 改动 TU 的 `lint` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md` §3.6（运行期开关的 macOS 接线）、§5（能力位段改为 Win32 / D3D11 / MacOS）、§6.2（`on_screen` 输入与构造期播种补 macOS 腿）。
- `codespec/SPECIFICATIONS.md` 的 `SPEC.FEAT.APP.MULTI-WINDOW.001` 行补 macOS 一档。
- CHANGELOG.md 的 [Unreleased] Added 记一条。

## 实施记录与偏离（落码期回写）

1. 四槽按提案落地：显隐走 `orderFrontRegardless` / `orderOut:`，置顶走 `setLevel:`，setter 先读原生实值判等；`is_always_on_top()` 以 `level > NSNormalWindowLevel` 判真。`sync_state()` 的第三输入接 `[window isVisible]`，构造尾部按可见性档播种 `last_state`；`raise()` / `focus_window()` 在原生不可见时短路。
2. 能力位对 `SurfaceKind::MacOS` 翻真；`utest_window_runtime_control` 的能力位矩阵与 `utest_macos_surface` 的成员指针签名钉住同步更新，两套用例全绿。
3. 真机探针 `aurora_verify_macos_surface` 扩段后 61/61 全过：隐藏后 `WindowState::Hidden`、frame / position 不变、`raise()` / `focus_window()` 不唤回、隐藏期间 `present` 与像素数据正常、显示不抢键盘焦点、两枚 setter 同值幂等、置顶读回与 `[window level]` 同源且切换不动 frame。
4. 偏离：无。`orderFrontRegardless` 不激活应用，与建窗 `NoActivate` 档语义一致；AppKit 没有布尔置顶位，读回按层级比较而非「曾由本库置顶」。
5. 门禁：macOS 平台定向 clang-tidy 与 clang-format 对改动 TU 复扫 0 命中；全量 `ctest` 450/450。
