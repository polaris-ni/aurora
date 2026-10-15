#pragma once

/// @brief HTTP/1.1 客户端的请求 / 应答值类型、方法令牌与失败分型（CHG-010 §6、§12）。
/// @file net/http.h
///
/// 本头只回答一个问题：**「一次 HTTP 交换的输入、输出与失败长什么样」**——方法是哪个、目标 URL、
/// 请求字段与消息体、一份已物化的应答、以及一枚失败该往哪儿归因。它同样是纯值类型：不含句柄、不含
/// socket、不含任何平台头，可在任意线程拷贝与比较。真正的「连上去、把 `render_head()` 写出去、把应答
/// 读回来」属发起侧（`net::http::Client`，建在传输与 TLS 之上），不在这里。
///
/// 三条设计约束值得在写代码前知道：
///
/// - **字符串只能经显式解析进入**：`Request` 的工厂收一段 URL 文本并当场 `Url::parse`，解析
///   失败不静默退化成「空 URL 直接发」——它被记在请求对象里，到 `render_head()` 才成为错误。
///   这一条与 §12「手拼 URL 恰是 AI 生成代码的头号错误源」同源：让错误在最早可确定的地方确定。
/// - **方法令牌与线上文本一一对应**：`method_text()` 给出大写线上形态（`GET` / `POST` …），
///   `method_from_text()` 是其逆且只认规范拼写（大小写敏感，`RFC 9110 §9`）；未知拼写报错而不是
///   当成「某种自定义方法」——自定义方法本批次不提供，静默接受只会让 `Content-Length` 的取舍失据。
/// - **`HEAD` 的请求恒不携带消息体、其应答恒无体**：`body()` 对 `HEAD` 请求无效，应答分帧由
///   `wire::framing_of(head, request_is_head)` 判定，本类型只提供 `is_head()` 这一枚事实来源。
///
/// @code
/// auto req = aurora::net::http::Request::get("https://example.com/api?q=1");
/// req.header("Accept", "application/json").timeout(std::chrono::seconds{5});
/// const auto head = req.render_head();  // Result<std::string>：请求行 + 字段（不含体）
/// @endcode
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"

namespace aurora::net::http {

/// @brief HTTP 请求方法；首批只列本库发起侧支持的同名工厂对应的六枚。
/// @note Thread: thread-safe
/// @note Side-effects: none
/// @note 豁免 performance-enum-size：公共 API 枚举，落在 `Request` 的单个字段而非密集数组；
/// 底层类型属公共 API 形态的一部分。
/// NOLINTNEXTLINE(performance-enum-size)
enum class Method {
    Get,  ///< `GET`：读取资源，应答有体
    Head,  ///< `HEAD`：只取头部块，应答恒无体
    Post,  ///< `POST`：提交，请求可带体
    Put,  ///< `PUT`：整体替换，请求可带体
    Patch,  ///< `PATCH`：部分修改，请求可带体
    Delete,  ///< `DELETE`：删除，请求一般无体但允许带
};

/// @brief 方法的线上文本（大写、`RFC 9110 §9` 的规范拼写）。
/// @param method 方法枚举。
/// @return 恒为非空、纯大写的短令牌（`"GET"` / `"HEAD"` / `"POST"` / `"PUT"` / `"PATCH"` / `"DELETE"`）。
[[nodiscard]] auto method_text(Method method) -> std::string_view;

/// @brief 由线上文本解析方法（`method_text()` 的逆）。
///
/// 只认上表六枚的规范大写拼写；其余（含小写与自定义方法）一律失败，错误码 `http-request-invalid`。
/// @param text 方法文本。
/// @return 成功为对应枚举；失败为 `http-request-invalid`。
[[nodiscard]] auto method_from_text(std::string_view text) -> Result<Method>;

/// @brief HTTP 请求的三档超时；各档独立，`nullopt` 或非正值 = 该档不设时限。
///
/// 单一时限无法分别表达「连不上」（`connect`）与「Body 慢」（`read_idle`），故拆三档：`connect` 覆盖建连
/// （含 TLS 握手），`read_idle` 是相邻两次交付之间的空闲上限（滑动，每读到字节即重置），`total` 是整请求
/// 时限（跨连接 / 读写 / 全部重定向）。任一档到点，发起侧一律落 `http-timeout`，`message` 的 detail 标明
/// 是哪一档，使「查服务起没起」与「查 Body 卡在哪」不至混为一谈。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct Timeouts {
    std::optional<std::chrono::milliseconds> connect;  ///< 建连（含 TLS 握手）时限
    std::optional<std::chrono::milliseconds> read_idle;  ///< 相邻两次读之间的空闲时限
    std::optional<std::chrono::milliseconds> total;  ///< 整请求时限
};

/// @brief 进度所属方向；一次交换里两侧各报一段，回调需据此分流。
///
/// 上传在请求体被传输层接受时推进，下载在应答体到达时推进。二者共用一枚回调是刻意的：调用方通常只关心
/// 「这件事进行到哪了」，而方向是载荷自带的属性，不该逼调用方挂两个口。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class ProgressPhase : std::uint8_t {
    Upload,  ///< 请求侧：已交出的消息体字节（含请求头部之外的部分）
    Download,  ///< 应答侧：已到达的消息体线上字节（透明解压的还原量不计入，见 `Progress`）
};

/// @brief 一次进度发射所携带的事实（合流口径见 `Request::on_progress()`）。
///
/// 字节口径只有一处需要留意：下载侧计的是**分帧解出后的线上字节**，不是透明解压之后的字节。透明解压在
/// 物化路径上发生在收完之后，中途无法给出还原量；流式路径若要拿「实际交出去的还原字节」，看该路径自己的
/// 完成事实而非进度值。
/// @note Thread: 回调内只读（值随每次发射新建）
/// @note Side-effects: none
struct Progress {
    ProgressPhase phase{ProgressPhase::Upload};  ///< 方向
    std::uint64_t transferred_bytes{0};  ///< 该方向已累计交付的字节数；重定向的每一跳各自重新计数
    std::uint64_t total_bytes{0};  ///< 该方向总量；`total_known` 为假时无意义
    bool total_known{false};  ///< 总量是否已知（上传恒真；下载取决于分帧是否给出长度）
    bool finished{false};  ///< 该方向是否已收摊（成功与失败都算收摊；成败由发起侧的出口值表达，不由本字段表达）
};

/// @brief 进度回调：在发起侧的发射时机被调用（线程与频次约束见 `Request::on_progress()`）。
/// @note Thread: 恒在主线程同步调用（无主线程投递器时即在承接线程）
/// @note Side-effects: 由回调实现决定
using ProgressHandler = std::function<void(const Progress &)>;

/// @brief 一次 HTTP 请求：方法 + 目标 + 字段 + 消息体 + 发起侧选项。
/// @note Thread: thread-safe（值类型，无共享状态；`render_head()` 只读）
/// @note Side-effects: none
class Request {
  public:
    /// @brief 生成 `GET` 请求。
    /// @param url 目标 URL 文本；当场 `Url::parse`，失败记录在对象内、到 `render_head()` 才报错。
    /// @return 请求值（方法 `Get`）。
    [[nodiscard]] static auto get(std::string_view url) -> Request;
    /// @brief 生成 `HEAD` 请求（应答恒无体）。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Head`）。
    [[nodiscard]] static auto head(std::string_view url) -> Request;
    /// @brief 生成 `POST` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Post`）。
    [[nodiscard]] static auto post(std::string_view url) -> Request;
    /// @brief 生成 `PUT` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Put`）。
    [[nodiscard]] static auto put(std::string_view url) -> Request;
    /// @brief 生成 `PATCH` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Patch`）。
    [[nodiscard]] static auto patch(std::string_view url) -> Request;
    /// @brief 生成 `DELETE` 请求。
    /// @param url 目标 URL 文本。
    /// @return 请求值（方法 `Delete`）。
    /// @note 名字尾下划线是因为 `delete` 是 C++ 关键字，无法直接作为工厂名。
    [[nodiscard]] static auto delete_(std::string_view url) -> Request;  // NOLINT(readability-identifier-naming)

    /// @brief 方法。
    /// @return 构造时确定的方法枚举。
    [[nodiscard]] auto method() const -> Method { return method_; }

    /// @brief 是否为 `HEAD` 请求（应答分帧判定的唯一输入）。
    /// @return 方法为 `Head` 时为 true。
    [[nodiscard]] auto is_head() const -> bool { return method_ == Method::Head; }

    /// @brief 目标 URL（仅当解析成功时可见）。
    /// @return 解析成功的 URL；解析失败时为 `nullopt`（错误在 `render_head()` 返回）。
    [[nodiscard]] auto url() const -> const std::optional<Url> & { return url_; }

    /// @brief 追加一枚请求字段（保留调用方书写次序；同名可多条）。
    /// @param name 字段名；原样存储。
    /// @param value 字段值。
    /// @return 自身引用，便于链式调用。
    auto header(std::string_view name, std::string_view value) -> Request &;

    /// @brief 置入单值请求字段：先移除同名字段再追加（`Host` / `Content-Type` 一类应唯一的字段）。
    /// @param name 字段名。
    /// @param value 字段值。
    /// @return 自身引用，便于链式调用。
    auto set_header(std::string_view name, std::string_view value) -> Request &;

    /// @brief 查看请求字段（不含 `render_head()` 补齐的 `Host` 与 `Content-Length`）。
    /// @return 字段表的只读引用。
    [[nodiscard]] auto headers() const -> const Headers & { return headers_; }

    /// @brief 设定消息体（覆盖既有）。`HEAD` 请求忽略之（应答无体、请求不得带体）。
    /// @param body 体字节。
    /// @return 自身引用，便于链式调用。
    auto body(std::vector<char> body) -> Request &;

    /// @brief 查看消息体。
    /// @return 体字节的只读引用；无体时为空。
    [[nodiscard]] auto body() const -> const std::vector<char> & { return body_; }

    /// @brief 设定整请求时限（`total` 档的便捷入口；要分档控制用 `timeouts()`）。
    /// @param value 时限。
    /// @return 自身引用，便于链式调用。
    auto timeout(std::chrono::milliseconds value) -> Request &;

    /// @brief 查看整请求时限（等价 `timeouts().total`）。
    /// @return 已设定则返回之；未设定为 `nullopt`。
    [[nodiscard]] auto timeout() const -> std::optional<std::chrono::milliseconds> { return timeouts_.total; }

    /// @brief 设定三档超时（连接 / 读空闲 / 总耗时）。
    /// @param value 超时集合。
    /// @return 自身引用，便于链式调用。
    auto timeouts(Timeouts value) -> Request &;

    /// @brief 查看三档超时。
    /// @return 超时集合的只读引用。
    [[nodiscard]] auto timeouts() const -> const Timeouts & { return timeouts_; }

    /// @brief 设定重定向跟随上限（`0` 表示不跟随）。缺省 10。
    /// @param value 上限次数。
    /// @return 自身引用，便于链式调用。
    auto max_redirects(int value) -> Request &;

    /// @brief 查看重定向跟随上限。
    /// @return 上限次数。
    [[nodiscard]] auto max_redirects() const -> int { return max_redirects_; }

    /// @brief 设定进度回调（覆盖既有；传空回调即取消）。
    ///
    /// 发起侧把两侧进度**在源头合流**后再发射：字节到达只更新一枚「最新值」槽，发射发生在每一次可发射
    /// 的机会（宿主回合）上，且**同一机会至多一次**——挂了回调的下载不会因为对端切得碎就每包写一次状态。
    /// 本库是响应式框架，每次回调都可能落到一个信号写入上，按 chunk 发射会把它放大成重绘风暴。装可注入的
    /// 帧号源（`Client::progress_frame_source()`）时改用帧号作闸，严格「每帧至多一次」；未装即以宿主回合
    /// 为闸（一次可发射机会即一回合）。两相都有一个例外：**每个方向的末值恒发一次**，否则调用方无法靠回调
    /// 判定「收了摊」。
    ///
    /// 回调恒在主线程同步调用（续体经主线程投递器恢复）；它只用于观测，其返回值与耗时都不参与协议判定。
    /// @param handler 回调；空回调 = 不设。
    /// @return 自身引用，便于链式调用。
    auto on_progress(ProgressHandler handler) -> Request &;

    /// @brief 查看进度回调。
    /// @return 已设则返回之；否则空回调。
    [[nodiscard]] auto on_progress() const -> const ProgressHandler & { return on_progress_; }

    /// @brief 序列化请求行与请求字段（不含消息体）。
    ///
    /// 补齐两处：无 `Host` 时按目标 URL 补；有消息体且未声明 `Content-Length` / `Transfer-Encoding`
    /// 时按体长度补。校验（方法令牌、字段名 token、字段值无 CR / LF、URL 含主机）由字节层执行。
    /// @return 请求头部字节（以空行收尾）；URL 解析失败或文法不合时为错误（`http-request-invalid`）。
    [[nodiscard]] auto render_head() const -> Result<std::string>;

  private:
    /// @brief 由工厂共用：绑定方法与已解析的目标。
    /// @param method 方法枚举。
    /// @param url 目标 URL 文本；当场解析。
    Request(Method method, std::string_view url);

    Method method_{Method::Get};  ///< 方法
    std::optional<Url> url_;  ///< 已解析目标；nullopt = 解析失败（错误暂存于 url_error_）
    std::optional<Error> url_error_;  ///< 解析失败的载荷；仅在 url_ 为空时有意义
    Headers headers_;  ///< 请求字段（不含补齐项）
    std::vector<char> body_;  ///< 消息体
    Timeouts timeouts_;  ///< 三档超时（connect / read_idle / total），缺省全不设
    int max_redirects_{10};  ///< 重定向跟随上限
    ProgressHandler on_progress_;  ///< 进度回调（空 = 不报）；发起侧负责合流后再调用
};

/// @brief 一次 HTTP 应答：状态行 + 字段 + 已物化（解压完毕）的消息体。
///
/// `body()` 交出的是未解码的原始字节，`text()` 才按 `Content-Type` 的 `charset` 现地解码为文本；
/// 二者对同一份体各表一层，故 charset 不支持时仍可回退到 `body()` 自处。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
class Response {
  public:
    /// @brief 由已物化的分量构造（发起侧把体读全后交进来）。
    /// @param status 三位状态码。
    /// @param reason 原因短语（原样，可为空）。
    /// @param headers 应答字段。
    /// @param body 已解压的消息体字节（`Content-Encoding` 的透明解压在物化之前完成；`charset` 解码留给
    /// `text()` 按 `Content-Type` 现场执行，故 `body()` 恒给出未解码的原始字节）。
    Response(std::uint16_t status, std::string reason, Headers headers, std::vector<char> body);

    /// @brief 状态码。
    /// @return 三位整数。
    [[nodiscard]] auto status() const -> std::uint16_t { return status_; }

    /// @brief 原因短语。
    /// @return 原样文本；对端未给时为空串。
    [[nodiscard]] auto reason() const -> std::string_view { return reason_; }

    /// @brief 应答字段表。
    /// @return 字段表的只读引用。
    [[nodiscard]] auto headers() const -> const Headers & { return headers_; }

    /// @brief 取首个同名应答字段的值。
    /// @param name 字段名；ASCII 大小写不敏感。
    /// @return 首个匹配值；无匹配为 `nullopt`。
    [[nodiscard]] auto header(std::string_view name) const -> std::optional<std::string>;

    /// @brief 消息体原始字节。
    /// @return 体字节的只读引用。
    [[nodiscard]] auto body() const -> const std::vector<char> & { return body_; }

    /// @brief 消息体的文本形态（按 `Content-Type` 的 `charset` 解码为 UTF-8 文本）。
    ///
    /// 首批只解 `charset` 为 UTF-8（剥前导 BOM）与 Latin-1（`iso-8859-1` / `latin-1`）两族；字段缺失
    /// 或无 `charset` 参数时按 UTF-8 处理（现代缺省）。其余 charset 报 `http-charset-unsupported`——
    /// 原始字节恒可经 `body()` 取回，本函数绝不把非法字节序列静默替换成 U+FFFD 或另作他解。透明
    /// 解压（`Content-Encoding`）不在此处：进入 `body_` 的字节已由发起侧物化管线解压完毕。
    /// @return 解码后的 UTF-8 文本；charset 不支持时为 `http-charset-unsupported`。
    [[nodiscard]] auto text() const -> Result<std::string>;

  private:
    std::uint16_t status_{0};  ///< 状态码
    std::string reason_;  ///< 原因短语
    Headers headers_;  ///< 应答字段
    std::vector<char> body_;  ///< 已物化消息体
};

/// @brief 一次 HTTP 失败的归因档：把「该往哪儿排查」这一件事从错误码里提出来。
///
/// 本档只回答**归因层**，不回答**能不能重试**——后者由 `Error::retryable`（与 `codespec/errors.toml`
/// 逐码同源）给出，两枚正交：`Timeout` 且可重试是「再来一次」，`Proxy` 且可重试
/// （`net-proxy-config-unreadable`）是「读系统配置的时机不对」，压进同一枚枚举只会让调用方靠猜分工。
/// 之所以还要第三枚分类而不是直接用生成表的 `ErrorCategory`：那是**全库**粗分类，本族 41 枚全落在
/// `io` 下，分不出「名字解析不了」与「对端把字节写坏了」这两种相反的排查方向。
///
/// 九档按「先具体后兜底」归一，同一枚码恒只进一档：`Cancelled` 优先于一切（调用方主动收摊，不再看
/// 链路）；`Timeout` 看的是**时限**而不是阶段（HTTP 三档超时与裸传输空闲超时同归此档）；`Dns` / `Proxy` /
/// `Tls` 按**失败发生在哪一跳**归类；`Unreachable` 是「链路断了，但没有更具体的档可归」；`Protocol` 只指
/// **对端把报文写坏了**；`Status` 指「应答已收到、状态码本身即失败」；`Invalid` 是兜底，涵盖调用点写错、
/// 本构建缺能力、本库旋钮设限这三类「动作在代码或配置里而不在网络上」的情形。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class HttpErrorClass : std::uint8_t {
    Invalid = 0,  ///< 兜底档：调用点 / 本构建能力 / 本库上限（要改的是代码与旋钮，不是网络）
    Dns,  ///< 名字解析不了
    Unreachable,  ///< 链路断了：建连、读写、事件循环侧的失败
    Proxy,  ///< 代理这一跳给的答案：不可达、要认证、握手字节不合、系统配置读不到
    Tls,  ///< 安全通道本身：握手、证书、版本、pin
    Timeout,  ///< 时限到点
    Protocol,  ///< 对端把报文写坏了：状态行、字段、分帧、编码后的字节
    Status,  ///< 应答已收到且状态码本身即失败（4xx / 5xx）
    Cancelled,  ///< 调用方取消
};

/// @brief 把一枚错误码归入 HTTP 失败分型档（全函数：任意 `ErrorCode` 都有档可归）。
///
/// 逐码映射以本函数为唯一真值源，覆盖 `net-*` 与 `http-*` 两族全部 41 枚（含发起侧用不到的服务端面码，
/// 同样给出档）；其余族的码（`general-*` / `widget-*` 之类）不是网络交换的产物，一律落 `Invalid` 而
/// **不**报错——分型是观测口，不是校验口。两处值得点名的归类：`http-redirect-limit` 归 `Protocol`
/// （对端把重定向写成了环，改不了它就得改跟随策略），`http-body-too-large` 归 `Invalid`（越的是本库
/// 上限旋钮，动作在调用方而不在对端）。
/// @param code 错误码（取自 `Error::code_enum`）。
/// @return 该码的归因档。
[[nodiscard]] auto error_class_of(ErrorCode code) -> HttpErrorClass;

/// @brief 由应答状态码折算归因档（状态码是数据不是错误，故这条映射不在错误码表里）。
///
/// `send` 与 `open_stream` 对非 2xx 交出的仍是**成功** `Result`：一份 404 的应答同样是完整的事实，把它
/// 折算成错误就丢掉了状态码本身。于是「这次算不算状态码类失败」由调用方判，本函数给出判完之后的档，
/// 使 `Status` 这一档有唯一产出点而不是枚举里的死项。
/// @param status 应答状态码（`Response::status()`）。
/// @return `>= 400` 恒为 `Status`；`< 400` 回 `Invalid`，此处语义退化为「没有状态码类失败可归因」，
///         不代表这次交换出错。
[[nodiscard]] auto error_class_of_status(std::uint16_t status) -> HttpErrorClass;

/// @brief 分型档的稳定小写令牌（日志与 Inspector 用；跨进程只认这枚文本，改名不影响它）。
/// @param error_class 归因档。
/// @return `"invalid"` / `"dns"` / `"unreachable"` / `"proxy"` / `"tls"` / `"timeout"` / `"protocol"` /
///         `"status"` / `"cancelled"` 之一，恒非空。
[[nodiscard]] auto error_class_text(HttpErrorClass error_class) -> std::string_view;

}  // namespace aurora::net::http
