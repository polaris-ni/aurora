// aurora_e2e_client — 进程外 E2E 驱动 CLI：经本机 InspectorServer REST 面驱动运行中的
// Aurora 应用（查树 / 定位 / 注入输入 / 抓帧）。
//
// 定位：CI 步骤、探针与人工终端共用的最小驱动入口。能力实现在 header-only 的
// `tools/include/e2e/inspector_driver.h`（本文件只做 argv 解析、输出与退出码），传输在
// `tools/servers/inspector_client.h`（只连 loopback、不做 DNS）。被测应用侧经
// `demo_common.h` 的 opt-in 开关启动 InspectorServer（BUILD_OPTIONS.md §5）。
//
// 用法：
//   aurora_e2e_client [--port <n>] <command> [args...]
//   tree [window]                      打印控件树 JSON
//   find key=<k> type=<t> text=<x>     按 id / 类型 / 文本定位（k=v 任意组合，AND）
//   get <path>                         打印控件属性 JSON（索引路径，空串 = 根）
//   tap <path>                         点击控件中心
//   drag <path> <dx> <dy>              从控件中心拖拽（语义盒 dp）
//   scroll <path> <dx> <dy>            在控件上滚动
//   text <path> <string>               向控件键入文本
//   snapshot [fb|win] [-o <file.png>]  抓帧 PNG（-o 落盘；缺省只报字节数）
//
// 端口：--port > 环境变量 AURORA_INSPECTOR_PORT > 6280（客户端侧解析，见 inspector_driver.h）。
// 输出：结果 JSON / 摘要走 AURORA_LOG_RAW（stdout，无前缀——「程序产品输出」定位，见
// CODING_STANDARDS.md §4.1）；错误行同通道输出但以 [transport-error] / [http-error] /
// [usage] 为前缀，便于脚本按行分类。
// 退出码：0 = 成功；1 = 请求失败（传输层或 HTTP 4xx/5xx）；2 = 用法错误。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/log.h"
#include "e2e/inspector_driver.h"

namespace {

using aurora::tools::e2e::CallResult;

constexpr std::string_view AURORA_USAGE =
    "usage: aurora_e2e_client [--port <n>] <command> [args...]\n"
    "  tree [window]                      print widget tree JSON\n"
    "  find key=<k> type=<t> text=<x>     locate widgets by id / type / text (AND)\n"
    "  get <path>                         print widget props JSON (index path, \"\" = root)\n"
    "  tap <path>                         click widget center\n"
    "  drag <path> <dx> <dy>              drag from widget center (dp)\n"
    "  scroll <path> <dx> <dy>            scroll on widget\n"
    "  text <path> <string>               type text into widget\n"
    "  snapshot [fb|win] [-o <file.png>]  capture PNG frame\n"
    "port: --port > AURORA_INSPECTOR_PORT > 6280 (client-side resolution)\n";

/// @brief 功能输出统一走 raw 通道（stdout、无前缀、不过级别过滤）。
auto emit(std::string_view text) -> void { AURORA_LOG_RAW("e2e", text); }

/// @brief 结果行按类别加前缀；调用方据此分类（成功时 body 原样透传不加前缀）。
auto emit_failure(const CallResult &call) -> void {
    if (call.kind == CallResult::Kind::TransportError) {
        emit("[transport-error] " + call.error + "\n");
        return;
    }
    emit("[http-error] status=" + std::to_string(call.status) + " " + call.body + "\n");
}

/// @brief dx/dy 解析（strtof 全串校验；失败返回 nullopt 走 usage 退出）。
[[nodiscard]] auto parse_float(const std::string &text) -> std::optional<float> {
    if (text.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables): strtof 的 endptr 惯例必须可空判
    const float value = std::strtof(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

/// @brief find 的 k=v 段解析；未知键 / 缺 '=' 均为用法错误。
[[nodiscard]] auto parse_find_filters(const std::vector<std::string> &args, std::string &key, std::string &type,
                                      std::string &text) -> bool {
    for (const std::string &arg : args) {
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        const std::string name = arg.substr(0, eq);
        const std::string value = arg.substr(eq + 1);
        if (name == "key") {
            key = value;
        } else if (name == "type") {
            type = value;
        } else if (name == "text") {
            text = value;
        } else {
            return false;
        }
    }
    return !key.empty() || !type.empty() || !text.empty();
}

/// @brief snapshot：PNG 二进制不进文本流；-o 落盘，缺省只报字节数。
auto run_snapshot(const std::string &host, std::uint16_t port, const std::vector<std::string> &args) -> int {
    std::string_view source = "fb";
    std::string out_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) {
                emit("[usage] snapshot: -o requires a file path\n");
                return 2;
            }
            out_path = args[++i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        } else if (args[i] == "fb" || args[i] == "win") {
            source = args[i];
        } else {
            emit("[usage] snapshot: unknown argument '" + args[i] + "'\n");
            return 2;
        }
    }
    const CallResult call = aurora::tools::e2e::snapshot(host, port, source);
    if (!call.ok()) {
        emit_failure(call);
        return 1;
    }
    if (!out_path.empty()) {
        std::ofstream png(out_path, std::ios::binary);
        if (!png) {
            emit("[error] cannot open output file: " + out_path + "\n");
            return 1;
        }
        png.write(call.body.data(), static_cast<std::streamsize>(call.body.size()));
        png.close();
        emit("[ok] snapshot -> " + out_path + " (" + std::to_string(call.body.size()) + " bytes)\n");
    } else {
        emit("[ok] snapshot: " + std::to_string(call.body.size()) + " bytes (use -o <file> to save)\n");
    }
    return 0;
}

}  // namespace

// argv 遍历按 aurora_cli 先例豁免指针算术告警（C main 签名即指针 + 计数，无更安全的替代形态）。
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// 入口函数允许分配等异常逃逸到 main（terminate 即失败路径），CLI 不做 try/catch 包装。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char *argv[]) -> int {
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);

    std::optional<std::uint16_t> explicit_port;
    std::size_t arg_index = 0;
    if (arg_index < args.size() && args[arg_index] == "--port") {
        if (arg_index + 1 >= args.size()) {
            emit("[usage] --port requires a value\n");
            emit(AURORA_USAGE);
            return 2;
        }
        const std::optional<float> parsed = parse_float(args[arg_index + 1]);
        if (!parsed.has_value() || *parsed < 1.0F || *parsed > 65535.0F ||
            *parsed != static_cast<float>(static_cast<std::uint16_t>(*parsed))) {
            emit("[usage] --port expects an integer in 1..65535, got '" + args[arg_index + 1] + "'\n");
            return 2;
        }
        explicit_port = static_cast<std::uint16_t>(*parsed);
        arg_index += 2;
    }
    if (arg_index >= args.size()) {
        emit("[usage] missing command\n");
        emit(AURORA_USAGE);
        return 2;
    }
    const std::string command = args[arg_index++];
    const std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(arg_index), args.end());

    const std::string host = "127.0.0.1";
    const std::uint16_t port = aurora::tools::e2e::resolve_port(explicit_port);

    // 树查询类：成功即 body 透传（JSON 文本）。
    if (command == "tree" || command == "get" || command == "find") {
        CallResult call{.kind = CallResult::Kind::TransportError};
        if (command == "tree") {
            // 只 parse 一次并先判 has_value 再解引用，杜绝双重求值与未检查的 optional 访问。
            const std::optional<float> window_arg = rest.empty() ? std::nullopt : parse_float(rest[0]);
            const std::uint32_t window = window_arg.has_value() ? static_cast<std::uint32_t>(*window_arg) : 0;
            call = aurora::tools::e2e::get_tree(host, port, window);
        } else if (command == "get") {
            if (rest.size() != 1) {
                emit("[usage] get <path>\n");
                return 2;
            }
            call = aurora::tools::e2e::get_widget(host, port, rest[0]);
        } else {
            std::string key;
            std::string type;
            std::string text;
            if (!parse_find_filters(rest, key, type, text)) {
                emit("[usage] find key=<k> type=<t> text=<x>  (at least one filter)\n");
                return 2;
            }
            call = aurora::tools::e2e::find(host, port, key, type, text);
        }
        if (!call.ok()) {
            emit_failure(call);
            return 1;
        }
        emit(call.body);
        emit("\n");
        return 0;
    }

    // 输入注入类：POST 后透传服务端确认（成功 2xx 才算过）。
    if (command == "tap" || command == "drag" || command == "scroll" || command == "text") {
        CallResult call{.kind = CallResult::Kind::TransportError};
        if (command == "tap") {
            if (rest.size() != 1) {
                emit("[usage] tap <path>\n");
                return 2;
            }
            call = aurora::tools::e2e::tap(host, port, rest[0]);
        } else if (command == "drag" || command == "scroll") {
            if (rest.size() != 3) {
                emit("[usage] " + command + " <path> <dx> <dy>\n");
                return 2;
            }
            const auto dx = parse_float(rest[1]);
            const auto dy = parse_float(rest[2]);
            if (!dx.has_value() || !dy.has_value()) {
                emit("[usage] " + command + ": dx/dy must be numbers\n");
                return 2;
            }
            call = command == "drag" ? aurora::tools::e2e::drag(host, port, rest[0], *dx, *dy)
                                     : aurora::tools::e2e::scroll(host, port, rest[0], *dx, *dy);
        } else {
            if (rest.size() != 2) {
                emit("[usage] text <path> <string>\n");
                return 2;
            }
            call = aurora::tools::e2e::type_text(host, port, rest[0], rest[1]);
        }
        if (!call.ok()) {
            emit_failure(call);
            return 1;
        }
        emit("[ok] " + command + " -> " + call.body + "\n");
        return 0;
    }

    if (command == "snapshot") {
        return run_snapshot(host, port, rest);
    }

    emit("[usage] unknown command '" + command + "'\n");
    emit(AURORA_USAGE);
    return 2;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
