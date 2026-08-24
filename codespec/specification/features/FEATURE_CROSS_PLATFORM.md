# E. 跨平台层（#14,#15）

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 相关核心子系统实现（H 系列）见 [`../subsystems/`](../subsystems)（H.1–H.10c 信号/动画/环境/事件/渲染/窗口/平台）与 [`../subsystems_api/`](../subsystems_api)（H.11–H.17 + Log + AI-First 序列化/布局/控件/Inspector/工具/日志）。

#### #14 零 #ifdef 跨平台 + 插件式平台扩展

**核心目标：** AI 无需处理平台分支

**规范：**

```cpp
// 应用代码 100% 跨平台，零 #ifdef
// 当前 API：au::Application 组合根（经类型安全后端工厂 create_window 适配平台）
auto win_res = au::create_window(au::Win32Options{ .title = "My App", .size = {640, 480} });
au::Application app{ au::Scene{std::move(root)},
                     win_res ? std::move(win_res.value()) : nullptr,
                     au::WindowOptions{ .title = "My App", .size = {640, 480} } };
app.run();  // 自动适配平台后端（Win32 / Glfw / Headless）

// 流式便捷封装（已实现，§H.9）：一行式启动
au::App().title("My App").size(640, 480).view(std::move(root)).run();

// 平台差异封装在内部，提供显式运行时查询（已实现，§H.9）
if (au::platform().is_mobile()) { /* 移动端适配 */ }   // au::platform().capabilities() 含 multitouch / high_frequency_pointer
```

**关键约束：**

- 应用层代码 **绝不出现** `#ifdef _WIN32`
- 平台差异通过 **运行时查询**而非编译期宏
- 所有平台差异封装在 **平台抽象层**，对外暴露统一接口
- 平台特定能力通过 **可选 trait/插件**显式启用，AI 不需要感知 `#ifdef`
- **库内部/无法回避的编译期剪裁**：平台/架构/位宽分支一律使用 `core/platform.h` 的规范化目标宏 （`AURORA_PLATFORM_*` /
  `AURORA_ARCH_*` / `AURORA_BIT_*`，编译器探测、恰好一个语义、完整映射表见头注释）， **禁止直接书写 `_WIN32` / `__linux__`
  等原生宏**（例外：该头自身、`third_party/`、CMake 脚本、
  `_WIN32_WINNT` 等 SDK 版本旋钮）；构建选项视角详见 `BUILD_OPTIONS.md` §5.1
- **版本常量（`core/version.h`）**：`AURORA_VERSION_MAJOR/MINOR/PATCH`（数字分量，CMake `project(VERSION)` 注入）、
  `AURORA_VERSION_SUFFIX_STR` + `AURORA_HAS_VERSION_SUFFIX`（semver 预发布后缀，如 `"alpha.1"`，来自 CMake 缓存变量
  `AURORA_VERSION_SUFFIX`）、合成宏 `AURORA_VERSION_STRING`（完整 semver 串，如
  `"1.0.0-alpha.1"`；稳定版无后缀）。库发布版本的单一事实来源是根 `CHANGELOG.json` 的
  `currentVersion`；未走 CMake 直接包含该头时回退到头内默认值

> 注：Web 后端的 **CMake 开关已接入**（`AURORA_BACKEND_WASM`，默认 OFF，须 `emcmake cmake` 配置），`WasmSurface` 头已实现基础结构，
> `present()` 写回 `<canvas>` 的像素胶水与 Emscripten 链接仍待补全（roadmap）； **Linux
桌面双后端已完整实现并经真实开窗验证**：X11（`AURORA_BACKEND_X11`，Linux 桌面 + libX11）与原生 Wayland（
> `AURORA_BACKEND_WAYLAND`，`wl_shm`+`xdg-shell`+`xkbcommon`）均 pimpl 隔离、软件 `Painter`
> 上屏、完整事件翻译，可单开或同时开启（同时开启时按会话类型运行期择优）；macOS（`AURORA_BACKEND_MACOS`，Cocoa/AppKit）开关已接入，
> `MacOSSurface` 的 Cocoa 实现待补全。当前默认可用后端为 `HeadlessSurface`（内存/离线 PNG）、`Win32Surface`（仅 `_WIN32`）、
> `GlfwSurface`（CMake `AURORA_BACKEND_GLFW` 开关）、`X11Surface`/`WaylandSurface`（Linux 桌面开关）。以下为 WASM 适配规划：

**Web/WASM 平台特殊处理：**

> Web 平台的线程模型（单线程 + Web Worker）、事件循环、渲染管线与原生平台差异极大。Aurora 的解决方案：
>
> - **事件循环适配：** 在 WASM 上，Aurora 的事件循环映射到浏览器的 `requestAnimationFrame` 循环，而非自旋等待。应用代码无需感知此差异。
> - **线程模型适配：** WASM 默认单线程。Aurora 在 WASM 上将 `au::async` 映射为 Web Worker + `postMessage` 通信，而非
    `std::thread`。若环境支持 `SharedArrayBuffer` + `Atomics`（需跨源隔离），则启用真正的多线程。应用代码使用相同的
    `au::async` / `co_await` API，无需 `#ifdef`。
> - **渲染适配：** WASM 上渲染目标为 `<canvas>` 元素（WebGL2/WebGPU），Aurora 内部自动选择，应用代码不感知。
> - **限制声明：** WASM 后端当前未实现；规划中 WASM 上不支持 `DirectHandler` 高频鼠标事件，自动降级为
    `requestAnimationFrame` 轮询，并在 `au::platform().capabilities()` 中声明此限制（该 API 已实现，见 §H.10）。

---

#### #15 跨平台一致行为 + 黄金文件验证

**核心目标：** AI 无需考虑平台差异

**规范：**

- 布局引擎 **自研**（不依赖平台布局），保证像素级一致
- 字体渲染统一（自带文本整形引擎）
- 事件模型统一（触摸/鼠标/键盘/手柄统一抽象）
- 配套 **跨平台布局测试套件**和 **黄金文件验证**，确保同布局在各平台结果一致
- AI 生成的代码无需为不同平台微调

---
