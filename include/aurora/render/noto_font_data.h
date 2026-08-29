// AUTO-GENERATED: embedded Noto Sans Regular (OFL). See noto_font_data.cpp.
#pragma once
#include <cstdint>
#include <span>
namespace aurora::render {
// 内嵌 Noto Sans 字体数据访问接口。
// 设计说明（性能特殊考虑）：数组本体（约 431 KB）留在 noto_font_data.cpp 单一定义，
// 不改为 inline 变量——否则每个包含本头的 TU 都会生成一份数据副本，
// 编译/链接体积与耗时显著膨胀。通过函数返回 span 而非 extern 变量：
// 调用方无需自行拼接 size，杜绝越界；字体仅加载期低频读取，span 返回值开销可忽略。
auto noto_sans_ttf() -> std::span<const std::uint8_t>;
} // namespace aurora::render
