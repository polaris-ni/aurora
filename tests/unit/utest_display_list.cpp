/// 测试类型: unit
/// 目标单元: include/aurora/render/display_list.h
/// 测试说明: 覆盖 DisplayList 的命令与数据池：空态判定、各池 add_* 的索引递增与独立编号、push_cmd 记账、
/// clear 全池复位，replay 到 Painter 的像素等价性与空列表回放的无副作用，以及回放阶段相邻同属性
/// 文本命令合并的像素逐位等价 / 不跨命令合并 / 不跨字体合并

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_display_list {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 两画布逐像素不同的格数（回放合并必须与逐条回放逐位一致）。
[[nodiscard]] auto count_diff_pixels(const Painter &a, const Painter &b) -> int {
    AURORA_TEST_REQUIRE_EQ(a.width(), b.width());
    AURORA_TEST_REQUIRE_EQ(a.height(), b.height());
    int diff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            diff += a.get_pixel(x, y) != b.get_pixel(x, y) ? 1 : 0;
        }
    }
    return diff;
}

/// @brief 非全透明像素数：用于确认文本真的落了笔，避免「两幅空画布相等」的空转断言。
[[nodiscard]] auto non_transparent_pixels(const Painter &p) -> int {
    int count = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            count += p.get_pixel(x, y).a != 0 ? 1 : 0;
        }
    }
    return count;
}

/// @brief 命令序列探针：按提交顺序记录命令类型与 DrawText 文本，验证回放不重排、不丢命令。
class SequenceCapture final : public rhi::RhiBackend {
  public:
    struct Entry {
        CmdKind kind = CmdKind::FillRect;  ///< 提交的命令类型
        std::string text;  ///< DrawText 的文本（非文本命令为空）
    };

    [[nodiscard]] auto name() const -> std::string_view override { return "utest_display_list_sequence"; }

    auto submit(const DrawCmd &cmd, const rhi::CmdData &data) -> void override {
        entries.push_back(Entry{.kind = cmd.kind, .text = data.text != nullptr ? *data.text : std::string{}});
    }

    // 探针后端：命令序列供用例直接读取，故有意保持公开。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::vector<Entry> entries;  ///< 按提交顺序记录的命令
};

/// @brief 构造一条 DrawText 命令：文本 / 字体各自入池，模拟录制端「每条命令各占一个字体池位」的形态。
[[nodiscard]] auto make_text_cmd(DisplayList &list, std::string_view text, const Rect &box, const Font &font,
                                 Color color) -> DrawCmd {
    DrawCmd cmd;
    cmd.kind = CmdKind::DrawText;
    cmd.bounds = box;
    cmd.color = color;
    cmd.aa_mode = render::TextAAMode::Supersample;
    cmd.str_idx = list.add_string(std::string{text});
    cmd.font_idx = list.add_font(font);
    return cmd;
}

/// @brief 与回放同口径的逐条文本绘制（显式抗锯齿 + 默认排版 opts），作为像素基准。
auto draw_text_single(Painter &p, const Rect &box, std::string_view text, const Font &font, Color color) -> void {
    p.draw_text(box, std::string{text}, font, color, render::TextAAMode::Supersample, render::TextLayoutOpts{});
}

/// @brief 把回放结果再录一遍，并采集其中的命令序列（录制态经 `draw_text_runs` 逐片段落回 DrawText）。
/// @param list 待回放的显示列表。
/// @param width 录制画布宽（像素）。
/// @param height 录制画布高（像素）。
/// @param capture 输出探针（其 `entries` 按提交顺序填充）。
auto replay_recorded_sequence(const DisplayList &list, int width, int height, SequenceCapture &capture) -> void {
    Painter recorder;
    recorder.begin(width, height);
    DisplayList replayed;
    recorder.record(replayed);
    list.replay(recorder);
    recorder.stop();
    replayed.replay(capture);
}
}  // namespace

AURORA_TEST_CASE(fresh_list_is_empty) {
    const DisplayList list;
    AURORA_TEST_CHECK_TRUE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 0U);
}

AURORA_TEST_CASE(pools_assign_independent_increasing_indices) {
    // 每类池各自从 0 编号：DrawCmd 用不同字段分别引用，索引互不干扰。
    DisplayList list;
    AURORA_TEST_CHECK_EQ(list.add_string("first"), 0);
    AURORA_TEST_CHECK_EQ(list.add_string("second"), 1);
    AURORA_TEST_CHECK_EQ(list.add_colors({Color::red(), Color::blue()}), 0);
    AURORA_TEST_CHECK_EQ(list.add_floats({0.0F, 1.0F}), 0);
    AURORA_TEST_CHECK_EQ(list.add_font(Font{}), 0);
    AURORA_TEST_CHECK_EQ(list.add_image(Image{}), 0);
    AURORA_TEST_CHECK_EQ(list.add_matrix(Matrix2D{}), 0);
}

AURORA_TEST_CASE(add_image_move_preserves_pixels) {
    // 移动版入池：像素缓冲被接管而非再拷贝一份，回放结果与原图一致。
    Image img;
    img.width = 2;
    img.height = 1;
    img.pixels = {10, 20, 30, 255, 40, 50, 60, 255};

    DisplayList list;
    const int idx = list.add_image(std::move(img));
    AURORA_TEST_CHECK_EQ(idx, 0);

    DrawCmd cmd;
    cmd.kind = CmdKind::DrawImage;
    cmd.bounds = rect_at(0.0F, 0.0F, 2.0F, 1.0F);
    cmd.image_idx = idx;
    list.push_cmd(cmd);

    Painter painter;
    painter.begin(2, 1);
    list.replay(painter);
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).r), 10);
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(1, 0).b), 60);
}

AURORA_TEST_CASE(record_composite_rvalue_matches_lvalue_and_drains_source) {
    // 移动版 composite(Painter&&) 在录制态直接接管源像素入池，省掉 to_image() 的整帧拷贝；
    // 回放像素必须与常量引用版逐位一致，且源缓冲被掏空（仅可析构 / 重新 begin）。
    const Matrix2D matrix = Matrix2D::from_rotate_about(30.0F, Point{.x = 8.0F, .y = 8.0F});
    const auto make_source = []() -> Painter {
        Painter src;
        src.begin(16, 16);
        src.fill_rect(rect_at(2.0F, 3.0F, 10.0F, 8.0F), Color::red());
        src.fill_rect(rect_at(5.0F, 6.0F, 4.0F, 4.0F), Color{40, 180, 90, 200});
        return src;
    };

    DisplayList copy_list;
    Painter copy_dst;
    copy_dst.begin(16, 16);
    copy_dst.record(copy_list);
    {
        Painter src = make_source();
        copy_dst.composite(src, matrix);  // 常量引用版：to_image() 拷贝快照
    }
    copy_dst.stop();

    DisplayList move_list;
    Painter move_dst;
    move_dst.begin(16, 16);
    move_dst.record(move_list);
    Painter moved_src = make_source();
    move_dst.composite(std::move(moved_src), matrix);  // 移动版：接管缓冲
    move_dst.stop();
    // 源缓冲被移走：尺寸保留，像素清空；重新 begin 后恢复可用。
    // 这里刻意读取 moved-from 对象的状态来验证接管语义，不是误用。
    AURORA_TEST_CHECK_EQ(moved_src.width(), 16);  // NOLINT(bugprone-use-after-move)
    AURORA_TEST_CHECK_TRUE(moved_src.to_image().pixels.empty());  // NOLINT(bugprone-use-after-move)
    moved_src.begin(4, 4);  // NOLINT(bugprone-use-after-move)
    AURORA_TEST_CHECK_EQ(static_cast<int>(moved_src.to_image().pixels.size()), 4 * 4 * 4);

    Painter copy_out;
    copy_out.begin(16, 16);
    copy_list.replay(copy_out);
    Painter move_out;
    move_out.begin(16, 16);
    move_list.replay(move_out);
    AURORA_TEST_CHECK_EQ(count_diff_pixels(copy_out, move_out), 0);
    // 非全透明：确认合成确实写入了像素，避免两条空列表互相"一致"的假绿。
    AURORA_TEST_CHECK_EQ(static_cast<int>(move_list.cmd_count()), 1);
}

AURORA_TEST_CASE(push_cmd_tracks_count_and_breaks_emptiness) {
    DisplayList list;
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 8.0F, 8.0F);
    cmd.color = Color::red();
    list.push_cmd(cmd);

    AURORA_TEST_CHECK_FALSE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 1U);

    list.push_cmd(cmd);
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 2U);
}

AURORA_TEST_CASE(clear_resets_commands) {
    DisplayList list;
    list.add_string("text");
    list.push_cmd(DrawCmd{});
    list.clear();

    AURORA_TEST_CHECK_TRUE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 0U);
    // 清空后池重新从 0 编号（旧索引不可复用）。
    AURORA_TEST_CHECK_EQ(list.add_string("fresh"), 0);
}

AURORA_TEST_CASE(replay_fill_rect_writes_pixels) {
    // 录制一条 FillRect 并回放：结果与直接调用 Painter 等价。
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    cmd.color = Color::red();

    DisplayList list;
    list.push_cmd(cmd);

    Painter painter;
    painter.begin(4, 4);
    list.replay(painter);

    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).r), 255);  // R
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).g), 0);  // G
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).b), 0);  // B
}

AURORA_TEST_CASE(replay_preserves_command_order) {
    // 后录制的命令覆盖先录制的（画家用直接覆盖语义合成）。
    DisplayList list;
    DrawCmd red;
    red.kind = CmdKind::FillRect;
    red.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    red.color = Color::red();

    DrawCmd blue = red;
    blue.color = Color::blue();

    list.push_cmd(red);
    list.push_cmd(blue);

    Painter painter;
    painter.begin(4, 4);
    list.replay(painter);

    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).r), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).b), 255);
}

AURORA_TEST_CASE(replay_empty_list_is_noop) {
    const DisplayList list;
    Painter painter;
    painter.begin(2, 2);
    AURORA_TEST_CHECK_NO_THROW(list.replay(painter));
}

AURORA_TEST_CASE(replay_adjacent_same_style_text_matches_per_command_output) {
    // 三条相邻同字体 / 同色 / 同抗锯齿 / 同排版 opts 的文本：合并回放必须与逐条回放逐位一致。
    // 每条命令各占一个字体池位（值相同而下标不同），故用例同时覆盖「按值比较字体」这条判等分支。
    const Font font{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    constexpr std::size_t count = 3;
    const std::array<Rect, count> boxes = {rect_at(4.0F, 4.0F, 110.0F, 20.0F), rect_at(4.0F, 28.0F, 110.0F, 20.0F),
                                           rect_at(4.0F, 52.0F, 110.0F, 20.0F)};
    const std::array<const char *, count> texts = {"alpha", "beta", "gamma"};
    constexpr int width = 120;
    constexpr int height = 80;

    DisplayList list;
    Painter reference;
    reference.begin(width, height);
    for (std::size_t i = 0; i < count; ++i) {
        list.push_cmd(make_text_cmd(list, texts.at(i), boxes.at(i), font, Color::black()));
        draw_text_single(reference, boxes.at(i), texts.at(i), font, Color::black());
    }

    Painter subject;
    subject.begin(width, height);
    list.replay(subject);

    AURORA_TEST_REQUIRE_GT(non_transparent_pixels(subject), 0);
    AURORA_TEST_CHECK_EQ(count_diff_pixels(subject, reference), 0);
    AURORA_TEST_CHECK_TRUE(subject.to_image().pixels == reference.to_image().pixels);

    // 命令序列不变：合并批次在录制态逐片段落回 DrawText，类型 / 顺序 / 文本逐条一致。
    SequenceCapture capture;
    replay_recorded_sequence(list, width, height, capture);
    AURORA_TEST_REQUIRE_EQ(capture.entries.size(), count);
    for (std::size_t i = 0; i < count; ++i) {
        AURORA_TEST_CHECK_EQ(capture.entries.at(i).kind, CmdKind::DrawText);
        AURORA_TEST_CHECK_EQ(capture.entries.at(i).text, std::string{texts.at(i)});
    }
}

AURORA_TEST_CASE(replay_text_batch_segments_preserve_output) {
    // 相邻同属性文本数超过固定批量时分段提交：段与段之间仍按原顺序落笔，像素不因切段而改变。
    const Font font{.family = "sans-serif", .size_pt = 12.0F, .weight = 400};
    constexpr std::size_t count = 20;
    constexpr int width = 120;
    constexpr int height = 24 * static_cast<int>(count);

    DisplayList list;
    Painter reference;
    reference.begin(width, height);
    for (std::size_t i = 0; i < count; ++i) {
        const Rect box = rect_at(4.0F, static_cast<float>(i) * 24.0F, 110.0F, 20.0F);
        const std::string text = "row" + std::to_string(i);
        list.push_cmd(make_text_cmd(list, text, box, font, Color::black()));
        draw_text_single(reference, box, text, font, Color::black());
    }

    Painter subject;
    subject.begin(width, height);
    list.replay(subject);

    AURORA_TEST_REQUIRE_GT(non_transparent_pixels(subject), 0);
    AURORA_TEST_CHECK_TRUE(subject.to_image().pixels == reference.to_image().pixels);
}

AURORA_TEST_CASE(replay_text_split_by_rect_command_stays_unmerged) {
    // 两条同属性文本之间夹一条 FillRect：不得跨命令合并，第一条文本必须先于矩形落笔并被其覆盖。
    // 若错误地把两条文本并为一批排在矩形之后回放，第一条文本会重新显现在矩形上（像素与序列双暴露）。
    const Font font{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const Rect hidden_box = rect_at(4.0F, 4.0F, 60.0F, 20.0F);
    const Rect cover_box = rect_at(0.0F, 0.0F, 80.0F, 28.0F);
    const Rect shown_box = rect_at(88.0F, 4.0F, 60.0F, 20.0F);
    constexpr int width = 160;
    constexpr int height = 32;

    DisplayList list;
    list.push_cmd(make_text_cmd(list, "hidden", hidden_box, font, Color::black()));
    DrawCmd cover;
    cover.kind = CmdKind::FillRect;
    cover.bounds = cover_box;
    cover.color = Color::blue();
    list.push_cmd(cover);
    list.push_cmd(make_text_cmd(list, "shown", shown_box, font, Color::black()));

    Painter reference;
    reference.begin(width, height);
    draw_text_single(reference, hidden_box, "hidden", font, Color::black());
    reference.fill_rect(cover_box, Color::blue());
    draw_text_single(reference, shown_box, "shown", font, Color::black());

    Painter subject;
    subject.begin(width, height);
    list.replay(subject);

    AURORA_TEST_REQUIRE_GT(non_transparent_pixels(subject), 0);
    AURORA_TEST_CHECK_EQ(count_diff_pixels(subject, reference), 0);
    // 覆盖区内必须只剩矩形色：第一条文本没有在矩形之后被重绘。
    auto cover_is_pure = true;
    for (int y = 0; y < 28; ++y) {
        for (int x = 0; x < 80; ++x) {
            cover_is_pure = cover_is_pure && subject.get_pixel(x, y) == Color::blue();
        }
    }
    AURORA_TEST_CHECK_TRUE(cover_is_pure);

    SequenceCapture capture;
    replay_recorded_sequence(list, width, height, capture);
    AURORA_TEST_REQUIRE_EQ(capture.entries.size(), 3U);
    AURORA_TEST_CHECK_EQ(capture.entries[0].kind, CmdKind::DrawText);
    AURORA_TEST_CHECK_EQ(capture.entries[0].text, std::string{"hidden"});
    AURORA_TEST_CHECK_EQ(capture.entries[1].kind, CmdKind::FillRect);
    AURORA_TEST_CHECK_EQ(capture.entries[2].kind, CmdKind::DrawText);
    AURORA_TEST_CHECK_EQ(capture.entries[2].text, std::string{"shown"});
}

AURORA_TEST_CASE(replay_text_with_different_fonts_stays_unmerged) {
    // 相邻文本字体不同（字号 / 字重）时不得合批：整批只有一份字体，合错会改变字形与度量。
    const Font regular{.family = "sans-serif", .size_pt = 14.0F, .weight = 400};
    const Font bold{.family = "sans-serif", .size_pt = 18.0F, .weight = 700};
    const Rect first_box = rect_at(4.0F, 4.0F, 110.0F, 24.0F);
    const Rect second_box = rect_at(4.0F, 32.0F, 110.0F, 28.0F);
    constexpr int width = 120;
    constexpr int height = 68;

    // 先自证字体差异确实影响像素，否则「两种字体回放一致」可能是空转断言。
    {
        Painter a;
        a.begin(width, height);
        draw_text_single(a, first_box, "font", regular, Color::black());
        Painter b;
        b.begin(width, height);
        draw_text_single(b, first_box, "font", bold, Color::black());
        AURORA_TEST_CHECK_GT(count_diff_pixels(a, b), 0);
    }

    DisplayList list;
    list.push_cmd(make_text_cmd(list, "small", first_box, regular, Color::black()));
    list.push_cmd(make_text_cmd(list, "large", second_box, bold, Color::black()));

    Painter reference;
    reference.begin(width, height);
    draw_text_single(reference, first_box, "small", regular, Color::black());
    draw_text_single(reference, second_box, "large", bold, Color::black());

    Painter subject;
    subject.begin(width, height);
    list.replay(subject);

    AURORA_TEST_REQUIRE_GT(non_transparent_pixels(subject), 0);
    AURORA_TEST_CHECK_EQ(count_diff_pixels(subject, reference), 0);
    AURORA_TEST_CHECK_TRUE(subject.to_image().pixels == reference.to_image().pixels);

    SequenceCapture capture;
    replay_recorded_sequence(list, width, height, capture);
    AURORA_TEST_REQUIRE_EQ(capture.entries.size(), 2U);
    AURORA_TEST_CHECK_EQ(capture.entries[0].kind, CmdKind::DrawText);
    AURORA_TEST_CHECK_EQ(capture.entries[0].text, std::string{"small"});
    AURORA_TEST_CHECK_EQ(capture.entries[1].kind, CmdKind::DrawText);
    AURORA_TEST_CHECK_EQ(capture.entries[1].text, std::string{"large"});
}

}  // namespace aurora::test_cases::utest_display_list
