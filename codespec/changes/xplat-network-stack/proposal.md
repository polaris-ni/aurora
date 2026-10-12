# CHG-010 跨平台网络栈（`aurora::net` 重构：事件驱动传输 + 可插拔引擎 + TLS/HTTP/WebSocket + 响应式绑定）

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-010 |
| 提出日期 | 2026-10-12 |
| 当前状态 | 实施中 |
| 关联需求 | SPEC.FEAT.NET.ENGINE-PLUGGABILITY.001 |
| 影响面 | 破坏性重做公共模块 `net/`（CHG-006 的同步 `StreamSocket` / `Listener` 面退位，见变更内容第 4 条）；新增异步传输引擎、DNS（含双栈与竞速）、代理族、本地 IPC、TLS、HTTP/1.1、SSE、WebSocket 公共 API，及 `Url` / `Headers` / `Framer` 三个共用基础件（前两者落 `core/`）；`Application` 事件循环新增网络完成事件通道与**全平台事件源注册口**（X11/Wayland 现有 DBus fd 通道收编为其消费者，Win32 / macOS / GLFW 等待集同步改造）；`state/` 新增 `Resource` 三态绑定类型；`tests/framework` 新增条件等待辅助与内置 `FakeEngine`；Inspector 路由由 `if` 链改注册表并新增 `network` 端点（配套 MCP 工具）；响应解压复用既有 wuffs gzip/zlib（无新增第三方依赖）；codespec/errors.toml 扩 `net-*` / `http-*` / `ws-*` 码组；codespec/BUILD_OPTIONS.md 新增「实现引擎」宏与 `AURORA_NET_TLS_PROVIDER` 缓存变量（**公共 API 面不加宏**）；codespec/CODING_STANDARDS.md §4.1 补凭据脱敏硬规则；InspectorServer 与 tools/servers/inspector_client.h 迁移为新传输层消费者；codespec/SPECIFICATIONS.md §5 登记 9 条 `SPEC.*.NET.*` 需求 |

## 动机

**框架现状：只有「本机能通一条 TCP」，没有任何网络架构。** `aurora::net`（CHG-006 落地）刻意把范围收敛为「IPv4 回环 + 同步阻塞 + 显式超时」，三个类型（`Session` / `StreamSocket` / `Listener`）不含非阻塞或事件驱动形态、不经 DNS、不接受非回环地址，UDP / UnixSocket / NamedPipe / TLS / HTTP / WebSocket 全部为零。库内唯一的网络消费者是 Inspector 工具链，其 HTTP 组帧散落在 tools/servers/inspector_client.h 里、由调用方自行拼接。

结果是：**面向使用者的网络能力完全缺失**，且缺口正在变成硬阻塞——

1. 基于本库的终端管理软件（aurora-view 仓库，产品名 Borealis）其核心场景就是连**任意远端主机**：SSH 走 libssh2、串口/原始 TCP 会话、远程文件传输。libssh2 需要「一条可收发的字节流 + 何时可读可写的就绪通知」，本库今天既给不了非回环连接，也给不了就绪事件源。
2. **图片与资源的远程加载没有实现方**：`include/aurora/widget/image_widget.h` 已经定下正确的解耦形态——库核心不内置 HTTP、由 App 注入 `ImageFetcher`（`from_file` 与 `from_url` 两条入口、进程级 `set_default_fetcher` 注入口都在），但**库里没有任何一个 `ImageFetcher` 实现**。同理，任何「拉配置 / 拉数据 / 推送流」的界面都没有承载体。本提案正是那个缺失的实现方，且接口位无需新造。
3. AI-first 定位要求运行时可观测、可交互（MCP / Inspector 后续要跨机器），而传输层被锁死在回环。

**业界对照（本次调研结论）**：网络引擎一律不进 UI 框架的渲染/组件核心——Qt 把 Qt Network 做成**独立模块但与核心事件循环紧耦合**（`QTcpSocket` 非阻塞 + `readyRead` 信号 → `QNetworkAccessManager` 协议栈，缓存/代理/DNS 内置）；Apple 是「底层连接抽象 `NWConnection` + 上层 `URLSession`，TLS/DNS/代理由系统代管」；Chromium 把网络做成**独立进程/服务**、经消息队列与 UI 通信；.NET / Android / Rust 系（egui、iced、Slint）则完全不内置，靠外挂 tokio/reqwest + channel 回投每帧。共同规律有三条：**(a)** 「底层 socket 抽象 + 上层协议引擎」两层独立演进是成熟共识；**(b)** API 形态已全面异步化（信号槽 → Future/Stream → 协程），同步网络 API 属被淘汰形态；**(c)** 声明式 UI 侧的通用模式是「异步结果写入响应式状态 → UI 是状态的纯函数」（`FutureBuilder` / Compose `StateFlow` / egui channel），即 `loading / data / error` 三态绑定。

本库已有的地基恰好能低成本承接这套模式：单线程帧循环 + 跨线程唤醒口（`Application::request_wake`）+ 主线程投递队列（`drain_posted`）、`Task<T>`（`then` / `cancel` / `with_timeout`）、协程面（`CoroTask` / `co_await au::co_async`）、`Result<T>` + 表驱动错误码。缺的是**事件驱动的传输层**与**协议层**，以及把结果**直连响应式状态**的绑定原语。

**已获用户裁决（本提案据此展开，不再复议）**：① 覆盖到全栈——事件驱动 socket、异步 TCP/UDP/本地 IPC、HTTP/1.1 + WebSocket、TLS；② **公共 API 面不加宏**（避免使用面受限），核心实现抽象成引擎接口，不同实现按平台加宏可选（如 WinHTTP 引擎、OpenSSL 引擎）；③ 上层 API 以协程为主 + 三态响应式绑定；④ TLS 采系统栈优先；⑤ CHG-006 的既有 socket 封装**允许破坏性修改**，设计无需迁就其形态。后续细化裁决（Linux TLS 形态、事件源落地深度、可选引擎取舍、绑定层默认值、代理族深度、charset 范围、Inspector 路由改造、脱敏强度、弹性件首批）与外部评审后的第 11 项改判，统一记在 §16（共十一项）。

## 变更内容

### 1 分层模型与依赖不变量

自底向上四层，**依赖单向向下**，任何一层可单独关闭实现而不改上层 API 面：

| 层 | 职责 | 命名空间 | 允许依赖 |
|:---|:---|:---|:---|
| L0 宿主集成 | 把网络完成事件送回 UI 线程；全平台通用事件源注册口 | `aurora::app`（扩展点） | `core/` |
| L1 传输引擎 | 非阻塞 socket、就绪通知、连接/收发/接受、DNS、本地 IPC | `aurora::net` | `core/`、L0 投递口 |
| L2 安全通道 | TLS 记录层/握手/证书校验，对上表现为「同构的字节流」 | `aurora::net` | L1 |
| L3 协议引擎 | HTTP/1.1、SSE、WebSocket 帧与语义 | `aurora::net::http`、`aurora::net` | L1、L2 |
| L4 声明式绑定 | 异步结果 → 可订阅三态状态，取消与作用域联动 | `aurora::state` | `state/` 现有异步原语 |

不变量：

- **UI 线程唯一契约不破**：所有面向使用者的完成回调、协程续体、`Resource` 写入一律发生在主线程（经既有 `Task<T>::set_main_poster` 安装的同一条投递器）；引擎线程绝不触碰 widget / `State`。
- **公共 API 面零平台分支**：消费者代码里不出现任何 `#ifdef`，能力差异经 `capabilities()` 运行期查询表达（与 `SPEC.PLATFORM.ZERO-IFDEF.001` 同构）。
- **失败一律 `Result<T>` + 表驱动 `Error`**，与 CHG-006 口径一致，不引入异常路径。
- 传输句柄不外露平台类型（延续 CHG-006：`std::intptr_t` 承载、`-1` 恒无效），pimpl 隔离重型平台头。

### 2 事件循环与线程模型（L0）

网络 I/O 的「等待就绪」不发生在 UI 线程。两条通道并存，前者是自带引擎的默认形态、后者是对外集成面：

- **引擎自持工作线程（默认）**：`aurora::net` 内置一条事件驱动线程（Win32 `WSAPoll` / POSIX `poll`，含到期定时器与任务队列），就绪事件打包成完成消息，经主线程投递器 + `request_wake` 唤醒帧循环。优点是自带引擎的行为**不依赖各平台主循环的等待形态差异**（Win32 消息泵、X11/Wayland `poll`、GLFW 事件等待、macOS run loop、wasm rAF 形态各异），跨平台一致，wasm 等无 fd 通道处天然可用；代价是每操作多一次跨线程投递（微秒级，与 `Task<T>` 同量级）。
- **通用事件源注册口（首批全平台落地）**：`app::EventSourceRegistry` 把「一个可等待句柄 + 就绪回调」并入各平台宿主等待集，供第三方非阻塞会话（libssh2、DBus 一类）与未来的 IOCP / `io_uring` 原生引擎复用；本库自带引擎同样以它为实现底座。逐平台并入方式：

| 宿主 | 等待集并入方式 |
|:---|:---|
| Win32 | `WSAEventSelect` 事件句柄并入 `MsgWaitForMultipleObjectsEx`，与既有 `PostMessageA` 唤醒共存 |
| X11 / Wayland | 并入 Surface 现有 `poll` 数组；DBus fd 通道（今天硬编码在两处等待里）收编为注册口的首例消费者 |
| macOS | `kevent` 或 dispatch source 挂入主 run loop |
| GLFW | 无自有等待集：由引擎线程等待 + `glfwPostEmptyEvent` 唤醒（与既有 `request_wake` 同路） |
| wasm | 无 fd 语义：`capabilities().event_source` 为 false，注册即返回 `net-platform-unavailable` |
| Headless | 自建 `poll` 循环，让事件源语义可在无 GUI 单测里逐值验证 |

> 风险注记（用户知情选择「全平台一次到位」）：Win32 与 macOS 的等待集改造是本提案最高风险面——消息泵 / run loop 的唤醒语义、句柄数上限（Win32 等待集 64 句柄）、注册与注销同帧竞态。落码须先以 Headless 与 X11/Wayland 建立行为基线，再逐平台推广，每平台各留一组回归。

停机与生命周期：引擎线程随首个需要它的对象惰性启动、进程退出或末个会话析构时收敛；`Application` 退出路径上排空已投递的完成事件，回调持有的宿主对象以 `weak`/取消令牌保护，避免「回调抵达时 widget 已析构」。

### 3 传输引擎抽象（L1）

公共 API 恒在，`NetEngine` 是实现侧接口（构建期装配，运行期可查询）：

```cpp
namespace aurora::net {

/// 能力声明：使用方据此降级，而非靠平台宏猜。
struct Capabilities {
    bool tcp;            // 任意主机 TCP
    bool udp;            // UDP 数据报
    bool local_ipc;      // UnixSocket / NamedPipe
    bool dns;            // 名称解析
    bool tls;            // TLS 通道（引擎是否可用）
    bool listens;        // 服务端监听（浏览器侧无此语义）
    bool event_source;   // 是否可并入宿主事件源
    bool streaming;      // 流式响应体（wasm 无 ReadableStream 时为 false）
    bool custom_headers; // 可自由设置非安全请求头（wasm fetch 受 CORS 预检约束）
};

class Engine {           // 抽象接口；实现：poll 线程引擎、平台原生引擎、wasm fetch 桥
  public:
    static auto instance() -> Engine &;
    [[nodiscard]] auto capabilities() const -> Capabilities;
    [[nodiscard]] auto name() const -> std::string_view;   // "poll" / "iocp" / "fetch"
};

struct Endpoint {        // 地址值类型：不经隐式转换，构造期完成字面量校验
    [[nodiscard]] static auto tcp(std::string_view host, std::uint16_t port) -> Result<Endpoint>;
    [[nodiscard]] static auto local(std::string_view path) -> Result<Endpoint>;   // UDS / \\.\pipe\
};

class Resolver {
  public:
    [[nodiscard]] static auto resolve(std::string_view host, std::uint16_t port)
        -> Task<Result<std::vector<Endpoint>>>;   // getaddrinfo 在引擎线程，结果回主线程
};

class StreamTransport {  // 面向字节的异步流：TCP / 本地 IPC / TLS 叠加后同构
  public:
    [[nodiscard]] static auto connect(const Endpoint &, const StreamOptions &)
        -> Task<Result<StreamTransport>>;              // 亦可 co_await
    auto read(std::span<char> buffer, ReadDone on_done) -> void;  // 完成恒回主线程
    auto write(std::span<const char> data, WriteDone on_done) -> void;
    [[nodiscard]] auto co_read(std::span<char>) -> CoRead;        // 协程挂起体：0 字节 = 对端 FIN
    auto set_high_water_mark(std::size_t bytes) -> void;          // 背压：写水位
    auto shutdown(ShutdownMode mode) -> void;                     // rd / wr / both
    [[nodiscard]] auto cancellation() -> Cancellation;            // 与 Task 取消令牌同族
};

class StreamListener {   // 服务端：任意/回环地址绑定，backlog 与访问控制由选项表达
  public:
    [[nodiscard]] static auto bind(const Endpoint &, int backlog) -> Task<Result<StreamListener>>;
    auto accept(AcceptDone on_ready) -> void;         // 事件驱动，不阻塞任何线程
    [[nodiscard]] auto local_port() const -> std::uint16_t;
};

class DatagramTransport;  // UDP：bind / send_to / receive_from，同 StreamTransport 回调与协程双形态

}  // namespace aurora::net
```

要点：连接、读、写、接受**四类操作都可取消**（`Cancellation`），取消后回调以 `net-cancelled` 抵达一次；`co_await` 形态的挂起点由引擎完成事件恢复（不是线程池轮询）；单次读的最大值与缓冲增长由 `StreamOptions` 表达，禁止无上限读入。双栈与竞速、代理族、原生选项逃生舱、本地 IPC 对端凭据、UDP 组播、分帧工具与读写双侧背压的补齐项见 §11。

### 4 CHG-006 同步面的处置（破坏性，经用户授权）

现有 `Session` / `StreamSocket` / `Listener` 三类型的处置：

- `Session`（Winsock 运行时 RAII）保留，降为 `detail` 内部件——引擎自身管理运行时生命周期，使用方不再需要感知。
- `StreamSocket` / `Listener` 的**同步阻塞公共面删除**；其能力收缩进 `aurora::net::blocking` 子命名空间并明确标注「仅供非 UI 线程的工具与测试宿主（InspectorServer、`tools/` 服务器、单测回环桩）」，避免阻塞语义再次渗进面向使用者的一侧。
- InspectorServer 与 tools/servers/inspector_client.h 迁移到新传输层；Inspector 的 REST 协议语义、`Connection: close`、4 MiB 上限、localhost 拒连口径逐值不变（CHG-006 的不变量继续有效）。
- 记 **MINOR + breaking 标注**：alpha 预览版允许破坏性变更，但必须给迁移路径（`SPECIFICATIONS.md` §12）——旧头保留一段 `@deprecated` 迁移说明期，`CHANGELOG.md` 写明替代类型与逐条对应关系。

### 5 TLS 通道（L2）

`TlsContext`（证书/ALPN/最低版本/校验开关）+ `TlsTransport`：握手是一个可 `co_await` 的 `Task<Result<TlsTransport>>`，成功后与 L1 流**完全同构**——L3 协议引擎因此不需要知道底下是否加密。

系统栈引擎按平台默认装配（用户裁决），Linux 无系统栈、由使用方以 **provider 缓存变量传入本地安装的库**：

| 平台 | 默认后端 | 宏 | provider 覆盖 |
|:---|:---|:---|:---|
| Windows | Schannel（系统，零依赖） | `AURORA_NET_TLS_SCHANNEL` | 可覆盖 |
| macOS | SecureTransport（系统） | `AURORA_NET_TLS_SECURE_TRANSPORT` | 可覆盖 |
| Linux（X11 / Wayland / GLFW 宿主） | **无默认**：未给 provider 即 `capabilities().tls` 为 false | `AURORA_NET_TLS_OPENSSL` / `AURORA_NET_TLS_MBEDTLS`（由探测择一） | **必选项** |
| wasm | 无原生 socket，TLS 由浏览器 `fetch` 代管 | — | 不适用 |

provider 语义（用户裁决的传参形态）：

```
-D AURORA_NET_TLS_PROVIDER=static:/opt/mbedtls    # 静态链：取 <prefix>/lib/*.a
-D AURORA_NET_TLS_PROVIDER=shared:/usr/local      # 动态链：取 <prefix>/lib/*.so|dylib|dll
```

- 值形如 `<linkage>:<prefix>`：`linkage` 取 `static` / `shared`，`prefix` 是约定 `include/` + `lib/` 布局的安装根。
- **后端种类由探测判定**：`<prefix>/include/openssl/ssl.h` 存在按 OpenSSL 语义编译、`<prefix>/include/mbedtls/ssl.h` 存在按 mbedTLS 编译，两者皆无则 configure 期硬失败并列出期望的头路径（不静默降级为「无 TLS」），多命中时 OpenSSL 优先并在配置日志显式记录；宏 `AURORA_NET_TLS_OPENSSL` / `AURORA_NET_TLS_MBEDTLS` 由此推导、不手工设置。
- 探测通过后生成导入目标（`aurora::net::tls`），`shared` 形态的运行期寻址交宿主部署负责，不引入 RPATH 魔法。
- `prefix` **只接受安装根**（形如 `/opt/mbedtls`、`/usr/local`），不做向上猜测或递归搜索：写错路径时诊断会打印实际检查过的 `include/` 与 `lib/` 组合，避免「路径语义太宽 → 链接到意外版本」这类难查故障。
- 仓库**不自带任何 TLS 源码**：断网可构建不变，许可证由 provider 提供方自负（不触发 `THIRD_PARTY_LICENSES.md` 变更）；CI `toggles` 矩阵新增两 job——Linux `static:` 指向 CI 预置 mbedTLS、Windows `shared:` 指向 vcpkg 布局 OpenSSL，保证两套适配都有真编译覆盖。

证书校验**默认开启且失败即断连**；关闭校验只能经显式命名的危险开关（`allow_insecure_*`），并在文档与错误提示里标注用途限于本地自签测试。

### 6 HTTP/1.1 客户端与 WebSocket（L3）

`namespace aurora::net::http`：

```cpp
struct Request {                      // 方法 / URL / 头 / 体 / 超时 / 重定向上限 / 进度回调
    [[nodiscard]] static auto get(std::string_view url) -> Request;   // 五方法同名工厂
    auto timeout(std::chrono::milliseconds) -> Request &;
    auto header(std::string_view name, std::string_view value) -> Request &;
};

struct Response {                     // status / headers / body（惰性或已物化）
    [[nodiscard]] auto status() const -> std::uint16_t;
    [[nodiscard]] auto header(std::string_view name) const -> std::optional<std::string_view>;
    [[nodiscard]] auto text() -> Result<std::string>;        // 物化正文（受 max_body_bytes 约束）
    [[nodiscard]] auto bytes() -> Result<std::vector<char>>;
};

class Client {
  public:
    [[nodiscard]] auto send(const Request &) -> Task<Result<Response>>;   // 协程友好
    [[nodiscard]] auto open_stream(const Request&, DataSink &) -> Task<Result<Response>>;
        // 流式：分块解码后逐段回调 Sink，支撑大文件不占内存 + 上传/下载进度
    [[nodiscard]] auto open_sse(const Request&) -> Task<Result<EventStream>>;
        // text/event-stream 解析：event/data/id/retry → 回调，含自动重连
};

class Session {                       // 会话面：Cookie  Jar（内存）+ keep-alive 连接池
  public:
    [[nodiscard]] auto client() -> Client &;
};

class WebSocket {                     // RFC 6455 客户端：握手/分帧/ping-pong/close code
  public:
    [[nodiscard]] static auto connect(std::string_view url, const WebSocketOptions &)
        -> Task<Result<WebSocket>>;
    auto send(std::string_view text, MsgDone on_done) -> void;
    auto send(std::span<const char> binary, MsgDone on_done) -> void;
    auto set_on_message(MessageHandler) -> void;   // 恒回主线程
    auto close(std::uint16_t code, std::string_view reason) -> void;
};
```

**协议语义由框架统一定义**（引擎化裁决）：默认实现是自研 `SocketHttpEngine`（跑在 L1/L2 上，跨平台逐值一致、可黄金文件验证）；wasm 侧因无原生 socket，唯一路径是浏览器 `fetch` 引擎。WinHTTP / NSURLSession 平台引擎经裁决**首批不做**——避免出现第二套协议语义与相应的平台差异测试矩阵，接口位保留在 `Engine` 抽象内，后续按系统代理/系统缓存联动需求再评估。JSON 载荷的读写复用自研 `au::json`（`SPEC.FEAT.CORE.JSON.001`），不引入第二个 JSON 依赖。

范围取舍（用户已确认的能力边界）：**含**基础请求集（五方法、自定义头、超时、重定向、状态码/体）、流式与进度、Cookie 与内存缓存、SSE、WebSocket；**不含** HTTP/2 与 HTTP/3、磁盘缓存、HTTP 服务端框架（Inspector 自用面除外）、mDNS、代理认证交互 UI。URL 值类型、gzip 透明解压、条件请求、分段超时、认证与 mTLS、multipart/Range、错误分型、进度节流与 WebSocket 细则的补齐项见 §12（二轮审计结论：本节原列举只是骨架，缺这些即语义不完整）。

### 7 声明式绑定（L4）

网络结果直连响应式状态，让「请求」在 widget 侧退化成一个状态来源：

```cpp
namespace aurora::state {
template <typename T>
class Resource {                         // 三态：Loading / Ready(T) / Failed(Error)
  public:
    [[nodiscard]] static auto from(Task<Result<T>> job) -> Resource<T>;
    [[nodiscard]] auto phase() const -> ResourcePhase;     // 可订阅
    [[nodiscard]] auto value() const -> const T *;         // 未就绪为 nullptr
    [[nodiscard]] auto error() const -> std::optional<Error>;
    auto reload() -> void;               // 重发：自动取消上一次未完成的请求
};
}  // namespace aurora::state
```

- 落点在 `state/`（include/aurora/state/resource.h，待建）而非 `net/`：它只依赖「可取消 + 完成回投」这一既有抽象（与 `Task<T>` 同形），因此**网络不进状态层的依赖图**，异步计算、文件读取同样能产出 `Resource`。
- 订阅语义与 `State<T>` 一致（细粒度定点刷新），widget 侧写法即 `loading / data / error` 三分支，对位业界 `FutureBuilder`。
- 持续数据流（SSE / WebSocket）用同族的 `StreamResource<T>`（追加/替换策略由选项表达），避免把「流」硬塞进一次性 `Resource`。
- 与作用域联动：宿主 widget 卸载或同一 `Resource` 被重新赋值时，前序请求经 `Cancellation` 取消，回调抵达时若宿主已失效则丢弃（不产生悬垂访问）。
- 默认值（经裁决，随 `SPEC.API.SENSIBLE-DEFAULTS.001` 在声明处可见）：`from()` 未显式给超时即 **30 s**；`reload()` **不去抖**——每次调用都真发并取消前序，需要去抖者显式开 `debounce(window)` 选项。

### 8 实现宏矩阵与构建面

**公共 API 恒在**（用户裁决的核心），宏只影响「哪个引擎被编进来」：

| 宏 | 默认 | 语义 |
|:---|:---|:---|
| `AURORA_ENABLE_NET` | `ON` | 网络栈编入（关掉后 API 面仍在、调用即返回「引擎不可用」错误，供极简体裁剪） |
| `AURORA_NET_ENGINE_POLL` | `ON` | 内置事件驱动线程引擎（跨平台基线实现） |
| `AURORA_NET_EVENT_SOURCE` | `ON` | 通用事件源注册口，首批覆盖 Win32 / X11 / Wayland / macOS / GLFW / Headless 全平台 |
| `AURORA_NET_TLS_SCHANNEL` / `AURORA_NET_TLS_SECURE_TRANSPORT` | 各自平台 `ON` | 系统栈 TLS 引擎 |
| `AURORA_NET_TLS_OPENSSL` / `AURORA_NET_TLS_MBEDTLS` | 由探测推导 | provider 库适配引擎，**不手工设置** |
| `AURORA_NET_HTTP_FETCH` | wasm `ON` | 浏览器 `fetch` 引擎（受 CORS 与无监听语义约束，是 wasm 的唯一 HTTP 路径） |

配套缓存变量（非宏）：`AURORA_NET_TLS_PROVIDER=<static|shared>:<prefix>`，语义与探测规则见 §5。

落码需同步：声明处 cmake 模块、`aurora_define_feature` 登记、codespec/BUILD_OPTIONS.md §4 清单行、运行期可查则进 include/aurora/debug/feature_flags.h。Windows 链接面 `ws2_32` 维持 CHG-006 口径（核心 `PUBLIC`）。第三方依赖原则（经裁决）：**优先系统栈、零 vendored 新库、不自带 TLS 源码**，Linux 由 provider 指向使用方本地安装，仓库 `third_party/` 无新增、`THIRD_PARTY_LICENSES.md` 不变、「断网可构建」不变。

### 9 跨平台能力矩阵（`capabilities()` 的运行期真值表）

| 能力 | Win32 | X11 | Wayland | macOS | GLFW | wasm | Headless |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TCP 客户端 / 服务端 | 是 | 是 | 是 | 是 | 是 | 否 | 是（回环） |
| UDP | 是 | 是 | 是 | 是 | 是 | 否 | 是（回环） |
| 本地 IPC | NamedPipe | UDS | UDS | UDS | 同宿主 | 否 | 同宿主 |
| DNS | 系统 | 系统 | 系统 | 系统 | 系统 | 否 | 系统 |
| TLS | Schannel | provider 或无 | provider 或无 | SecureTransport | provider 或无 | `fetch` 代管 | provider 或无 |
| HTTP/1.1 | 自研栈 | 自研栈 | 自研栈 | 自研栈 | 自研栈 | `fetch` | 自研栈 |
| WebSocket | 自研栈 | 自研栈 | 自研栈 | 自研栈 | 自研栈 | 否（首批不做） | 自研栈 |
| 事件源注册口 | 是 | 是 | 是 | 是 | 是（经引擎线程桥） | 否 | 是 |

wasm 上不可用的能力，构造即返回 `net-platform-unavailable`（携明确 hint 指向本矩阵），**不提供假的降级**。wasm 的 HTTP 细分限制（流式体与自定义头两枚能力位、代理为空操作）见 §18 第 6 项。

### 10 错误码规划

CHG-006 的十枚 `net-*` slug 是冻结对外契约，只增不改。新增组（category 沿用 `io`，`gen_error_codes` 刷新生成物与 codespec/ERROR_CATALOG.md）：

- `net-dns-failed` / `net-cancelled` / `net-platform-unavailable` / `net-buffer-limit` / `net-option-failed` / `net-option-conflict`
- `net-proxy-unsupported` / `net-proxy-unreachable` / `net-proxy-auth-required` / `net-policy-violated`
- `net-tls-handshake-failed` / `net-tls-cert-rejected` / `net-tls-version-mismatch` / `net-tls-engine-unavailable` / `net-tls-pin-mismatch`
- `net-url-invalid` / `net-framer-overflow`
- `http-request-invalid` / `http-timeout` / `http-redirect-limit` / `http-body-too-large` / `http-protocol-error` / `http-charset-unsupported` / `http-content-encoding-unsupported`
- `ws-handshake-rejected` / `ws-protocol-error` / `ws-closed` / `ws-send-queue-full`

每码在 `codespec/errors.toml` 里如实标注 `retryable`（供 `HttpErrorClass` 与 `RetryPolicy` 消费，不新造判定机制）。

### 11 §3 的补充：地址策略、代理与传输选项（二轮审计补齐）

- **双栈与连接竞速**：`Resolver` 同查 A / AAAA，`Endpoint` 支持 IPv6 字面量（含 zone id）；连接按 Happy Eyeballs（RFC 8305 形态，默认 200 ms 交错、可关）竞速，一族失败不阻塞另一族。今天的 IPv4 假设硬编码在 src/aurora/net/socket.cpp 的 `AF_INET` 构造点，须整体改造为地址族无关，`is_loopback_peer()` 同步扩到 `::1`。
- **代理族（经裁决首批全量纳入，原始 TCP 与 HTTP 共用一条实现）**：
  - `ProxyConfig`：手工 HTTP/1.1 `CONNECT`（含 `Proxy-Authorization` Basic）、SOCKS5（含用户名密码）、显式关闭。
  - 环境变量 `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY` 默认读取（支持端口、认证、`NO_PROXY` 的后缀与 CIDR 例外），可整体关闭。
  - 系统代理读取：Win32 走 WinHTTP 查询、macOS 走系统配置、Linux 交回环境变量——**只取地址与例外，不执行 PAC**（PAC 仍在非目标）。
  - 代理不可达时**不静默回退直连**，报 `net-proxy-unreachable`（静默回退是企业网里最难排查的一类故障）。
- **原生选项逃生舱**：`NativeOptions`（`TCP_NODELAY`、`SO_RCVBUF` / `SO_SNDBUF`、`SO_REUSEADDR`、`SO_LINGER`、TCP keepalive 三相、绑定网卡 / 源地址）+ `native_handle()` 交出原始描述符供第三方接管（libssh2 一类）。原则是「描述符级逃生舱 + 少量高频选项」，不封装全选项矩阵。
- **本地 IPC 对端凭据**：UDS 经 `SO_PEERCRED`（Linux）/ `LOCAL_PEERCRED`（macOS）回报对端 uid / pid，Windows NamedPipe 明确回报匿名语义。本机服务的访问控制由此从「只看回环地址」升级为「看对端身份」。
- **UDP 补齐**：connect 模式（默认过滤非对端数据报）、组播加入 / 离开与回环开关、广播允许位。既然已提供 UDP，这三项成本极低而设备发现场景常用。
- **分帧工具 `net::Framer`**：按行（含长度上限拒收）、按分隔符、按长度前缀（字节序 + 最大帧）三型，把 `StreamTransport` 的字节读事件产出为「一条完整消息」；SSE 行解析、本地 IPC 协议、终端远端协议三处共用同一实现。
- **读写双侧背压**：`StreamOptions::max_pending_bytes` + `pause_read()` / `resume_read()`，与写侧高水位配对，契约明确为**拉模型**——`Framer` 与 `DataSink` 的回调返回值决定是否继续投递，慢消费者绝不导致无界缓冲（对齐 .NET Pipelines 的 pause / resume 阈值形态）。

### 12 §6 的补充：HTTP 语义完整性（二轮审计补齐）

- **`Url` 值类型（新增，全栈共用）**：解析与构造、相对解析、scheme / host / port / path / query / fragment 访问、查询参数多值且保序、百分号编解码（区分保留字符集）、IDNA punycode、IPv6 字面量、`is_secure()`。今天全仓没有任何 URL 工具（`core/string_util.h` 只有格式化），而手拼 URL 恰是 AI 生成代码的头号错误源——`Request` 与 `WebSocket::connect` 一律收 `Url`，字符串只能经 `Url::parse` 显式进入。
- **大小写不敏感头表 `Headers`**：ASCII 大小写不敏感存取、重复头保序、`Set-Cookie` 严禁折叠为一条；落 `include/aurora/core/` 供 Inspector 与 HTTP 共用（今天只有散落的临时 `tolower` 实现）。
- **内容编码**：响应侧 `gzip` / `deflate` 透明解压，**复用仓内已随 PNG 解码编入的 wuffs `std/gzip` / `std/zlib` 解压器**（零新增依赖）；`Accept-Encoding` 因此只声明 `gzip, deflate`，`br` / `zstd` 因需 vendored 新库列非目标；请求侧压缩不做。
- **charset 解码**：`Response::text()` 按 `Content-Type` 的 `charset` 解码，首批 UTF-8（剥 BOM）与 Latin-1，其它 charset 报 `http-charset-unsupported` 并保留接口位；`text()` 之外恒可取原始字节，绝不静默替换非法字节序列。
- **条件请求与缓存校验**（把「内存缓存」补成语义完整的 RFC 9111 最小集）：`Cache-Control` 新鲜度、`ETag` / `If-None-Match`、`Last-Modified` / `If-Modified-Since`、`Vary` 按头维度分键、`no-store` 强制 bypass、`must-revalidate` 遵守；命中 304 后对外表现为「同一 URL 二次请求不触网」，`Request::no_cache()` 可越。
- **分段超时与连接池治理**：`Timeouts{connect, read_idle, total}` 三档（单一 `timeout` 无法分别表达「连不上」与「Body 慢」）、keep-alive 空闲回收、每 host 并发上限、连接最大寿命（防 DNS 漂移后仍连旧 IP）、`Retry-After` 尊重。
- **上传与下载便捷面**：`MultipartWriter`（字段 + 文件部件，`filename` 与部件 `Content-Type` 由内置最小扩展名表推断）、`Request::range()` 与 206 语义、断点续传（`If-Range` + 偏移由调用方持有）、`Expect: 100-continue` 可选开。
- **认证**：`Authorization` 直填 + `Bearer` / `Basic` 便捷构造；`Authenticator` 回调处理 401 / 407 挑战（一次注入机会，失败即上抛，不无限重试）；Digest / NTLM 列非目标；mTLS 客户端证书与自定义信任根（企业 CA / 自签链）经 `TlsContext` 提供，走同一 provider 引擎面。
- **错误分型**：`HttpErrorClass`（`dns` / `unreachable` / `proxy` / `tls` / `timeout` / `protocol` / `status` / `cancelled`）+ 每码 `retryable`（沿用 codespec/errors.toml 既有 `retryable` 字段，不新造机制），让三态绑定能直接判「该不该给用户一个重试按钮」。
- **重试策略（首批实现，默认关闭的声明式可选件）**：`RetryPolicy{attempts, base, jitter}` 仅在**幂等白名单**方法上生效，指数退避 + 抖动 + 尊重 `Retry-After` 上限。
- **进度节流**：上传 / 下载进度**在源头合并到每帧至多一次**（对齐帧循环），与 Qt `downloadProgress`、dio `onReceiveProgress` 的按 chunk 发射形态相反——响应式绑定下每包写状态会放大成重绘风暴，这是本库作为响应式框架的必要约束。
- **链路标识**：`X-Request-Id` 自动生成（可关）并同时写入日志与请求时间线，使「一次界面异常」可反查「一次请求」。
- **WebSocket 细则**：子协议协商（选择结果回报）、心跳间隔与 pong 超时、发送队列上限与溢出策略（超限报 `ws-send-queue-full`，由调用方决定是否关闭）、单帧与总帧大小上限；`permessage-deflate` 因需额外依赖列非目标。
- **SSE 续传**：自动重连携带 `Last-Event-ID`，`retry:` 字段驱动重连窗口。
- **与既有注入点对接（零 widget 改造）**：本栈提供 `ImageFetcher` 默认实现——`ImageView::from_url` 已按「显式实参 > 进程级默认 > `Environment` 注入」三级解析、未注入时优雅停在占位态，应用侧只需一次 `set_default_image_fetcher` 即获得远程取图，widget 侧无新增 API。

### 13 可观测性与安全策略（本库差异化落点）

网络是最难事后排查的一类 I/O，而本库的立身之处正是「AI 可观测运行时」（`SPEC.FEAT.TOOLING.UI-INSPECTOR.001`），因此本节不是附加项而是核心交付面之一：

- **请求时间线**：每请求记录 DNS / connect / TLS 握手 / 首字节 / Body 完成 / 总耗时与收发字节数（OkHttp `EventListener` 与 W3C `PerformanceResourceTiming` 的形态）；关闭时零开销（数据成员不占位）。
- **活动表**：进程内 in-flight 与最近 N 条已完成请求的环形表（method、host、状态、错误码、耗时、字节数），经 `aurora::debug` 门面 JSON 导出，沿用「薄封装、子系统留原地、Release 裁切」的既有门面模式。
- **Inspector 与 MCP 暴露**：新增 `network` 端点与「列活动 / 看详情」两枚 MCP 工具。**前置改造（经裁决并入本提案）**：Inspector 路由今天是 src/aurora/inspector/inspector_server.cpp 里 `route_request` 的 `if` 链、无注册表，须先做路由注册表化才能无侵入挂端点。匹配次序是这次改造的唯一真风险点：现链中**精确匹配与前缀匹配并存**（`route == "/api/debug/state"` 一类 与 `route.starts_with("/api/widget/")`、`starts_with("/api/input/")` 两类交错），注册表须保持「精确表优先、前缀表按登记次序兜后」的两级查找，否则会悄悄改写既有路径的分派。约束：REST 行为逐值不变（沿用 CHG-006 口径），并以既有回环夹具（tests/unit/utest_inspector_server.cpp 的 `http_roundtrip` 一族）为逐端点回归底座。
- **凭据脱敏纪律**：`Authorization`、`Proxy-Authorization`、`Cookie`、`Set-Cookie` 在任何日志、时间线、Inspector 响应与错误 `message` 中一律打码（只留认证方案与长度）。今天 `AURORA_LOG_*` 只有级别过滤与 sink 重定向、**无任何脱敏机制**，codespec/CODING_STANDARDS.md §4.1 也只规定双通道与禁用 `cout` / `printf`，对凭据入日志无约束——本提案补一条硬规则，并在同章「反例速查」加一条反例。执行强度（经裁决）＝**条款 + 四出口单测断言 + lint 规则**，不新增 `check_*` 门禁脚本。
- **声明式安全策略（首批实现，默认保守）**：`NetPolicy{ allow_cleartext_hosts, min_tls_version, require_proxy }`（iOS ATS / Android `network_security_config` 的形态），默认拒绝非白名单明文、TLS 下限 1.2，违规报 `net-policy-violated`。
- **证书固定（经评审改期：与 TLS 同批实现、默认不启用）**：SPKI pin（SHA-256）作为 `TlsContext` 选项、失败报 `net-tls-pin-mismatch`。原裁决把它后置，理由是「策略件而非连通性件」——评审指出这对连任意远端主机的终端管理场景不成立，据此修正：**校验代码与错误码随 TLS 切片同批编入**，但默认关闭、只经 `NetPolicy{ pin_spki }` 显式开启，且在系统栈 / OpenSSL / mbedTLS 三套适配全部跑通一致性测试之前不写进推荐用法。两条分工要说清：企业自签 CA 场景由首批的**自定义信任根**（`TlsContext::trust_root`）覆盖，pinning 解决的是「信任根被污染 / CA 误签」，其真正的落地难点在密钥轮换运维而非校验实现——故默认关、可开、不推荐，是既不阻塞也不失守的位置。
- **凭据不进框架**：`aurora::preferences` 今天落在单个明文 JSON 文件、无安全层，故**不提供 token 保险箱**；凭据由宿主注入，框架只保证不落日志（见上条）。

### 14 测试替身与离线验证

- **`FakeEngine` 内置**：`Engine` 抽象已把传输藏在接口后，据此提供脚本化引擎（预设收发序列 + 可编程故障：慢连接、半关闭、乱序、TLS 失败、超大帧、代理不可达），使 HTTP / WS / 重试 / 超时 / 取消 / 背压全部可在 CI 内断言，**零真网访问**（对齐 OkHttp `MockWebServer`、dio `MockAdapter`、MSW 的业界标准做法）。
- **管道 hook**：`Client::use(hook)` 提供「改请求 / 看响应 / 短路返回」三能力，供认证注入、日志与用例断言。刻意**不照抄** OkHttp `Chain` 的全链可替换语义（那等于把协议正确性交给第三方），拦截点只做观察与最小改写。
- **异步测试辅助**：测试框架现无 `spin_until` 一类条件等待辅助（用例内靠手写 thread + cv），随本提案补进 tests/framework，让所有网络用例共用同一等待与超时口径。
- **能力位口径**：`Capabilities` 为运行期结构，与既有编译期 `AURORA_CAP_*` 宏并存且不重叠——编译期可求值的位留在 include/aurora/core/platform.h，运行期引擎能力只经 `Engine::capabilities()` 报告，避免两套真相。

### 15 非目标（明确不做）

不做 HTTP 服务端框架（Inspector 自用面除外）；不做 HTTP/2、HTTP/3、QUIC；不做磁盘持久缓存与离线策略；不做代理自动发现（PAC）与代理认证交互 UI；不做网络状态监听与自动重连编排（`RetryPolicy` 只覆盖单请求重试，链路级重连留给使用方与 `Scheduler` 组合）；不做跨进程网络服务（与 `SPECIFICATIONS.md` §4.1「不包含：跨进程」一致）；首批不做 WinHTTP / NSURLSession 平台 HTTP 引擎、不做 wasm WebSocket 桥接（见 §16 裁决记录）；不做 `br` / `zstd` 解压、请求体压缩、`permessage-deflate`（皆需 vendored 新依赖）；不做 GBK / Big5 等 legacy charset 解码；不做 Digest / NTLM / Negotiate 认证栈；不做凭据安全存储；不做全量 socket 选项封装与拦截器全链语义；不 vendored 任何 TLS 源码；不引入 GPU / 线程池语义变更。

### 16 裁决记录（十一项，全部定案）

评审应答的逐条定案见 §18（其中第 8 项改判了本节第 10 条关于证书 pinning 的取舍），预算与迁移时间表见 §19、§20。

**已定案（2026-10-12，落码不再复议）**：

1. **Linux TLS 后端 = 使用方 provider**：`AURORA_NET_TLS_PROVIDER=<static|shared>:<prefix>`，后端种类由头文件探测推导、探测失败即 configure 期报错；Win/macOS 仍默认系统栈且可被 provider 覆盖。仓库零 vendored TLS、`THIRD_PARTY_LICENSES.md` 不变（细则见 §5）。
2. **事件源注册口 = 首批全平台一次到位**（Win32 / X11 / Wayland / macOS / GLFW / Headless），按 §2 风险注记先立 Headless 与 X11/Wayland 基线、再逐平台推广。
3. **wasm WebSocket = 首批不做**：能力矩阵标「否」，调用返回 `net-platform-unavailable`，不做 embind 桥接。
4. **平台 HTTP 引擎（WinHTTP / NSURLSession）= 首批不做**：只出自研栈 + wasm `fetch`，接口位保留在 `Engine` 抽象内。
5. **`Resource` 默认值 = 30 s 超时、`reload()` 不去抖**：去抖须显式 `debounce(window)`；默认值在声明处可见（`SPEC.API.SENSIBLE-DEFAULTS.001`）。

**二轮审计后的追加定案（同日，全部落码不再复议）**：

6. **代理族 = 首批全量纳入**：`CONNECT`（含代理认证）+ SOCKS5（含用户名密码）+ 环境变量 + 系统代理读取一次做完（终端管理软件跨企业网是刚需场景）；仅 PAC 不做。
7. **charset = UTF-8 + Latin-1**：其余报 `http-charset-unsupported`，不引编码映射表、不 vendored 转换库；原始字节恒可取得，遗留站点由应用侧转码。
8. **Inspector 路由注册表化 = 并入本提案**：作为可观测切片的前置子切片，同批解掉「每加一端点必改巨函数」的痛点；REST 行为逐值不变的约束沿用 CHG-006 口径。
9. **脱敏执行强度 = 条款 + 单测 + lint**：不新增 `check_*` 门禁脚本（避免多一个需维护豁免的扫描器）；四出口断言集即为回归防线。
10. **`RetryPolicy` 与 `NetPolicy` = 首批实现**，前者默认关闭、后者默认保守（拒非白名单明文 + TLS 1.2 下限）；本条原含「证书 pinning 后置」，由第 11 项取代。
11. **SPKI 证书 pinning = 与 TLS 同批实现、默认关闭、经 `NetPolicy` 显式开启**（外部评审推翻第 10 条后半的后置结论后改判；理由与「自定义信任根 vs pinning」的分工见 §13、§18 第 8 项）。

### 17 落地切片（按依赖顺序，可独立评审与验收）

底座（引擎线程 + 完成投递 + 全平台事件源注册口 + `Endpoint` / `Capabilities` / `Cancellation`）→ 双栈与竞速 + 代理族全量（`CONNECT` / SOCKS5 / 环境变量 / 系统代理读取，含 `Resolver` 与 IPv4 硬编码改造）→ 传输族（TCP / UDP / 本地 IPC / `Framer` / 背压，含 `blocking` 面迁移与 Inspector 改造）→ `Url` 与 `Headers` 基础件 → 绑定层（`Resource` / `StreamResource`，可先只接 `Task`）→ `FakeEngine` 与异步测试辅助 → TLS（provider 探测 + 两套适配 + 系统栈 + mTLS / 信任根）→ HTTP（流式 / 进度节流 / gzip / 条件请求 / 分段超时 / 认证 / multipart / `RetryPolicy` / `NetPolicy`）→ 可观测性（前置 Inspector 路由注册表化 → 时间线 + 活动表 + `network` 端点 + MCP 工具 + 脱敏条款与 lint 规则）→ SSE → WebSocket。每个切片自带单测与文档回写，不跨切片留红灯；平台 HTTP 引擎与 wasm WebSocket 桥接不列本提案切片（见 §16 裁决记录），SPKI 证书 pinning 按 §13 改期结论随 TLS 切片同批实现、默认关闭。

**实施进度**：底座切片已落**值类型部分**——`net::Endpoint` / `AddressFamily`（仓内手写的 IPv4 / IPv6 / zone id / 本机 IPC 路径解析与 RFC 5952 规范化，含 §18 定案的全部校验判据）、`net::Cancellation`（含「钩子在调用线程同步执行」与「取消先于登记立即触发」两条契约）、错误码 `net-endpoint-invalid` / `net-cancelled`，配套 `utest_net_address` + `utest_net_cancellation` 与 codespec 回写（`06-app-platform.md` §8.8.1、`ARCHITECTURE.md` §4、`CHANGELOG.md`）。底座切片再落**事件源注册口的平台无关核心**——`app::EventSourceRegistry` + move-only RAII 句柄 `EventSourceRegistration`（§18-1 三条竞态契约：析构注销幂等 / 移动不双重注销 / 尾批抵达一次 vs `cancel_pending()` 作废尾批；Poller 与 Dispatcher 两枚接缝令 app 层不依赖 net、不触碰 socket.cpp 的「平台 socket 头唯一落点」不变量），错误码 `net-platform-unavailable`，配套 `utest_app_event_source_registry` 与回写（`06-app-platform.md` §8.8.2、`ARCHITECTURE.md` §4、`CHANGELOG.md`）。底座切片又落**事件驱动就绪基元**——`net::wait_ready`（`net/poll.h`，实现仍落 `socket.cpp`：POSIX `poll` / Winsock `WSAPoll` 的多句柄四向等待，纯超时非错误、无效句柄不毁整批、`EINTR` 同预算重武装），同步面补 `StreamSocket` / `Listener` 的 `native_handle()` 借用出口，并把基元包成 Poller 装入注册口交付 §2 的 **Headless 行为基线**（真实回环 fd 上「注册→就绪回调==就绪次数→注销后 `sources()` 清空、`wait_and_dispatch` 空转」），配套 `utest_net_poll` 与回写（`06-app-platform.md` §8.8.3、`ARCHITECTURE.md` §4、`CHANGELOG.md`）；基元复用既有 `net-wait-failed`，无新增码。同切片未落的剩余项：`Capabilities`（随引擎一并落地，避免先立一个无人填写的真相源）、把 §8.8.3 基元包成**常驻 poll 线程**并接主线程完成投递（`aurora::detail::post_to_main` + `request_wake`）、把注册口**接入各平台等待集**的具体改造（Win32 `MsgWaitForMultipleObjectsEx` 句柄数组、X11/Wayland 的 DBus watch fd 收编、Headless 常驻 poll 循环、GLFW/macOS 经引擎线程桥接）——这批按 §2 风险注记须先立 Headless 基线（已落）、再逐平台推广并各留一组真机回归；本机验证腿覆盖 Win32 + WSL 的 X11/Wayland。§17 顺序里的 `core/` 基础件**两枚均已落地**。其一 `aurora::Headers`（`core/headers.h`，header-only 值类型：ASCII 大小写不敏感存取、同名条目保序不折叠、原始拼写留到序列化、`set` = 清同名后追加到末尾、逐条目且顺序敏感的相等判定），配套 `utest_core_headers` 与回写（`01-core.md` §10、`ARCHITECTURE.md` §4.1、`CHANGELOG.md`）。其二 `aurora::Url`（`core/url.h` + `src/aurora/core/url.cpp`，实现离头）：RFC 3986 的文法与§5 相对解析**仓内手写、不经平台 API**（Windows `UrlEscape` 是 IE 依赖，畸形串的容忍面会随 libc 漂移，违反`SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001`）；解析即规范化（scheme 与注册名主机小写化、三元组大写化且还原无保留字符、`%2F` 不还原成 `/`、绝对 URL 点段消除、主机尾点去除，`parse(to_string())` 幂等）；分量出口恒为编码态而解码出口是 `percent_decode()` / `query_params()`；主机三型显式，注册名走**缩减版 IDNA**（ASCII 小写 + RFC 3492 punycode，不做 NFC/NFD 与 Unicode 大小写折叠，主机段拒绝百分号转义，`1.2.3` 按注册名而非 `inet_aton` 缩写）；四套 `PercentSet` 与「`+` 不是空格」；`QueryParams` 同名保序多值且键大小写敏感（与 `Headers` 的折叠口径相反）；`port()` 与 `effective_port()` 分离「写没写」与「连到哪」；`is_secure()` 只看 scheme，供 `NetPolicy` 拒明文）。新增错误码 `net-url-invalid`，配套 `utest_core_url`（14 例）与回写（`01-core.md` §11 新增、`ARCHITECTURE.md` §4.1 基础层行、`CHANGELOG.md`）。其中两例由黄金语料驱动——`tests/fixtures/url_vectors/parse.tsv`（解析 / 规范化 / 拒绝，IDN 与 punycode 多次插入向量逐条对齐 Node 的 `url.domainToASCII` 与 CPython 标准库 punycode 模块 `encodings.punycode`）与 `resolve.tsv`（RFC 3986 §5.4 两张示例表逐条照录），来源、口径裁决与加行方式见该目录 `README.md`；**验收判据 10 的 `Url` 部分就此闭合**（其余项随对应切片）。`Headers` 与 `Url` 互不预支职责：前者不解析 URL，后者不管理头表。

**提案编号说明**：本伞形提案先后用过 CHG-007 与 CHG-008，两次都因同日另一工作流的提案重号而让号（让号的一方一律是本提案，`zero-drift-hit-alignment`、`windows-csd-native-parity` 等他人提案的编号不改），现固定为 **CHG-010**。历史提交信息里指向 `xplat-network-stack` 的 CHG-007 / CHG-008 一律读本提案；编号只是句柄，先后次序由「提出日期」与目录名承载。

### 18 评审澄清定案（八项，规范性）

针对外部审查提出的八个「未说明」处，逐条定案如下——它们不是补充说明，落码与文档回写一律以此为准。

1. **事件源句柄的生命周期**：`register_source()` 返回 move-only RAII 句柄 `EventSourceRegistration`，析构即注销且注销幂等。注销语义定为「**停止产生新回调**」：已投入主线程队列的回调仍会抵达一次，但其载荷是弱令牌，宿主失效即丢弃——因此「注销后仍有事件」不构成悬垂访问，而是明确的一次性尾批。需要连尾批一起作废时另用 `cancel_pending()`，该调用与析构不互相隐含。Win32 等待集 64 句柄上限以运行期错误显式暴露（不静默丢弃、不排队等位）。Headless 基线必含三条竞态用例：注册→立即注销→同句柄再注册、注销与就绪同帧发生、句柄移动后源对象析构不双重注销。
2. **`co_read` 得 0 与半关闭**：0 字节 = 对端 FIN，**本端此后仍可继续写**（TCP 半关闭合法且必须支持，libssh2 会话收尾与 HTTP 优雅关闭都依赖它）。FIN 之后再读恒返回 0，**不报错、不设 `net-half-closed-*` 码**——把合法状态做成错误会诱导 AI 去分支一个不该存在的失败路径。单向停读经 `shutdown(ShutdownMode::read)` 显式表达，其效果是释放引擎对该句柄的可读关注。
3. **UDP connect 与组播的冲突**：二者**互斥**而非「connect 后静默过滤组播」。已 connect 的套接字上调用 `join_multicast_group()` 即报 `net-option-conflict`（新增码），因为内核语义本就是「connect 后只收对端数据报」，静默接受只会造出一个永远收不到包的配置。需同时收单播与组播时用两个 `DatagramTransport`。**不提供** `disconnect()`：POSIX 无可靠取消连接语义、Windows 需换协议技巧，跨平台一致性不值得为此破例。
4. **取消与协程挂起的交互**：取消**不抛异常、也不「不恢复」**——`co_await` 恒求得 `Result<T>`，取消时为 `net-cancelled`。这条与既有 `Task::cancel()`（回调不再触发）**刻意不同**并须在文档写明差异：协程帧只能靠 `resume` 前进到 `co_return` 正常析构，「不恢复」等于泄漏帧。多层挂起（DNS → connect → TLS → HTTP）的中间资源由各层对象自持 RAII，取消沿链向下传播后逐层展开释放；引擎侧硬约束是**先释放 fd、再投递取消回调**，否则「回调里立刻重连却撞句柄上限」会成为稳定复现的故障。
5. **Inspector 路由注册表化的匹配次序影响**：已就地写进 §13——现链中精确匹配与 `starts_with` 前缀匹配**交错并存**，注册表必须保持「精确项整体优先、前缀项按登记次序兜后」，并把该次序做成注册表的显式优先级字段而非依赖插入顺序；迁移判据是对全部现存端点（含两条前缀族）跑同一组请求/响应对照片段、逐值不变。
6. **wasm `fetch` 引擎的 CORS 边界**：受限项一律**以能力位表达，绝不静默退化**——非安全自定义头（含 `Authorization`）会触发预检、预检失败即以浏览器错误形态抵达（`Capabilities::custom_headers` 报告是否可自由设头）；跨源重定向不自动跟随；`Set-Cookie` 响应头不可读；流式体取决于 `ReadableStream` 支持度，故新增 `Capabilities::streaming`，为 false 时 `open_stream()` 直接报 `net-platform-unavailable`（**不做「悄悄一次性物化」**，那会让调用方对内存的判断失效），而 `text()` / `bytes()` 恒可用；`ProxyConfig` 在 wasm 上是空操作（代理由浏览器代管），不报错。
7. **「每帧至多一次」里帧的定义**：一帧 = `Application::run` 的一次循环迭代（`on_frame` 到帧尾之间）；wasm 下与一次 `requestAnimationFrame` 回调重合，其它平台是本库自己的帧节奏，不与浏览器刷新率绑定。合并语义 = **累计不丢弃**：字节计数与最新偏移在引擎侧累加，帧末一次写入状态——故 UI 阻塞 200 ms 期间 50 次进度事件损失的是**回调次数**，不是数据。`StreamResource` 同族：本帧多次追加合并为一次 append 批次，并暴露 `batch_bytes()` 供进度条取累计量。
8. **证书固定后置的理由不成立**：**接受批评并改判**（已落 §13、并记入 §16 追加项）——pinning 随 TLS 切片同批实现、默认关闭、经 `NetPolicy` 显式开启；同时把「企业自签 CA」与「pinning」的分工写清，避免把前者误当成后者的需求来源。

**次要建议的处置**

| 建议 | 结论 |
|:---|:---|
| `Capabilities` 改 `bitset` / 位标志 | **不采纳**：它是运行期查询而非位掩码协议，具名 bool 直读对 AI 更友好；QUIC / HTTP3 / mDNS 全在 §15 非目标内，膨胀压力不存在，将来加字段即可。 |
| `Endpoint` 构造期校验规则 | **采纳并定规则**：host 空串非法；port `0` 仅对监听端点合法（`Endpoint::tcp_listen`）；IPv6 字面量必须方括号；zone id 仅接受 link-local 作用域；`is_loopback()` 由解析后的地址族判定而非字符串猜测。黄金用例表落 `06-app-platform.md` §8.8。 |
| `StreamOptions` 默认值 | **采纳并给数字**：单块读 64 KiB、待发读缓冲上限 1 MiB、写高水位 256 KiB / 低水位 64 KiB、单请求物化正文默认上限 4 MiB（与 Inspector 现有口径同源）；超限报 `net-buffer-limit`，字段全部可显式覆盖。 |
| WS 子协议协商失败行为 | **采纳并定**：客户端未要求必选时降级为无子协议、`negotiated_subprotocol()` 回报空；要求必选而服务端未回 `Sec-WebSocket-Protocol` 时报 `ws-handshake-rejected`，不静默降级（降级等于用错协议跑业务数据）。 |
| MIME 扩展名表范围 | **采纳并定首批**：`txt` `html` `css` `js` `json` `xml` `png` `jpg` `jpeg` `gif` `webp` `svg` `pdf` `zip` `gz` `tar` `mp3` `mp4`，未命中一律 `application/octet-stream`；表以数据形式登记，不做运行时注册表（那是第二个膨胀点）。 |

### 19 API 预算实测与超限预案

**先纠正估算口径**：`check_api_budget.py` 计量的是 `aurora_api.json` 的**整个文件字节数 ÷ 4**（现 266577 字节 ≈ 66.6k / 75k token），而该 JSON 的顶层键只有 `library` / `language` / `include` / `alias` / `widgets` / `enums` / `layout_rules` / `state_patterns` / `error_codes` / `debug`——**不含任何 C++ 类与方法签名**（实测 `StreamSocket`、`Listener`、`Session`、`ImageFetcher`、`CoroTask`、`Resource` 在其中零出现）。所以「~15 类 + ~80 方法 → 8–12k token」的前提不成立，方法注释同样不进这个文件。真实边际成本按段实测：`error_codes` ≈ 340 字节/条、`widgets` ≈ 1892 字节/条、`enums` ≈ 116 字节/条、`debug` ≈ 279 字节/条。

据此重算本提案增量：约 30 枚错误码 ≈ 10.2 KB ≈ **2.55k token**；8 枚新枚举 ≈ **0.24k**；若为远程取图新增 1 个 widget ≈ **0.48k**；`debug` 段网络活动导出 1 条 ≈ **0.28k**（且 Release 构建不导出）。**合计约 3.5k token，落点约 70.1k / 75k，不超限**——但吃掉现有余量的 42%，必须带预案，按性价比次序：

1. **合并同源错误码**：凡「同一失败的不同阶段」（`net-tls-version-mismatch` 与 `net-tls-handshake-failed`、`ws-closed` 与 `ws-protocol-error`）先合一枚、以 `message` 表阶段，每合一条省约 340 字节。
2. **`hint` 一行说清动作**：不写背景——它是错误码条目里最大的可变成本。
3. **`widgets` 段按个位数控制**：远程取图优先走既有 `ImageFetcher` 注入点（不新增 widget 即零预算），只有真需要独立属性面时才登记。
4. **枚举克制**：只有会出现在属性 / 序列化里的枚举进 `enums`；引擎内部状态机不进 schema。
5. **仍超限时**：不得改 `TOKEN_BUDGET`，按 §12 语义裁能力面，裁剪顺序 SSE → 条件请求缓存 → multipart 便捷面，被裁项转入后续提案。
6. **明确排除**「裁减文档注释」这条路：注释不进 `aurora_api.json`，裁它省不到预算，还直接违反 AGENTS.md 规则 12 与 `check_doc_comments`。

### 20 CHG-006 同步面的迁移时间表

现行规则里有两条并存且互有张力：`CHANGELOG.md` 的 SemVer 节要求「废弃 API 须以 `[[deprecated]]` 标注并保留至少 1 个 MINOR 周期，再于下个 MAJOR 移除」，而 codespec/SPECIFICATIONS.md §12.1 判据写明 alpha 阶段允许任意破坏性改动（且 §12.3 breaking 流程至今是**占位、未填实**）。本提案按**更严的那条**执行并给出确定时刻，避免实施期再议：

| 时点 | 动作 |
|:---|:---|
| 本变更所在的 MINOR（`1.0.0-alpha.10`） | 新栈落地；旧 `aurora::net::StreamSocket` / `Listener` 转为 `[[deprecated]]` 转发别名，指向 `net::blocking` 对应类型；别名旁注释写清替代类型与逐方法对应表；InspectorServer 与三处测试**同批**迁移完成，不留「旧别名还在、新面已改语义」的中间态 |
| 下一个 alpha（`1.0.0-alpha.11`） | 删除别名与旧同步面；`CHANGELOG.md` 的 Breaking 与 Migration 子节记录逐符号替代表 |
| 顺延条款 | 若 Inspector 迁移未能在同批完成，别名整体顺延一个 alpha 周期，但**禁止**在别名仍存时改动 `net::blocking` 的行为语义 |

对审查问题的直接回答：**既不是「立即删除」也不是「保留到下一个 MINOR 末」**，而是保留一个 alpha 周期的 `[[deprecated]]` 转发别名、下个数个周期移除。附带建议：本条时间表可作为 §12.3 占位内容的第一个真实样本，建议一并回填，免得后续 breaking 变更各自发明流程。

### 21 影响面 → 必跑门禁映射

判据权威在 AGENTS.md §3.1 与 codespec/ARCHITECTURE.md §14.6；本表只登记「本提案影响面 → 门禁」的对应关系，不复制细则（复制必然随权威表漂移）。每个落地切片收尾时跑对应行，批次收口时跑末行全量。

| 本提案的影响面 | 必跑门禁 / 用例 |
|:---|:---|
| 新增 `net/` 全栈公共 API、`state::Resource`、`core` 新基础件 `Url` / `Headers` | 先 `cmake --build build --target aurora_api_json` 与 `gen_error_codes`，再 `check_api_schema_sync`、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`lint`、`format-check` |
| `core/` 目录与边界新增（`Url` / `Headers` 落 `include/aurora/core/`）、伞头收录 | `check_core_layer_boundary`、`check_arch_module_map`、`check_umbrella_header`（`tools/check/umbrella_manifest.txt` 同步） |
| 新枚举（`HttpMethod` / `ResourcePhase` / `ShutdownMode` / `ProxyKind` / `TlsVersion` 等） | `tools/include/known_enums.h` 登记 + `tests/integration/itest_known_enums.cpp`（编译期锚点与属性类型覆盖率）+ `tests/unit/utest_serialization.cpp`（真实键名 codegen 用例）+ `check_api_schema_sync` |
| 平台 / 后端分支、feature 宏、`AURORA_NET_TLS_PROVIDER` 探测 | `check_platform_macros` + CI `toggles` 矩阵（含两枚 provider job） |
| 事件源注册口改造各平台等待集 | `check_platform_macros`、各平台 ctest 分片（`ninja-shards`）、X11/Wayland 的 IME 与系统通知手工测试回归（DBus fd 通道收编）、真机探针 |
| Inspector 路由注册表化与 `network` 端点 | `utest_inspector_server`、`utest_inspector_client`、`itest_inspector_robustness` 逐值回归（REST 行为零变化） |
| codespec 九处文档回写与交叉引用 | `check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_manual_test_format` |
| 若因新门类触碰 AGENTS.md 本身 | `check_agents_size`（> 8 KiB 红灯，只下沉不放宽） |
| 版本号与 `CHANGELOG.md`（Breaking + Migration 子节） | `check_version_consistency` |
| 新增 `.cpp` 源文件与「一源一测一 demo」 | GLOB `CONFIGURE_DEPENDS` 刷新 + `check_no_hardcoded_paths` + 全量 ctest |
| **批次收口** | `ctest --preset ninja-test`（含全部 `check_*` 与全部测试，并行、进程隔离） |

红灯一律不得以添加豁免放行；确属误报的，改门禁脚本并在提交信息里说明，不留白名单注释。

## 验收判据

1. **传输层单测**（tests/unit/utest_net_transport.cpp，无 GUI、进程内回环）：`bind(0)` 实端口回报 → `accept` 事件驱动抵达 → `co_read` / `co_write` 逐字节往返 → 对端 `shutdown` 后读得 0 字节且不误报错误 → 无监听者时 `connect` 失败且 code 为 `net-connect-failed` → 取消令未完成的 `connect` 立刻以 `net-cancelled` 收尾 → 高水位背压下 `write` 不越界堆积 → UDP 数据报往返与 `receive_from` 源地址正确 → `Endpoint::local` 在 UDS / NamedPipe 两型下连通。
2. **线程与事件源契约判据**：所有完成回调与协程续体在断言中核对线程标识等于主线程（`Application::run` 帧内）；`HeadlessSurface` 下无宿主循环时行为与 `Task<T>` 现有口径一致（无 poster 即同步回调）。事件源另需：Headless 与回环下「注册 → 就绪回调抵达次数 == 就绪次数 → 注销后不再抵达」逐值可断言，同帧内注册与注销竞态不产生悬垂句柄；X11 侧 DBus fd 通道收编进注册口后既有 IME / 通知相关手工测试仍全绿，Win32 侧句柄数越过等待集上限时注册返回明确错误而非静默丢弃。
3. **HTTP 层单测**（tests/unit/utest_net_http.cpp 与 tests/integration/itest_net_http.cpp）：以本库 `StreamListener` 起进程内 HTTP 桩，覆盖五方法往返、自定义头大小写不敏感取值、分块传输解码、`Content-Length` 与流式回调字节数逐值一致、重定向跟随与上限报错、超时改道 `http-timeout`、正文超限拒收、Cookie 会话携带与过期、连接池 keep-alive 复用计数、进度回调单调不减。
4. **SSE / WebSocket 单测**：SSE 多行 `data`、`id`/`retry` 解析与断线重连；WebSocket 握手 101、掩码与分帧、ping/pong、close code 透传、超大帧拒收（桩服务器 + 自研客户端闭环，不依赖公网）。
5. **TLS 判据（离线可跑）**：回环自签证书桩下握手成功、未知 CA 被拒且 code 为 `net-tls-cert-rejected`、过期证书被拒、显式危险开关生效时给出 warning 级诊断；`ALPN` 协商与最低版本约束经 `TlsContext` 选项验证。系统栈引擎与 provider 引擎（OpenSSL、mbedTLS 各一次）须各自跑通本组判据。真实公网证书的验证只进手工测试（codespec/manual-test/ 相应文档），不进 CI。
6. **绑定层单测**（tests/unit/utest_state_resource.cpp）：`Resource::from` 三态迁移、`reload` 取消前次请求且旧结果不覆盖新状态、宿主失效后回调被丢弃、`StreamResource` 追加/替换两种策略。
7. **能力矩阵与构建面判据**：wasm 构建下 `capabilities().tcp == false` 且 `capabilities().event_source == false`、构造返回 `net-platform-unavailable`，`WebSocket::connect` 在 wasm 下同样返回该码（裁决记录第三条的可执行化）；Linux 未给 provider 时 configure 成功但 `capabilities().tls == false`、TLS 构造报 `net-tls-engine-unavailable`；给出 `AURORA_NET_TLS_PROVIDER=static:<dir>` 时探测按预期推导引擎宏并生成导入目标，而前缀非法、目录缺 `include/` 或两枚特征头皆不存在时 **configure 期即失败**且诊断列出被检查的路径；`FetchEngine` 的 `Client::send` 走浏览器 `fetch`（手工测试文档记录验收步骤）；宏矩阵 CI（`toggles`）逐组合可编译、可链接，并新增两枚 provider job。
8. **回归与门禁**：既有 Inspector 用例逐值绿（协议面零变化）；`cmake --build build --target aurora_api_json` 与 `gen_error_codes` 刷新后 `check_api_schema_sync`、`check_naming_conventions`、`check_doc_comments`、`check_umbrella_header`、`check_core_layer_boundary`、`check_arch_module_map`、`check_platform_macros`、`check_api_budget`、`lint`、`format-check` 全绿，`ctest --preset ninja-test` 全量绿。API 预算侧注意 `aurora_api.json` 现约 66.6k/75k token，新增错误码与文档字符串须留在预算内（`SPEC.PERF.API.TOKEN-EFFICIENCY.001`）。
9. **一源一测一 demo**：新增 examples/demos/demo_net_http.cpp（远端取图或拉 JSON 写 `Resource` 后渲染），与 §3 的单测同批；golden 或手工验证口径写进 manual-test。
10. **二轮补齐判据**：`Url::parse` 黄金文件用例集（IDN、IPv6 字面量、多值 query 保序、百分号保留字符集、相对解析、非法输入报 `net-url-invalid`）；`Headers` 大小写不敏感存取与 `Set-Cookie` 不折叠；`gzip` / `deflate` 响应透明解压逐字节等价、未知编码报 `http-content-encoding-unsupported`；UTF-8（含 BOM）与 Latin-1 解码正确、其余报 `http-charset-unsupported`；`ETag` 命中 304 时不触网、`no-store` 强制 bypass、`Vary` 按头分键；`connect` / `read_idle` / `total` 三档超时各自可独立触发；`NO_PROXY` 例外生效、代理不可达时不回退直连且报 `net-proxy-unreachable`；`Framer` 三型分帧与溢出拒收；背压下慢消费者期间缓冲字节数不增长；进度回调节流用高速桩服务器断言「回调次数 ≤ 经历帧数」。
11. **可观测性与脱敏判据**：请求时间线各阶段单调递增且总耗时与端到端实测相符；活动表容量上限与淘汰顺序可断言；`Authorization` / `Proxy-Authorization` / `Cookie` / `Set-Cookie` 在日志、时间线、Inspector 响应、错误 `message` 四条出口全部呈现为打码形态（同一断言集跑四出口，任一泄漏即红灯）；`NetPolicy` 拒绝非白名单明文与 TLS 下限违规分别报对应码。
12. **测试替身与离线判据**：`FakeEngine` 可脚本化覆盖超时、取消、半关闭、乱序、TLS 失败、超大帧、代理不可达七类故障；全部网络用例在禁网环境（CI 无出站）下跑通；`spin_until` 类等待辅助无忙等、超时自报且不吞断言。
13. **§18 定案的可执行化判据**：半关闭——对端 `shutdown(write)` 后本端读得 0、**继续写仍成功**、再读恒 0 且不产生新错误码；UDP 互斥——已 connect 的套接字上 `join_multicast_group()` 报 `net-option-conflict`（不静默失效）；取消链——多层 `co_await`（DNS→connect→TLS→HTTP）在中间层取消时协程帧正常展开、无帧泄漏（以构造/析构计数断言），且「先释放 fd、后投递回调」可被「回调内立刻重连」的用例验证；事件源——`EventSourceRegistration` 三条竞态用例（注册即注销再注册、注销与就绪同帧、移动后源析构不双重注销）与 Win32 等待集上限显式报错；路由注册表——精确项优先于前缀项的次序断言 + 全部现存端点（含 `/api/widget/`、`/api/input/` 两族）迁移前后响应逐值对照；wasm 能力位——`streaming` / `custom_headers` 为 false 时对应调用报 `net-platform-unavailable`，**不存在静默退化路径**；进度合并——阻塞事件循环 200 ms 后帧末状态等于累计字节最大值且期间数据零丢失。
14. **文档回写完成**：见下节全部落点，`check_codespec_xref`、`check_code_doc_sync`、`check_version_consistency`、`check_change_proposals` 绿；`CHANGELOG.md` 记 Added（网络栈）与 Breaking（CHG-006 同步面退位及迁移路径，含 §20 别名时间表）。

## 回写落点

- `codespec/SPECIFICATIONS.md`（§4.1 范围补「网络」、§5 特性清单登记 9 条 `SPEC.*.NET.*` 需求行）
- `codespec/ARCHITECTURE.md`（§4 模块映射 `net/` 行与新增头/源文件、运行时章节补「引擎线程 → 主线程投递」数据流）
- `codespec/specification/06-app-platform.md`（§8.8 Socket 操作层扩为网络栈契约：分层与依赖不变量、引擎接口与能力矩阵、`AURORA_NET_TLS_PROVIDER` 探测规则、事件源注册口的逐平台语义、`blocking` 面定位）
- `codespec/specification/02-state.md`（异步章节增补 `Resource` / `StreamResource` 三态绑定语义）
- `codespec/BUILD_OPTIONS.md`（§4 新增 `AURORA_ENABLE_NET` 与 `AURORA_NET_*` 引擎宏行、`AURORA_NET_TLS_PROVIDER` 缓存变量与探测规则、wasm 降级说明、Windows 链接面）
- `codespec/ERROR_CATALOG.md`（生成物：`net-*` / `http-*` / `ws-*` 码组）
- `codespec/specification/08-tooling.md`（InspectorServer 与 inspector_client 传输层改述为新 `aurora::net` 消费者；路由注册表化机制；`network` 端点与配套 MCP 工具的契约）
- `codespec/CODING_STANDARDS.md`（§4.1 增「凭据与 Cookie 不得入日志 / 时间线 / 错误 message」硬规则，§15 反例速查补一条泄漏反例；§3.1 测试写法登记条件等待辅助的用法口径）
- `codespec/specification/01-core.md`（`Url` 与大小写不敏感 `Headers` 两个新基础件的契约与归属）
- `codespec/GUIDELINE.md`（新增「网络请求 → 三态渲染」最小可编译配方）
- `codespec/changes/xplat-network-stack/proposal.md` 自身状态随批次流转（已提议 → 实施中 → 已归档）
