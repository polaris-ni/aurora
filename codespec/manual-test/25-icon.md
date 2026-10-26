# icon 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 `01-core.md`。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

图标组件（`Icon` / `IconButton`）及其跨后端渲染与降级语义。库不内置任何图标资产：字体由应用经 `FontEngine` 注册，SVG 经字节注入，语义名由应用经 `IconRegistry` 注册。本卷只覆盖**必须由真实窗口与人工目视参与**的那一段——图标在不同主题与不同后端下的实际观感、禁用态透明度，以及「未注册资产」的降级行为。

| 头 / 实现 | 职责 |
|:---|:---|
| `include/aurora/icon/icon.h` | `IconData` 两变体值语义、`IconRegistry` 语义名注册表、`paint_icon_data` 绘制入口 |
| `include/aurora/icon/icon_svg.h` | `SvgIconSource` 与内容摘要 + 尺寸 + 着色三元组 LRU 光栅缓存、SVG 特性白名单 |
| `include/aurora/widget/icon_widget.h` | `Icon`（第 75 个 widget）与 `IconButton`（第 76 个 widget），尺寸档位 16 / 20 / 24 / 32 dp |

离线可跑的判据（值语义、注册表命中与未命中、缓存三元组、四态取色与主题取色、换肤重绘与 DisplayList 命令结构复用、明暗 golden 基线）全部在 `tests/unit/utest_icon.cpp` 与 `tests/integration/itest_icon_theme.cpp` 里，**不在本文档重复**。

### 1.2 执行载体

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_icon_gallery.cpp` | `demo_icon_gallery` | 图廊演示：亮 / 暗两段，各含 16 / 20 / 24 / 32 dp 四个图标与常态 / 禁用 / 边框三个 `IconButton`。有窗口系统时开窗，建窗失败时回退渲染 `build/Icon gallery - Aurora Demo.png` |
| `aurora_cli` | `aurora_cli` | 反射（`components` / `describe`）、校验（`validate`）与离屏渲染（`render`），用于降级语义用例 |

### 1.3 环境

| 后端 | 需求 |
|:---|:---|
| Windows | 桌面会话，`demo_icon_gallery` 开真窗 |
| X11 | `DISPLAY` 可用 |
| Wayland | `WAYLAND_DISPLAY` 可用 |
| GLFW | 有窗口系统；无窗口系统时 `run_demo` 回退 PNG |

## 2 用例

### 2.1 后端走查

#### TC-ICON-001 Windows 后端明暗主题图标走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-ICON-001 |
| 测试目的 | 验证 Windows 后端下 `Icon` / `IconButton` 的绘制、尺寸档位、显式着色与主题默认取色在明暗两档下均正确 |
| 前置条件 | Windows 桌面会话；已构建 `demo_icon_gallery`（`cmake --build build --target demo_icon_gallery`） |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `build/demo_icon_gallery`（纯执行，无预期结果）<br>2. 观察窗口「Light theme」段：「Sizes: 16 / 20 / 24 / 32 dp」一行四个图标均可见、无空白占位，其中两个为显式蓝 / 红，另两个为暗灰<br>3. 观察「Dark theme」段：两个跟随主题的图标由暗灰转为浅灰（明暗取色不同），两个显式着色图标颜色不变；「IconButton」一行中间那个（禁用态）图标明显半透明<br>4. 关闭窗口后记录进程退出码 |
| 预期结果 | 2. 四个尺寸档位图标均完整绘制、无占位方块；显式着色图标呈设定色，跟随主题图标呈 `on_surface_variant` 对应的暗灰<br>3. 跟随主题的两个图标在暗色段变浅、与亮色段可见不同；显式着色两图标颜色跨段不变；禁用 `IconButton` 图标呈 38% 不透明度的降透明观感<br>4. 进程退出码为 0 |

#### TC-ICON-002 X11 后端明暗主题图标走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-ICON-002 |
| 测试目的 | 验证 X11 后端下图标观感与 Windows 后端一致（跨后端渲染一致性），且明暗两档取色正确 |
| 前置条件 | `DISPLAY` 可用的 X11 会话；以 X11 后端构建 `demo_icon_gallery` |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `build/demo_icon_gallery`（纯执行，无预期结果）<br>2. 若开窗，核对「Light theme」段四个尺寸图标与三个 `IconButton` 的绘制与取色<br>3. 核对「Dark theme」段跟随主题图标变浅、禁用按钮半透明；若无窗口系统，改为检查生成的 `build/Icon gallery - Aurora Demo.png` 中明暗两段的对应像素<br>4. 记录进程退出码 |
| 预期结果 | 2. 四个尺寸档位图标完整绘制、无占位；显式着色与跟随主题两类取色与 Windows 后端一致<br>3. 暗色段跟随主题图标明显变浅；禁用 `IconButton` 图标半透明；回退 PNG 时两段区域可见对应明暗差异<br>4. 进程退出码为 0 |

#### TC-ICON-003 Wayland 后端明暗主题图标走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-ICON-003 |
| 测试目的 | 验证 Wayland 后端下图标观感与明暗两档取色正确，与其它后端一致 |
| 前置条件 | `WAYLAND_DISPLAY` 可用的 Wayland 会话；以 Wayland 后端构建 `demo_icon_gallery` |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `build/demo_icon_gallery`（纯执行，无预期结果）<br>2. 核对「Light theme」段四个尺寸图标与三个 `IconButton` 的绘制与取色<br>3. 核对「Dark theme」段跟随主题图标变浅、禁用按钮半透明；若无窗口系统，改为检查回退生成的 `build/Icon gallery - Aurora Demo.png`<br>4. 记录进程退出码 |
| 预期结果 | 2. 四个尺寸档位图标完整绘制、无占位；取色与 Windows / X11 后端一致<br>3. 暗色段跟随主题图标明显变浅；禁用 `IconButton` 图标半透明；回退 PNG 时两段区域可见对应明暗差异<br>4. 进程退出码为 0 |

#### TC-ICON-004 GLFW 后端明暗主题图标走查

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-ICON-004 |
| 测试目的 | 验证 GLFW 后端下图标观感与明暗两档取色正确，与其它后端一致 |
| 前置条件 | 有窗口系统的桌面会话；以 GLFW 后端构建 `demo_icon_gallery` |
| 依赖用例 | 无 |
| 操作步骤 | 1. 运行 `build/demo_icon_gallery`（纯执行，无预期结果）<br>2. 核对「Light theme」段四个尺寸图标与三个 `IconButton` 的绘制与取色<br>3. 核对「Dark theme」段跟随主题图标变浅、禁用按钮半透明；无窗口系统时改为检查回退生成的 `build/Icon gallery - Aurora Demo.png`<br>4. 记录进程退出码 |
| 预期结果 | 2. 四个尺寸档位图标完整绘制、无占位；取色与其它后端一致<br>3. 暗色段跟随主题图标明显变浅；禁用 `IconButton` 图标半透明；回退 PNG 时两段区域可见对应明暗差异<br>4. 进程退出码为 0 |

### 2.2 降级语义

#### TC-ICON-005 未注册语义名的结构化错误与不画占位

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-ICON-005 |
| 测试目的 | 验证引用未注册语义名时运行期返回结构化错误并跳过绘制（不崩溃、不画占位字形），且校验器对空注册表不误报 |
| 前置条件 | 已构建 `aurora_cli`；树文件 `icon_tree.json` 内容为 `{"type":"Column","props":{},"children":[{"type":"Icon","props":{"icon":"settings"}},{"type":"IconButton","props":{"icon":"close","enabled":false}}]}` |
| 依赖用例 | 无 |
| 操作步骤 | 1. 准备 `icon_tree.json`（纯执行，无预期结果）<br>2. 运行 `aurora_cli validate icon_tree.json`（进程内 `IconRegistry` 为空），查看输出与退出码<br>3. 运行 `aurora_cli render icon_tree.json -o icon_out.png -w 200 -h 120`，查看输出、PNG 与 stderr 日志 |
| 预期结果 | 2. 输出 `{"ok": true}`、退出码 0——注册表为空时校验器跳过语义名核对（应用尚未注册任何图标，不误报）<br>3. 输出含 `"ok": true` 与 `width 200` / `height 120`、生成 PNG、退出码 0；stderr 出现 `icon-name-not-registered` 结构化告警（含已注册名候选），PNG 中两图标位置为背景像素、无占位图形 |

## 3 执行记录表

本卷五条例均**尚未执行**：结果列与其联动列（执行日期 / 执行人 / 失败步骤号 / 实际现象 / 缺陷编号 / 备注）一并留空
（未执行不得填占位符）。四条后端走查需对应窗口系统与会话，无头环境无法产生真实窗口，故不留假记录；
降级语义用例需先在目标平台构建 `aurora_cli` 后由人照单执行。

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-ICON-001 | | | | | | | |
| TC-ICON-002 | | | | | | | |
| TC-ICON-003 | | | | | | | |
| TC-ICON-004 | | | | | | | |
| TC-ICON-005 | | | | | | | |
