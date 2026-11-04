# CHG-062 macOS 装饰映射补齐：Borderless 档保留原生边缘缩放掩码

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-062 |
| 提出日期 | 2026-11-02 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `src/aurora/window/detail/macos_decoration.h`（新增：档位 → 装饰布尔的纯映射真源）、`tests/unit/utest_macos_decoration.cpp`（新增：映射表驱动单测，非 Apple runner 亦跑）、`src/aurora/window/macos_surface.cpp`（建窗期只保留「布尔 → 掩码 / 属性」的逐字映射 + 标准窗钮显隐）、`include/aurora/window/macos_surface.h`（类注释的装饰口径）、`tools/verify/macos_surface_live_probe.mm`（新增掩码能力与真拖拽段）；`codespec/specification/07-environment-modifier.md` §4.1 映射表、`codespec/manual-test/10-window.md` 一条新用例、CHANGELOG.md。**不改公共类型与函数签名**：`DecorationPolicy` / `WindowResizeEdge` 枚举项与值序、`Surface` 虚函数集、错误码均不动，且本提案不新增 `MacOSSurface::begin_window_resize` 覆写 |

## 动机

1. **`Borderless` 的公共契约在 macOS 上被丢掉。** `DecorationPolicy::Borderless` 的权威语义写的是「无标题栏，**保留**可拖拽缩放边框；移动靠修饰键拖拽」（`include/aurora/window/surface.h` 枚举注释、[`specification/07-environment-modifier.md`](../../specification/07-environment-modifier.md) §4.1）。而 macOS 建窗期把 `frameless`、`Frameless`、`Borderless` 三者一并塌成 `NSWindowStyleMaskBorderless`（掩码 `0x0`）。真机实测该掩码下 `isResizable == 0`，且经 AppKit 事件队列向右下角投递一次拖拽（按下 + 12 段拖动 + 抬起）**窗口尺寸纹丝不动**——即该档在 macOS 上既无标题栏、也无缩放，与 `Frameless`「完全无装饰」撞到同一份实现，档位语义被抹平。

2. **AppKit 有现成且不依赖私有 API 的替代掩码，实测可用。** 同批真机量测：`Titled \| Closable \| Miniaturizable \| Resizable \| FullSizeContentView` 配 `titlebarAppearsTransparent = YES` + `titleVisibility = NSWindowTitleHidden`，得到 `isResizable == 1` 且 **内容区与窗框完全重合**（320×240 的窗口 `frame` 与 `contentRectForFrameRect:` 尺寸相同、顶边内缩 0.0pt；对照标准装饰档 320×272、顶边内缩 32.0pt）。同一次队列拖拽把该窗口从 320×240 真实拖到 320×336，高度增量与拖拽位移（12×8pt）逐字一致——「无标题栏 + 原生边缘缩放」在 macOS 是可落地的既有能力，不是需要新造的东西。

3. **`begin_window_resize` 在 macOS 不该按「覆写一次 OS 抓取」来补。** Win32 用 `WM_NCLBUTTONDOWN + HT*`、Wayland 用 `xdg_toplevel.resize`，是因为自绘边框的按下事件必须由 App 转交给窗口过程／合成器去接管整段拖拽；macOS 的 `NSWindow` frame view 本就原生承担边缘拖拽（上面第 2 条的实测正是这条原生通道），再补一层覆写等于给同一次拖拽写第二份实现，只会引入行为分叉。**本提案的落点是装饰掩码映射，不是新增 `Surface` 覆写。**

4. **顺带记下一条不属于 macOS 的缺口，且本提案不承担它。** `WindowChrome::begin_resize` / `Surface::begin_window_resize` 目前在全仓**没有任何控件调用方**：`TitleBar` 只在栏内空白区调 `chrome->begin_move()`，控件层没有边缘命中测试；覆写方只有 Win32 两型与 Wayland 两型，X11 连 `begin_window_move` 都未覆写（落基类空实现）。CSD 缩放整链是跨后端缺口，须单独立项；macOS 侧即便补了覆写也无法经产品路径触发，属不可达代码。

5. **另一条现状差异只记录、不在本提案改动。** macOS 无 CSD 路径，故 `ClientSide`（「即便 compositor 支持 SSD 也强制自绘」）与 `Auto` / `ServerSide` 落到同一个原生标题栏掩码——强制自绘档在 macOS 上退化为 SSD。该档位是否应当兑现自绘，取决于第 4 条的控件层消费链是否落地，本提案不预判。

## 变更内容

1. **拆档**：`Impl::create_window` 的 frameless 判定按契约拆成两路。`Borderless`（含 `WindowStyleOptions::frameless` 为真但档位要求保留缩放边框的形态）走第 2 条实测的 FullSizeContentView 组合掩码；`Frameless` 维持 `NSWindowStyleMaskBorderless`（其契约本就是完全无装饰、无缩放边框）。
2. **缩放能力受 `style.resizable` 约束**：组合掩码只在 `style.resizable` 为真时挂 `NSWindowStyleMaskResizable`；为假时不得因本提案获得缩放能力（保持与标准档一致的约束来源）。
3. **隐藏三枚标准窗钮**：`titleVisibility` 与 `titlebarAppearsTransparent` 只藏标题文本与标题带底色，**不藏** close / miniaturize / zoom 三钮，须对 `standardWindowButton:` 显式隐藏，否则「无标题栏」档会同时出现自绘装饰与原生圆点。
4. **内缩口径复核**：FullSizeContentView 下实测顶边内缩为 0（内容区 == 窗框），须确认 `content_inset()` 与 CSD 自绘标题栏带高在 macOS `Borderless` 档不再叠加原生 32pt 标题带偏移；`position()` / `size()` 的窗口框与客户区口径随之回写文档。
5. **探针取证**：`macos_surface_live_probe` 增一段——按 `Borderless` 建窗后读 `isResizable` 与 `contentRectForFrameRect:` 顶边内缩，并经 AppKit 事件队列（与 OS 输入探针同一条队列腿）投递一次右下角拖拽，断言窗口尺寸确实改变、且新尺寸能从 `Surface::size()` / `framebuffer_size()` 读回；同段以 `Frameless` 建窗断言 `isResizable == 0` 且拖拽不改尺寸。
6. **单测**：`utest_macos_surface` 以静态口径钉住「`Borderless` 保留可缩放、`Frameless` 不保留」的映射；非 Apple 构建维持整文件 skip。

## 验收判据

1. 真机探针：macOS `Borderless` 档建窗后 `isResizable == 1`，`frame` 与 `contentRectForFrameRect:` 尺寸相同（顶边内缩 0.0pt）；队列投递的右下角拖拽使窗口尺寸改变，且改变后的尺寸能在 `Surface` 侧读回（证明原生缩放真的接进了框架尺寸回传链）。
2. 真机探针：`Frameless` 档 `isResizable == 0` 且同一拖拽不改尺寸——契约要求的「完全无装饰」不得被本提案顺手放宽。
3. `style.resizable == false` 的 `Borderless` 窗口实测仍 `isResizable == 0`。
4. 真机探针截图判据：`Borderless` 档左上角原窗钮区域不再出现原生圆点（三钮隐藏生效），该区域由自绘装饰层承担。
5. 既有装饰档回归：标准（SSD）档仍带原生标题带与窗钮、顶边内缩非 0；`Auto` / `ServerSide` / `ClientSide` 的现有 macOS 行为不被本提案改变。
6. 全量 `ctest` 与改动 TU 的 macOS 定向 `lint`、`format-check` 无新增红灯；`--target aurora_api_json` 后 schema 零差异（无公共签名变更）。

## 回写落点

- `codespec/specification/07-environment-modifier.md` §4.1：「三后端映射」表补 macOS 行（`Borderless` / `Frameless` 两档的掩码与窗钮口径），并记 macOS 无 CSD 路径、`ClientSide` 退化为 SSD 的现状。
- `codespec/manual-test/10-window.md`：新增一条 macOS 无边框缩放真机用例，探针自动取证段与人工拖拽段分开记录。
- `codespec/specification/06-app-platform.md`：窗口运行期能力条目若涉及装饰/缩放口径，同步 macOS 档位描述。
- CHANGELOG.md 的 [Unreleased] 记一条 Changed（macOS `Borderless` 档恢复可拖拽缩放）。

## 实施记录与偏离（落码期回写）

1. **变更内容第 6 项的实现形态换了载体，判据强度提高**：首稿写「`utest_macos_surface` 以静态口径钉住映射」，落码否决——该文件在含 AppKit 的构建里整文件 `AURORA_TEST_SKIP`（建窗触碰真实 OS 资源），静态口径钉不住任何映射。改为把档位判断从 ObjC++ 里抽出为库内私有纯函数 `src/aurora/window/detail/macos_decoration.h`（`detail::map_macos_decoration`：复用 `csd::normalize_policy` 折叠 `frameless` 旧字段，输出 `native_borderless` / `full_size_content` / `sizable` / `hide_title` / `hide_window_buttons` 五个布尔，不 include 任何平台头、不出现 `NSWindowStyleMask` 字面量），`Impl::create_window` 只保留「布尔 → 掩码 / 属性」的逐字映射；新增 `tests/unit/utest_macos_decoration.cpp` 六例表驱动（含不变量「`sizable == (resizable && !native_borderless)`」与「`frameless==true` 跨五档一票否决」），该文件不设平台门故非 Apple runner 照常跑，实测 6/6 通过。`tests/unit/utest_macos_surface.cpp` 本批未因装饰映射改动。
2. **验收判据 1 与 2 的实测读数**：`Borderless` 档 `isResizable` 为真、`frame` 与 `contentRectForFrameRect:` 同为 300×200（顶边内缩 0），同进程队列注入的右下角拖拽（12 段 ×8pt）把窗口真实拖到 396×296 且 `Surface::size()` 读回同数；`resizable=false` 的 `Borderless` 实测 `isResizable` 为假（判据 3），`Frameless` 档 `isResizable` 为假且同一拖拽 `300x200 -> 300x200`（判据 2）。探针装饰段加总后为 `82/82 checks passed`、退出码 0，映射抽出前后各连跑五轮同为 82/82、逐字读数一致——纯函数重构行为不变。
3. **拖拽前提与首稿预期不同，且 key 位按实降级为观测**：首稿与第一版实现都默认「窗口成为 key」是拖拽生效的前提。实测前提只有两条——窗口已在序、事件队列已泵空（载体改为投递前 `makeKeyAndOrderFront:` + 泵 60 轮）。`isKeyWindow` 读数不构成判据：同一条改尺成功行先后取到过 `key=0` 与 `key=1`，两读皆绿，故规格 §4.1、预期 4 与探针注释都改成「`key=` 只作观测」，不写「`key=0` 属正常」这种会把另一种读数说成异常的口径。
4. **另一条实测约束：所有事件必须先入队、后泵送**。可缩放的原生窗框视图在 `sendEvent:` 内进入 AppKit 的嵌套跟踪循环，逐条「投递—泵送」会把主线程卡在第一次 `sendEvent:` 里等后续事件（本机一次实跑挂死，进程由外部终止）；载体改为 down + 12 段 drag + up 全部投完再泵 400 轮，事件时间戳基线取 `[NSProcessInfo processInfo].systemUptime`。
5. **曾被提出并否定的失效假设如实留下**：拖拽两条判据曾连续两轮转红（`300x200 -> 300x200`）。第一版归因于「`[NSApp currentEvent]` 为 nil 使时间戳坍缩、事件被 AppKit 判为乱序」，改时间戳基线后仍是 80/82——该假设被公开证伪；随后加一行几何观测（`drag geometry:`）才定位到第 3 条的在序/泵空前提。旁证：同一队列腿上裸 `NSWindow`（无框架内容视图）也能被同样拖拽改尺，阳性结果不依赖框架侧额外接线，证的是掩码映射本身。
6. **判据 4 未按截图取证**：首稿写「截图判据：原窗钮区域不再出现原生圆点」，落码改判三枚 `standardWindowButton:` 的 `isHidden == YES`（三行独立判定），未新增像素比对段。理由：本探针已有 `capture_window` 段负责像素链路，而窗钮显隐的判据在 AppKit 侧有直接可读的状态，不必绕经截图；「该区域由自绘装饰层承担」属控件侧结论，不在本探针取证范围。
7. **`ClientSide` 退化为 SSD 未改**（动机第 5 条的预判成立）：三档仍落同一原生标题栏掩码，实测顶边内缩 32pt，本批只把该现状写进规格 macOS 行，不动语义、不接 CSD / margin 层。动机第 4 条的控件链缺口同样未承担：`WindowChrome::begin_resize` 全仓无调用方，规格另起一段「控件侧消费现状」如实登记，并写明该缺口在 macOS 上无产品可见后果（原生窗框路径已覆盖同一需求）。
8. **回写落点微调（四处）**：`codespec/specification/07-environment-modifier.md` 除 §4.1 映射表外还改了 §4.2 的 `Surface` 虚接口覆写分布（`begin_window_move` / `begin_window_resize` 各后端谁覆了，原表只有三后端口径），并把该段小标题由「三后端映射」改为「四后端映射」；映射真源点名到 `src/aurora/window/detail/macos_decoration.h`，使代码—文档同步门禁可对账。`codespec/GUIDELINE.md` §28 的自绘标题栏配方原把 `Borderless` 注释成「移除系统标题栏（CSD 接管）」，与本档契约（保留可拖拽缩放边框）及 macOS 一侧由原生窗框承担缩放的事实都不符，改按契约措辞并补一段「边缘缩放由谁承担随后端不同 + `begin_resize` 尚无控件调用方」的用法申报（本批不改控件层，故只登记不兑现）。`codespec/specification/06-app-platform.md` 经全文核对无装饰/缩放档位表述，未改动。CHANGELOG 该条记在 `### Fixed` 而非首稿写的 `### Changed`：恢复的是已声明却被后端抹平的契约，不是改变对外行为。`codespec/manual-test/10-window.md` 新增 TC-WINDOW-025（含 §1.2 载体行与「本例为何不给 `begin_window_resize` 立判据」一节），其中步骤 9（真人鼠标拖拽）本批未执行，执行记录如实标注未取证。
9. **公共面零改动经门禁确认**：`--target aurora_api_json` 后 schema 无差异（不新增公共类型、字段、虚函数或错误码）；`check_platform_macros`、`check_code_doc_sync`、`check_manual_test_format`、`check_change_proposals`、`check_naming_conventions`、`format-check` 与全量 `ctest` 见批末回归记录。
