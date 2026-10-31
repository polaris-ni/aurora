# CHG-049 macOS 无障碍桥（NSAccessibility）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-049 |
| 提出日期 | 2026-10-30 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | 新增 `src/aurora/window/detail/macos_ax_bridge.{h,cpp}`（`MacOSAxBridge : a11y::Provider` + `NSAccessibilityElement` 元素类）；`src/aurora/window/macos_surface.cpp`（`Impl` 持有桥、内容视图无障碍查询转发、`accessibility_provider` / `set_accessibility_root` 落地、销毁先收桥）；`include/aurora/window/macos_surface.h`（两枚覆写与类注释）；`cmake/AuroraBackends.cmake`（ObjC++ TU 登记）；`tests/unit/utest_macos_surface.cpp`（覆写签名静态钉住）；`codespec/` 三处回写 |

## 动机

1. **macOS 是四座无障碍桥里唯一缺位的一座。** Win32 UIA、Linux AT-SPI2、Wasm ARIA 都已落地并按「惰性激活 + 拉取式快照」的同一形态工作；macOS 的 `Surface::accessibility_provider()` / `set_accessibility_root()` 仍落回基类默认（返回 nullptr、注入 no-op），VoiceOver 在本库窗口上读不到任何控件。
2. **AppKit 不缺机制，缺的是投影。** `NSAccessibilityElement` 可按需构造任意元素树，`NSAccessibilityPostNotification` 家族可推送焦点 / 标题 / 值 / 布局 / 播报事件；本批只把语义树快照投影成这棵元素树，不新增线程、不引第三方库。
3. **必须与既有三桥同口径，否则四套行为各说各话。** 惰性激活（无读屏在线时零构建）、拉取式重建（事件只置脏）、按值快照（控件树销毁不留下悬垂指针）、根销毁即切断、去掉激活时从注册表注销——这五条是前三桥的既有不变量，macOS 桥逐条对齐。
4. **AppKit 的坐标与动作语义必须显式折算。** `AccessibilityNode::bounds` 是窗口本地逻辑 dp、y 向下，AppKit 要屏幕点、y 向上；控件动作是 `AccessibilityAction` 枚举，AppKit 要 `accessibilityPerformPress/Increment/Decrement` 与 `setAccessibilityFocused:`——折算只发生在桥内一处。

## 变更内容

1. **桥类**：`MacOSAxBridge final : a11y::Provider`，`create(void *view)` 构造不激活；`activate()` 注册 `a11y::ProviderRegistry` 并回填 `screen_reader_active = true`；`deactivate()` 注销、清元素与快照、复位设置；`sync_if_dirty()` / `mark_dirty()` / `set_root()` / `name()` / `on_announcement()` / `on_widget_destroying()` 按 Provider 契约实现。`set_root` 恒置脏（帧是元素属性）。
2. **元素类**：`AuroraAxElement : NSAccessibilityElement`（MRR，`dealloc` 逐项 release），字段全部按值持有（role / subrole / label / value / hint / identifier / frame / children / parent / enabled / focused），按 `runtime_id` 在重建间复用；父指针在递归建树后统一回填。
3. **坐标与动作**：帧经内容视图 `convertRect:toView:nil` + `[window convertRectToScreen:]` 换屏幕点；`accessibilityPerformPress` 取 Invoke → Click → Toggle 首个可用者；Increment / Decrement 以 `range.step`（≤0 时取 1）步进并夹在 `[min,max]` 后走 `Value`；`setAccessibilityFocused:` 走 `Focus`。
4. **推送**：`diff.focused_id` → `FocusedUIElementChanged`；`FieldChange::Name` → `TitleChanged`；`Value` / `Range` → `ValueChanged`；结构 / `Bounds` / `State` / `Actions` 合并一条视图级 `LayoutChanged`；播报经 `NSAccessibilityPostNotificationWithUserInfo(NSApp, ..., AnnouncementRequested)` 带文本与 medium 优先级。
5. **窗口接线**：`MacOSSurface::Impl` 增 `std::unique_ptr<detail::MacOSAxBridge> a11y` 与 `Widget *a11y_root`；`ensure_a11y()` 在首个查询时构造、补喂根并激活；内容视图覆写 `isAccessibilityElement`（NO）/ `accessibilityRole`（group）/ `accessibilityChildren` / `accessibilityHitTest:` / `accessibilityFocusedUIElement`；`destroy_window()` 以 `deactivate()` + 重置开头；`accessibility_provider()` 返回桥裸指针、`set_accessibility_root()` 记录并转发。
6. **编译登记**：新 TU 命名为 `.cpp`（与既有 macOS ObjC++ TU 同形），在 `cmake/AuroraBackends.cmake` 的 ObjC++ 列表登记 `LANGUAGE OBJCXX`；门控 `AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS`。

## 验收判据

1. `MacOSSurface` 的 `accessibility_provider` / `set_accessibility_root` 两枚覆写签名与基类一致（成员指针类型 `static_assert`），仍不可拷贝 / 移动 / 默认构造。
2. 无读屏查询时 `accessibility_provider()` 返回 nullptr（桥未构造）；首个 `accessibilityChildren` / `accessibilityHitTest:` 查询后桥激活、`screen_reader_active` 为真。
3. 语义树变化只置脏；下一次查询重建元素并发出对应通知；焦点清空时 `focused_id` 归零。
4. 根控件销毁后，窗口存活期间的后续查询返回空子元素，不解引用已销毁控件。
5. 窗口销毁先 `deactivate()` 再释放视图；`ProviderRegistry` 中无残留实例。
6. AppKit 真机行为（VoiceOver 读音、动作回灌）留给人工走查；无头构建不驱动 AppKit 模态，故单测只钉类型契约。
7. `check_platform_macros` / `check_arch_module_map` / `format-check` 与改动 TU 的 `lint` 绿；全量 `ctest` 无新增红灯。

## 回写落点

- `codespec/specification/06-app-platform.md` §6.3（macOS 桥的激活 / 重建 / 拆除 / 坐标动作折算）。
- `codespec/ARCHITECTURE.md` §8.5（四座平台实现并列，补 macOS 差异段）。
- CHANGELOG.md 的 [Unreleased] Added 记一条。
- 公共 API 面只有 `MacOSSurface` 的两枚覆写，aurora_api.json 的 schema 不承载成员方法，预期零差异。

## 实施记录与偏离（落码期回写）

1. 桥与元素类按提案落地：`MacOSAxBridge` 惰性构造、`sync_if_dirty()` 拉取式重建、元素按节点 id 复用、父指针回填、根销毁切断、`deactivate()` 先注销再从注册表摘除；`AuroraAxElement` 以 MRR 逐项持有字段，`dealloc` 逐项 release。
2. 窗口接线：`MacOSSurface::Impl` 持有桥，内容视图转发 `accessibilityChildren` / `accessibilityHitTest:` / `accessibilityFocusedUIElement`，`accessibility_provider()` / `set_accessibility_root()` 由基类默认改为真覆写；`utest_macos_surface` 以成员指针类型把两枚覆写钉在编译期，用例全绿。
3. 新增 TU 以 `.cpp` + `LANGUAGE OBJCXX` 登记；`AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS` 门控。全量构建与门禁无新增红灯。
4. 偏离与未覆盖面：VoiceOver 读音与动作回灌属系统级行为，无自动化手段，留人工走查；单测只钉类型契约与既有语义路径（无头构建不驱动 AppKit 模态）。
5. lint 收口：`AuroraAxElement` 的 `@public` ivar 改为仓内 ObjC 桥同款的无尾下划线命名（`impl` / `owner` 同形），不对 public member 命名规则做整块豁免；优先级的装箱写法改为 `numberWithInteger:`（`@标识符` 在 ObjC++ 下编译失败，去掉括号的 `@(...)` 又被判冗余）。macOS 平台定向 clang-tidy 选集 51 个 TU 复扫 0 命中。
