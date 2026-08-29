# BUILD_OPTIONS_BUILD

> 本文件由 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md) 划分而出（AURORA_BUILD_* 构建产物开关）。
> 返回主线见 [`BUILD_OPTIONS.md`](../BUILD_OPTIONS.md)。

**本文包含章节：**

- [1. `AURORA_BUILD_*` —— 构建产物开关](#1-aurora_build_--构建产物开关)

## 1. `AURORA_BUILD_*` —— 构建产物开关

控制「是否编译某个额外交付物」。这些开关 **不向库代码注入 feature 宏**，只是决定 `add_executable` 等目标是否被加入构建。

| 选项                            | 默认值 | 含义                                                                                                        | 引入的目标                                     |
|---------------------------------|--------|-------------------------------------------------------------------------------------------------------------|------------------------------------------------|
| `AURORA_BUILD_DEMOS`            | `ON`   | **定义**（非默认构建）`examples/demos/` 下每组件一个的可运行窗口 demo 目标；均 `EXCLUDE_FROM_ALL`，按需构建 | 各 `demo_<组件>` 可执行文件 + 聚合目标 `demos` |
| `AURORA_BUILD_TESTS`            | `ON`   | 编译 `tests/` 下全部用例并接入 CTest：`AURORA_TEST()` 注册、单一 runner 一次链接，逐条 `--run=<stem>` 隔离 | `aurora_test_runner` 可执行 + `enable_testing()` + `registry_integrity` 守护 |
| `AURORA_BUILD_INSPECTOR_SERVER` | `OFF`  | 编译 Inspector 远程 HTTP 服务器（跨平台：Windows 链 `ws2_32` / POSIX 链 `pthread`）                         | `aurora_inspector_server` 静态库               |
| `AURORA_BUILD_IMAGE_JPEG`       | `OFF`  | 启用 JPEG 图像解码支持（关闭时需由消费者自行提供解码后的像素）                                              | 仅改变编译期可用编解码能力，无独立目标         |
| `AURORA_BUILD_IMAGE_WEBP`       | `OFF`  | 启用 WebP 图像解码支持                                                                                      | 同上                                           |
| `AURORA_BUILD_IMAGE_PNG`        | `OFF`  | 启用 PNG 图像解码支持（与 `HeadlessSurface` 输出 PNG 相互独立：此为**解码**入站 PNG 的能力）                | 同上                                           |

> 图像编解码三选项（`AURORA_BUILD_IMAGE_JPEG/WEBP/PNG`）默认均 `OFF`，由 `cmake/AuroraImageCodecs.cmake` 定义，
> 且已在 `CMakeLists.txt` 中 `include(AuroraImageCodecs)`，是真实生效的构建开关；关闭任一选项则该格式的解码路径不参与编译。
> 注：`AURORA_BUILD_IMAGE_*` 虽归入 `AURORA_BUILD_*` 组，但会注入 **非 PUBLIC** 的 `AURORA_BUILD_IMAGE_*` 编译宏（codec TU 以 `#ifdef` 剪裁），属「编译期能力开关」而非「交付物开关」的例外——该宏不向消费者传播，关闭时仅损失解码能力、不改变像素输出。

demo 不进默认构建（`EXCLUDE_FROM_ALL`）：日常 `cmake --build build` 只建库/工具/测试； 单个 demo 按名构建（
`cmake --build build --target demo_lazy_list`），全部 demo 用聚合目标 （`cmake --build build --target demos`）。关闭
`AURORA_BUILD_DEMOS` 则连目标都不定义。

预编译头（PCH，两份 `.gch` 全局共享）：

- 库自身：`include/aurora/aurora_pch.h` 收录标准库 + `nlohmann/json.hpp`（不含 aurora 自有头， 保证库开发时命中率），`aurora`
  库 PRIVATE 编译一份；
- 消费者：`aurora_consumer_pch` 锚定目标把 `aurora.h` 伞头整体预编译一份，全部 demo/测试/工具经
  `target_precompile_headers(REUSE_FROM aurora_consumer_pch)` 复用（实测每 TU 省 ~1.6s 解析； aurora 头变更本就触发消费者重编，不增加失效面）；
- 覆盖率/ASan 开启时 PCH 全部自动关闭。

### 1.1 `AURORA_BUILD_INSPECTOR_SERVER`

| 属性     | 值                                                                                                     |
|:---------|:-------------------------------------------------------------------------------------------------------|
| 类型     | `option()`                                                                                             |
| 默认值   | `OFF`                                                                                                  |
| 说明     | 编译 Inspector 远程 HTTP 服务器（`InspectorServer`），暴露 REST 端点供外部工具远程访问运行时 widget 树 |
| 平台限制 | 跨平台（Windows: `ws2_32` / POSIX: `pthread`）                                                         |
| 链接依赖 | `ws2_32`（Winsock2）                                                                                   |
| 产物     | `aurora_inspector_server` 静态库（`src/aurora/inspector/inspector_server.cpp`）                        |
| 头文件   | `include/aurora/inspector/inspector_server.h`                                                          |

> 注：`inspector_server.cpp` 已从核心 `aurora` 库源文件列表中排除（`list(FILTER ... EXCLUDE)`），
> 仅当 `AURORA_BUILD_INSPECTOR_SERVER=ON` 时编入独立静态库 `aurora_inspector_server`，
> 避免未开启开关时引入 Winsock2 依赖。

开启示例：

```powershell
cmake -S . -B build -DAURORA_BUILD_INSPECTOR_SERVER=ON
```


