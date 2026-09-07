/// 测试类型: unit
/// 目标单元: include/aurora/render/png.h
/// 测试说明: 内置最小 PNG 编码器（内存编码签名/块结构、尺寸非法报错、build_idat 成帧、write_png 落盘）单元测试

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "aurora/render/png.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_png {

namespace {
constexpr std::array<std::uint8_t, 8> kSignature = {137, 80, 78, 71, 13, 10, 26, 10};

/// 在字节流中查找子序列（判断是否含 IHDR/IDAT/IEND 块类型）。
auto contains(const std::vector<std::uint8_t> &hay, const char *needle) -> bool {
    const std::vector<std::uint8_t> n(needle, needle + std::string(needle).size());
    if (hay.size() < n.size()) {
        return false;
    }
    for (std::size_t i = 0; i + n.size() <= hay.size(); ++i) {
        bool hit = true;
        for (std::size_t j = 0; j < n.size(); ++j) {
            if (hay[i + j] != n[j]) {
                hit = false;
                break;
            }
        }
        if (hit) {
            return true;
        }
    }
    return false;
}
}  // namespace

AURORA_TEST() {
    // ---- 1. 合法尺寸：编码成功，含 PNG 签名与三类块 ----
    {
        std::vector<std::uint8_t> rgba(2U * 2U * 4U, 0xFFU);  // 2×2 全不透明白
        const auto r = detail::write_png_to_memory(rgba.data(), 2, 2);
        AURORA_TEST_CHECK(r.ok());
        const auto &bytes = r.value();
        AURORA_TEST_CHECK(bytes.size() > 8);
        bool sig_ok = true;
        for (std::size_t i = 0; i < kSignature.size(); ++i) {
            sig_ok = sig_ok && (bytes[i] == kSignature[i]);
        }
        AURORA_TEST_CHECK(sig_ok);
        AURORA_TEST_CHECK(contains(bytes, "IHDR"));
        AURORA_TEST_CHECK(contains(bytes, "IDAT"));
        AURORA_TEST_CHECK(contains(bytes, "IEND"));
    }

    // ---- 2. 非法尺寸 / 空指针：返回错误，不崩溃 ----
    {
        std::vector<std::uint8_t> rgba(4, 0);
        AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(nullptr, 1, 1).ok());
        AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(rgba.data(), 0, 1).ok());
        AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(rgba.data(), 1, -2).ok());
    }

    // ---- 3. build_idat：zlib stored-block 成帧（首两字节为 CMF/FLG，含空负载的 2 字节头） ----
    {
        std::vector<std::uint8_t> raw(16, 0xAB);
        const auto idat = detail::build_idat(raw.data(), raw.size());
        AURORA_TEST_CHECK(idat.size() >= 3);
        AURORA_TEST_CHECK_EQ(idat[0], std::uint8_t{0x78});
        AURORA_TEST_CHECK_EQ(idat[1], std::uint8_t{0x01});

        const auto empty = detail::build_idat(raw.data(), 0);
        AURORA_TEST_CHECK_EQ(empty.size(), std::size_t{2});  // 仅 zlib 头，无 stored block
    }

    // ---- 4. write_png 落盘：文件存在且以 PNG 签名开头（用完即删） ----
    {
        std::vector<std::uint8_t> rgba(1U * 1U * 4U, 0x10U);
        const auto path = std::filesystem::temp_directory_path() / "aurora_utest_png.tmp";
        const auto r = write_png(path.string().c_str(), 1, 1, rgba.data());
        AURORA_TEST_CHECK(r.ok());
        if (r.ok()) {
            std::ifstream f(path, std::ios::binary);
            std::vector<std::uint8_t> head(8, 0);
            f.read(reinterpret_cast<char *>(head.data()), 8);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            bool sig_ok = true;
            for (std::size_t i = 0; i < head.size(); ++i) {
                sig_ok = sig_ok && (head[i] == kSignature[i]);
            }
            AURORA_TEST_CHECK(sig_ok);
            std::error_code ec;
            std::filesystem::remove(path, ec);  // 清理本测试自建的临时文件
        }
    }
}

}  // namespace aurora::test_cases::utest_png
