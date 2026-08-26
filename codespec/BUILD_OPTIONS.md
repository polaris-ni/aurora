# 编译选项与宏定义统一参考（Build Options Reference）

> 本文件是 Aurora 项目 **所有**编译期可配置开关、缓存变量、传播宏与运行时环境变量的 **唯一权威来源**。
> 凡涉及 CMake 构建选项、feature 宏、路径/插桩变量，一律以本文件为准；其余文档（`AGENTS.md` / `SPECIFICATIONS.md` /
> `ARCHITECTURE.md`）只做指针，不再重复罗列，以避免文档漂移。

---


本文档已划分为以下子文档（位于 `./<主题>/` 下）：

- [BUILD_OPTIONS_BUILD.md](./build_options/BUILD_OPTIONS_BUILD.md) — AURORA_BUILD_* 构建产物开关
- [BUILD_OPTIONS_BACKEND.md](./build_options/BUILD_OPTIONS_BACKEND.md) — AURORA_BACKEND_* 后端开关（= feature 宏）
- [BUILD_OPTIONS_ENABLE.md](./build_options/BUILD_OPTIONS_ENABLE.md) — AURORA_ENABLE_* 插桩 / 分析 / 能力开关
- [BUILD_OPTIONS_INTERNAL.md](./build_options/BUILD_OPTIONS_INTERNAL.md) — 强制缓存变量 / 全局编译定义 / 运行时·测试环境变量 / 标准 CMake 变量 / 安装与 find_package / 快速参考

## 0. 总览（三层命名分类法）

所有构建选项按 **语义**严格归入三组，组内前缀一致：

| 前缀               | 类别               | 语义                                                                                                                                 | 是否向库注入 feature 宏                                                                                                        |
|--------------------|--------------------|--------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `AURORA_BUILD_*`   | 构建产物开关       | 是否**构建**某个额外交付物（demos / tests）                                                                                          | 否（例外：`AURORA_BUILD_IMAGE_*` 注入非 PUBLIC 的 `AURORA_BUILD_IMAGE_*` 宏，见 `BUILD_OPTIONS_BUILD.md` §1）                                           |
| `AURORA_BACKEND_*` | 内置后端开关       | 每个 `Surface` 后端一个开关；**开关名 = PUBLIC feature 宏名**                                                                        | 是（`#ifdef` 剪裁 + PUBLIC 传播给消费者）                                                                                      |
| `AURORA_ENABLE_*`  | 插桩/分析/能力开关 | 是否注入编译/链接期分析工具（覆盖率 / 内存检测 / 调试 / 性能插桩）或开启构建加速/内部能力（lld 链接器 / ccache 缓存 / SIMD / DEBUG） | 多数否；`PROFILING`/`TRACING` 注入 PUBLIC 宏、`SIMD`/`DEBUG` 注入内部宏、`LLD`/`CCACHE` 不注入宏（见 `BUILD_OPTIONS_ENABLE.md` §3 系） |

> 注：`Win32/GDI` 后端仅在 `_WIN32` 下编译， **无需**额外开关，已由 `AURORA_BACKEND_WIN32` 的内置默认值覆盖。

### 0.1 CMake 脚本布局

顶层 `CMakeLists.txt` 只做「工程声明 + 核心库目标 + 模块编排」，具体逻辑按职责划分在 `cmake/` 下的模块 （`include()`
不创建新作用域，各模块内 `option()`/目标定义与写在顶层完全等价，开关名与默认值不变）：

| 模块                                | 职责                                                                                                                                           |
|-------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|
| `cmake/AuroraThirdParty.cmake`      | FreeType/HarfBuzz 源码构建（须在告警注入前 include）                                                                                           |
| `cmake/AuroraBackends.cmake`        | 全部 `AURORA_BACKEND_*` 后端剪裁开关 + P3 优化宏（`AURORA_LAYOUT_CACHE` 等）                                                                   |
| `cmake/AuroraImageCodecs.cmake`     | `AURORA_BUILD_IMAGE_JPEG` / `AURORA_BUILD_IMAGE_WEBP` / `AURORA_BUILD_IMAGE_PNG`（编译期能力开关，注入非 PUBLIC 的 `AURORA_BUILD_IMAGE_*` 宏） |
| `cmake/AuroraSimd.cmake`            | `AURORA_ENABLE_SIMD`（光栅内核 SIMD 双实现，内部宏，不 PUBLIC 传播）                                                                           |
| `cmake/AuroraCcache.cmake`          | `AURORA_ENABLE_CCACHE`（ccache 编译缓存启动器）                                                                                                |
| `cmake/AuroraTools.cmake`           | 工具/基准可执行（`aurora_add_tool()` 统一样板）+ `AURORA_BUILD_INSPECTOR_SERVER`                                                               |
| `cmake/AuroraTests.cmake`           | `AURORA_BUILD_TESTS` CTest 目标（GLOB `tests/*.cpp`）                                                                                          |
| `cmake/AuroraInstrumentation.cmake` | `AURORA_ENABLE_COVERAGE` / `AURORA_ENABLE_ASAN` / `AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING`（须在全部目标定义之后 include）          |
| `cmake/AuroraInstall.cmake`         | 安装 + `find_package(Aurora)` 导出（须在后端开关之后 include）                                                                                 |

---

> 参考 [1. AURORA_BUILD_* 构建产物开关](./build_options/BUILD_OPTIONS_BUILD.md#1-aurora_build_--构建产物开关)。

> 参考 [2. AURORA_BACKEND_* 后端开关 feature 宏](./build_options/BUILD_OPTIONS_BACKEND.md#2-aurora_backend_--后端开关--feature-宏)。

> 参考 [3. AURORA_ENABLE_* 插桩/分析/能力开关](./build_options/BUILD_OPTIONS_ENABLE.md#3-aurora_enable_--插桩分析能力开关)。

> 参考 [4. 强制缓存变量 FreeType HarfBuzz GLFW 源码构建内部](./build_options/BUILD_OPTIONS_INTERNAL.md#4-强制缓存变量freetype--harfbuzz--glfw-源码构建内部)。

> 参考 [5. 全局编译定义 非选项 固定注入](./build_options/BUILD_OPTIONS_INTERNAL.md#5-全局编译定义非选项固定注入)。

> 参考 [6. 运行时 测试环境变量](./build_options/BUILD_OPTIONS_INTERNAL.md#6-运行时--测试环境变量)。

> 参考 [7. 标准 CMake 变量](./build_options/BUILD_OPTIONS_INTERNAL.md#7-标准-cmake-变量)。

> 参考 [8. 安装与 find_package 消费端集成](./build_options/BUILD_OPTIONS_INTERNAL.md#8-安装与-find_package消费端集成)。

> 参考 [9. 快速参考 速查表](./build_options/BUILD_OPTIONS_INTERNAL.md#9-快速参考速查表)。
