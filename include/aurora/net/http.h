#pragma once

/// @brief HTTP/1.1 客户端的请求 / 应答值类型与方法令牌（CHG-010 §6、§12）。
/// @file net/http.h
///
/// 本头只回答一个问题：**「一次 HTTP 交换的输入与输出长什么样」**——方法是哪个、目标 URL、
/// 请求字段与消息体、以及一份已物化的应答。它同样是纯值类型：不含句柄、不含 socket、不含任何
/// 平台头，可在任意线程拷贝与比较。真正的「连上去、把 `render_head()` 写出去、把应答读回来」
/// 属发起侧（`net::http::Client`，建在传输与 TLS 之上），不在这里。
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

    /// @brief 设定整请求时限（连接 + 读 + 总耗时的上限，缺省由发起侧兜底）。
    /// @param value 时限。
    /// @return 自身引用，便于链式调用。
    auto timeout(std::chrono::milliseconds value) -> Request &;

    /// @brief 查看整请求时限。
    /// @return 已设定则返回之；未设定为 `nullopt`。
    [[nodiscard]] auto timeout() const -> std::optional<std::chrono::milliseconds> { return timeout_; }

    /// @brief 设定重定向跟随上限（`0` 表示不跟随）。缺省 10。
    /// @param value 上限次数。
    /// @return 自身引用，便于链式调用。
    auto max_redirects(int value) -> Request &;

    /// @brief 查看重定向跟随上限。
    /// @return 上限次数。
    [[nodiscard]] auto max_redirects() const -> int { return max_redirects_; }

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
    std::optional<std::chrono::milliseconds> timeout_;  ///< 整请求时限
    int max_redirects_{10};  ///< 重定向跟随上限
};

/// @brief 一次 HTTP 应答：状态行 + 字段 + 已物化的消息体。
/// @note Thread: thread-safe（值类型，无共享状态）
/// @note Side-effects: none
class Response {
  public:
    /// @brief 由已物化的分量构造（发起侧把体读全后交进来）。
    /// @param status 三位状态码。
    /// @param reason 原因短语（原样，可为空）。
    /// @param headers 应答字段。
    /// @param body 消息体字节（透明解压 / charset 解码在物化之前完成）。
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

    /// @brief 消息体的文本形态（按原字节直接转换，不做 charset 解码）。
    /// @return 体的文本；charset / 解压在物化阶段处理，这里只反映已物化的字节。
    [[nodiscard]] auto text() const -> std::string { return std::string{body_.begin(), body_.end()}; }

  private:
    std::uint16_t status_{0};  ///< 状态码
    std::string reason_;  ///< 原因短语
    Headers headers_;  ///< 应答字段
    std::vector<char> body_;  ///< 已物化消息体
};

}  // namespace aurora::net::http
