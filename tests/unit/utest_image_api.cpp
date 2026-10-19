/// 测试类型: unit
/// 目标单元: src/aurora/image/image_api.cpp
/// 测试说明: image_api.cpp 是 ImageCodecRegistry 实例的薄转发层（7 个自由函数）。逐一调用即可覆盖
/// 转发行；无对应 codec / 文件不存在时注册表返回错误 Result，但转发行仍被执行。decode_async 返回的
/// future 必须可阻塞解析（注册表同步就绪）。
/// 判据：7 个转发函数均被调用且不抛异常；decode_async 的 future 可取得。

#include <filesystem>
#include <span>

#include "aurora/image/image_codec.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image_api {

AURORA_TEST_CASE(forwarding_functions_execute_without_throwing) {
    // 无对应 codec / 文件不存在时注册表返回错误 Result，但转发行仍被执行。
    (void)aurora::image::decode_file("/nonexistent/aurora_ci_image.png", aurora::image::DecodeOptions{});
    (void)aurora::image::decode_memory(std::span<const std::uint8_t>{}, aurora::image::DecodeOptions{});
    (void)aurora::image::decode(aurora::image::ImageSource{}, aurora::image::DecodeOptions{});
    (void)aurora::image::decode_animated_file("/nonexistent/aurora_ci_anim.gif", aurora::image::DecodeOptions{});
    (void)aurora::image::encode(aurora::Image{}, aurora::image::EncodeOptions{});

    const std::filesystem::path out =
        std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_ci_image_save.png";
    (void)aurora::image::save(aurora::Image{}, out.string(), aurora::image::EncodeOptions{});

    // decode_async 返回的 future 必须可阻塞解析（注册表同步就绪）。
    auto fut = aurora::image::decode_async(aurora::image::ImageSource{}, aurora::image::DecodeOptions{});
    AURORA_TEST_CHECK_NO_THROW((void)fut.get());
}

}  // namespace aurora::test_cases::utest_image_api
