# NVDA 真机无障碍验收 Runbook（Win32 UIA 桥）

> 配套 `aurora_verify_win32_ua` 自动化探针（COM UIA 客户端同款入口）。探针证明的是 **平台接线**；
> 本 runbook 证明的是 **真读屏串讲**——前者绿不代表真人用 NVDA 能顺畅念出界面。两者互补。
>
> 适用：Windows 10/11 + NVDA 最新版（建议 2023.1+）+ 已构建 `aurora_verify_win32_ua` 的机器。

## 0. 前置

1. 安装 NVDA：https://www.nvaccess.org/download/ （安装时勾选「在 Windows 登录屏幕使用」可选）。
2. 启动 NVDA，默认「插入键」为 NVDA 修饰键。
3. 构建探针：
    - GDI 宿主：`cmake -S . -B build-mingw -G "MinGW Makefiles" -DAURORA_BACKEND_WIN32=ON && cmake --build build-mingw --target aurora_verify_win32_ua`
    - D3D11 宿主：`cmake -S . -B build-d3d11 -G "MinGW Makefiles" -DAURORA_BACKEND_D3D11=ON -DAURORA_BACKEND_WIN32=OFF && cmake --build build-d3d11 --target aurora_verify_win32_ua`
4. 先跑自动探针确认平台接线绿：`build-mingw/tools/verify/aurora_verify_win32_ua.exe`（退出码 0）。

> ⚠️ **编译器坑（实测）**：本项目默认 `CC/CXX` 解析为 LLVM/clang，而 clang 编译 `UIAutomationCore.h`
> 时不认 `interface` 关键字（`win32_ua.cpp` / `win32_host.cpp` 编不过）。 **构建验证探针必须用 mingw64 g++**：
> 在 CMake 配置时显式 `-DCMAKE_C_COMPILER=<mingw64>/gcc.exe -DCMAKE_CXX_COMPILER=<mingw64>/g++.exe`，
> 且确认 `AURORA_BUILD_VERIFY_TOOLS=ON`（验证探针目标才生成）。D3D11 路径同理。

## 1. 启动探针窗（保持它运行）

探针窗会显示一个含 5 个控件的列：Button「确定」、Checkbox、Slider、TextInput「abc」、Text「订单总额」。 **不要让探针窗失去焦点中途退出**——它存续期间桥才在投影。

## 2. NVDA 串讲步骤与预期播报

| 步骤 | 操作（NVDA 修饰键 = Insert）                      | 预期播报                                                             | 若不符                                  |
|------|---------------------------------------------------|----------------------------------------------------------------------|-----------------------------------------|
| A    | 焦点进入探针窗（Alt+Tab 切到它，或点一下窗内）    | NVDA 报窗口标题 / 面板                                               | 窗口根未投影 → 桥未激活，检查探针退出码 |
| B    | `Insert+↓`（读当前控件）                          | 「确定 按钮」                                                        | Name 缺失 → §16.2 #1（显式名兜底）      |
| C    | `Tab` 依次遍历                                    | 确定(按钮) → 复选框(未勾选) → 滑块(50%) → 编辑(abc) → 订单总额(文本) | 顺序错乱 → §16.2 #4 RTL（仅 RTL 应用）  |
| D    | 停在 Checkbox 按 `Space`                          | 「已勾选」/「未勾选」切换                                            | TogglePattern 未暴露 → 桥 pattern 映射  |
| E    | 停在 Slider 按 `←/→`                              | 「30%」「40%」…数值变化                                              | RangeValuePattern 未暴露                |
| F    | 停在 TextInput 输入字符                           | 逐字符/逐词朗读 + 编辑角色                                           | ValuePattern/TextPattern                |
| G    | `Insert+B`（读整个窗口）                          | 自上而下念出全部控件与文本                                           | 结构未完整投影                          |
| H    | 滚动/缩放窗口使某控件移出视口（若有 Scroll 容器） | 移出元素不再被念到；`Insert+B` 不含它                                | 离屏元素仍报几何 → §16.2 #5             |

## 3. 关键观察点（对照 §16.2 决议）

- **#1 兄弟标签**：本探针列未含兄弟标签，Checkbox/Slider 在 NVDA 下预期 **无名字**（与探针报告一致）。
  真机应另测一个「标签与控件同容器相邻」的布局，确认 NVDA 念出标签（兄弟关联生效）。
- **#3 离屏**：移出视口的控件，NVDA 的「元素浏览器」 (`Insert+F7`) 不应列出其几何（坐标应空）。
- **#4 RTL**：仅当应用推送 `Window::set_accessibility_rtl(true)` 时，`Tab` 遍历应为从右到左；默认 LTR 与表一致。
- **#7 文本几何**：文本控件的字符级光标/选区几何应正确（如 `Insert+F7` 对象浏览文本段不串位）。

## 4. 记录模板

每次验收回填：

```
日期：____  后端：GDI / D3D11   NVDA 版本：____
步骤 A-H 全过：是 / 否（卡在 ____）
#1 兄弟标签实测：____
#3 离屏实测：____
#4 RTL 实测：____
#7 文本几何实测：____
备注：____
```

## 5. 已知限制（非缺陷）

- 探针列控件均为「无语义标签来源」的裸控件；真实应用的标签多来自 `accessibility_label()` 或兄弟 `Text`。
- 无头 CI 永远绿灯：无 UIA 客户端则 `WM_GETOBJECT` 不投递、桥不构造（见 §16.3 两处真机崩溃）。真机 NVDA 是
  唯一能触发桥构造与同步重入的路径。
