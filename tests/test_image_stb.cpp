// test_image_stb.cpp — stb_image 通用解码（PNG 等）1:1 测试：
// 覆盖成功路径（编码→解码往返）、不存在文件、空文件、损坏内容、目录等错误路径。

#include <cstdio>
#include <fstream>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::Image;
using aurora::Node;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_image_stb ===\n");

    // 用一个真实 PNG 做「编码→解码」往返，避免依赖仓库相对路径资源。
    const std::string png_path = "stb_roundtrip.tmp.png";

    // 1) 无头渲染生成合法 PNG。
    {
        auto root = Node{Text{"png"}};
        auto rp = render_to_png(root, 48, 24, png_path.c_str());
        AURORA_TEST_CHECK(rp.ok());
    }

    // 2) 成功加载真实 PNG：ok + 尺寸 > 0。
    auto r = Image::load(png_path);
    AURORA_TEST_CHECK(r.ok());
    const Image &img = r.value();
    AURORA_TEST_CHECK(img.width > 0 && img.height > 0);

    // 3) 像素缓冲长度 = width*height*4（RGBA8）。
    AURORA_TEST_CHECK(img.pixels.size() == static_cast<std::size_t>(img.width) * img.height * 4);

    // 4) 像素缓冲非空。
    AURORA_TEST_CHECK(!img.pixels.empty());

    std::remove(png_path.c_str());

    // 5) 不存在的文件 → 失败，错误码为 "io-file-not-found"（无法打开文件）。
    {
        auto bad = Image::load("__no_such_file__.png");
        AURORA_TEST_CHECK(!bad.ok());
        AURORA_TEST_CHECK(bad.error().code == "io-file-not-found");
    }

    // 6) 空文件（0 字节）→ 解码失败（内容损坏）。
    {
        const std::string path = "stb_empty.tmp.png";
        {
            std::ofstream f(path, std::ios::binary);
        }
        auto bad = Image::load(path);
        AURORA_TEST_CHECK(!bad.ok());
        std::remove(path.c_str());
    }

    // 7) 非空但非图像内容 → 解码失败。
    {
        const std::string path = "stb_garbage.tmp.bin";
        {
            std::ofstream f(path, std::ios::binary);
            f << "this is definitively not an image file";
        }
        auto bad = Image::load(path);
        AURORA_TEST_CHECK(!bad.ok());
        AURORA_TEST_CHECK(bad.error().code == "io-image-decode-failed");  // 文件存在但内容损坏 → stb 解码失败
        std::remove(path.c_str());
    }

    // 8) 目录（而非文件）→ 解码失败，不崩溃。
    {
        auto bad = Image::load("tests");
        AURORA_TEST_CHECK(!bad.ok());
    }
}
