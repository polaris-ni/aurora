# CHG-088 macOS WebView 引擎腿：WKWebView 补上第三足

| 字段 | 值 |
| :--- | :--- |
| 变更编号 | CHG-088 |
| 提出日期 | 2026-11-20 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.ZERO-IFDEF.001 |
| 影响面 | **新增库内部引擎腿**（控件公共 API 零改动）：`src/aurora/webview/engine_wkwebview.cpp`（新文件，OBJCXX / MRC，门控 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW`）、`src/aurora/webview/native_view_host.cpp`（懒安装接第三腿）、`cmake/AuroraBackends.cmake`（宏由 `AURORA_ENABLE_WEBVIEW ^ APPLE` 推导注入 + `-framework WebKit` + OBJCXX 语言标记）、`include/aurora/debug/feature_flags.h` 与 `src/aurora/debug/feature_flags.cpp`（**公共结构体 `FeatureFlags` 新增一枚 bool 字段 `webview_wkwebview`**，源码兼容：带默认初值，`to_json()` 多一键）、`tests/unit/utest_feature_flags.cpp`（宏键表补行）、`tests/unit/utest_web_view.cpp`（引擎枚举第三腿的桩路径用例）、`tools/verify/macos_wkwebview_live_probe.mm` + `cmake/AuroraVerify.cmake`（新探针）、`.github/workflows/ci.yml`（macOS 腿注释口径）、`codespec/BUILD_OPTIONS.md`、`codespec/specification/06-app-platform.md` §12.1、`codespec/ARCHITECTURE.md` §14、`codespec/manual-test/11-widget.md`、根 `CHANGELOG.md`。不改 `WebView` / `NativeViewHost` 任何既有签名与语义 |

## 动机

1. **macOS 是唯一「WebView 控件整体不可用」的平台。** WebView 能力落地时（见 `codespec/changes/webview-platform-embedding/proposal.md` 实施记录第 3 条）macOS 只预留了 `WebViewEngine::WkWebView` 枚举位与注册表入口，明确申报「补实现须另立提案并配真机探针」。现状是：即便 `AURORA_ENABLE_WEBVIEW=ON`，macOS 上懒安装也只能落 Null 桩，`WebView` 两种模式一律占位降级——这是 macOS 后端缺口盘点里唯一的控件级功能缺口。
2. **扩展点与范式已就绪，补齐路径清晰。** `NativeViewHost` 宿主侧书架（几何换算 / 去重 / 可见性边沿 / 票据判活）与另两腿（WebView2 / WebKitGTK）的实现范式完全平台无关；`MacOSSurface::native_handle()` 已返回 `NSWindow*`，WKWebView 作为 `contentView` 的 NSView 子视图挂入即可，不需要新的宿主接缝。
3. **本机已具备 Apple 构建与验证条件。** 当初不落实现的理由是「本机与 CI 均无 Apple 目标平台可构建」；如今 macOS 后端九座桥均已交付并有真机探针先例，该理由已不成立。

## 变更内容

1. **新引擎腿 `engine_wkwebview.cpp`**（ObjC++，**MRC** 与 `macos_surface.cpp` / `macos_drag.cpp` 同口径）：整文件门控 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW`，非 Apple 平台编译为空 TU。与另两腿同构地实现 `NativeViewEngine` / `WebViewSink` 全契约：
   - **探测**：`NSClassFromString(@"WKWebView")` 运行期判类存在，结果缓存一次；缺类报 `MissingRuntime`，控件走既有占位降级。
   - **建视图**：`create_view(parent_window)` 的 macOS 语义 = `NSWindow*`（`WindowHost` 经 `bind_window` 传入的 `native_handle()`）；WKWebView 加为 `window.contentView`（flipped，左上原点）的子视图，缺省 `hidden=YES`（可见性一律由宿主授予）。
   - **几何**：宿主下发的是**物理像素**矩形；AppKit frame 以 point 计，故按 `window.backingScaleFactor` 除回 point 再 `setFrame:`（superview flipped ⇒ 无需 Y 翻转）。
   - **导航三态**：`WKNavigationDelegate` 的 `didStartProvisionalNavigation` → Loading、`didFinishNavigation` → Finished、`didFailProvisionalNavigation` / `didFailNavigation` → Failed；`webViewWebContentProcessDidTerminate` → `on_process_failed`。
   - **标题**：KVO `title` 键路径，去重后上报（契约是「变化」才报）。
   - **双向消息 shim**：与 WebKitGTK 腿逐字同义——`WKUserScript`（document start、全 frame）注入 `window.aurora.postMessage`（经 `WKScriptMessageHandler` 名 `aurora` 投 JSON 文本）与 `window.aurora._receive`（Native 经 evaluate 调用，`JSON.parse` 后转 `onmessage`）。
   - **evaluate**：`evaluateJavaScript:completionHandler:` 的 id 结果折算为 JSON 文本（NSString / NSNumber 布尔与数值 / NSNull / NSDictionary / NSArray 经 `NSJSONSerialization`；失败回空串——既有「结果失败」编码）。
   - **快照**：`takeSnapshotWithConfiguration:`（`snapshotWidth` 取视口物理宽，与另两腿的像素口径对齐）→ NSImage → CGImage → RGBA8 **非预乘**位图（反预乘与 cairo 腿同式）。快照模式视图恒隐藏，WKWebView 隐藏时不产帧，故沿用 WebView2 腿的**屏外停放**手法：捕获前临时授予可见并停到父客户区之外，完成 / 超时后先隐藏再回位。在途捕获登记引擎侧，由 `pump_events` 心跳按墙钟超时（15s，对位另两腿同名义常量）兜底兑现空图，竞态门保证不回调两次；`detach` 时就地兑现全部在途项。
   - **zoom / js 开关**：`pageZoom` 与 `defaultWebpagePreferences.allowsContentJavaScript` 均 macOS 11+，按 `@available(macOS 11.0, *)` 守卫（与 `SMAppService` 的 13+ 守卫同口径），11 以下如实空操作并在文档申报；不引入弃用 API（`WKPreferences.javaScriptEnabled` 已弃用，不取）。js 开关对已加载文档不即时生效（WKWebView 机制如此），按「作用于后续导航」如实申报。
   - **teardown**：`detach` 置门 → 摘 KVO → `removeScriptMessageHandlerForName:`（断 UCC 强持有环）→ `navigationDelegate=nil` → `removeFromSuperview` → release；异步在途项以空结果兜底。
2. **懒安装第三腿**：`native_view_host.cpp` 的 `ensure_engine_installed()` 增 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW` 分支（前向声明工厂 `create_wkwebview_engine()`），排序在另两腿之后、Null 桩之前；显式 `install_engine` 表态优先的既有语义不变。
3. **编译开关**：`AURORA_ENABLE_WEBVIEW_WKWEBVIEW` 由 `AURORA_ENABLE_WEBVIEW=ON ^ APPLE` **推导注入，无独立 CMake 旋钮**（对齐 CHANGELOG 已收口的「平台子开关不读手工 -D」口径与 WebView2 腿同形）；`aurora_define_feature(... SCOPE PRIVATE)` 不 EXPORT；链接 `-framework WebKit`（系统 SDK 恒在，取 CoreAudio 直链先例，不做 dlopen 逃逸——Apple 无「构建机缺 dev 包」问题）；OBJCXX 语言标记随 macOS 后端既有清单下发。
4. **`FeatureFlags` 新增 `webview_wkwebview`**：与三枚同族字段并列（去前缀口径），`to_json()` 键 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW`；`utest_feature_flags` 宏键表与默认值断言补行。这是本批唯一公共 API 面变化（新增带默认初值字段，源码兼容；按 CoreAudio 批次实证，`FeatureFlags` 标量字段不进 API schema，`aurora_api.json` 预期零漂移，仍以生成器实跑核对）。
5. **真机探针 `macos_wkwebview_live_probe.mm`**（`EXCLUDE_FROM_ALL`、不进 CTest）：与 WebKitGTK 探针同套阶段状态机，观测侧改为**独立读 NSView 层级**——从 `native_handle()` 的 `contentView.subviews` 找 WKWebView 实例，frame（point）× `backingScaleFactor` 与 `to_native_rect(window_bounds())` 预期闭环比对；交互视图 `hidden=NO`、快照视图 `hidden=YES`；改尺寸意图后几何再下发闭环比对；快照像素判据（背景色为主 + 有内容像素）。无窗口服务器 / 建窗回退无头时以退出码 2 报环境不可用。配套 `codespec/manual-test/11-widget.md` 手工条目。
6. **不做清单**：DevTools / cookie / 下载 / 自定义 scheme / 权限打印等首版不做项保持不变（沿用 WebView 能力提案 §9）；不做 Aurora ↔ Web Tab 焦点贯通；不做语义树穿透；不引入 private API；`base_uri` 对 `loadHTMLString:baseURL:` 直传（WKWebView 原生支持，不像 WebView2 腿那样忽略）。

## 验收判据

1. **本机构建与能力面**：`-DAURORA_ENABLE_WEBVIEW=ON` configure + 构建通过；`feature_flags().webview_wkwebview == true` 且 `to_json()` 含新键；懒安装挑中 `WkWebView`（探针 `engine-selected` 项或等价断言）；`-DAURORA_ENABLE_WEBVIEW=OFF` 时全仓构建与既有用例零回归（新 TU 为空壳）。
2. **单测**：`utest_feature_flags` 新键行与默认值断言绿；`utest_web_view` 补一条以 `WkWebView` 为引擎标识的桩引擎用例（能力快照经控件面如实读出，确定性、无 GUI）；全量 `ctest --preset ninja-test` 绿。
3. **真机探针**：自动段逐项 `pass`——engine-selected / views-not-degraded / nav-loading-then-finished（标题断言并入本项，与 WebKitGTK 探针同口径）/ eval-ready-and-title / page-to-native-message / native-to-page-message / nsview 几何与可见性 / 改档几何随动 / snapshot-bitmap；本机执行结果如实记录（无桌面会话则记环境不可用退出码，不谎报通过）。
4. **门禁**：`check_platform_macros`（新 TU 零原生平台宏）、`check_change_proposals`、`check_codespec_xref`、`check_code_doc_sync`、`check_manual_test_format`、`check_naming_conventions`、`check_doc_comments`、`check_no_cjk_literals`、`format-check`、`lint`（改动文件）、`aurora_api_json` 重跑后 `check_api_schema_sync` 绿。
5. **零公共头污染**：公共头不出现新宏；`WebView` / `NativeViewHost` 签名与 vtable 零变化；另两腿（Windows / Linux）构建面与行为零回归（懒安装分支互斥，宏不共存）。

## 回写落点

- `codespec/BUILD_OPTIONS.md`：§4 开关表 `AURORA_ENABLE_WEBVIEW` 行的 `FeatureFlags` 字段计数与新增 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW` 行（推导注入、无独立旋钮、macOS 11+ 功能下限、直链 WebKit）、§2.4 探针清单补新探针、附录命令清单口径。
- `codespec/specification/06-app-platform.md` §12.1：引擎腿清单与「运行期 dlopen / LoadLibrary 绑定」表述补 Apple 直链系统框架例外；`feature_flags()` 查询字段补 `.webview_wkwebview`；可用性矩阵的 `MissingRuntime` 释义补 WKWebView。
- `codespec/ARCHITECTURE.md` §14：macos-clang 腿「WebView 在 macOS 没有原生引擎」的陈述改为第三腿已编入；探针编译面清单补新探针。
- `codespec/manual-test/11-widget.md`：新探针配套手工测试条目（编号顺延，受 `check_manual_test_format` 管辖）。
- `codespec/changes/macos-wkwebview-engine/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）。
- 根 CHANGELOG.md（codespec 外唯一落点，按硬规则 5 变更记录归它）：Unreleased Added 记 macOS WKWebView 引擎腿与 `FeatureFlags` 新字段。

## 实施记录与偏离（落码期回写）

1. **落码完成。** 引擎腿落 `src/aurora/webview/engine_wkwebview.cpp`（OBJCXX / MRC，整文件门控 `AURORA_ENABLE_WEBVIEW_WKWEBVIEW`）；懒安装第三腿、CMake 推导注入 + `-framework WebKit`、`FeatureFlags` 新字段与 `to_json()` 新键、`utest_feature_flags` 宏键表补行、`utest_web_view` 新增 `wkwebview_engine_identity_flows_through_capability_surface` 用例（StubEngine 增可配引擎标识形参，默认 Null 不改既有用例语义）均在位。ObjC 桥类按实测纪律落**全局作用域**（「Objective-C declarations may only appear in global scope」，与 `macos_drag.cpp` 同款注记），方法体以限定名引用匿名命名空间内的状态与转发函数。
2. **js 开关生效口径经独立小样实证**：WKWebView 建视图后改自持 configuration 的 `defaultWebpagePreferences.allowsContentJavaScript`，后续导航的页内脚本确实不执行（偏好对象按引用共享）；「作用于后续导航、已加载文档不即时重解析」如实申报于 BUILD_OPTIONS 与 §12.1。
3. **真机验收本机收口**：`aurora_verify_webview_wkwebview` 在本机 macOS 图形会话连续 3 次全绿（11 项判据全 pass，退出码 0），执行记录落 `codespec/manual-test/11-widget.md` TC-WIDGET-018。首跑一处失败是**探针自身判定缺陷**（Resize 阶段过早接受旧矩形），改为「先等预期矩形离开旧值、再轮询观测几何落定」后消除，非引擎缺陷。快照位图 1200x880（2x backingScale 的物理视口）、背景占比 99.0%，屏外停放手法产出真实像素。
4. **与提案表述的两处微偏离**：① 消息 handler 摘除选择器实为 `removeScriptMessageHandlerForName:`（提案初稿误写 WithName，已就地更正）；② 标题断言并入 `nav-loading-then-finished` 项（与 WebKitGTK 探针同口径），不单设 doc-title 项。
5. **门禁与回归（归档当轮实测）**：全量 `ctest`（build-macos，488 项）100% 通过；`check_platform_macros` / `check_change_proposals` / `check_codespec_xref` / `check_code_doc_sync` / `check_manual_test_format` / `check_naming_conventions` / `check_doc_comments` / `check_no_cjk_literals` / `check_declared_gates` 全绿；`aurora_api_json` 重跑后 `check_api_schema_sync` 绿且 schema 零漂移（`FeatureFlags` 标量字段不进 schema，与 CoreAudio 批次实证一致）；改动文件 clang-tidy（clang-tidy-22）零 finding；`format-check` 改动文件全部合规。`AURORA_ENABLE_WEBVIEW=OFF` 全新构建（build-webview-off）零错误、webview 三套件单测全绿（新 TU 为空壳）。两处**既有环境问题**如实记录、不属本批：① `tests/unit/utest_event.cpp` 在本机 Apple clang-format 21 下有 14 行排版漂移（该文件未经本批改动，CI 钉死 clang-format-22）；② `include/aurora/event/event.h` 的 6 项 tidy finding 为未改动头文件的既有平台条件形态（`#if defined(AURORA_PLATFORM_MACOS)` 等），经包含链浮现，非本批引入。