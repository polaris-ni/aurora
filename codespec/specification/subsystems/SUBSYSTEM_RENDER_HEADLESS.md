# H.9 渲染与无头校验

> 本文件是「三、特性详细规范」按功能域/子系统划分出的子文档；返回主线索引见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。
> 后续核心子系统 API 章节（H.11–H.17 + Log + AI-First）见 [`../subsystems_api/`](../subsystems_api)：SUBSYSTEM_API_SERIALIZE / SUBSYSTEM_API_LAYOUT_ENGINE / SUBSYSTEM_API_WIDGETS / SUBSYSTEM_API_INSPECTOR / SUBSYSTEM_API_TOOLING / SUBSYSTEM_API_LOG_AI。
> 相关功能域规范（A–G）见 [`../features/`](../features)：FEATURE_API_DESIGN / FEATURE_ARCH_STATE / FEATURE_RUNTIME_SAFETY / FEATURE_LAYOUT_RENDER / FEATURE_CROSS_PLATFORM / FEATURE_AI_INSPECTION / FEATURE_AI_TOOLING / FEATURE_ENGINEERING。

#### #H.9 渲染与无头校验（Headless）

核心目标：在无显示环境（CI / 测试 / AI 调试）中确定性地渲染并校验 UI。

- **`Painter`**：绘制后端抽象（事实渲染内核，软件栅格）。`fill_rect(Rect, Color)`、`draw_rect(Rect, Color)`、
  `draw_text(Rect, str, Font, Color)`（委托 `FontEngine`）、`draw_image(Rect, Image)`（双线性采样，premultiplied-alpha
  空间插值以避免半透明边缘暗边/光晕），`blend_pixel(x,y,Color)` / `blend_rect(Rect, Color)` 提供 alpha 混合（抗锯齿字体）。跨平台零依赖。新增
  **`set_scale(float)` / `scale()`**：高 DPI 下把 dp 坐标几何绘制 ×scale 放大到物理像素帧缓冲；像素级写入（`blend_pixel`/
  `draw_text`）直接落在物理像素，不再乘 scale。 **`fill_rect` 行级快速路径**：全局透明度为 1
  且裁剪栈全为非圆角矩形时，把裁剪收缩进矩形边界后整行写入（不透明→逐行 memcpy；半透明→内联 source-over，与 `set_pixel`
  同一浮点公式、位级一致），矩形裁剪边界与 `ClipRegion::coverage` 的 `contains`（含右/下边界）像素级等价；圆角裁剪（SDF
  覆盖度）或全局透明度 <1 仍走逐像素慢路径。 **逐像素原语的裁剪收缩（shrink_to_clips）**：慢路径
  fill_rect、渐变、阴影、composite、draw_image 的迭代范围均先与裁剪栈各矩形求交（取整与 coverage 的 contains
  含右/下边界语义像素级等价）——coverage 对裁剪矩形外恒为 0，收缩只剔除必被丢弃的迭代、结果逐位不变；部分脏区重绘帧（push_clip
  脏矩形）下避免全屏圆角背景/渐变按整窗面积逐像素白扫（大窗口数十 ms/帧）。此前逐像素路径在最大化窗口（2560×1440
  物理）单次全屏填充 >12ms，每帧两次全屏填充（begin_frame 清屏 + 容器背景）即拖选时选区高亮卡顿的主因；快速路径后不透明全屏填充
  ≈0.5ms（24×）、半透明 ≈5.8ms。 **`shift_pixels(float dy)`**（滚动 PAINT 提速）：就地垂直平移帧缓冲——`dy` 为逻辑 dp（>0
  内容下移、<0 内容上移），内部按 `m_scale` 换算物理行后 `std::memmove` 连续块搬移、末端 `memset` 归零（位移绝对值 ≥
  缓冲高则整块归零）；录制态静默 no-op（Display List 无像素搬移命令，避免录出与回放不一致的半成品）；只服务离屏缓冲的滚动
  reanchor，把已栅格化像素按新旧锚点差搬到新位置， **替代整块 `composite` 逐像素矩阵求逆位移**（3 屏缓冲 ~30ms → ~1ms）。 **
  `composite` 纯平移 + 同 scale 快路径**（同专项）：当变换矩阵为单位对角（`m11==m22==1, m12==m21==0`）、源 `sscale==m_scale`
  、无圆角裁剪（`!m_has_rounded_clip`）、`global_alpha==1` 时，把慢路径的逐像素矩阵求逆 **预计算为两张一维映射表**
  （表项用与慢路径逐字相同的浮点表达式求得，结果逐位一致），内层退化为「查表 + 连续内存扫描」；滚动容器每帧把离屏缓冲 blit
  到视口即走此路径（视口级 composite 是 60fps 预算大头），为 120fps 预留余量。
- **`FontEngine`**：文本引擎单例（`render/font_engine.h`），以 **FreeType** 为字体内核（栅格化）、 **HarfBuzz** 为文本
  shaping（经仓库 `third_party/` 源码 `add_subdirectory` 编入静态库，`project(... LANGUAGES CXX C)`，跨平台一致、确定性）；
  `draw_text_impl`/`measure_width`/`caret_x` 统一按文本 run 调 `hb_shape` 经 `hb_ft_font` 桥接 FreeType
  渲染，度量/光标/命中三者逐位一致。公共 API：`set_default_font(ttf_path)` / `register_font(family, ttf_path)` /
  `register_font_from_memory(family, ttf_bytes)` 注入字体（family 为空表示默认 sans-serif）；`measure_width` /
  `measure_height` / `draw_text`；选中原语 `caret_x(text, idx, font)` / `hit_test_char(text, x, font)` /
  `hit_test_char_inclusive`（以码点为索引，UTF-8 安全）；抗锯齿策略 `enum class TextAAMode{Supersample, ClearType}` + 进程级
  `set_text_aa_mode(mode)` / `text_aa_mode()`（默认 `ClearType`）。 **默认字体 = 内置 Noto Sans（OFL）**，引擎首次使用时经
  `register_font_from_memory` 自动注册为 `""/"sans-serif"/"Noto Sans"/"default"`，含 Headless，保证跨机文本渲染逐位确定；缺字按候选
  `FT_Face` 链回退（含系统 CJK 字体，如 msyh/simsun/MSGOTHIC）避免豆腐块；无任何可用字体文件时回退内置 `BitmapFont`
  （零依赖位图字体）保底。 **高 DPI**：度量 / 光标定位在逻辑 dp 空间，`draw_text` 按 `Painter::scale()`
  推导物理尺寸光栅生成物理分辨率字形，二者解耦保证高 DPI 清晰。 **锚定契约（TA_TOP 历史语义）**：`draw_text(r, ...)` 的
  `r.origin.y` 是 **行盒顶**而非基线——FreeType 以基线定位字形，实现内部首行基线 = `origin.y + 主 face ascender`（回退 face
  字形统一按主 face 基线对齐，混排同行基线一致）；全库调用方（Text/按钮/自定义控件）均按顶锚定传值，不得自行加减 ascent。
  **实显度量（display_*）**：FT hinting（`FT_LOAD_DEFAULT`）把每个字形 advance 取整到整像素，同一字形在不同像素尺寸下的
  advance **不成 scale 比例**（度量并非随 px 线性），故 `display_width` / `display_caret_x` /
  `display_hit_test_char{,_inclusive}` 必须按「绘制同源的物理像素尺寸 `lround(px×scale)` 真算前缀推进后折回 dp」（同 px、同
  hinting advance、同 kerning、dp 间距×scale，与 `draw_text` 的 pen 推进逐字符对齐）， **不得写成自然度量的转发别名**
  ——否则缩放屏下行内累计误差跨字符边界，造成「按 'a' 选中 'b'」的命中 off-by-one（回归：`test_text_selection`#13/#14）；scale=1
  退化为对应自然版（同源逐位相等，Headless/golden 路径不受影响）。 **命中复杂度契约**：`hit_test_char{,_inclusive}` 与
  `display_` 变体均为单趟扫描 O (n)（一次前缀推进内完成全部边界比较，边界值与逐次`caret_x`/`display_caret_x`
  逐位一致），禁止退化为「逐边界重算前缀」的 O (n²)——后者在全屏不折行长行（约 250 码点）下单次命中 ≈15ms，拖选高亮不跟手（bench
  `char_hit_x100_*` 场景监控）。 **shape 缓存**：`shape_line(line, faces, px, opts)` 是全库唯一 shaping 入口（
  `measure_width` / `caret_x` / `hit_test*` / `draw_text` 全部经它），其外包一层进程级 **LRU 缓存** —— 键为
  `{line, px, opts, faces_key}`（`faces_key` 为回退链各 `FontFace::id` 的 FNV 混合），双上限 **4096 条 / 8 MiB**，命中即复用已
  shape 的 `ShapedLine::glyphs` 并跳过 `hb_shape`，结果与重算 **逐位一致**。公共 API：
  `FontEngine::shape_cache_stats() -> ShapeCacheStats{hits, misses, entries, bytes}`（命中率 = `hits/(hits+misses)`，即性能门槛
  G-10 的度量口径）、`FontEngine::shape_cache_clear()`；二者 **不受 `AURORA_ENABLE_PROFILING` 门控，任何构建下均可读**（区别于
  `RenderCounters::shape_cache_hits/misses`，后者仅 PROFILING=ON 时累加）。`set_default_font` / `register_font` /
  `register_font_from_memory` 内部自动 `shape_cache_clear()`，避免字体集合变更后复用旧字形索引。 **字形栅格化短路**：
  `FT_Set_Pixel_Sizes` / `apply_italic` / `FT_Load_Glyph` 仅在 `GlyphAtlas` **未命中**时执行 —— blit 只读 atlas 的
  `left/top/advance`，命中时跳过这三步不改变任何像素；此前对每个字形无条件执行是 CJK 长文本的主要耗时来源（
  `text_cjk_12lines` 10.27 → 0.486 ms）。
    - **排版 opts（`TextLayoutOpts`）**：`measure_width` / `caret_x` / `hit_test_char` / `draw_text` 均提供接受
      `TextLayoutOpts` 的重载，携带 `letter_spacing`（相邻字形间间距，整串共 (n-1) 次）、`word_spacing`（词间距，仅空格后追加）、
      `italic`（经 FreeType `FT_Set_Transform` 仿斜，跨平台生效）。统一 opts 保证度量、光标、命中、绘制四者完全一致：
      `letter_spacing` 仅在相邻字形间添加；`italic` 进入字形图集 key 避免正/斜缓存串扰；间距为逻辑 dp，绘制侧 pen
      在物理像素空间累加时 **×`Painter::scale()`** 换算（实显度量同源 ×scale，自然度量 ×1），保证缩放屏下实绘间距与布局度量一致。
    - **文本抗锯齿（FreeType 驱动，跨平台一致）**：默认 `TextAAMode::ClearType`——`FT_RENDER_MODE_LCD` 输出 3× 水平 RGB
      子像素覆盖度，由 `Painter::blend_subpixel` 逐通道合成，得到真·子像素锐利文本（非灰度降级）； **仅当文本不透明（
      `c.a == 255`）时启用**，半透明或字体不可用时自动回退 `Supersample`。`Supersample` 为基线/兜底：`FT_RENDER_MODE_NORMAL`
      输出 A8 灰度覆盖度，盒式/逐像素 source-over 合成——颜色安全、背景无关，headless 与任意背景均正确。两种模式均依赖
      `Painter` 的 source-over 合成（Win32 帧缓冲 `SetDIBitsToDevice` 忽略 alpha，AA 须由 `Painter` 合成进 RGB）。
- **`Surface`**：绘制目标抽象（窗口 / 离屏）。`begin_frame()` / `present()` / `painter()`；`set_event_handler(EventHandler)`
  契约——所有后端只「采集原生事件并翻译为 `aurora::Event` 上抛」，事件派发集中到 `Application` 经
  `EventDispatcher + FocusManager`。`scale_factor() -> float`（默认 1.0；`Win32Surface`/`D3D11Surface` 返回 `dpi/96`，启用
  Per-Monitor DPI 感知，按物理像素创建窗口与帧缓冲，事件坐标 `/scale` 还原为 dp）。新增
  `set_present_dirty(const std::vector<Rect>&)` 钩子：`Window::present_root`
  在清脏前把本帧脏矩形（逻辑→设备坐标）交给后端，支持增量上屏的后端仅更新变化区，空向量表示全量上传；`Headless` 忽略（全量
  blit）， **`Win32Surface` 脏区带状 blit**：脏区非空时 `present()` 逐脏矩形把变化行带作为独立 top-down DIB 推给
  `SetDIBitsToDevice`（biWidth 保持整幅使行跨度一致、xSrc 选列，脏矩形向外取整覆盖裁剪绘制触及的全部像素），脏区一次性消费（present
  后清空，begin_frame 也清空），未设置则全量 blit（首帧/尺寸变化/低阶调用方行为不变）——大窗口下整窗 blit 是拖选帧绝对大头（5760×3132px
  实测纯 blit ~130ms、占帧成本 93%；带状 blit 后拖选帧 140→7.6ms，bench `bench_win32_present` 监控）。新增
  `set_title(const std::string&)` 虚方法（默认空实现；`Win32Window` 经 `SetWindowTextA`+`utf8_to_acp` 生效，`Headless`/
  `Glfw` 忽略）；`Window::set_title` 写 `m_title` 后同步下发 `Surface::set_title`，使运行期标题变更到达 OS 窗口。
- **`render_to_png(root, width, height, path)`**（其中 `root` 为已 mounted 的 `Node &`；`width`/`height`为画布逻辑尺寸）/**
  `render_to_logical_snapshot(root, width, height)`**：离屏渲染（见 `render/offscreen.h`，与`HeadlessSurface` 解耦）。

```cpp
au::render_to_png(root, 800, 600, "snapshot.png");                 // 像素级回归（人类审阅用）

au::Node btn_node{ au::Button(au::ButtonProps{ .label = "Test" }) };  // 包成 Node 树
auto snap = au::render_to_logical_snapshot(btn_node, 800, 600);        // 逻辑快照（AI 无头校验，返回 JSON）
TCHECK(std::string{ snap["type"].get<std::string>() } == "Button");
TCHECK(std::abs(snap["box"]["w"].get<float>() - 800.0f) < 0.001f);
```

> **统一后端 API（类型安全工厂）**：`SurfaceKind{Headless, Win32, Glfw, X11, Wayland, MacOS, Wasm}` 现仅为 **类型标签**（仅
> `auto_detect_surface()` 返回类型与 `Platform::surface` 字段，不再用于构造选择）；跨后端通用的
> `WindowOptions{size,title,max_frames}` + 各后端专属选项 `HeadlessOptions{png_path}` / `Win32Options{}` /
> `GlfwOptions{gl_major,gl_minor,resizable}` / `X11Options{}` / `WaylandOptions{}` / `MacOSOptions{}` /
> `WasmOptions{canvas_id}` + 工厂 `create_window(const XxxOptions&)`（类型安全重载，后端选择收口于此，编译器拒绝把某后端专属字段误用到不相关后端）（见
> `window/window.h`）。`Headless` 用软件 `Painter`；`Win32` 为零三方依赖的 Win32/GDI 后端（仅 `_WIN32`）；`GlfwSurface`（OpenGL
> 3.3 兼容剖面，绘制采用 1.1 立即模式）经 `AURORA_BACKEND_GLFW` 启用；`X11Surface`（X11/Xlib 原生窗口，仅 Linux 桌面，需
> `libX11`
> ，经 `AURORA_BACKEND_X11` 启用）与 `WaylandSurface`（原生 Wayland，`wl_shm`+`xdg-shell`+`xkbcommon`，仅 Linux 桌面，经
> `AURORA_BACKEND_WAYLAND` 启用）均含完整实现（pimpl + `src/aurora/window/*.cpp`）；`MacOSSurface`（Cocoa/AppKit，仅 Apple，经
> `AURORA_BACKEND_MACOS` 启用）与 `WasmSurface`（`<canvas>` 像素写回，仅 Emscripten，经 `AURORA_BACKEND_WASM`
> 启用）的窗口/上屏实现仍待对应平台工具链补全（roadmap）。应用层代码经 `create_window(XxxOptions)` 选择后端，不感知具体实现；不指定时
> `App::run()` 经 `auto_detect_surface()` 自动选用（优先级：原生 Wayland/X11/MacOS/Wasm > Win32 > Glfw >
> Headless；X11+Wayland
> 同时编译时按运行期会话类型择优——`WAYLAND_DISPLAY` 存在选 Wayland，否则 X11）。`create_native_window()`在 Linux
> 上按同序尝试并在真实显示不可用时回退 `Headless`。
> - **自定义 backend（稳定入口，不随 backend 数量增长）**：扩展点收口于 `Surface` 子类与 `create_window` 工厂，而非
    `Application` 构造重载。`Application`/`App` 只认两种形态： (a) 已组装的 `unique_ptr<Window>`——由
    `create_window(XxxOptions)` 工厂产出，类型安全在工厂处保证； (b) 已构造的 `unique_ptr<Surface>`——任意自定义/第三方后端经
    `Application(Scene, unique_ptr<Surface>, WindowOptions)` 或 `App().surface(...)` 注入，仅此一个入口覆盖所有自定义后端。空
    `Surface`/`Window` 仅 WARN 降级（错误归属调用方，其持有工厂 `Result`）。无头便捷构造 `Application(Scene, w, h)` 保持不变。
> - **后端能力 feature 宏（编译/链接期代码剪裁）**：每个内置后端由一对 CMake 开关 + feature 宏控制，可整体剔除：
    `AURORA_BACKEND_HEADLESS`（无头内存/PNG，默认 ON）/ `AURORA_BACKEND_WIN32`（Win32/GDI，Windows 默认 ON）/
    `AURORA_BACKEND_GLFW`（GLFW/OpenGL，由 `AURORA_BACKEND_GLFW` 开关控制）/ `AURORA_BACKEND_X11`（X11/Xlib，Linux 桌面，需
    `libX11`，默认 OFF）/ `AURORA_BACKEND_WAYLAND`（原生 Wayland，Linux 桌面，需 `wayland-client`/`xkbcommon`/
    `wayland-protocols`，默认 OFF）/ `AURORA_BACKEND_MACOS`（Cocoa/AppKit，Apple 平台，默认 OFF）/ `AURORA_BACKEND_WASM`
    （WebAssembly/Canvas，Emscripten 工具链，默认 OFF）。关闭后对应 `Surface` 子类、工厂重载与重型平台头（`<windows.h>`
    /GLFW/OpenGL/Xlib/wayland-client）被预处理器剔除，链接产物不再含该后端；自定义 `Surface` 注入路径始终可用，故「只用自定义
    backend」可不编译任何内置后端。详见 `window/window.h` 顶部契约注释。所有后端开关的默认值（GLFW 依赖经仓库内置
    `third_party/glfw` 源码构建，无伴随缓存变量）与
    `AURORA_BUILD_*` / `AURORA_ENABLE_*` 开关的完整列表见 `codespec/BUILD_OPTIONS.md`。
> - **`enable_dpi_awareness()`**（自由函数，`window/window.h`，规范入口、无 `PlatformBackend` 孪生；非 Windows 为空实现）：在进程创建
    **任何窗口之前**启用高 DPI 感知。 **这是 OS/进程级设置，非 per-Window、非 per-Surface**——Win32 经
    `SetProcessDpiAwarenessContext`（Per-Monitor V2 → V1 → `SetProcessDPIAware`）一次性启用；macOS（Cocoa 默认 HiDPI 感知）/
    Linux（X11 读 `Xft.dpi`、Wayland 由 compositor 经 `wl_output.scale` 下发）无需 opt-in，为空实现，扩展新平台只需在
    `enable_dpi_awareness()` 增分支。每窗口的 scale 查询仍是各 `Surface::scale_factor()` 的事，与「启用」正交。 **关键不变量：
    `init_console()`（`AllocConsole` 会创建控制台窗口）与 `create_window()` 之前必须已调用它**，否则 Windows 上
    `SetProcessDpiAwarenessContext` 失败 → 进程退化为 DPI 未感知（scale=1.0，高分屏发虚）。`run_demo` 已按此顺序调用；直接
    `create_window(Win32Options)` 的测试由 `Win32Surface` 构造器委托同一入口兜底。
