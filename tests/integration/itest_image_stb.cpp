/// 测试类型: integration
/// 目标单元: include/aurora/core/image.h
/// 测试说明: stb_image 通用解码链路集成——无头渲染产出真实 PNG 后经 Image::load（编解码注册表）
///           文件解码往返，并覆盖缺失文件/空文件/损坏内容/目录四类错误路径的结构化失败

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/image.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/node.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "support/paths.h"

namespace aurora::test_cases::itest_image_stb {

namespace {

/// @brief 在当前用例唯一临时目录写入文本/字节文件并返回路径字符串。
auto write_temp_file(const std::string &name, const std::string &bytes) -> std::string {
    const std::filesystem::path dir = aurora::testing::isolation::temp_dir();
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    out << bytes;
    return file.string();
}

}  // namespace

AURORA_TEST_CASE(stb_roundtrip_decodes_rendered_png) {
    // 用真实 PNG 做「渲染编码 → stb 解码」往返：无头渲染生成合法 PNG，避免依赖仓库相对路径资源。
    const std::filesystem::path png_path =
        std::filesystem::path(aurora::testing::isolation::temp_dir()) / "stb_roundtrip.png";
    aurora::Node root{aurora::Text{"png"}};
    const auto written = aurora::render_to_png(root, 48, 24, png_path.string().c_str());
    AURORA_TEST_CHECK_TRUE(written.ok());

    // 成功加载真实 PNG：ok + 尺寸 > 0。
    const auto r = aurora::Image::load(png_path.string());
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    const aurora::Image &img = r.value();
    AURORA_TEST_CHECK_TRUE(img.width > 0);
    AURORA_TEST_CHECK_TRUE(img.height > 0);

    // 像素缓冲长度 = width*height*4（RGBA8）且非空。
    AURORA_TEST_CHECK_EQ(img.pixels.size(),
                         static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 4U);
    AURORA_TEST_CHECK_FALSE(img.pixels.empty());
}

AURORA_TEST_CASE(stb_load_missing_file_returns_not_found) {
    // 不存在的文件 → 失败，错误码为 "io-file-not-found"（无法打开文件）。
    const auto bad = aurora::Image::load("__no_such_file__.png");
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code, std::string{"io-file-not-found"});
}

AURORA_TEST_CASE(stb_load_empty_file_fails) {
    // 空文件（0 字节）→ 解码失败（无任何编解码器可处理）。
    const std::string path =
        write_temp_file("stb_empty.tmp.png", std::string{});  // 仅创建、不写入任何字节
    const auto bad = aurora::Image::load(path);
    AURORA_TEST_CHECK_FALSE(bad.ok());
}

AURORA_TEST_CASE(stb_load_garbage_content_fails_with_decode_error) {
    // 非空但非图像内容 → 文件存在但内容损坏，所有编解码器兜底失败 → "io-image-decode-failed"。
    const std::string path = write_temp_file("stb_garbage.tmp.bin", "this is definitively not an image file");
    const auto bad = aurora::Image::load(path);
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code, std::string{"io-image-decode-failed"});
}

AURORA_TEST_CASE(stb_load_directory_fails_without_crash) {
    // 目录（而非文件）→ 打开/解码失败，返回错误而非崩溃。
    const auto bad = aurora::Image::load(aurora::testing::paths::under_repo("tests"));
    AURORA_TEST_CHECK_FALSE(bad.ok());
}

}  // namespace aurora::test_cases::itest_image_stb
