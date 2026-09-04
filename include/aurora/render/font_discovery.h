#pragma once

#include <ft2build.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include FT_FREETYPE_H

namespace aurora::render {

/// @brief 一个已加载的字体面（来自内存字节或系统字体文件）。
struct FontFace {
    FT_Face face = nullptr;
    int id = 0;  ///< 图集缓存键所用的稳定序号
    std::shared_ptr<std::vector<std::uint8_t>> mem;  ///< 内存字体字节（须保持存活至 face 释放）
};

/// @brief 字体发现：注册内嵌默认字体与平台系统回退，并提供 family→候选 FT_Face 的解析。
///
/// 引擎首次用到字体时由 `init_font_discovery()` 自动初始化（懒注册内嵌 Noto Sans），
/// 无需显式 init 调用点；`shutdown_font_discovery()` 在进程退出时释放所有 FT_Face。
auto init_font_discovery() -> void;
auto shutdown_font_discovery() -> void;

/// @brief 注册内存字体（family 为空表示默认 sans-serif）。
auto register_font_memory(const std::string &family, std::vector<std::uint8_t> bytes) -> void;

/// @brief 注册字体文件（family 为空表示默认）。
auto register_font_file(const std::string &family, const std::string &path) -> void;

/// @brief 覆盖默认链：清除 "" / "sans-serif" 并以指定字体文件作为默认（family 为空）。
auto set_default_font_file(const std::string &path) -> void;

/// @brief 解析逻辑 family 为有序候选 FT_Face 列表（含默认链兜底，供缺字回退）。
[[nodiscard]] auto resolve_faces(const std::string &family) -> const std::vector<FontFace *> &;

/// @brief 内部：向默认链追加候选 FT_Face（平台字体发现使用）。
auto add_default_face(const std::shared_ptr<FontFace> &ff) -> void;

}  // namespace aurora::render
