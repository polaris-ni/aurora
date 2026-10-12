# CHG-007 命中派发整窗表面坐标口径（绘制/命中零漂移）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-007 |
| 提出日期 | 2026-10-12 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001 |
| 影响面 | `WindowHost` 位置类事件派发根（鼠标/滚轮/触控/文件拖放/程序化指针）、新增 `Window::hit_root()` / `hit_root_ready()` 公共访问器、Wayland 后端指针三入口 + 滚轮的转发出参口径（表面坐标直发）；X11/Win32/GLFW/Headless/Wasm 后端转发口径不变（已是整窗坐标）；不改 `DecorationPolicy` / `content_inset()` / 事件结构体字段语义 |

## 动机

`demo_csd_gallery` 的 ClientSide 预览窗暴露缺陷：向下拉伸窗口后点可见的「Close this preview」按钮无效，须点击按钮**上方约一个标题栏高度**才生效。根因是绘制与命中对 `content_inset()` 的消费不同源：

- 绘制侧：`Window::present_root` 把应用根挂进私有壳 `detail::ContentInsetRoot`，`PaddingEdges(content_inset())` 修饰在布局期收紧约束、绘制/命中期平移内容盒——`present_root` 的注释与 `itest_content_inset_sink` 的头部说明均以「绘制/命中同源」为契约。
- 命中侧：`WindowHost::dispatch` / `dispatch_pointer` / `dispatch_touch` / `dispatch_file_drop` 却以 `scene_.root()`（应用根，坐标原点 = 内容区左上角）为派发起点，事件坐标是后端上抛的**整窗表面坐标**，未经任何折算。inset 非零时命中链整体偏移 `inset.top`（X11/Win32 ClientSide ≈ 标题栏高；Wayland 另含四向 margin）。

跨后端核对：X11（窗口坐标直发）与 Win32（吞框后客户区=整窗）转发的本就是整窗表面坐标，偏差全部来自派发根选择；Wayland（CHG-001）在 ptr_enter/ptr_motion/ptr_button/ptr_axis 把转发坐标先做了 −margin 的「表面→内容」映射——在派发根修正后该映射即成双重内缩来源，须改为转发整幅表面坐标、映射仅供后端内部 CSD 热区判定。GLFW/Headless/Wasm 的 inset 恒零，行为逐位不变。

选定方案：**命中派发根改用 `ContentInsetRoot` 壳**（而非在事件路径二次减法）。理由：几何真源唯一——命中偏移完全复用绘制那条修饰平移，inset 逐帧变化（进出全屏/最大化塌缩、Wayland margin）结构上不可能与已绘几何漂移；且事件路径读实时 `content_inset()` 与绘制期快照 `applied_inset_` 的一致性不再需要人工维持。

## 变更内容

1. `Window` 新增公共访问器 `hit_root()`：返回 `ContentInsetRoot` 壳节点引用（present 管线既成事实——「后续帧管线统一以壳为根」——对派发侧的同一出口）；配套 `hit_root_ready()` 报告壳是否已收养应用根（首个 `present_root` 之后为 true）。
2. `WindowHost` 位置类派发入口（`dispatch` 的鼠标/滚轮/触控/文件拖放分支、`dispatch_pointer`、`dispatch_touch`、`dispatch_file_drop`）的派发根由 `scene_.root()` 改为 `hit_root()` 等价物（无头宿主无 Window，或首个 `present_root` 之前壳未收养应用根，均回退 `scene_.root()`）；键盘/文本输入/IME 组合不经命中链，维持 `scene_.root()` 不变。焦点序遍历根（`focus_.set_root`）不变。
3. 事件坐标契约统一为：**`Surface` 上抛的位置类事件一律使用整幅表面逻辑坐标**（与 `content_inset()` 同域），下沉折算由框架派发根一次性承担。
4. Wayland 后端按契约 3 收口：ptr_enter / ptr_motion / ptr_button（Press 与 Release 两径）/ ptr_axis 的转发出参从「内容坐标（−margin）」改回**表面坐标原值**；后端内部 CSD 交互（标题栏按钮/拖拽、margin 缩放热区、全屏揭示条）继续在内容坐标域判定，不变。
5. X11 / Win32 后端零改动（转发已是整窗坐标；标题带事件本就被子绘拦截层消费）。
6. 生命周期核对：首个 `present_root` 之前壳未收养应用根，派发经 `hit_root_ready()` 门控回退场景根——与改动前的派发根逐位一致（应用根尚未布局时命中空链）；此回退保住「先 `dispatch_*` 后 `present_root`」的既有用法（如 `utest_application` 的光标接线用例），无回归面。

## 验收判据

1. headless 集成测试：注入非零 inset（top=36）后，按控件 `paint_bounds()` 的**实际绘制位置**经派发根点击可命中并触发 `on_click`；inset 归零时同点点击行为与历史一致（新增 `itest_content_inset_sink` 用例，逐值断言绘制盒与命中结果）。
2. `WindowHost` 派发根语义有明确回退：`has_window()==false`（无头宿主）仍走 `scene_.root()`，既有程序化派发测试（`utest_application`）全绿。
3. Win32 真机走查 `demo_csd_gallery`：ClientSide 预览窗按按钮可见位置点击「Cycle title-bar theme / Toggle window frame shadow / Toggle title-bar icon / Close this preview」逐一生效，无偏移；Borderless/Frameless 控件级 TitleBar 点击同步走查。**实测补记（2026-10-12）**：控件级 TitleBar 的 `−` / `□` / `×` 在 Win32 为无害空动作——经 `WindowChrome` 转发的 `Surface::close()` / `minimize()` / `toggle_maximize()` / `set_fullscreen()` 为默认空实现、仅 Wayland 覆盖（既有平台范围，见 `specification/07-environment-modifier.md`「程序化窗口控制」段，非本变更引入）；命中链对齐以 `Info` 动作日志（按绘制位置逐次触发）与 `Close this preview` 可见位置点击判定，走查全过。执行记录见 `codespec/manual-test/10-window.md` TC-WINDOW-016。
4. 全量 `ctest --preset ninja-test` 绿（含 `check_api_schema_sync`、`check_naming_conventions`、`check_doc_comments`、`lint`、`format-check` 等全部 `check_*`）。

## 回写落点

- `codespec/specification/06-app-platform.md`（事件派发路径：WindowHost 派发根 = 整窗下沉壳；Surface 上抛坐标契约=整幅表面坐标；Wayland 指针转发口径回归表面坐标）
- `codespec/specification/07-environment-modifier.md`（§3.1 CSD 安全区下沉：命中期消费同一条 `PaddingEdges` 平移，绘制/命中零漂移不变量）
