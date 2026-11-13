# CHG-061 macOS 原生窗口截图（capture_window）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-061 |
| 提出日期 | 2026-10-31 |
| 当前状态 | 实施中 |
| 关联需求 | 无 |
| 影响面 | `src/aurora/window/macos_surface.cpp`（`capture_window` 实现：内容视图缓存位图 → PNG）、`include/aurora/window/macos_surface.h`（覆写声明与注释）、`include/aurora/window/surface.h`（基类注释的后端清单补 macOS）、`tests/unit/utest_macos_surface.cpp`（成员指针类型钉住）、`tools/verify/macos_surface_live_probe.mm`（真机探针补截图段）、`codespec/` 两处回写 |

## 动机

1. **调试门面的 OnScreenWindow 截图在 macOS 默认后端上缺位。** `Surface::capture_window` 的覆写只有 Win32 / D3D11 / X11 / GLFW；Apple 上默认选中的 `MacOSSurface` 落回基类，恒返回 `GeneralNotSupported`。GLFW 虽有 GL 读回腿，但本仓 macOS 的默认窗口后端是 AppKit，实际应用拿不到窗口截图。
2. **AppKit 有不需要录屏授权的现成机制。** `NSView` 的 `bitmapImageRepForCachingDisplayInRect:` + `cacheDisplayInRect:toBitmapImageRep:` 让视图把自己的绘制结果缓存进位图，不经过 WindowServer 抓屏，背景图形会话同样可用；`CGWindowListCreateImage` 在新系统弃用且需要屏幕录制授权，`screencapture` 在无人值守会话不可用——两条都不可取。
3. **口径必须与既有截图层一致。** 失败返回 `Result`、DEBUG 宏门控、零 `printf`、内容区语义可解释（不含标题栏，与 X11 / GLFW 的客户区截图同口径），不静默、不谎报。
4. **验证必须可复现。** 真机探针直接比对 PNG 解码后的像素尺寸与 `framebuffer_size()`、文件非空且含已绘像素，避免「写了个空文件也算成功」。

## 变更内容

1. **头文件**：`MacOSSurface` 增 `capture_window(const std::string &path) -> Result<bool> override` 声明，注明内容视图语义、DEBUG 门控与失败口径。
2. **实现**：`AURORA_ENABLE_DEBUG` 内走 `bitmapImageRepForCachingDisplayInRect:` → `cacheDisplayInRect:toBitmapImageRep:` → `representationUsingType:NSBitmapImageFileTypePNG` → `writeToFile:atomically:`。窗口 / 视图缺失返回 `SurfaceLost`；视图零尺寸或位图分配失败返回 `GeneralNotSupported`；PNG 编码或写盘失败返回 `GeneralUnknown`；路径非法 UTF-8 返回 `GeneralInvalidArgument`。DEBUG 关闭时返回 disabled 错误（与另三腿同文）。
3. **探针**：`macos_surface_live_probe` 在第二次 `present` 后调用 `capture_window`，核对：返回成功、文件存在且非空、`NSBitmapImageRep` 可解码且 `pixelsWide/pixelsHigh` 等于 `framebuffer_size()`、解码图里能找到已绘红色矩形的红色像素。
4. **单测**：`utest_macos_surface` 用成员指针类型 `static_assert` 钉住覆写签名；非 Apple 构建维持整文件 skip。
5. **文档**：`06-app-platform.md` §11 的 `CaptureSource` 表与门控段补 macOS 内容视图腿；基类 `surface.h` 的 `capture_window` 注释同步后端清单。

## 验收判据

1. `AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS` 下 `&MacOSSurface::capture_window` 的类型是 `MacOSSurface::*`，漏写声明即编译期转红。
2. 真机探针：`capture_window` 返回 `Ok(true)`；写出文件非空；解码 PNG 的像素尺寸与 `framebuffer_size()` 一致；解码图含已绘红色矩形像素。
3. `AURORA_ENABLE_DEBUG` 关闭的构建返回 `GeneralNotSupported`，不产出截图代码。
4. 窗口 / 视图缺失、零尺寸、非法 UTF-8 路径均返回机器可读错误，不崩溃、不写半截文件。
5. 既有 macOS 用例与全量 `ctest` 无新增红灯；`check_platform_macros` / `format-check` / 改动 TU 的 `lint` 绿。

## 回写落点

- `codespec/specification/06-app-platform.md` §11（`CaptureSource` 表、门控段与后端清单补 macOS 内容视图腿）。
- `codespec/SPECIFICATIONS.md` 的调试能力条目（如无独立需求 ID，则只在 §11 回写）。
- CHANGELOG.md 的 [Unreleased] Added 记一条。
- 公共 API 面只有 `MacOSSurface` 的覆写，根目录的 API schema 生成物不承载成员方法，预期零差异。

## 实施记录与偏离（落码期回写）

1. **落码完成，真机判据全绿。** `MacOSSurface::capture_window` 按 §变更内容 2 的四条错误分支实现；`aurora_verify_macos_surface` 在同一次运行里给出 **67/67 PASS**，含新增的六条截图判据：返回 `Ok(true)`、文件存在、字节数 7726（非空）、PNG 可解码、解码尺寸 720×480 与 `framebuffer_size()` 一致、缓存位图含已绘红色像素。
2. **像素判据口径调整（唯一偏离）。** 原写「任一点为红」按 `r>0.9 && g<0.1 && b<0.1` 判，实测失败：`bitmapImageRepForCachingDisplayInRect:` 产出的位图带显示器的广色域标签，`colorUsingColorSpace:deviceRGBColorSpace` 会做色域适配，纯红落到 `(240,74,45)`。这不是绘制结果的真实偏差——同一帧缓冲的直接读回（`red rect pixel at (0,0)`）一直是绿的。判据因此改为色域无关的「红主导」（`r>0.7` 且 `r-g>0.4` 且 `r-b>0.4`），既仍能区分红色矩形与浅灰底色（`(247,247,248)` 差值为 0），也不再把色彩管理当缺陷报。
3. **截图腿只在有 Aqua 图形会话时可验。** 首轮探针跑出 `zero-size content view` / `set_position` 往返失败 / `display_id == -1`，查因是虚拟机重启后停在登录窗口、没有 console 用户，与代码无关；开启自动登录并重启恢复会话后同一构建直接 67/67。零尺寸视图那条错误分支本身是按 §验收判据 4 写的，此处的表现是环境而非缺陷，但也是它第一次被真实触发到。
4. **基类口径同步。** `Surface::capture_window` 的默认错误文案与注释的后端清单都补上 macOS；`06-app-platform.md` §11 的 `CaptureSource` 行写明 macOS 原生后端取内容视图、不含标题栏，与 X11 / GLFW 客户区截图同口径。
5. **验证状态。** 改动文件 `clang-format` 干净；`macos_surface.cpp` / `utest_macos_surface.cpp` / `macos_surface_live_probe.mm` 的定向 clang-tidy 与全量 `ctest`、`check_manual_test_format` / `check_codespec_xref` 见本轮收尾记录。`aurora_api.json` 无差异（schema 不承载成员方法），未再生成。
