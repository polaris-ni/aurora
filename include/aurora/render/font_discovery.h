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
    FT_Face face = nullptr;  ///< 底层 FreeType face 句柄；生命周期由 FontFace::mem（内存字体）或平台字体管理器持有
    int id = 0;  ///< 图集缓存键所用的稳定序号
    int weight = 400;  ///< CSS 字重 100..900；FT style_flags 判定（bold→700，否则 400）；供 resolve_faces 按字重选面
    std::shared_ptr<std::vector<std::uint8_t>> mem;  ///< 内存字体字节（须保持存活至 face 释放）
};

/// @brief 初始化字体发现：懒注册内嵌 Noto Sans 与平台系统回退；重复调用为幂等。
/// @note 引擎首次用到字体时自动完成，无需显式调用点。
auto init_font_discovery() -> void;

/// @brief 关闭字体发现：释放所有已注册 FT_Face 并清空注册表；仅在进程退出路径调用。
auto shutdown_font_discovery() -> void;

/// @brief 注册内存字体（family 为空表示默认 sans-serif）。
/// @param family 逻辑字体族名；空串代表默认链首（同时挂到 `""`/`sans-serif` 键）。
/// @param bytes TTF/OTF 字体字节；由本接口接管所有权（内部转 shared_ptr 常驻）。
auto register_font_memory(const std::string &family, std::vector<std::uint8_t> bytes) -> void;

/// @brief 注册字体文件（family 为空表示默认）。
/// @param family 逻辑字体族名；空串代表默认链。
/// @param path 字体文件绝对/相对路径；由 FreeType 打开。
auto register_font_file(const std::string &family, const std::string &path) -> void;

/// @brief 覆盖默认链：清除 "" / "sans-serif" 并以指定字体文件作为默认（family 为空）。
/// @param path 用作新默认链首的字体文件路径。
auto set_default_font_file(const std::string &path) -> void;

/// @brief 解析逻辑 family 为有序候选 FT_Face 列表（含默认链兜底，供缺字回退）。
/// @param family 逻辑字体族名；空串走默认链。
/// @param weight 请求字重 100..900；精确匹配的面排最前，其余按字重距离升序稳定排列。
/// @return 该 (family, weight) 的候选 FontFace 指针列表引用（内部缓存常驻，勿持久化裸指针跨 shutdown）。
/// @note `find_glyph` 取首个含该字形的面：有粗体面时粗体优先命中；缺字回退到其他字重/回退面
///       （脚本回退语义不变）。
[[nodiscard]] auto resolve_faces(const std::string &family, int weight = 400) -> const std::vector<FontFace *> &;

/// @brief 内部：向默认链追加候选 FT_Face（平台字体发现使用）。
/// @param ff 待加入 `""`/`sans-serif` 默认链的字体面共享指针；空指针被忽略。
auto add_default_face(const std::shared_ptr<FontFace> &ff) -> void;

}  // namespace aurora::render
