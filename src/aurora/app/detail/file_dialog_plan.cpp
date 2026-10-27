// file_dialog_plan.cpp — 文件对话框折算层的实现（零平台依赖，见同目录内部头）。

#include "aurora/app/detail/file_dialog_plan.h"

#include <string>
#include <string_view>

#include "aurora/core/log.h"

namespace aurora::file_dialog_plan {

auto last_path_segment(std::string_view path) -> std::string_view {
    const auto separator = path.find_last_of("/\\");
    if (separator == std::string_view::npos) {
        return path;
    }
    return path.substr(separator + 1);
}

auto suggested_file_name(std::string_view raw) -> std::string {
    if (raw.empty()) {
        return {};  // 不表达意图：与不传该字段同形，也不留痕
    }
    const std::string_view segment = last_path_segment(raw);
    if (segment.empty()) {
        AURORA_LOG_WARN("file_dialog",
                        std::string("suggested_name names a directory, not a file name: ") + std::string(raw));
        return {};
    }
    if (segment != raw) {
        AURORA_LOG_WARN("file_dialog", std::string("suggested_name carries a path; using last segment: ") +
                                           std::string(raw) + " -> " + std::string(segment));
    }
    return std::string(segment);
}

auto normalize_extension(std::string_view ext) -> std::string {
    const auto is_blank = [](char c) -> bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::string_view body = ext;
    // 前导点与首尾空白一起反复削：`" .txt "` 里点前面还有空格，逐段单削会留下 `"  .txt "` 的残点。
    // 只削前导点（`"txt."` 的尾点是用户写的一部分，不动）。
    while (!body.empty()) {
        if (body.front() == '.' || is_blank(body.front())) {
            body.remove_prefix(1);  // `.txt` 与 `txt` 同解：OS 侧要不含点的那一个
            continue;
        }
        if (is_blank(body.back())) {
            body.remove_suffix(1);
            continue;
        }
        break;
    }
    return std::string(body);  // 首尾空白不是扩展名的一部分
}

auto with_default_extension(std::string_view name, std::string_view ext) -> std::string {
    const std::string normalized = normalize_extension(ext);
    if (normalized.empty() || name.empty()) {
        return std::string(name);  // 不表达意图 / 空名 = 取消，都不补
    }
    if (last_path_segment(name).find('.') != std::string_view::npos) {
        return std::string(name);  // 用户已打过扩展名，不改写用户的输入
    }
    std::string out{name};
    out += '.';
    out += normalized;
    return out;
}

}  // namespace aurora::file_dialog_plan
