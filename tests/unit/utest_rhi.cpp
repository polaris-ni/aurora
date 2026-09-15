/// 测试类型: unit
/// 目标单元: include/aurora/render/rhi/rhi_backend.h
/// 测试说明: 覆盖 RHI 回放抽象：记录型后端按录制顺序收到同批命令、且变长数据已由 DisplayList 解析为
/// 指针（含负下标 → nullptr 的回退）；SoftwareRhi 逐条转发回 Painter 后帧缓冲与直接绘制逐位一致；
/// 未绑定目标的 submit 为 no-op；bind 可切换目标。SoftwareRhi 头亦在此直接 include（见下）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/render/rhi/software_rhi.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_rhi {

namespace {

[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 记录型后端：把收到的命令与解析后的数据指针存下来供断言。
///
/// 它就是「第二个平级消费者」的最小证据 —— 证明 `DisplayList` 的命令流可以喂给一个**完全不碰
/// `Painter`** 的后端；GPU 后端将来替换的是这里的实现，而不是 `DisplayList`。
class RecordingRhi final : public rhi::RhiBackend {
  public:
    struct Entry {
        CmdKind kind = CmdKind::FillRect;
        Rect bounds{};
        Color color;
        const std::string *text = nullptr;
        const Font *font = nullptr;
        const std::vector<Color> *colors = nullptr;
        const std::vector<float> *stops = nullptr;
        const Image *image = nullptr;
        const Matrix2D *matrix = nullptr;
    };

    [[nodiscard]] auto name() const -> std::string_view override { return "recording"; }

    auto submit(const DrawCmd &cmd, const rhi::CmdData &data) -> void override {
        entries.push_back(Entry{.kind = cmd.kind,
                                .bounds = cmd.bounds,
                                .color = cmd.color,
                                .text = data.text,
                                .font = data.font,
                                .colors = data.colors,
                                .stops = data.stops,
                                .image = data.image,
                                .matrix = data.matrix});
    }

    std::vector<Entry> entries;
};

/// @brief 逐字节比较两块帧缓冲（golden 红线的等价表述：像素输出不许有任何差异）。
[[nodiscard]] auto same_pixels(const Painter &a, const Painter &b) -> bool {
    if (a.width() != b.width() || a.height() != b.height()) {
        return false;
    }
    const auto bytes = static_cast<std::size_t>(a.width()) * static_cast<std::size_t>(a.height()) * 4U;
    const std::uint8_t *pa = a.data();
    const std::uint8_t *pb = b.data();
    if (pa == nullptr || pb == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < bytes; ++i) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

AURORA_TEST_CASE(backend_name_identifies_implementation) {
    Painter painter;
    painter.begin(4, 4);
    rhi::SoftwareRhi software{painter};
    RecordingRhi recording;
    AURORA_TEST_CHECK_EQ(std::string{software.name()}, std::string{"software"});
    AURORA_TEST_CHECK_EQ(std::string{recording.name()}, std::string{"recording"});
}

AURORA_TEST_CASE(replay_reaches_non_painter_backend_in_recorded_order) {
    // 录制端仍是 Painter（录制是 RecordPattern 的既有能力），但**回放端完全不碰 Painter**。
    Painter recorder;
    recorder.begin(32, 32);
    DisplayList list;
    recorder.record(list);
    recorder.fill_rect(rect_at(0.0F, 0.0F, 8.0F, 8.0F), Color::red());
    recorder.draw_text(rect_at(0.0F, 8.0F, 16.0F, 8.0F), std::string{"hi"}, Font{}, Color::black());
    recorder.draw_linear_gradient(rect_at(0.0F, 16.0F, 16.0F, 8.0F), Point{.x = 0.0F, .y = 16.0F},
                                  Point{.x = 16.0F, .y = 16.0F}, {Color::red(), Color::blue()}, {0.0F, 1.0F});
    recorder.stop();

    RecordingRhi sink;
    list.replay(sink);

    AURORA_TEST_REQUIRE(list.cmd_count() == 3U);
    AURORA_TEST_REQUIRE(sink.entries.size() == 3U);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].kind == CmdKind::FillRect);
    AURORA_TEST_CHECK_TRUE(sink.entries[1].kind == CmdKind::DrawText);
    AURORA_TEST_CHECK_TRUE(sink.entries[2].kind == CmdKind::LinearGradient);

    // 变长数据已按池下标解析为指针（后端不必理解池语义）。
    AURORA_TEST_REQUIRE(sink.entries[1].text != nullptr);
    AURORA_TEST_CHECK_EQ(*sink.entries[1].text, std::string{"hi"});
    AURORA_TEST_CHECK_TRUE(sink.entries[1].font != nullptr);
    AURORA_TEST_REQUIRE(sink.entries[2].colors != nullptr);
    AURORA_TEST_CHECK_EQ(sink.entries[2].colors->size(), 2U);
    AURORA_TEST_REQUIRE(sink.entries[2].stops != nullptr);
    AURORA_TEST_CHECK_EQ(sink.entries[2].stops->size(), 2U);
    // 未引用池的命令其数据字段为空指针（回退语义由后端决定）。
    AURORA_TEST_CHECK_TRUE(sink.entries[0].text == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].colors == nullptr);
}

AURORA_TEST_CASE(negative_pool_indices_resolve_to_null_pointers) {
    // 手工构造（等价于「录制方未引用任何池」）：负下标必须解析为 nullptr 而非越界访问。
    DisplayList list;
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    cmd.color = Color::green();
    list.push_cmd(cmd);

    RecordingRhi sink;
    list.replay(sink);

    AURORA_TEST_REQUIRE(sink.entries.size() == 1U);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].text == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].font == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].colors == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].stops == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].image == nullptr);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].matrix == nullptr);
}

AURORA_TEST_CASE(image_and_matrix_indices_resolve_for_composite) {
    // Composite 命令引用图像 + 变换矩阵两个池：两者都要解析到，且指向池内同一元素。
    DisplayList list;
    DrawCmd cmd;
    cmd.kind = CmdKind::Composite;
    cmd.image_idx = list.add_image(Image{});
    cmd.matrix_idx = list.add_matrix(Matrix2D::from_translate(3.0F, 4.0F));
    cmd.composite_scale = 1.5F;
    list.push_cmd(cmd);

    RecordingRhi sink;
    list.replay(sink);

    AURORA_TEST_REQUIRE(sink.entries.size() == 1U);
    AURORA_TEST_CHECK_TRUE(sink.entries[0].image == &list.image_at(cmd.image_idx));
    AURORA_TEST_CHECK_TRUE(sink.entries[0].matrix == &list.matrix_at(cmd.matrix_idx));
}

AURORA_TEST_CASE(software_rhi_replay_matches_direct_painting_bitwise) {
    // 零行为变化红线：同一批绘制，经 SoftwareRhi 回放与直接调用 Painter 必须逐位一致。
    constexpr int AURORA_WIDTH = 24;
    constexpr int AURORA_HEIGHT = 24;

    Painter direct;
    direct.begin(AURORA_WIDTH, AURORA_HEIGHT);
    direct.fill_rect(rect_at(0.0F, 0.0F, 12.0F, 12.0F), Color::red());
    direct.draw_line(Point{.x = 0.0F, .y = 0.0F}, Point{.x = 23.0F, .y = 23.0F}, 1.0F, Color::blue());
    direct.draw_linear_gradient(rect_at(0.0F, 12.0F, 12.0F, 12.0F), Point{.x = 0.0F, .y = 12.0F},
                                Point{.x = 12.0F, .y = 12.0F}, {Color::red(), Color::green()}, {0.0F, 1.0F});

    Painter recorded;
    recorded.begin(AURORA_WIDTH, AURORA_HEIGHT);
    DisplayList list;
    recorded.record(list);
    recorded.fill_rect(rect_at(0.0F, 0.0F, 12.0F, 12.0F), Color::red());
    recorded.draw_line(Point{.x = 0.0F, .y = 0.0F}, Point{.x = 23.0F, .y = 23.0F}, 1.0F, Color::blue());
    recorded.draw_linear_gradient(rect_at(0.0F, 12.0F, 12.0F, 12.0F), Point{.x = 0.0F, .y = 12.0F},
                                  Point{.x = 12.0F, .y = 12.0F}, {Color::red(), Color::green()}, {0.0F, 1.0F});
    recorded.stop();
    list.replay(recorded);  // 走 replay(Painter&) → SoftwareRhi

    AURORA_TEST_REQUIRE(list.cmd_count() == 3U);
    AURORA_TEST_CHECK_TRUE(same_pixels(direct, recorded));

    // 同一命令流手工喂给 SoftwareRhi：结果同帧（证明 SoftwareRhi 与 replay(Painter&) 同一路径）。
    Painter via_rhi;
    via_rhi.begin(AURORA_WIDTH, AURORA_HEIGHT);
    rhi::SoftwareRhi software{via_rhi};
    list.replay(software);
    AURORA_TEST_CHECK_TRUE(same_pixels(direct, via_rhi));
}

AURORA_TEST_CASE(unbound_software_rhi_submits_are_noop) {
    rhi::SoftwareRhi software;  // 未绑定
    AURORA_TEST_CHECK_TRUE(software.painter() == nullptr);
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    cmd.color = Color::red();
    const rhi::CmdData data;
    AURORA_TEST_CHECK_NO_THROW(software.submit(cmd, data));
}

AURORA_TEST_CASE(bind_switches_target_painter) {
    Painter first;
    first.begin(4, 4);
    Painter second;
    second.begin(4, 4);

    rhi::SoftwareRhi software{first};
    AURORA_TEST_CHECK_TRUE(software.painter() == &first);
    software.bind(second);
    AURORA_TEST_CHECK_TRUE(software.painter() == &second);

    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    cmd.color = Color::red();
    const rhi::CmdData data;
    software.submit(cmd, data);

    AURORA_TEST_CHECK_EQ(static_cast<int>(second.get_pixel(0, 0).r), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(first.get_pixel(0, 0).r), 0);  // 旧目标未被写入
}

AURORA_TEST_CASE(replay_empty_list_into_recording_backend_submits_nothing) {
    const DisplayList list;
    RecordingRhi sink;
    list.replay(sink);
    AURORA_TEST_CHECK_TRUE(sink.entries.empty());
}

}  // namespace aurora::test_cases::utest_rhi
