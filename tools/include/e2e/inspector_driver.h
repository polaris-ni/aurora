#pragma once

// 进程外 E2E 驱动客户端（header-only 能力层）：经 `InspectorServer` 的 REST 面查询控件树、
// 按 key/type/text 定位、注入指针/键盘输入（含拖拽）、抓帧（PNG）。
//
// 分层定位：`tools/servers/inspector_client.h` 是裸 HTTP 传输（一个函数、一个响应结构）；
// 本头把「按端点命名的一次调用」收敛出来，并把失败一分为二——
//   - `CallResult::Kind::TransportError`：**未获得 HTTP 响应**（服务未启动的 connect 拒绝 /
//     超时 / 非回环拒单）。这是「服务不在」与「树为空」的可区分判据：前者 error 非空且
//     status 恒 0，调用方绝不该把它解释成空结果。
//   - `CallResult::Kind::HttpError`：服务端回了 4xx/5xx（语义错误，详情在响应体 JSON 的
//     "error" 字段）。
//
// 本头**不解析 JSON**：body 原样上交，解析归调用方（CLI 透传打印、单测用第三方库断言），
// 因此除 `inspector_client.h` 外零依赖。请求体 JSON 由本头手工拼装（`escape_json` 负责
// 字符串字面量转义）；数值字段由服务端按「存在即须为数字」校验，缺省即 0。
//
// 端口解析（**客户端侧约定**）：入参显式值 > 环境变量 `AURORA_INSPECTOR_PORT`（值非法按
// 未声明处理）> 默认 6280。`InspectorServer` 本身不读环境变量（`start(port)` 只认实参），
// 解析放在客户端是为了让外部驱动者（CI 步骤 / 探针 / 人工终端）不必与被测应用共享端口常量。
//
// 与 `inspector_client.h` 同一信任模型：只连 loopback、不做 DNS。
//
// 参数直通契约：query 与请求体的字符串一律按原始 UTF-8 字节传递。服务端 query 解析按
// `&` / `=` 字节切分、无 URL 解码，故含 `&` / `=` / 空格的定位值在当前协议下不可用
// （服务端既定行为，本层不做假转义——转义了服务端也不解）。

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "inspector_client.h"  // tools/servers（目标侧注入 include 路径）

namespace aurora::tools::e2e {

/// @brief 一次 REST 调用的归一结果。
struct CallResult {
    enum class Kind : std::uint8_t {
        Ok,  ///< 2xx
        HttpError,  ///< 服务端回了 4xx/5xx（语义错误，详情在 body JSON 的 "error" 字段）
        TransportError,  ///< 未获得 HTTP 响应：连不上（服务未启动）/ 超时 / 非回环拒单
    };
    Kind kind = Kind::TransportError;
    int status = 0;  ///< HTTP 状态码（TransportError 时恒 0）
    std::string body;  ///< 响应体（`snapshot` 为 PNG 字节，其余为 JSON 文本）
    std::string error;  ///< TransportError 时的传输层原因；其余为空

    [[nodiscard]] auto ok() const -> bool { return kind == Kind::Ok; }
};

namespace detail {

/// @brief 把传输层响应归类为 `CallResult`（全 driver 唯一分类点）。
[[nodiscard]] inline auto classify(const inspector::HttpResponse &r) -> CallResult {
    if (!r.ok()) {
        return CallResult{.kind = CallResult::Kind::TransportError, .status = 0, .body = {}, .error = r.error};
    }
    if (r.status >= 200 && r.status < 300) {
        return CallResult{.kind = CallResult::Kind::Ok, .status = r.status, .body = r.body, .error = {}};
    }
    return CallResult{.kind = CallResult::Kind::HttpError, .status = r.status, .body = r.body, .error = {}};
}

/// @brief JSON 字符串字面量转义（引号 / 反斜杠 / 控制字符）。
[[nodiscard]] inline auto escape_json(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 其余控制字符按 \u00XX：JSON 不允许字面控制字节入串。半字节经算术生成
                    // 十六进制字符——常量表下标会触发非常量下标告警，static 常量另触发命名门禁。
                    const auto byte = static_cast<unsigned char>(c);
                    const auto hi = static_cast<unsigned char>((byte >> 4) & 0xF);
                    const auto lo = static_cast<unsigned char>(byte & 0xF);
                    out += "\\u00";
                    out += static_cast<char>(hi < 10 ? '0' + hi : 'a' + (hi - 10));
                    out += static_cast<char>(lo < 10 ? '0' + lo : 'a' + (lo - 10));
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

/// @brief 追加 query 参数（空值跳过；首个参数前补 `?`，其后补 `&`）。
inline auto append_param(std::string &target, std::string_view key, std::string_view value) -> void {
    if (value.empty()) {
        return;
    }
    target += (target.find('?') == std::string::npos) ? '?' : '&';
    // 显式带长度：string_view 的 data() 不保证以空字符结尾，append(string_view) 单参重载
    // 会被解析到 const char* 形态引发误用告警。
    target.append(key.data(), key.size());
    target += '=';
    target.append(value.data(), value.size());
}

/// @brief POST JSON 体的公共尾巴：`{"path":"..."` 之后接各 action 自己的字段。
[[nodiscard]] inline auto input_body(std::string_view path) -> std::string {
    std::string body = R"({"path":")";
    body += escape_json(path);
    body += '"';
    return body;
}

}  // namespace detail

/// @brief 端口解析：显式入参 → `AURORA_INSPECTOR_PORT` → 默认 6280（客户端侧约定，见文件头）。
///
/// 环境变量值须为**全串十进制**且落在 1..65535，否则按未声明处理回落默认——半途而废的
/// 脏值（"6280 "、"12x"）不该被静默截断成合法端口。
[[nodiscard]] inline auto resolve_port(std::optional<std::uint16_t> explicit_port) -> std::uint16_t {
    if (explicit_port.has_value()) {
        return *explicit_port;
    }
    // string_view 的 data() 不保证 NUL 结尾；getenv 需要 C 串，落一份 std::string 再 c_str()。
    const std::string env_name(inspector::AURORA_PORT_ENV);
    const char *env = std::getenv(env_name.c_str());
    if (env != nullptr && *env != '\0') {
        char *end = nullptr;
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables): strtoll 的 endptr 惯例必须可空判
        const long long value = std::strtoll(env, &end, 10);
        if (end != nullptr && *end == '\0' && value >= 1 && value <= 65535) {
            return static_cast<std::uint16_t>(value);
        }
    }
    return inspector::AURORA_DEFAULT_PORT;
}

// ---- 树查询 / 定位 -----------------------------------------------------------

/// @brief GET /api/tree：整棵控件树 JSON；`window` 非 0 时加 `?window=<id>`（多窗口）。
[[nodiscard]] inline auto get_tree(std::string_view host, std::uint16_t port, std::uint32_t window = 0) -> CallResult {
    std::string target = "/api/tree";
    if (window != 0) {
        detail::append_param(target, "window", std::to_string(window));
    }
    return detail::classify(inspector::http_request("GET", host, port, target));
}

/// @brief GET /api/find：按 id / 类型 / 文本属性定位控件。
///
/// 三参至少给一（全空由服务端回 400，本层不重复校验）；多参数为 AND 组合。命中的
/// `path` 与 `get_widget` / 输入注入的寻址口径一致，可直接回填使用。
[[nodiscard]] inline auto find(std::string_view host, std::uint16_t port, std::string_view key, std::string_view type,
                               std::string_view text) -> CallResult {
    std::string target = "/api/find";
    detail::append_param(target, "key", key);
    detail::append_param(target, "type", type);
    detail::append_param(target, "text", text);
    return detail::classify(inspector::http_request("GET", host, port, target));
}

/// @brief GET /api/widget/{path}：按索引路径读控件属性 JSON（空串 = 树根）。
[[nodiscard]] inline auto get_widget(std::string_view host, std::uint16_t port, std::string_view path) -> CallResult {
    std::string target = "/api/widget/";
    target += path;
    return detail::classify(inspector::http_request("GET", host, port, target));
}

// ---- 输入注入（目标式：以 path 命中的控件中心为原点） -------------------------

/// @brief POST /api/input/click：点击目标控件中心。
[[nodiscard]] inline auto tap(std::string_view host, std::uint16_t port, std::string_view path) -> CallResult {
    std::string body = detail::input_body(path);
    body += "}";
    return detail::classify(inspector::http_request("POST", host, port, "/api/input/click", body));
}

/// @brief POST /api/input/drag：从目标控件中心拖拽 `(dx, dy)`（语义盒 dp）。
[[nodiscard]] inline auto drag(std::string_view host, std::uint16_t port, std::string_view path, float dx, float dy)
    -> CallResult {
    std::string body = detail::input_body(path);
    body += ",\"dx\":";
    body += std::to_string(dx);
    body += ",\"dy\":";
    body += std::to_string(dy);
    body += "}";
    return detail::classify(inspector::http_request("POST", host, port, "/api/input/drag", body));
}

/// @brief POST /api/input/scroll：在目标控件上滚动 `(dx, dy)`。
[[nodiscard]] inline auto scroll(std::string_view host, std::uint16_t port, std::string_view path, float dx, float dy)
    -> CallResult {
    std::string body = detail::input_body(path);
    body += ",\"dx\":";
    body += std::to_string(dx);
    body += ",\"dy\":";
    body += std::to_string(dy);
    body += "}";
    return detail::classify(inspector::http_request("POST", host, port, "/api/input/scroll", body));
}

/// @brief POST /api/input/text：向目标控件（须持焦点的文本类控件）键入文本。
[[nodiscard]] inline auto type_text(std::string_view host, std::uint16_t port, std::string_view path,
                                    std::string_view text) -> CallResult {
    std::string body = detail::input_body(path);
    body += R"(,"text":")";
    body += detail::escape_json(text);
    body += R"("})";
    return detail::classify(inspector::http_request("POST", host, port, "/api/input/text", body));
}

// ---- 抓帧 --------------------------------------------------------------------

/// @brief GET /api/debug/snapshot：抓帧 PNG（body 即 PNG 字节）。
///
/// `source` 取 "fb"（帧缓冲，默认）或 "win"（上屏窗口）。需被测应用已注入 Surface getter，
/// 未注入时服务端回 400。
[[nodiscard]] inline auto snapshot(std::string_view host, std::uint16_t port, std::string_view source = "fb")
    -> CallResult {
    std::string target = "/api/debug/snapshot";
    detail::append_param(target, "source", source);
    return detail::classify(inspector::http_request("GET", host, port, target));
}

}  // namespace aurora::tools::e2e
