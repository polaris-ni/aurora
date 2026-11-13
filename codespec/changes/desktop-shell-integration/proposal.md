# CHG-054 桌面外壳集成四件：文档输出与打印会话、URL 方案注册、文件关联、登录项

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-054 |
| 提出日期 | 2026-10-31 |
| 当前状态 | 已归档 |
| 关联需求 | 无 |
| 影响面 | `include/aurora/window/platform.h`（`PlatformCapabilities` 增 `native_print_session` / `url_scheme_registration` / `file_association` / `run_at_login` 四枚位）；`include/aurora/app/` 新增五个公共头（`document_output.h` / `url_scheme.h` / `file_association.h` / `run_at_login.h` / `single_instance.h`），并同步 `tools/check/umbrella_manifest.txt`（伞头门禁的清单文件，新增头必须登记，该文件不属 `codespec/` 故不入回写落点）；`src/aurora/app/detail/` 与 `src/aurora/window/detail/` 新增零平台头折算层与 Win32 / macOS / Linux 三套桥；`src/aurora/render/detail/` 新增最小 PDF 写出器（复用 `include/aurora/core/compression.h` 的 DEFLATE 编解码器）；`codespec/errors.toml` 及生成物在 `Platform` 域下扩码；`tests/unit/` 新增五个 `utest_*.cpp`，**逐一以新头为「目标单元」**（`check_code_doc_sync` 的 TEST-R5：每个公共单元头都要被某测试显式声明为目标单元，存量按基线、**新增违规立即转红**），再加折算层用例；`tools/verify/` 增四枚 `*_live_probe.cpp`（沿用 `EXCLUDE_FROM_ALL`，macOS 侧同既有 Apple 探针一样需配 `<name>_probe_info.plist`）；`codespec/` 六处回写 |

## 动机

1. **这四项的缺口处在比「没实现」更靠前的位置：它们从未被建模为可查询的能力。** `PlatformCapabilities` 目前十个位（`multitouch` / `high_frequency_pointer` / `desktop` / `mobile` / `system_drag_in` / `system_drag_out` / `recent_documents_os_projection` / `native_menu_bar` / `runtime_visibility` / `runtime_always_on_top`）是本库唯一自认的 OS 集成清单，打印、URL 方案、文件关联、登录项一个位都没有。既有位确立的是**双层口径**——能力位回答「OS 机制在不在」，`Result` 回答「这一次调用成了没」，二者不互相替代；缺位方向上公开入口还要退成可观测的 no-op 或结构化错误，而不是静默失效。这四项连这套表达框架都没进入，于是应用连「问一句这台机器支不支持」都做不到，只能退回猜平台宏。

2. **需求表里没有它们，说明这是能力扩张而不是需求欠账。** `SPECIFICATIONS.md` 需求表现有 53 条条目，`NATIVE-MENU-BAR` / `RECENT-DOCUMENTS` / `FILE-DIALOG` / `SYSTEM-DRAG-DROP` / `CLIPBOARD-RICH-FORMATS` 各自对应本目录前序提案，而打印 / URL 方案 / 文件关联 / 登录项一条都没有。两种诚实姿态必须二选一：补进需求表并承认这是新增对外承诺，或明确写「不在本库职责内」。**既不承诺也不排除是当前的实际状态，也是最坏的状态**——调用方无从判断某个平台上的空结果是「还没做」还是「永远不会做」。本提案取前者，并在回写落点里把需求行一并补上。

3. **打印侧缺的不是渲染能力，而是「页」这个概念。** 现有落盘通道只有两条：`Application::render_to_png`（见 `include/aurora/app/application.h`）与 `Surface::capture_window`（`include/aurora/window/win32_surface.h`，仅 Windows 且 DEBUG 下生效）。二者都是**屏幕几何的快照**——承袭既有画布的宽高与 device pixel ratio，没有页尺寸、页边距、目标 DPI、多页与分页。而 `Painter` 本身是软件栅格：`begin()` 收**逻辑 dp** 尺寸、缩放旋钮是 `set_scale()`（`scale()` 可读回，物理缓冲按 dp × scale **四舍五入**，见该头第 57、66、69 行），也就是说「按 A4 几何与 600 DPI 重跑一遍既有绘制路径」当前就已成立，缺的只是输出目标那一层。**本提案不写新渲染器，只加输出目标**——这是把四项里最重的一项做小的最好理由。

4. **文档输出走栅格入 PDF，局限必须写死而不是留给人碰。** `Painter` 产出 RGBA8 位图，落到 PDF 就是按页嵌入的图像 XObject；这与 Qt `QPrinter` 输出 PDF 时保留矢量绘图与可检索文本不是一回事。**三条代价写进规格**：文本不可选择不可搜索、放大会失真、文件体积随页数线性膨胀。不做矢量是有理由的——那要给 `Painter` 增加记录式后端，而 DisplayList 语义与 PDF 操作集并不一一对应（渐变、bidi 重排后的字形 advance、`.notdef` 回退都要另行映射），还要 CIDFontToWidth 与 ToUnicode CMap 才能让拷出来的文本是原文。代价远超「打印出来是对的」这个主诉求，故本批如实申报局限，不暗示超越实际的能力。

5. **深链断在两端，不在一端。** `Navigator::open_uri` 与 `NavigatorHost::open_uri`（`include/aurora/navigation/navigator.h`）已经能把一个按斜杠切分的名称序列重建为整栈路由，所以**应用内解析并不缺**。缺的是两端：**OS 不知道本应用能处理某个方案**（注册表 / Launch Services / 桌面条目缺位），以及**第二个进程被唤起时无从把 URI 交给已在运行的那个进程**（无单实例、无转交通道）。只补注册不补转交，用户点链接的观感就是闪一个新的空窗口，等于没做。因此单实例不是可选附件，而是这一项的前提。

6. **文件关联的立场是「查询 + 声明候选」，不是「抢占默认」。** Qt 在这一块的对应物主要是 `QMimeDatabase` 的查询：告诉你某个扩展名归谁，不替你改成你。本提案沿用同一条克制线——查询侧尽力做全平台，声明侧把「本应用可处理」写进各自的本机数据库，但**不主动把默认处理程序改成自己**。默认程序属用户偏好且抢占不可逆，是越界动作。这条边界在本仓库已有同源判例：`tools/servers/aurora_mcp.cpp` 明确拒绝写入自启动目录以防 confused-deputy，把「写到 OS 会自动生效的位置」看成需要显式授权的事，而不是库该代劳的事。

7. **登录项必须是三态，不能是布尔。** 「OS 侧此刻没让我自启」至少有四种成因：从未设置过、用户手动禁用、该平台无此机制、查询本身失败（注册表打不开、登录项服务状态不明）。布尔会把后三种并成同一个 `false`，应用就分不清「我调一下就能开」和「这台机器不可能」。故状态值是 `Enabled` / `Disabled` / `Unsupported` 三枚。三态真正的价值体现在用户手工禁用那一条上：它仍然存在且带禁用标记，查询应报 `Disabled` 而非 `Unsupported`，写入也不该把它当成「没设置过」而抹掉。

8. **macOS 三条注册线共享同一个硬前提，写进能力位而不是留给调用方踩。** 最近文档 OS 投影那一批的 macOS 实测口径第一条是「只在 `.app` 包内生效，裸可执行文件调用该 API 不落记录」。URL 方案、文件关联、登录项在 macOS 上同样要求 bundle 身份（`CFBundleURLTypes` / `CFBundleDocumentTypes` / `SMAppService` 三者都认 bundle identifier，沙箱形态下登录项另有约束）。能力位回答的是「机制在不在」，故 macOS 位的正确标注是「机制在、桥已接、但需要 bundle；不在 bundle 内时调用走 `Result` 失败并在 detail 里点名原因」，而不是简单翻 `true` 了事。

9. **这四项里三项都依赖「我是谁、我从哪条命令行起来」，而这两个问题本库当前一个都答不了。** 全仓 `include/aurora` 除 `cli/`（命令行工具的解析层，不是应用入口）外**零 `argc` 与 `argv`**——`Application` 的三个构造只收 `Scene` 加 `Surface` / `Window` 加 `WindowOptions`，进程命令行对库完全不可见。而 URL 方案注册必须写出「OS 唤醒本应用时执行什么」（Windows 的 open 命令 `%1`、Linux 桌面条目的 `Exec=`）、单实例必须把第二个进程命令行里那个 URI 读出来才谈得上转交、macOS 一侧认的是 bundle identifier。**应用身份与命令行取值的来源因此不是「到时候补一个判断就行」的实现细节，而是本批的先行项**，故变更内容里单列一条把它建模出来——调用方显式传入或新增一枚取值入口二者择一，写进规格后三座桥共用，不许各自去写 `GetModuleFileName` / `readlink` / `NSBundle`。

## 变更内容

### 共用骨架

1. **能力位四枚**落于 `PlatformCapabilities`（`include/aurora/window/platform.h`），语义与既有个位同款（只答机制存在性，运行期失败走 `Result`）：`native_print_session`、`url_scheme_registration`、`file_association`、`run_at_login`。**文档输出（PDF）不入能力位**——它是纯库内能力，不依赖任何 OS 机制，设位会污染「能力位等于 OS 机制存在性」这条既有口径；它缺依赖时（compression 层的 DEFLATE 编解码器不可用）走既有的 `compression-unavailable`，不新开口子。
2. **错误出口**统一落 `ErrorDomain::Platform`（现有枚举里注释为 platform backend and OS integration 的通用域，无需新增域）。**每项各两枚码，共八枚**，成对理由是本目录前序那批留下的判例——`native-menu-unsupported`（kind `unsupported`）与 `native-menu-install-failed`（kind `io`，message 带 `{detail}` 占位）就是这么拆的：前者答「机制不在」，后者答「机制在但这一回没做进去」。本批四项如法炮制，`X-unsupported` 一律 kind `unsupported`，`X-failed` 一律 kind `io`，retryable 一律 `false`（注册表与登录项写入没有天然幂等，标成可重试会诱导上层写出错误的恢复逻辑）。原拟的一枚通用 `shell-integration-not-installed` 作废——它会把四项的失败并成同一张脸，正是这套体系要避免的事。**macOS 缺 bundle 的情形属于后者而非前者**：机制在、桥已接、只是此刻装不进去，detail 要点名「不在 `.app` 包内」，也正是动机第 8 条要求落成「失败且带明因」而非把能力位翻成 `false` 的落点。
3. **分层形态**每项一致：零平台头折算层（可无头逐值单测）产出平台无关的中间态（计划 / 文本 / 清单），三套平台桥共用同一份中间态，不存在两套各自演化的语义。桥头文件只被桥内部包含，不泄漏到公共面；`#ifdef` 不进公共头，沿用本目录前序提案的做法（声明在下、实现在独立 TU）。**另补一条对这些头部贯穿生效的约束**：这四项的 OS 侧操作无一例外会触碰磁盘或系统服务（打印后台处理程序枚举、注册表读写、桌面条目写入、登录项服务），故每个公共入口的文档注释必须写明 `@note Thread:` 与 `@note Side-effects:`——前者答「是否允许在渲染/主线程以外调用」，后者答「是否独占文件名/注册表键」。本仓库公共头的注释契约里这两个标签是常态（`include/aurora/app/mime_types.h` 即是范例），不写等于把「会不会卡住帧循环」留给调用方去猜；「可能阻塞」的入口一律显式标注，由调用方决定是否下沉到工作线程，库不代为开线程。
4. **应用身份与命令行（本批先行项，见动机第 9 条）**：三项注册都要拿「本应用的可执行文件路径」填 OS 的命令模板，单实例要拿一份命令行快照才有所谓「第二个进程收到的 URI」，macOS 侧还要 bundle identifier。来源**要么**由调用方显式传入一枚 `AppIdentity`（可执行路径 + 稳定实例键 + 命令行参数快照），**要么**新增一枚取值入口读取它们，二者必须择一并写进 `06-app-platform.md`；三座桥不许各自去调用 `GetModuleFileName` / `readlink` / `NSBundle`。实例键的稳定性要一并规定：默认派生自可执行路径的规范化形式（不做版本 / mtime 混入），因为键一变，单实例的互斥量与锁文件就认不出自己，表现为「每次都当成首个实例」。
5. **公共落位**在 `include/aurora/app/` 新增五个头，各自取「一个主题一个入口、进程级单例或自由函数组」的形态，与 `recent_documents.h` / `native_menu_bar.h` 同格，并受伞头门禁管辖。

### 文档输出与打印会话

6. **文档合成层（跨平台，无 OS 依赖）**：新增公共头 `document_output.h`（放在 include/aurora/app/ 下）给出 `PageSpec` 与多页导出入口。五条约束**必须**写成硬条款，任一条含糊都会产出「看起来对、形体错位」的 PDF：

   - **页边长用物理单位，不用 dp。** 页尺寸与页边距一律以毫米（或 PostScript 点）表达，`PageSpec` 另带 `dpi` 决定栅格密度。理由：A4 是 210×297mm，折到 96 dpi 是 793.7 dp，**整数 dp 表达不了**；写成 `size_dp` 等于先在纸张几何上损失约 0.4% 再一路传到每页像素。内容侧仍走既有的 dp 绘制路径，由本层的换算把 dp 映射到页内容区的物理像素。
   - **每页的物理像素数以 Painter 的读回值为唯一真源。** 流程是 `set_scale(dpi / 96.0F)` → `begin(内容区 dp 宽高)` → 读 `width()` / `height()`，它们按 dp × scale 四舍五入。本层不得另算一套向上取整——两套规则会差一个像素，而这种差错在肉眼走查里看不出来。
   - **先合成到不透明底色，再嵌进 PDF。** Painter 的帧缓冲是 RGBA8 直色，未绘制区域是透明黑（`include/aurora/render/offscreen.h` 的 `background` 参数正是为此存在），而 `DeviceRGB` 图像 XObject 不带 alpha 通道；直接嵌等于丢掉 alpha，未覆盖区域会印成黑块。故每页必须在纸白（或调用方给定底色）上先行合成。
   - **多页必须落在签名上。** `export_pdf(scene, spec)` 这种只收一棵树的形态与「多页边界由调用方决定」自相矛盾，改为「页数 + 逐页绘制回调」：回调每收到一枚 `begin()` 过的内容区 Painter 就画一页。
   - **不得复用既有离屏通道。** `include/aurora/render/offscreen.h` 的 `render_to_image` 注释明写「逻辑像素 = 设备像素」，没有 device pixel ratio 形参、缩放恒为 1，出不了 600 dpi 的页；本层必须直接建 Painter。它与 `Application::render_to_png` / `Scene::render_to_png` 的关系要在规格里写明——后者仍是屏幕密度快照，不是文档路径，两条通道不许混为一谈。

   每页 RGBA8 交由 `src/aurora/render/detail/` 的最小 PDF 写出器：一个 `DeviceRGB` 图像 XObject、内容流用 `FlateDecode`（直接复用 `include/aurora/core/compression.h` 的 `AURORA_COMPRESS_FORMAT_DEFLATE`，即 RFC 1951 裸 DEFLATE，正是 PDF 需要的那一种），末尾 xref 表与 trailer，产出 PDF 1.4。
7. **打印会话层（OS）**：同一份头给出 `print_targets()`（返回值含标识 / 名称 / 是否默认）、`open_print_session(target_id, spec)` 及其上的 `submit_page(pixels)` / `finish()` / `abort()`。**打印面板一律自绘**：本库本来就是 UI 库，自绘既避免为 Linux 引入 GTK 依赖（本库迄今零 GTK 依赖），也保证三平台观感一致。这条立意差异要写进 `06-app-platform.md`，不要让人以为漏了系统对话框。
8. **三平台的落法不对称，且这个不对称要说出来**：Windows 走打印后台处理程序会话（`StartDoc` / `StartPage` / `EndDoc` 系列）；macOS 走 AppKit 打印操作配自绘附件视图；**Linux 侧本批不声称支持原生打印会话，能力位如实为 `false`，替代路径是「导出 PDF 后交给默认处理程序」**。理由是本库第三方形态目前全是 vendored 源码（十个 vendored 子目录加一个散装单头），引入 libcups 会第一次开出「系统动态库依赖」这个口子，而换来的只是一段打印会话——这笔账本批不认。是否引入 vendored 的 CUPS 作为未决项登记并由验收判据第 9 条强制收口，不允许停在「先把位翻上去再碰运气」。

### URL 方案注册与跨实例转交

9. **新增公共头 `url_scheme.h`（放在 include/aurora/app/ 下）**：`register_scheme(scheme, display_name)` / `unregister_scheme(scheme)` / `is_scheme_registered(scheme)` / `registered_scheme_owner(scheme)`。折算层负责 RFC 3986 方案文法校验（`ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`）与保留方案拒收（`http` / `https` / `file` / `mailto` 等），这是纯字符串逻辑，无头可逐值测，三平台桥共用同一份判定。注册一律落在**用户级**位置（Windows 取 `HKCU` 下的 Software 分支、Linux 取用户私有的应用程序目录），不碰需要提权的位置，与登录项不动 `HKLM` 同源于同一条边界。另附一条：`unregister_scheme` 须先确认当前归属是自己才动手——归属已经是空则视为幂等成功，归属是别的应用则**一个字节都不改**并返回带原因的 `Result`；「撤销」不得越过别人的登记，这条与登录项不碰机器上其它条目同源。
10. **新增公共头 `single_instance.h`（放在 include/aurora/app/ 下）**：单实例守卫加 URI 转交通道。`acquire()` 返回「我是首个实例」还是「已有实例在跑且我的命令行 URI 已送达它」，`set_on_uri_received(cb)` 供首个实例在存续期内接收后续唤起。Windows 用有名互斥量加跨进程消息；macOS 直接借用 AppKit 的应用级唤起通道（`kAEGetURL` 一系）；Linux 用运行时目录下的锁文件加套接字。收到的 URI 落地形态是复用既有的 `open_uri`，不在 URL 方案这一层重新发明路由语义。三条语义必须写死：

   - **交付必须有人负责后果。** 「我的命令行 URI 已送达它」这句话里藏着一个未定义的失败情形：首个实例正在退出、或 URI 在它那里路由不掉——这种情形必须有归属。故规定：交付后等待确认，超时或收到明确的失败 `Result` 时**回落为「按新实例正常启动」**，「第二实例默默退出」这条路径被禁：它表现为「点了链接什么都没发生」，正是动机第 5 条要消灭的观感。
   - **本机通道要挡住同机其他用户。** Linux 的锁文件与套接字必须落在用户私有的运行时目录（`$XDG_RUNTIME_DIR` 优先，退回用户私有的候选目录）并以 `0700` 权限创建；不得用 `/tmp` 下的可预测名字。否则同机其他用户可以预先占住该名字做拒绝服务，或抢先监听那个套接字接收本应用的 URI——这与本仓库既有的一条自制口径同源（`tools/servers/aurora_mcp.cpp` 拒绝写入会被 OS 自动生效的位置以防 confused deputy），同样是「写到 OS 会信任的地方就有安全问题」。Windows 有名互斥量与 macOS 唤起通道由 OS 兜底，无需额外处理，但要在注释里写明这一不对称。
   - **转交来的 URI 是不可信输入。** 它由外部进程随意构造，故只交给既有的 `open_uri` 走路由注册表；命中不到路由时的行为沿用导航层现状（不改此处语义），本层不替它兜底，也不把它拼进任何待执行的命令行。

### 文件关联

11. **新增公共头 `file_association.h`（放在 include/aurora/app/ 下）**：`associated_application(extension)`（查询当前归属）、`register_association(decl)`（声明本应用可处理某扩展名与 MIME 类型）、`unregister_association(extension)`，能力位 `file_association`。折算层做扩展名规范化（前导点、大小写折叠）与扩展名到 MIME 的方向映射。**词汇表不许另起炉灶**：`include/aurora/app/mime_types.h` 已是剪贴板与系统拖放共用的 MIME 唯一真源，其文件头也写明了同一条取舍（「表内只登记本库自身会产出 / 消费的词汇」），故本层引用该表的常量，只在这张表现在没覆盖的方向上补一枚「扩展名 → MIME」的函数——现有头只有 MIME → 布尔这一个方向，两张表若是各自演化，同一扩展名迟早查出两个 MIME。
12. **注册侧的平台现实要写进规格，否则实施者会对 Windows 侧的落法判错。**

   - **Windows**：只写**用户级**的 `HKCU` 分支，不写需要提权的 `HKLM`（与登录项那条红线同源自同一条边界）。更关键的是，Windows 自 Win8 起**不允许以编程方式设置默认处理程序**——默认归用户且受保护，故 `register_association` 能做的只有「声明本应用能处理」，而要让本应用出现在「打开方式」列表里，还须在 `HKCU` 的 `RegisteredApplications` 下登记并指明 Capabilities 键；只写 ProgID 的实施会得到「注册成功但列表里没有我」的假阴性，必须提前写进规格让人照着查。
   - **macOS**：由 Uniform Type Identifier 与 Launch Services 查询，同样要求 bundle 身份。
   - **Linux**：归属取决于桌面条目数据库（`mimeinfo.cache` 与 `mimeapps.list` 两个纯文本文件），需要一个小的纯文本解析层（可无头测）。若判据显示查询侧收益不抵养一个解析层的代价，就把 Linux 的查询列为能力缺位、只开放声明侧——两种结局都可接受，但必须落在某一端，不许悬着。
   - **查询侧兜底一致**：`AssocQueryString` 系答的是「系统默认」，而用户另有一层偏好记录（Windows 的用户选择、macOS 的历史关联列表）。本批只答「当前实际生效的那个归属是谁」并在 detail 里点明来源，不把两层口径混成一次查询——混了就必须回答「谁覆盖谁」，而那在不同 Windows 版本上还不是同一个答案。

### 登录项

13. **新增公共头 `run_at_login.h`（放在 include/aurora/app/ 下）**：三态枚举 `RunAtLoginStatus { Enabled, Disabled, Unsupported }`，入口 `run_at_login()` 与 `set_run_at_login(bool)`。Windows 写 `HKCU` 下的 Run 值——**不用 `HKLM`**，后者需要提权，属动用户机器的越界动作；macOS 经系统登录项服务（要求 bundle，同动机第 8 条），沙箱形态如实报 `Unsupported` 并点名原因；Linux 经 freedesktop 自动启动桌面条目，启用与禁用**表达在同一个文件里的开关键**，不做「删文件当禁用」——删掉会同时抹掉用户手工禁用留下的那条记录。

## 验收判据

1. **页面折算单测**（无头）：页边距与尺寸按毫米给定的前提下，断言的内容区物理像素**取自 Painter 的读回值**（`set_scale(dpi / 96.0F)` → `begin()` → `width()` / `height()`），而不是层里另算一套取整——两层各有一套取整规则时，错一个像素且肉眼走查看不出来。覆盖三组：96 dpi、600 dpi、以及 A4 内容区宽度在 96 dpi 下落到非整数那一组（专门钉住「页尺寸不能用整数 dp 表达」这条约束）。**变异自证**：把 `dpi / 96.0F` 换成 `dpi / 72.0F`，或把 Painter 侧的四舍五入替换为截断冒充层内规则，相应用例必须转红。
2. **PDF 结构单测**（无头，配 golden）：产出 PDF 的头标识、版本号、对象类型齐全且 xref 偏移正确，用一份最小页面做逐字节 golden 比对；`FlateDecode` 流解压后与输入 RGBA8 逐字节一致。**变异自证**：把压缩格式从 DEFLATE 换成 ZLIB（RFC 1950 带两字节头与 Adler-32 尾的包装），结构用例必须转红——这条专治图省事写错格式标识。
3. **栅格局限要有机械证据，不能只在文档里说一句**：PDF 里没有文本操作符、只有图像 XObject 这件事实本身要能被断言（遍历内容流即可证）。用意是防止后续提案把它顺手宣称成矢量能力。
4. **能力位矩阵单测**：逐 `SurfaceKind` 断言四枚位的期望值。macOS 上要区分「在 bundle 内」与「裸可执行」两种形态——前者调用成功，后者调用失败且 detail 点名缺 bundle；两者必须落成两条独立判据，不许只断言能力位。**变异自证**：给未接线的平台翻位，相应用例转红。
5. **方案文法折算单测**（无头）：`myapp`、`my-app`、`v2+y` 通过；数字开头、含下划线、含冒号的被拒；保留方案被拒且具体点名它之所以被拒是因为保留身份——不是因为解析崩溃。**变异自证**：从保留名单里删掉 `mailto`，该用例必须转红。
6. **扩展名规范化单测**（无头）：`.PNG`、`png`、`PNG` 折叠为同一键；空串与孤立的点被拒而不是当成合法扩展名。
7. **登录项三态单测**：机制缺位时 `run_at_login()` 报 `Unsupported`，已启用报 `Enabled`，已禁用报 `Disabled`，三态互不顶替；「用户手工禁用的条目仍存在」这一形态单独成例。**变异自证**：把三态压成布尔（缺位与禁用同值），该用例必须转红。
8. **桌面条目文本单测**（Linux 侧，无头）：启用态与禁用态的差异只在开关键、而不是「有没有这个文件」；不删除用户手工禁用的那一条。
9. **未决项强制收口**：Linux 是否引入 CUPS 必须在动手写第一座 Linux 打印桥之前给出结论，且结论无论落在哪一端都写进 `06-app-platform.md`。本提案不接受「先翻位、边碰运气」——能力位停在未定义状态等同于对调用方说谎。
10. **真机探针**（`tools/verify/`，沿用 `*_live_probe.cpp` 命名，`AURORA_BUILD_VERIFY_TOOLS` 口径、`EXCLUDE_FROM_ALL`、不进 CTest）：导出一份 PDF 到临时目录并确认能被外部阅读器打开；注册一枚临时方案后系统查询承认它归属本应用、撤下后归属回到空；一枚临时扩展名在声明后可被查到、撤销后恢复原状；登录项启用后登记出现、撤销后消失，且机器上其它登录项一条都不被触碰。
11. **回归与门禁**：`utest_recent_documents` / `utest_platform` / `utest_preferences` 等既有套件零改动全绿；`--target aurora_api_json` 重建后 `check_api_schema_sync` 零漂移、`check_api_budget` 增量通过；`check_naming_conventions` / `check_doc_comments` / `check_platform_macros` / `check_core_layer_boundary` / `check_no_hardcoded_paths` / `check_codespec_xref` / `check_change_proposals` 全绿；`lint`（只跑改动相关 TU）与 `format-check` 绿；全量 `ctest -j` 绿。
12. **降级路径必须报错，不许静默成功**：在未接线的平台上调用四个家族的入口（开打印会话 / 注册方案 / 声明关联 / 写登录项），必须返回各自那枚 `X-unsupported`（kind `unsupported`）；机制在但这一回装不进去时返回对应的 `X-failed`（kind `io`）且 message 带明因。**变异自证**：把早退改成无条件返回成功值，相应用例必须转红。这一条是「能力位 + `Result` 双层口径」唯一能被机器证明的地方——不留这条判据，那套口径就只是文档里的一句说法。
13. **每个新增公共头都要有一枚以其为目标单元的测试**：五个新头各自被某个 `utest_*.cpp` 的头部三行块声明为「目标单元」。依据是 `check_code_doc_sync` 的 TEST-R5——公共单元头若不被任何测试声明或包含即记覆盖缺口，且该规则对**新增违规立即转红**（存量才按基线）。**变异自证**：删掉任一新头在测试侧的声明，门禁须转红而不是静默放过。
14. **空集合不是错误**：机器上没装任何打印机时 `print_targets()` 返回**空列表配 `Ok`**，而不是错误码——这个区别决定上层弹的是「没有可用打印机」还是「未知故障」。Linux 侧能力位为 `false` 的情形另行落入判据 12 的路径，两者不许互相顶替。
15. **每页必须不透明**：在内存侧逐像素断言——进入 PDF 写出器之前，每页位图的 alpha 恒为 255（即已合成到不透明底色）。**变异自证**：去掉合成那一步直接把原始 RGBA 喂给写出器，该用例必须转红。这条专治「透明区域印成黑块」，也让第 5 条那句「先合成到纸白」不是纸面承诺。
16. **实例键稳定性单测**：同一可执行路径的不同进程、命令行带不带 URI，派生出**同一个**实例键；路径需先规范化（大小写、分隔符、符号链接解析到同一实录路径）。**变异自证**：把 mtime 或命令行内容混进派生函数，该用例必须转红——那样每回都认不出自己，观感就是「永远当成首个实例」。

## 回写落点

- `codespec/specification/06-app-platform.md`（§5 能力位表增四枚位及其逐后端取值矩阵；新增文档输出 / 打印会话 / URL 方案 / 文件关联 / 登录项五节：入口契约、栅格入 PDF 的三条局限、自绘打印面板的立意、文件关联不抢默认的边界、登录项三态与 `Disabled` 语义、Linux 侧的能力缺位与未决项登记）
- `codespec/SPECIFICATIONS.md`（补四行需求条目：把这四项从「不承诺也不排除」推向「明示的能力 + 明示的边界」，需求行的缺位口径按本提案的实际落法写实）
- `codespec/BUILD_OPTIONS.md`（§2.4 登记新增真机探针；若最终采纳引入 CUPS，相关 CMake 变量一并在此登记）
- `codespec/manual-test/19-app.md`（**必改**：该文件 §1.1 写死「`app/` —— 应用与窗口聚合层（20 个公共头）」并逐名列了 20 个头名，本批后须改写为实际数目并补进五个新头名。附带一处存量漂移：该文档实为 26 个头文件，其中六个（event_source_registry、mime_types、native_menu_bar、notification、os_hotkey、recent_documents）已不在那份清单里，本批回写时一并纠正。另按 `check_manual_test_format` 的连续编号口径从 `TC-APP-015` 续排真机走查项：打印面板观感、点链接是否唤起已运行实例而非新开空窗、文件管理器「打开方式」里是否出现本应用、登录项开关后 OS 是否兑现——这四件的最终形态只能在真实桌面上判定，无头用例只能证明筋骨、不能证明观感）
- `codespec/specification/08-tooling.md`（§10.1 需求规格的 `Error` 字段清单同步；本目录前序提案在扩错误码时均有此回写）
- `codespec/ERROR_CATALOG.md`（生成物，随生成器重建、**不手改**；列出只为说明八枚新码会出现于该表，实施时以重建产物为准）

## 实施记录与偏离（落码期回写）

1. **落地面**：五个公共头按提案归位 `include/aurora/app/`（`document_output.h` / `url_scheme.h` / `file_association.h` / `run_at_login.h` / `single_instance.h`），伞头与 `tools/check/umbrella_manifest.txt` 同批登记。平台桥落三处：`src/aurora/app/detail/win32_shell_integration.cpp`、`linux_shell_integration.cpp`、`macos_shell_integration.cpp`（后两者按 `AURORA_PLATFORM_LINUX` / `AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS` 门控），门面经进程级后端单槽 `shell_integration_backend.{h,cpp}` 分族登记（URL 方案 / 文件关联 / 登录项 / 打印会话 / 单实例），无桥时各公共入口回落 `*-unsupported`。零平台头的折算层落 `shell_integration_translate.{h,cpp}`（方案文法与保留名、扩展名归一与 MIME 映射、URI 参数提取、Exec 引号化、桌面条目与 ini 编辑、分号清单去重）。最小 PDF 写出器落 `src/aurora/render/detail/pdf_writer.{h,cpp}`。`PlatformCapabilities` 四枚位与 `capabilities()` 的逐平台置位在 `include/aurora/window/platform.h`。
2. **判据 1（页面折算）兑现，dpi 覆盖档与提案不同**：`utest_document_output` 的 `content_raster_tracks_the_painter_readback` 断言 96 dpi（A4 内容区 184.6 mm 在 96 dpi 下不是整数 dp）与 300 dpi 两档，物理像素数逐位等于「dp × dpi / 96 再取整」；提案写的 600 dpi 未单列——它与 300 dpi 属同一等价类（非 1 倍缩放），而 96 dpi 档已覆盖「页尺寸不能用整数 dp 表达」这条真正的约束。物理像素取自 `Painter` 读回而非层内另算，实现上 `page_content_raster` 是唯一取整点。
3. **判据 2（PDF 结构）以结构断言 + 内存往返兑现，未落逐字节 golden；变异方向按实现口径重述**：`export_pdf_writes_a_well_formed_document` 断言头标识 / 版本 / 对象类型 / xref / trailer，`pdf_image_streams_round_trip_through_zlib` 解压每幅图像流并与提交像素逐字节比对（`AURORA_ENABLE_COMPRESSION` 开启时另用框架解压器复核同一份字节）。**偏离一**：没有 golden 文件——写出器带运行期时间无关但逐字节确定的形态，golden 的价值主要在被多份实现共享时，本批只有一份实现，故以结构 + 往返替代。**偏离二**：提案写「把压缩格式从 DEFLATE 换成 ZLIB，结构用例必须转红」，而实现选的正是 zlib 包装（PDF `FlateDecode` 的规范要求），按实现口径应重述为「剥掉 zlib 头尾、改成裸 DEFLATE，解压与结构用例必须转红」。
4. **未复用 `core/compression.h` 的编解码器**（提案影响面一行的写法随之失效）：该头的编码器产出通用 DEFLATE，而 PDF 的 `FlateDecode` 要求 RFC 1950 包装（两字节头 + Adler-32 尾）。为不把「加一层包装」散进写出器、也不为此新增公共 API，写出器自带 stored-block 编码器（`0x78 0x01` + 长度补码 + Adler-32），约百行且可被独立探针逐字节复算。代价是文件体积（不压缩），已在 §8.11 的三条局限里申报；公共面零新增。
5. **判据 3（无文本操作符）兑现**：`pdf_content_streams_carry_no_text_operators` 遍历内容流断言 `BT` / `ET` / `Tj` / `TJ` / `Tf` 零命中，真机探针另做一次独立复核——「栅格入 PDF」不是文档里的一句说法。
6. **判据 4（能力位矩阵）兑现，口径是**平台级**而不是逐 `SurfaceKind`（**偏离三**）：四枚位的入口是进程级自由函数，与上屏后端无关，故 `capabilities()` 按编译期平台宏整体置位（Windows 全 true；macOS 在 `AURORA_BACKEND_MACOS` 下全 true；Linux 除 `native_print_session` 外全 true）。`utest_document_output` 的 `desktop_shell_capability_bits_follow_the_wired_bridges` 锚定该矩阵，与 `runtime_visibility` 等逐 `SurfaceKind` 的位刻意不同；理由写进 [`06-app-platform.md`](../../specification/06-app-platform.md) §5。macOS「包内 / 裸可执行」的两形态在单测层无法区分（LS 只在真机可达），由 macOS 桥的 `require_bundle` 与探针的退出码 2 两条路承担，故没有落成两条独立单测（留白）。
7. **判据 5 / 6 / 8（文法、扩展名、桌面条目）兑现**：`utest_shell_integration_translate` 十一例覆盖方案文法（含 `v2+y` 通过、数字开头 / 下划线 / 冒号被拒）、保留名单（`mailto` 被拒且 detail 点明 reserved）、`.PNG` / `png` / `PNG` 折叠为同一键、空串与孤点被拒、`Hidden=` 翻转只改开关键与其余字段逐行不变、ini 编辑限段落内、分号清单去重保序。`utest_url_scheme` / `utest_file_association` 各另验「拒收发生在触达后端之前」。
8. **判据 7（登录项三态）兑现**：`utest_run_at_login` 的 `tri_state_is_not_collapsed_to_bool` 逐值断言 `Enabled` / `Disabled` / `Unsupported` 互不顶替；「用户手工禁用仍存在」由真机探针用独立通道（`StartupApproved` 禁用记录 / `Hidden=true`）造出并核对查询回 `Disabled`，单测层以伪后端传参锁定。Windows 桥关闭只删 `Run` 值、不碰 `StartupApproved`；Linux 只翻转 `Hidden=`、不删文件（与判据 8 同源）。
9. **判据 9（Linux 打印未决项）收口为不引入 libcups**：能力位如实 false，`print_targets()` 报 `print-session-unsupported`，替代路径是导出 PDF 后交默认处理程序；结论与理由写入 §8.12，探针在 Linux 上按「如实报 unsupported」取证而非跳过。`BUILD_OPTIONS.md` 因此没有新增 CUPS 相关变量。
10. **判据 10（真机探针）四枚全部落地**，并按各自平台的实测口径取证：`aurora_verify_document_output` 自带 xref 偏移核对、stored zlib 解析与 Adler-32 复算，解压字节数等于 `page_content_raster()` 读回像素数 × 3，`--interactive` 交人用阅读器逐页确认；`aurora_verify_url_scheme` 走文法闸、注册、归属、异主撤销拒绝、撤销回空、二次撤销幂等；`aurora_verify_file_association` 以**独立通道**读 `RegisteredApplications` / Capabilities / `[Added Associations]`，并逐字节比对声明前后的默认处理程序证据；`aurora_verify_run_at_login` 在写入前后对整棵 `Run` 键 / autostart 目录做快照比对并还原基线。**探针抓到一个真缺陷**：`AssocQueryStringW` 首调用回 `S_FALSE` 并写长度、二调用才回 `S_OK`，初版只认 `S_OK` 使「声明成功但库内查询恒失败」，已修（`S_FALSE` 且长度非 0 才判有值，零长度判无归属），URL 方案命令查询同批修正；回归见 `CHANGELOG.md` 的 Fixed 条。
11. **判据 11（回归与门禁）以本轮全量构建与 `ctest --preset ninja-test` 为准**（结果见提交记录；`aurora_api_json` 与 `check_api_schema_sync` / `check_api_budget` 同批复核）。既有逐 `SurfaceKind` 能力位用例与 `utest_recent_documents` / `utest_preferences` 等未改语义；新增测试共 `utest_document_output` 14 例、`utest_url_scheme` 6 例、`utest_single_instance` 10 例、`utest_file_association` 7 例、`utest_run_at_login` 5 例、`utest_shell_integration_translate` 11 例。
12. **判据 12（降级不静默）兑现**：四族各有 `degradation_reports_unsupported_without_a_wired_bridge` 一例（打印会话另有一例端口径），断言无后端时回 `*-unsupported` 而不是成功值；机制在但本次失败走 `*-failed`，message 带平台明因。能力位与 `Result` 两层口径不互相顶替——前者答「这台机器有没有机制」，后者答「这一回成没成」。
13. **判据 13（每头一枚目标单元测试）兑现**：五个新头各被对应 `utest_*.cpp` 的头部三行块声明为目标单元，`utest_shell_integration_translate` 不占目标单元（折算层不是公共单元头）。
14. **判据 14 / 15 / 16 兑现**：`print_targets_reports_an_empty_system_as_success` 钉住「空列表配 `Ok`」；`pdf_pages_are_composited_onto_the_page_background` 与 `print_session_composites_pages_before_submission` 逐像素断言 alpha 恒 255（去掉合成即转红）；`single_instance` 的 `instance_key_ignores_arguments_and_call_history` 断言同一路径的不同命令行派生同一键，`non_uri_arguments_are_not_queued` 与 `uri_extraction_skips_windows_drive_paths` 区分 URI 与盘符路径。
15. **macOS 侧的不对称与未测部分（留白）**：URL 方案与文档类型必须在包内 `Info.plist` 静态声明，运行期撤销如实回 `url-scheme-unsupported`；登录项要求 `SMAppService`（13+）、沙箱回 `Unsupported`、`RequiresApproval` 折成 `Disabled`；单实例在该平台恒回 `Primary`（唤起由 AppKit 应用级通道承担，命令行无第二进程可验）。四枚探针在 macOS 上未封装 `.app`（提案提到的 `<name>_probe_info.plist` 未随本批提供），裸可执行按退出码 2 报 ENV-UNAVAILABLE——**这三族的 macOS 真机兑现因此尚未取证**，记为后续补做项。
16. **Linux 与 macOS 两侧桥未在本机编译**：本机是 Windows，`linux_shell_integration.cpp` / `macos_shell_integration.cpp` 只经代码审查与同构对照，未参与本轮构建；其编译期与运行期行为待 Linux / macOS CI 或真机补证。这是本批最大的验证缺口，不折算成「已验收」。
17. **文档回写按实际落法改写**：`06-app-platform.md` §5 的能力位段与逐平台矩阵、§8.11–§8.15 五节；`SPECIFICATIONS.md` 的四枚需求行与特性计数（§5 实际 49 条，原表头写 43 已陈旧，同批改正）；`BUILD_OPTIONS.md` §2.4 的四枚探针登记；`manual-test/19-app.md` §1.1 的公共头计数由 20 改为**实际 31**，并补进提案点名的六个存量缺失头与五个新头，§1.8 补四个子域与七个头的覆盖取舍，新增 `TC-APP-015` 至 `TC-APP-018`；`08-tooling.md` §10.1 补 `Platform` 域「机制缺位 / 本次未落地」两分口径；`ERROR_CATALOG.md` 随生成器重建（本批 +8；rebase 到上游后全表 166）。
18. **收官门禁（本机 Windows / MinGW 树）**：改动面 21 个 TU 的 scoped `lint` 清零（`select_lint_tus --changed-only` 选中 21/792，`run_clang_tidy` 报 0 findings；修法按判因拆为字节边界 `NOLINT` 就地记因与算术 / 视图 / 循环形态改写，没有整文件排除）；`format-check` 1302/1302 合规；全量构建后 `ctest --preset ninja-test` 456/456 通过；四枚真机探针 auto 阶段全 PASS——PDF 结构 + 独立 stored-zlib 解压与 Adler-32 复算、方案注册与异主撤销拒绝、文件关联声明前后默认处理程序证据逐字节不变、登录项三态与整棵 `Run` / `StartupApproved` 基线还原。
19. **上游 rebase 后的编号与复验**：上游同批落了 `CHG-049`（macOS 运行期窗口显隐与置顶接线）与 `CHG-053`（队列值班），本提案落码期间编号与之撞车，故 rebase 后改号为 `CHG-054`（改动仅限标题与编号行）。冲突三处按「保留双方」处理：`platform.h` 保留上游 macOS 显隐 / 置顶翻位并追加四枚外壳位，`AuroraBackends.cmake` 合并 macOS 框架列表，`CHANGELOG.md` 与 `SPECIFICATIONS.md` 条目并存；生成物从合并后的 `errors.toml` 重新生成。复验：全量重编后 `format-check` 1302/1302、scoped lint 0 findings、`ctest --preset ninja-test` 456/456。
