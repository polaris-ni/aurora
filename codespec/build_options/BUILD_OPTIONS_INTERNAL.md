# BUILD_OPTIONS_INTERNAL

> 本文件由 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) 划分而出（强制缓存变量 / 全局编译定义 / 运行时·测试环境变量 / 标准 CMake 变量 / 安装与 find_package / 快速参考）。
> 返回主线见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**本文包含章节：**

- [4. 强制缓存变量（FreeType / HarfBuzz / GLFW 源码构建内部）](#4-强制缓存变量freetype--harfbuzz--glfw-源码构建内部)
- [5. 全局编译定义（非选项，固定注入）](#5-全局编译定义非选项固定注入)
- [6. 运行时 / 测试环境变量](#6-运行时--测试环境变量)
- [7. 标准 CMake 变量](#7-标准-cmake-变量)
- [8. 安装与 find_package（消费端集成）](#8-安装与-find_package消费端集成)
- [9. 快速参考（速查表）](#9-快速参考速查表)

## 4. 强制缓存变量（FreeType / HarfBuzz / GLFW 源码构建内部）

FreeType 与 HarfBuzz 均从仓库内置源码（`third_party/freetype`、`third_party/harfbuzz`）经
`add_subdirectory` 编入 `aurora` 静态库（断网可构建、版本确定），。
`CMakeLists.txt` 先 `add_subdirectory(third_party/freetype)` 后 `add_subdirectory(third_party/harfbuzz)`—— harfbuzz 在
`if (TARGET freetype)` 时自动开启 `HB_HAVE_FREETYPE`（提供 `hb-ft.h` 并链接 freetype）。 aurora 直接
`target_link_libraries(aurora PUBLIC freetype harfbuzz)`，文本 shaping 由 HarfBuzz （`hb_shape` + `hb_ft_font`）完成，故
FreeType 自身保持 `FT_DISABLE_HARFBUZZ=ON`（standalone，避免别名耦合）。

以下变量由 Aurora 以 `CACHE BOOL "" FORCE` 强制设置， **普通消费者无需手动配置**：

| 变量                  | 值    | 说明                                                                   |
|-----------------------|-------|------------------------------------------------------------------------|
| `FT_DISABLE_BZIP2`    | `ON`  | 关闭 FreeType 的 bzip2 依赖                                            |
| `FT_DISABLE_PNG`      | `ON`  | 关闭 FreeType 的 PNG 依赖                                              |
| `FT_DISABLE_HARFBUZZ` | `ON`  | FreeType 不自带 HarfBuzz（shaping 由 aurora 直接链接的 harfbuzz 提供） |
| `BUILD_SHARED_LIBS`   | `OFF` | FreeType/HarfBuzz 静态链接，消费者无需额外 DLL                         |
| `HB_BUILD_SUBSET`     | `OFF` | 关闭 HarfBuzz subset 库                                                |
| `HB_BUILD_RASTER`     | `OFF` | 关闭 HarfBuzz raster 库                                                |
| `HB_BUILD_VECTOR`     | `OFF` | 关闭 HarfBuzz vector 库                                                |
| `HB_BUILD_GPU`        | `OFF` | 关闭 HarfBuzz GPU 后端                                                 |
| `HB_BUILD_UTILS`      | `OFF` | 关闭 HarfBuzz 命令行工具                                               |
| `GLFW_BUILD_EXAMPLES` | `OFF` | GLFW 源码构建（仅 `AURORA_BACKEND_GLFW=ON`）：关示例                   |
| `GLFW_BUILD_TESTS`    | `OFF` | GLFW：关测试                                                           |
| `GLFW_BUILD_DOCS`     | `OFF` | GLFW：关文档                                                           |
| `GLFW_INSTALL`        | `OFF` | GLFW：关安装规则                                                       |

FreeType + HarfBuzz 经 `add_subdirectory(third_party/...)` 固定版本源码编入 `aurora` 静态库，全平台确定性。
GLFW 同口径自 `third_party/glfw`（3.5.1）源码构建，但仅在 `AURORA_BACKEND_GLFW=ON` 时经
`add_subdirectory(... EXCLUDE_FROM_ALL)` 引入并静态链接；默认 OFF 时链接产物不含 GLFW。

---

## 5. 全局编译定义（非选项，固定注入）

| 宏                        | 注入方式                                      | 作用域         | 说明                                                                                                |
|---------------------------|-----------------------------------------------|----------------|-----------------------------------------------------------------------------------------------------|
| `NOMINMAX`                | `add_compile_definitions(NOMINMAX)`           | 全局           | 抑制 `<windows.h>` 的 `min/max` 宏，保证 `std::min/max` 在 Windows 可用（`preferences.cpp` 等用到） |
| `_CRT_SECURE_NO_WARNINGS` | `target_compile_definitions(aurora PUBLIC …)` | 仅 MSVC        | 抑制 MSVC 对 `std::fopen` 等 POSIX 函数的弃用警告（`render/png.h`）                                 |
| `AURORA_BACKEND_*`        | `target_compile_definitions(aurora PUBLIC …)` | 由 §2 开关控制 | 后端 feature 宏，见 §2                                                                              |

---

## 6. 运行时 / 测试环境变量

以下变量不进入编译，仅在运行测试（尤其 `tests/test_golden.cpp`）时被 `std::getenv` 读取：

| 变量                       | 取值           | 作用                                                                       |
|----------------------------|----------------|----------------------------------------------------------------------------|
| `AURORA_GOLDEN_DIR`        | 目录路径       | golden 真值目录；缺省为 `tests/golden`。从仓库根运行测试以保证相对路径解析 |
| `AURORA_UPDATE_GOLDEN`     | 非空（如 `1`） | 把当前渲染覆盖为新的 golden（首次生成 / 主动更新真值）                     |
| `AURORA_GOLDEN_MAX_DIFF`   | 整数           | 像素最大允许色差阈值（默认见测试）                                         |
| `AURORA_GOLDEN_MAX_PIXELS` | 整数           | 允许不一致像素数上限（默认见测试）                                         |

> golden 测试因默认字体更换为内置 Noto Sans 须 `AURORA_UPDATE_GOLDEN=1` 整体重生成后再人工比对 diff（纯字体位移，非逻辑回归）。
> `ctest` 默认 CWD=`build/`，故依赖相对路径的 golden 测试须从仓库根直接运行可执行文件（如 `./build/test_offscreen`，原 test_golden 已并入）。

---

## 7. 标准 CMake 变量

| 变量                 | 默认值              | 说明                                                                                                                                            |
|----------------------|---------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| `CMAKE_BUILD_TYPE`   | `Release`（若未设） | 常规构建默认 Release；覆盖率/ASan 开关会自行清除其中的 `-O3/-Os/-DNDEBUG`                                                                       |
| `CMAKE_CXX_STANDARD` | `20`                | 强制 C++20（`CMAKE_CXX_STANDARD_REQUIRED ON`，`CMAKE_CXX_EXTENSIONS OFF`）                                                                      |
| 生成器               | —                   | 本机推荐 `Ninja`（空转/增量调度远快于 `MinGW Makefiles`，实测空转 16.6s → 0.2s）；Make 仍支持。GLFW/D3D11 后端链接依赖对应工具链的 `lib-*` 目录 |

---

## 8. 安装与 find_package（消费端集成）

Aurora 以静态库交付，并提供 `find_package(Aurora)` 消费端集成。安装产物布局（前缀 `<PREFIX>`）：

```text
<PREFIX>/include/aurora/...        # 公共 API 头（aurora.h 入口）
<PREFIX>/include/nlohmann/...      # 随附的 nlohmann/json 头（aurora.h 传递包含）
<PREFIX>/include/freetype2/...     # FreeType 头（freetype 自带 install 安装）
<PREFIX>/include/harfbuzz/...      # HarfBuzz 头
<PREFIX>/lib/libaurora.a           # 主静态库
<PREFIX>/lib/libfreetype.a         # 随附 FreeType 静态库
<PREFIX>/lib/libharfbuzz.a         # 随附 HarfBuzz 静态库
<PREFIX>/lib/cmake/Aurora/AuroraConfig.cmake   # 包配置（定义 Aurora::aurora 等导入目标）
<PREFIX>/lib/cmake/Aurora/AuroraConfigVersion.cmake
```

### 8.1 安装

```powershell
cmake --install build --prefix <PREFIX>     # 或 CMake 的 CMAKE_INSTALL_PREFIX
```

安装规则由 `CMakeLists.txt` 末尾引入的 `cmake/AuroraInstall.cmake` 提供（采用 **手写 `AuroraConfig.cmake`**，不依赖 `third_party`
自带的 export 集，以避免整图导出冲突与 `ZLIB::ZLIB` 等跨工程引用失效）。

### 8.2 消费端用法

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Aurora REQUIRED)               # 指向 <PREFIX>/lib/cmake/Aurora
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE Aurora::aurora)
```

`AuroraConfig.cmake` 定义导入目标（全部 `STATIC IMPORTED`，路径相对安装前缀解析， **不依赖绝对路径硬编码**）：

| 导入目标         | 含义                                                                                                                                        |
|------------------|---------------------------------------------------------------------------------------------------------------------------------------------|
| `Aurora::aurora` | 主静态库：携带 `cxx_std_20`、公开 include、`AURORA_BACKEND_*` 等 feature 宏（与编译期 `aurora` 目标 PUBLIC 编译定义一致）、以及下列传递依赖 |
| `freetype`       | 随附 FreeType 静态库（位置 `<PREFIX>/lib/libfreetype.a`）                                                                                   |
| `harfbuzz`       | 随附 HarfBuzz 静态库                                                                                                                        |

`Aurora::aurora` 自动传递链接：`freetype` + `harfbuzz` + zlib（FreeType 解压字体表需要，尽力定位；找不到则跳过）

+ `winpthread`/`pthread`（HarfBuzz 内部互斥，MinGW 下为 `winpthread`）+ Win32 系统库（`user32 gdi32 shell32 ole32 uuid`，仅
  `WIN32`）。 消费者 **无需** 手动 `find_package(FreeType)` / `find_package(HarfBuzz)`。

### 8.3 feature 宏导出约定

安装期将编译期生效的 `AURORA_BACKEND_*` / `AURORA_LAYOUT_CACHE` / `AURORA_OCCLUSION_CULLING` / `AURORA_DISPLAY_LIST` 与全局
`NOMINMAX` 收集进 `AURORA_EXPORTED_DEFINES`， 由 `Aurora::aurora` 的 `INTERFACE` 编译定义导出，使消费者以与库 **完全一致**
的宏集编译 `aurora.h`（避免 ODR/剪裁不一致）。新增 feature 宏时须同步本段与 `CMakeLists.txt` 的 `foreach` 收集列表。

`AURORA_ENABLE_PROFILING` / `AURORA_ENABLE_TRACING` 亦为 PUBLIC feature 宏，但导出策略特殊
（见 `cmake/AuroraInstall.cmake`）：

- `AURORA_ENABLE_PROFILING=ON`（显式强制）→ 宏对全部配置生效，写入 `AURORA_EXPORTED_DEFINES`；
- `AURORA_ENABLE_PROFILING=AUTO` → 宏由生成器表达式按 **构建配置** 决定，安装期无法用单一值表达，
  故 **不导出**；消费者若需插桩接口，自行 `-DAURORA_ENABLE_PROFILING` 或安装 `=ON` 的构建；
- `AURORA_ENABLE_TRACING=ON` → 写入 `AURORA_EXPORTED_DEFINES`（其隐含的 PROFILING 亦被强制为 `ON`，一并导出）。

### 8.4 最小验证示例

`examples/consumer_find_package/`（独立工程， **不归属主构建**）即一个最小消费端：Headless 渲染一段文本到 PNG， 可用来验证
`find_package` + 静态链接（含 FreeType/HarfBuzz）在目标工具链上工作。先安装 Aurora，再：

```powershell
cd examples/consumer_find_package
cmake -S . -B build -G "MinGW Makefiles" -DAurora_DIR="<PREFIX>/lib/cmake/Aurora"
cmake --build build
./build/consumer.exe          # 输出 consumer_out.png
```

> 注意：消费端生成器须与安装库的生成器/工具链一致（本机用 `MinGW Makefiles`，链接 MinGW 构建的 `.a`）。

---

## 9. 快速参考（速查表）

```text
# 产物开关
-D AURORA_BUILD_DEMOS=ON|OFF        # demos（默认 ON）
-D AURORA_BUILD_TESTS=ON|OFF        # CTest（默认 ON）
-D AURORA_BUILD_INSPECTOR_SERVER=ON|OFF  # Inspector HTTP 服务器（默认 OFF，跨平台）

# 后端开关（= feature 宏，PUBLIC 传播）
-D AURORA_BACKEND_HEADLESS=ON|OFF   # 无头 PNG（默认 ON）
-D AURORA_BACKEND_WIN32=ON|OFF      # Win32/GDI（Win 默认 ON，否则 OFF）
-D AURORA_BACKEND_D3D11=ON|OFF      # D3D11 GPU 上屏（默认 OFF）
-D AURORA_BACKEND_GLFW=ON|OFF       # GLFW/OpenGL1.1（默认 OFF；源码构建 third_party/glfw，无伴随变量）
-D AURORA_BACKEND_X11=ON|OFF        # X11/Xlib（Linux 桌面，默认 OFF；需 libX11-devel）
-D AURORA_BACKEND_WAYLAND=ON|OFF    # 原生 Wayland（Linux 桌面，默认 OFF；需 wayland-devel/wayland-protocols-devel/libxkbcommon-devel）

# 插桩（COVERAGE 与 ASAN 互斥）
-D AURORA_ENABLE_COVERAGE=ON|OFF    # gcov（默认 OFF）
-D AURORA_ENABLE_ASAN=ON|OFF        # ASan/UBSan（默认 OFF）
-D AURORA_ENABLE_PROFILING=AUTO|ON|OFF  # 渲染插桩（默认 AUTO：Debug 开、Release 关）
-D AURORA_ENABLE_TRACING=ON|OFF     # Chrome Trace 落盘（默认 OFF，隐含 PROFILING=ON）
-D AURORA_ENABLE_SIMD=ON|OFF        # 光栅内核 SIMD 双实现（默认 ON，仅内部优化、不导出）
-D AURORA_ENABLE_DEBUG=AUTO|ON|OFF  # 真实后端 DEBUG 能力（默认 AUTO：Debug/RelWithDebInfo 开、Release 关；仅内部宏、不导出）
-D AURORA_ENABLE_CCACHE=ON|OFF      # ccache 编译缓存（默认 ON，加速重复编译）

# 安装 / 消费端
cmake --install build --prefix <PREFIX>          # 安装静态库 + 头 + AuroraConfig.cmake
cmake -S app -B app/build -DAurora_DIR="<PREFIX>/lib/cmake/Aurora"   # 消费端 find_package

# 运行时（测试）
AURORA_GOLDEN_DIR=<dir> AURORA_UPDATE_GOLDEN=1 ./build/test_offscreen
```

