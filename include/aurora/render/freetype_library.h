#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

namespace aurora::render {

/// @brief 进程级 FT_Library 单例访问器。
/// 线程模型为单线程 UI，故不加锁；首次调用时懒初始化。
[[nodiscard]] auto ft_library() -> FT_Library;

/// @brief 释放 FT_Library（进程退出时可选调用）。
auto ft_shutdown() -> void;

}  // namespace aurora::render
