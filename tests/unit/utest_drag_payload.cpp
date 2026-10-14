/// 测试类型: unit
/// 目标单元: include/aurora/event/event.h（DragPayload 载荷读取与 system_drag 拖出会话）
///           + include/aurora/window/platform.h（系统拖放能力位的立骨期锚定）
/// 测试说明: 以内存载荷覆盖按 MIME 读取与 files / text 便利读取的往返与空口径；以 system_drag
/// 入口覆盖「无后端 = 机制缺失」与 inert 测试后端三件套；末尾锚定两个能力位在全部 SurfaceKind 下
/// 恒为 false（防「未接线先声称」）。全程不触碰平台，不建真实窗口。

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/event/event.h"
#include "aurora/window/platform.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_drag_payload {

namespace {

/// 由文本构造字节向量（字面量一律 ASCII：MIME 与路径内容都走 ASCII，避免终端代码页依赖）。
auto to_bytes(const std::string &s) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(s.size());
    for (const char c : s) {
        bytes.push_back(static_cast<std::uint8_t>(c));
    }
    return bytes;
}

/// 构造一份「两条本地路径 + 一段文本」的 uri-list / plain 双格式载荷。
auto make_file_payload() -> std::shared_ptr<const DragPayload> {
    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_URI_LIST), to_bytes("file:///C:/docs/a.txt\r\nfile:///C:/docs/b b.txt\r\n"));
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("plain payload"));
    return DragPayload::from_bytes(std::move(data));
}

}  // namespace

AURORA_TEST_CASE(payload_roundtrips_files_and_text_by_mime) {
    const auto payload = make_file_payload();

    AURORA_TEST_CHECK_TRUE(payload->has(std::string(AURORA_MIME_URI_LIST)));
    AURORA_TEST_CHECK_TRUE(payload->has(std::string(AURORA_MIME_TEXT_PLAIN)));

    const auto files = payload->files();
    AURORA_TEST_REQUIRE(files.ok());
    AURORA_TEST_REQUIRE_EQ(files.value().size(), 2U);
    AURORA_TEST_CHECK_EQ(files.value()[0], std::string{"C:/docs/a.txt"});
    AURORA_TEST_CHECK_EQ(files.value()[1], std::string{"C:/docs/b b.txt"});  // 百分号编码的空格还原

    const auto text = payload->text();
    AURORA_TEST_REQUIRE(text.ok());
    AURORA_TEST_CHECK_EQ(text.value(), std::string{"plain payload"});

    const auto raw = payload->bytes(std::string(AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_REQUIRE(raw.ok());
    AURORA_TEST_CHECK_EQ(raw.value(), to_bytes("plain payload"));
}

AURORA_TEST_CASE(payload_missing_format_is_ok_empty_not_error) {
    const auto payload = make_file_payload();

    // 空口径：格式不存在 = Ok 空值。空内容不是失败（与 Clipboard 同口径）。
    const auto missing = payload->bytes("application/x-nothing");
    AURORA_TEST_REQUIRE(missing.ok());
    AURORA_TEST_CHECK(missing.value().empty());
    AURORA_TEST_CHECK_FALSE(payload->has("application/x-nothing"));

    // 便利读取同理：无该格式时返回空清单 / 空串，而不是 Error。
    const auto empty_only = DragPayload::from_bytes({});
    AURORA_TEST_REQUIRE(empty_only->files().ok());
    AURORA_TEST_CHECK(empty_only->files().value().empty());
    AURORA_TEST_REQUIRE(empty_only->text().ok());
    AURORA_TEST_CHECK(empty_only->text().value().empty());
    AURORA_TEST_REQUIRE(empty_only->image().ok());
    AURORA_TEST_CHECK_EQ(empty_only->image().value().width, 0);
}

AURORA_TEST_CASE(payload_decoding_skips_non_file_schemes) {
    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_URI_LIST), to_bytes("https://example.com/x\r\nfile:///C:/docs/c.txt\r\n"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    // 非 file scheme 的行不映射为路径、静默跳过；完整原始行仍可经 bytes(uri-list) 取得。
    const auto files = payload->files();
    AURORA_TEST_REQUIRE(files.ok());
    AURORA_TEST_REQUIRE_EQ(files.value().size(), 1U);
    AURORA_TEST_CHECK_EQ(files.value()[0], std::string{"C:/docs/c.txt"});

    const auto raw = payload->bytes(std::string(AURORA_MIME_URI_LIST));
    AURORA_TEST_REQUIRE(raw.ok());
    AURORA_TEST_CHECK(raw.value().size() > files.value()[0].size());  // 原始行保留了被跳过的那条
}

AURORA_TEST_CASE(system_drag_begin_without_backend_reports_missing_mechanism) {
    // 复位入口状态：前序用例可能把后端留在安装态。
    (void)system_drag::remove_test_backend();

    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };

    const auto action = system_drag::begin(offer);
    AURORA_TEST_REQUIRE_FALSE(action.ok());
    AURORA_TEST_CHECK_EQ(action.error().code_enum, ErrorCode::SystemDragNotSupported);
    AURORA_TEST_CHECK_EQ(action.error().code, std::string{"system-drag-not-supported"});
    AURORA_TEST_CHECK_FALSE(action.error().retryable);  // 机制缺失是稳态，重试无意义
}

AURORA_TEST_CASE(system_drag_begin_rejects_malformed_offer) {
    // 空 formats 与空 provide 都是调用方参数问题，与「平台没机制」分开报。
    DragOffer empty_formats;
    empty_formats.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
        return std::vector<std::uint8_t>{};
    };
    const auto no_formats = system_drag::begin(empty_formats);
    AURORA_TEST_REQUIRE_FALSE(no_formats.ok());
    AURORA_TEST_CHECK_EQ(no_formats.error().code_enum, ErrorCode::GeneralInvalidArgument);

    DragOffer no_provide;
    no_provide.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    const auto no_callback = system_drag::begin(no_provide);
    AURORA_TEST_REQUIRE_FALSE(no_callback.ok());
    AURORA_TEST_CHECK_EQ(no_callback.error().code_enum, ErrorCode::GeneralInvalidArgument);
}

AURORA_TEST_CASE(system_drag_test_backend_lifecycle) {
    // 未安装时卸载为 no-op（不触碰平台）。
    (void)system_drag::remove_test_backend();
    AURORA_TEST_CHECK_FALSE(system_drag::remove_test_backend());

    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };
    const auto action = system_drag::begin(offer);
    AURORA_TEST_REQUIRE(action.ok());
    AURORA_TEST_CHECK_EQ(action.value(), DragAction::Copy);  // inert 后端的成功动作

    // 安装态下卸载：确有后端被卸下，随后回到「无后端」语义，再卸载即为 no-op。
    AURORA_TEST_CHECK_TRUE(system_drag::remove_test_backend());
    AURORA_TEST_REQUIRE_FALSE(system_drag::begin(offer).ok());
    AURORA_TEST_CHECK_FALSE(system_drag::remove_test_backend());

    // reset 语义：inert 后端无内容，复位即退装——此后 begin 同样落回「机制缺失」。
    AURORA_TEST_REQUIRE(system_drag::install_test_backend());
    system_drag::reset_test_backend();
    AURORA_TEST_REQUIRE_FALSE(system_drag::begin(offer).ok());
    AURORA_TEST_CHECK_FALSE(system_drag::remove_test_backend());  // reset 已清掉安装态
}

AURORA_TEST_CASE(capability_bits_for_system_drag_are_false_until_backends_land) {
    // 立骨期锚定：后端尚未接线，任何 SurfaceKind 下都不得声称支持系统拖放。
    // 接线后会翻位，届时本用例由接线方按实测替换（它是过渡锚点，不是永久契约）。
    const SurfaceKind kinds[] = {SurfaceKind::Headless, SurfaceKind::Win32,   SurfaceKind::Glfw,  SurfaceKind::D3D11,
                                 SurfaceKind::X11,      SurfaceKind::Wayland, SurfaceKind::MacOS, SurfaceKind::Wasm};
    for (const SurfaceKind kind : kinds) {
        Platform p;
        p.surface = kind;
        const auto caps = p.capabilities();
        AURORA_TEST_CHECK_FALSE(caps.system_drag_in);
        AURORA_TEST_CHECK_FALSE(caps.system_drag_out);
    }
}

}  // namespace aurora::test_cases::utest_drag_payload
