#pragma once

// 内置 Cascadia Code（SIL OFL 1.1）字体数据访问：**内部头**（位于 src/，不进 include/）。
// 与 `noto_font_data.h` 的唯一差别是可见性——本头只被 `font_discovery.cpp` 一个 TU 包含。
//
// 为什么不放进 include/：消费方（终端等）按**族名** `"Cascadia Code"` 选字即可，没有拿原始
// 字节的必要；把 379 KB 的数组门面开成公共 API 只会平白扩大公共面与 API 预算，还牵动
// `check_umbrella_header` 的直连集合判定。内嵌字体对外的契约是「这个族名一定解析得到面」。

#include <cstdint>
#include <span>

namespace aurora::render {

/// @brief 内嵌 Cascadia Code 字体数据访问接口。
/// @note 数组本体（约 379 KB）留在 cascadia_font_data.cpp 单一定义，不改为 inline 变量——否则
///       每个包含本头的 TU 都会生成一份数据副本，编译 / 链接体积与耗时显著膨胀。通过函数返回
///       span 而非 extern 变量：调用方无需自行拼接 size，杜绝越界；字体仅加载期低频读取。
/// @return 只读字节 span，覆盖完整 Cascadia Code TTF 二进制内容。
auto cascadia_code_ttf() -> std::span<const std::uint8_t>;

}  // namespace aurora::render
