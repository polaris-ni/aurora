// 真实后端 DEBUG 能力门面实现（specification/06-app-platform.md §11）。
// 见 include/aurora/debug/debug_backend.h 的设计说明。

#include "aurora/debug/debug_backend.h"

#include <filesystem>

namespace aurora::debug {

namespace {

// 缺省输出目录（全局，函数局部静态）：空串表示「当前程序运行目录下的 ./aurora_debug/」。
auto debug_output_dir() -> std::string & {
    static std::string dir;
    return dir;
}

#ifdef AURORA_ENABLE_DEBUG
// 确保目标路径的父目录存在（忽略权限等错误，尽力而为，不抛异常）。
// 仅在 DEBUG 分支使用（Release 提前返回，无需建目录）。
auto ensure_parent_dir(const std::string &path) -> void {
    std::error_code ec;
    const std::filesystem::path p(path);
    const std::filesystem::path parent = p.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
}
#endif

} // namespace

auto set_output_directory(const std::string &dir) -> void { debug_output_dir() = dir; }

auto output_directory() -> std::string {
    if (debug_output_dir().empty()) {
        return (std::filesystem::current_path() / "aurora_debug").string();
    }
    return debug_output_dir();
}

auto resolve_output_path(const std::string &path) -> std::string {
    if (path.empty()) {
        return output_directory();
    }
    const std::filesystem::path p(path);
    if (p.is_absolute()) {
        return path; // 显式绝对路径
    }
    // 相对路径：若含目录分隔（parent 不是 "." 也不是空）则视为显式相对路径，原样使用。
    const std::filesystem::path parent = p.parent_path();
    if (!parent.empty() && parent != std::filesystem::path(".")) {
        return path;
    }
    // 纯文件名：落入缺省输出目录。
    return (std::filesystem::path(output_directory()) / path).string();
}

auto capture(Surface &s, const std::string &path, CaptureSource src) -> Result<bool> {
#ifdef AURORA_ENABLE_DEBUG
    const std::string out = resolve_output_path(path);
    ensure_parent_dir(out);
    if (src == CaptureSource::Framebuffer) {
        return s.save_snapshot(out);
    }
    return s.capture_window(out);
#else
    (void)s;
    (void)path;
    (void)src;
    return make_error(ErrorCode::GeneralNotSupported,
                      "aurora::debug::capture: AURORA_ENABLE_DEBUG not enabled "
                      "(build in Debug/RelWithDebInfo or set -DAURORA_ENABLE_DEBUG=ON)");
#endif
}

auto surface_state(const Surface &s) -> Json {
#ifdef AURORA_ENABLE_DEBUG
    Json j;
    const auto sz = s.size();
    j["width"] = static_cast<int>(sz.width);
    j["height"] = static_cast<int>(sz.height);
    j["scale_factor"] = s.scale_factor();
    j["frame_count"] = s.frame_count();
    const Color c = s.clear_color();
    Json cc = Json::array();
    cc.push_back(c.m_r);
    cc.push_back(c.m_g);
    cc.push_back(c.m_b);
    cc.push_back(c.m_a);
    j["clear_color"] = cc;
    j["should_close"] = s.should_close();
    j["has_native_window"] = (s.native_handle() != nullptr);
    j["available"] = true;
    return j;
#else
    (void)s;
    Json j;
    j["available"] = false;
    j["reason"] = "AURORA_ENABLE_DEBUG not enabled";
    return j;
#endif
}

} // namespace aurora::debug
