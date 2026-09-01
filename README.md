# Aurora

**AI-first 的 C++20 跨平台 GUI 库** —— 声明式、响应式、概念可枚举；纯软件栅格 `Painter`
渲染，不依赖 GPU；以编译型静态库交付。

- 单一入口头：`#include "aurora/aurora.h"`，命名空间 `namespace aurora;`（推荐别名 `namespace au = aurora;`）
- 当前版本：`1.0.0-alpha.1`（早期预览开发版，**尚不构成 API 稳定性承诺**）
- 运行期版本常量：`AURORA_VERSION_STRING`（`include/aurora/core/version.h`）

## 特性总览

| 子系统 | 内容 |
|--------|------|
| core | 强类型尺寸/颜色（`Length`/`Color` 无裸构造）、`Result`/`Error` 错误模型、结构化日志与诊断、平台目标宏 |
| widget | 40+ 声明式控件：基础控件、布局容器、高级交互（表格/树/懒加载列表/选择器）、富文本 |
| render | 软件 `Painter` 栅格内核（SSE2/AVX2 双实现逐位一致）、DisplayList 录制回放、脏区追踪、FreeType+HarfBuzz 文本整形 |
| state | 细粒度响应式信号（State/Computed/Effect）、Store、UndoStack、线程池 + 协程异步 |
| layout | Flex/Grid 布局器、`Length` 四态约束求解、两阶段测量-定位、布局缓存与溢出策略 |
| event / animation / navigation | 命中测试 + 同步派发、焦点、手势与拖拽、快捷键；`Easing`/`Spring`/`Animator` 帧动画；`Route`/`Navigator`/`Router`、转场与 Hero、deep linking |
| theming / i18n / environment | 设计令牌与 `StyleProps`、`Provider` 依赖注入、媒体查询与响应式构建、多语言、`Modifier` 正交修饰 |
| window | Headless / Win32(GDI) / GLFW / X11 / Wayland / WASM / macOS / D3D11 上屏偏置；事件驱动帧循环（空闲 CPU 趋近 0） |
| storage / preferences | 信封式记录仓储（内存/文件系统后端）；JSON 键值配置（多进程安全 LWW 合并） |
| inspector / 工具链 | 本地回环 HTTP 检视服务、`aurora_lsp` 语言服务器、`aurora_mcp` MCP Server、`aurora_cli` |

## 快速上手

### 最小离屏渲染（零后端依赖，全平台可用）

```cpp
#include "aurora/aurora.h"
using namespace aurora;

int main() {
    Node root = au::Text("Hello, Aurora!").font_size(20);
    Scene scene{ std::move(root) };
    scene.render_to_png("hello.png", 200, 60);
}
```

### 桌面窗口应用（Windows 默认后端）

```cpp
#include "aurora/aurora.h"
using namespace aurora;

int main() {
    Node root = Node{ au::Column(au::ColumnProps{.children = {
        Node{ au::Text("Hello, Aurora!").font_size(24) },
        Node{ au::Button(au::ButtonProps{.label = "Click me"}) },
    }})};

    au::WindowOptions base{ .title = "Hello Aurora" }; // .title 为基类成员，指定初始化器不能指名基类
    au::Win32Options opts;                             // 先构造基类选项再拷贝至派生选项
    static_cast<au::WindowOptions &>(opts) = base;
    auto win_res = au::create_window(opts);
    if (!win_res)
        return 1;
    Scene scene{ std::move(root) };
    Application app{ std::move(scene), std::move(win_res.value()) };
    app.run(); // 事件驱动帧循环：静态界面空闲 CPU 趋近 0
}
```

等价的流式写法（推荐，屏蔽后端选项与 `Window` 组装细节）：

```cpp
au::App().title("Hello Aurora").size(800, 600).view(std::move(root)).run();
```

更多复制即用配方（状态、异步、序列化、导航、主题等）见
[codespec/GUIDELINE.md](codespec/GUIDELINE.md)；跨框架概念映射见
[codespec/CONCEPTS.md](codespec/CONCEPTS.md)。

## 构建

要求：CMake ≥ 3.20、C++20 编译器、Ninja（推荐）。

```powershell
cmake --preset ninja             # 已预置 Ninja + gcc/g++；如需自定义用 cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=…
cmake --build build              # 库 + 工具 + 测试（demo 不进默认构建）
ctest --test-dir build --output-on-failure   # 运行测试
```

全部 CMake 开关、编译宏与运行期环境变量统一见
[codespec/BUILD_OPTIONS.md](codespec/BUILD_OPTIONS.md)。常用项：

- `-DAURORA_BACKEND_WIN32=ON/OFF`（Windows 默认 ON）、`AURORA_BACKEND_GLFW/X11/WAYLAND/D3D11/MACOS/WASM`（默认 OFF）
- demo 目标按名构建：`cmake --build build --target demo_lazy_list`

## 作为第三方库消费（安装 + find_package）

```powershell
cmake --install build --prefix <安装目录>
```

```cmake
find_package(Aurora REQUIRED)
target_link_libraries(my_app PRIVATE Aurora::aurora)
```

## 平台支持矩阵

| 平台 | 后端 | 状态 |
|------|------|------|
| Windows | Win32Surface（GDI）/ D3D11 上屏偏置 | ✅ 完整（默认） |
| Linux | X11Surface / WaylandSurface | ✅ 可用（CMake 开启） |
| 跨平台 | GlfwSurface（OpenGL 3.x） | ✅ 可用（CMake 开启，默认 OFF） |
| Headless（内存 PNG） | HeadlessSurface | ✅ 全平台，测试与 CI 基座 |
| macOS | MacOSSurface | 🚧 骨架 |
| Web | WasmSurface | 🚧 骨架 |

## 已知限制（alpha）

- API 在 1.0 正式版前仍可能有破坏性变更（遵循 semver 并提供迁移路径）
- `media/`、`perf/` 模块为 experimental 成熟度
- Windows 为首要测试平台，Linux/macOS 覆盖有限
- 存储层 SQLite 后端尚未实现（当前 Memory/Filesystem 两后端）

## 文档

| 文档                                                         | 内容                                    |
|--------------------------------------------------------------|-----------------------------------------|
| [codespec/SPECIFICATIONS.md](codespec/SPECIFICATIONS.md)     | 需求 / 功能规格 / API 契约（权威）      |
| [codespec/ARCHITECTURE.md](codespec/ARCHITECTURE.md)         | 架构 / 运行时 / 分层 / 设计原则（权威） |
| [codespec/CONCEPTS.md](codespec/CONCEPTS.md)                 | 核心概念 / React·Flutter·Qt 跨框架映射  |
| [codespec/CODING_STANDARDS.md](codespec/CODING_STANDARDS.md) | 编码规范 / AI 友好性规则                |
| [codespec/GUIDELINE.md](codespec/GUIDELINE.md)               | 复制即用配方（最小可编译片段）          |
| [codespec/BUILD_OPTIONS.md](codespec/BUILD_OPTIONS.md)       | CMake 开关 / 编译宏 / 环境变量（权威）  |
| [CHANGELOG.json](CHANGELOG.json)                             | 版本与变更记录                          |
| [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)           | 第三方组件许可清单                      |

## 许可证

MIT，见 [LICENSE](LICENSE)。第三方组件许可见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

> **维护要求**：新增、删除或修改 `third_party/` 下的任何第三方组件时，必须同步更新
> [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)（组件清单与对应许可证保持一致），
> 避免许可声明与实际依赖漂移。详见 [codespec/CODING_STANDARDS.md](codespec/CODING_STANDARDS.md) §7.3。
