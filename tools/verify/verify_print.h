#pragma once

// tools/verify/ 真机验收探针共用的小工具：定宽文本拼装 + 句柄/整数格式化。
//
// 为什么不用 `printf`：AGENTS.md §5 规则 8 禁止在 tools/ 下**新增**直接触达标准输出的
// `printf` / `std::cout` / `puts`。探针的功能输出（表格、结论）统一走 `AURORA_LOG_RAW`
// ——`Logger` 的 raw 通道：无前缀、不过日志级别过滤、恒写 stdout，正是「程序产品输出」
// 的定位。本头只负责把值格式化成字符串，自身不触碰任何输出流。
//
// 范围：仅服务 tools/verify/ 下的真机验收探针；不进 aurora 库、不进 CTest、无被
// include/ 公共 API 引用的义务。

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace aurora_verify {

/// @brief 右填充到定宽（表格列左对齐）；已超宽则原样返回。
inline auto pad_right(std::string text, std::size_t width) -> std::string {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

/// @brief 句柄 / 指针 → `0x` + 16 位零填充十六进制（32/64 位下宽度一致，便于逐行比对）。
inline auto format_handle(const void *handle) -> std::string {
    std::ostringstream oss;
    // 豁免 cppcoreguidelines-pro-type-reinterpret-cast：指针→std::uintptr_t 是标准背书的整值转换
    // （仅显示用途）；std::bit_cast 要求两侧等宽，而 sizeof(void*) == sizeof(uintptr_t) 并非标准
    // 保证（不等时 bit_cast 为 UB，reinterpret_cast 仍良定义），跨平台探针不宜替换。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << reinterpret_cast<std::uintptr_t>(handle);
    return oss.str();
}

/// @brief 有符号整数 → 十进制串。
inline auto format_int(long long value) -> std::string {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

/// @brief 无符号整数 → 十进制串（用于计数/尺寸列）。
inline auto format_uint(unsigned long long value) -> std::string {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

}  // namespace aurora_verify
