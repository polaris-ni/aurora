/// 测试类型: unit
/// 目标单元: include/aurora/event/event.h（DragPayload 载荷读取与 system_drag 拖出会话）
///           + include/aurora/window/platform.h（系统拖放能力位的立骨期锚定）
/// 测试说明: 以内存载荷覆盖按 MIME 读取与 files / text 便利读取的往返与空口径；以 system_drag
/// 入口覆盖「无后端 = 机制缺失」与 inert 测试后端三件套；末尾锚定两个能力位在全部 SurfaceKind 下
/// 恒为 false（防「未接线先声称」）。全程不触碰平台，不建真实窗口。
/// 非阻塞读取段覆盖「发起即返回、帧循环排空兑现」（大载荷经 SystemDropEvent 便利读取与
/// DragPayload 直连两条路）、并发读取互不取消、回调在排空阶段内兑现、宿主不排空即不兑现
/// （不谎报）、会话收口的就地兑现与只调一次、墙钟超时的 read-failed 口径；
/// 事件驱动段覆盖 begin 不阻塞（Ok 不等于落放成功）、终态经帧循环投递、cancel 时机不受限、
/// Move 确认严格早于终态投递与删除失败的回滚上报、拖拽影像与热点偏移的字段往返；
/// Widget 适配层段覆盖五种路径的回调序列（被拒与中途异常序列逐字相同、靠 outcome 区分）、
/// 无要约不派发终态、控件销毁即取消会话（provide 不再被回调、终结回调不解引用已回收对象）、
/// begin 回调内树写操作不触发嵌套派发

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"
#include "aurora/window/platform.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_drag_payload {

namespace {

using aurora::testing::require_value;  ///< 取 optional 值并断言 engaged（规避 bugprone-unchecked-optional-access）

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

/// 单格式文本要约（拖出侧测试的常用基底）。
auto make_text_offer() -> DragOffer {
    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };
    return offer;
}

/// 结局名（回调序列台账用可读标签，而非裸枚举值）。
auto outcome_name(SystemDragOutcome outcome) -> const char * {
    switch (outcome) {
        case SystemDragOutcome::Succeeded:
            return "Succeeded";
        case SystemDragOutcome::Cancelled:
            return "Cancelled";
        case SystemDragOutcome::Rejected:
            return "Rejected";
        case SystemDragOutcome::Failed:
            break;
    }
    return "Failed";
}

/// 动作名（同上）。
auto action_name(DragAction action) -> const char * {
    switch (action) {
        case DragAction::Copy:
            return "Copy";
        case DragAction::Move:
            return "Move";
        case DragAction::Link:
            return "Link";
        case DragAction::None:
            break;
    }
    return "None";
}

/// @brief 拖出会话适配层（`Widget::begin_system_drag`）的测试控件：要约由用例给定，
///        回调序列记入台账（`begin` / `cancel` / `end(结局,动作)`）。
class DragSourceBox final : public Widget {
  public:
    std::optional<DragOffer> offer;  ///< 由用例给定；`std::nullopt` = 控件自称不可拖出
    std::vector<std::string> sequence;  ///< 回调序列台账
    SystemDragOutcome last_outcome = SystemDragOutcome::Failed;  ///< 最近一次终态结局
    DragAction last_action = DragAction::None;  ///< 最近一次终态动作
    int end_calls = 0;  ///< 终态回调次数（「只调一次」判据）
    std::function<void()> on_begin_hook;  ///< `on_system_drag_begin` 内的附加动作（无重入探针）
    std::function<void()> on_end_hook;  ///< `on_system_drag_end` 内的附加动作（调用序号探针）

    [[nodiscard]] auto type_name() const -> const char * override { return "DragSourceBox"; }

    [[nodiscard]] auto on_provide_drag() -> std::optional<DragOffer> override { return offer; }

    auto on_system_drag_begin() -> void override {
        sequence.emplace_back("begin");
        if (on_begin_hook) {
            on_begin_hook();
        }
    }

    auto on_system_drag_cancel() -> void override { sequence.emplace_back("cancel"); }

    auto on_system_drag_end(SystemDragOutcome outcome, DragAction action) -> void override {
        ++end_calls;
        last_outcome = outcome;
        last_action = action;
        if (on_end_hook) {
            on_end_hook();
        }
        sequence.emplace_back(std::string{"end("} + outcome_name(outcome) + "," + action_name(action) + ")");
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

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

    const auto started = system_drag::begin(offer, {});
    AURORA_TEST_REQUIRE_FALSE(started.ok());
    AURORA_TEST_CHECK_EQ(started.error().code_enum, ErrorCode::SystemDragNotSupported);
    AURORA_TEST_CHECK_EQ(started.error().code, std::string{"system-drag-not-supported"});
    AURORA_TEST_CHECK_FALSE(started.error().retryable);  // 机制缺失是稳态，重试无意义
}

AURORA_TEST_CASE(system_drag_begin_rejects_malformed_offer) {
    // 空 formats 与空 provide 都是调用方参数问题，与「平台没机制」分开报。
    DragOffer empty_formats;
    empty_formats.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
        return std::vector<std::uint8_t>{};
    };
    const auto no_formats = system_drag::begin(empty_formats, {});
    AURORA_TEST_REQUIRE_FALSE(no_formats.ok());
    AURORA_TEST_CHECK_EQ(no_formats.error().code_enum, ErrorCode::GeneralInvalidArgument);

    DragOffer no_provide;
    no_provide.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    const auto no_callback = system_drag::begin(no_provide, {});
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
    // 发起即返回：Ok 只表「会话已发起」，最终动作改由 on_end 投递（见 on_system_drag_end 系列用例）。
    const auto started = system_drag::begin(offer, {});
    AURORA_TEST_REQUIRE(started.ok());

    // 安装态下卸载：确有后端被卸下，随后回到「无后端」语义，再卸载即为 no-op。
    AURORA_TEST_CHECK_TRUE(system_drag::remove_test_backend());
    AURORA_TEST_REQUIRE_FALSE(system_drag::begin(offer, {}).ok());
    AURORA_TEST_CHECK_FALSE(system_drag::remove_test_backend());

    // reset 语义：inert 后端无内容，复位即退装——此后 begin 同样落回「机制缺失」。
    AURORA_TEST_REQUIRE(system_drag::install_test_backend());
    system_drag::reset_test_backend();
    AURORA_TEST_REQUIRE_FALSE(system_drag::begin(offer, {}).ok());
    AURORA_TEST_CHECK_FALSE(system_drag::remove_test_backend());  // reset 已清掉安装态
}

AURORA_TEST_CASE(capability_bits_for_system_drag_track_wired_backends_per_direction) {
    // 方向级分叉锚定：两个能力位**按各自后端独立翻位**（不是成对翻位），故每一方向对每个
    // SurfaceKind 都有明确期望值。接线方落地哪个方向就翻哪个方向，本表随之更新——
    // 「未接线先声称」的变异（为未接线方向翻位）会让对应用例转红。
    struct Expectation {
        SurfaceKind kind;
        bool drag_in;
        bool drag_out;
    };
    // Win32 / D3D11 共用同一个 `Win32Host` 与同一 OLE 拖入桥实例（D3D11 只是上屏形态），故同翻；
    // 拖出同批接线（`IDropSource` + `DoDragDrop` + 库自管拖拽线程，注册面进程级常驻、与窗口句柄
    // 无关），两个方向对 Win32 / D3D11 均翻位。
    // Glfw 恒 false：零 `#ifdef` 公共头无法按宿主 OS 分叉，且 GLFW 自持 drop 不承载四态协商。
    // X11 两个方向同批接线（Xdnd 单向消息协议：拖入走 `XdndAware` + selection round-trip，
    // 拖出走指针 grab + `XdndSelection` 所有权，会话跑在 UI 线程上）；
    // Wayland 两个方向同批接线（`wl_data_device` / `wl_data_source` + `start_drag`）；
    // MacOS 两个方向同批接线（`NSDraggingDestination` 五方法 / `NSDraggingSource` +
    // `beginDraggingSessionWithItems:event:source:`，取数同步发生在 `performDragOperation` 内）。
    // 三个平台的会话都跑在 UI 线程上（不阻塞事件泵），与 Win32 的嵌套模态循环不同。
    const Expectation table[] = {
        {.kind = SurfaceKind::Headless, .drag_in = false, .drag_out = false},
        {.kind = SurfaceKind::Win32, .drag_in = true, .drag_out = true},
        {.kind = SurfaceKind::Glfw, .drag_in = false, .drag_out = false},
        {.kind = SurfaceKind::D3D11, .drag_in = true, .drag_out = true},
        {.kind = SurfaceKind::X11, .drag_in = true, .drag_out = true},
        {.kind = SurfaceKind::Wayland, .drag_in = true, .drag_out = true},
        {.kind = SurfaceKind::MacOS, .drag_in = true, .drag_out = true},
        {.kind = SurfaceKind::Wasm, .drag_in = false, .drag_out = false},
    };
    for (const Expectation &row : table) {
        Platform p;
        p.surface = row.kind;
        const auto caps = p.capabilities();
        AURORA_TEST_CHECK_EQ(static_cast<int>(caps.system_drag_in), static_cast<int>(row.drag_in));
        AURORA_TEST_CHECK_EQ(static_cast<int>(caps.system_drag_out), static_cast<int>(row.drag_out));
    }
}

// ---- 非阻塞读取：发起即登记、帧循环排空兑现 ----

AURORA_TEST_CASE(async_read_does_not_block_the_frame_loop) {
    // 大载荷（8 MiB）走两条路：事件层便利读取（SystemDropEvent::text_async）与
    // DragPayload 直连（read_async）。契约：登记后同步侧零兑现（发起不阻塞），
    // pump 一拍全量兑现且只兑现一次，内容与同步读取全等。
    // 本用例依赖排空阶段钩子（test_in_drain_phase / test_withhold_format / test_set_read_timeout），
    // 仅在 inert 后端编入时（AURORA_ENABLE_DEBUG + AURORA_ENABLE_TEST_HOOKS）可观测；否则跳过。
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    constexpr std::size_t payload_bytes = 8U << 20U;  // 8 MiB：同步读完足以卡住帧循环的量级
    std::vector<std::uint8_t> big(payload_bytes);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::uint8_t>(((i * 31U) + 7U) & 0xFFU);  // 确定性图案
    }
    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), big);
    const auto payload = DragPayload::from_bytes(std::move(data));

    int event_calls = 0;
    int direct_calls = 0;
    bool event_in_drain = false;
    bool direct_in_drain = false;
    std::string event_text;
    std::vector<std::uint8_t> direct_bytes;

    SystemDropEvent ev;
    ev.payload = payload;
    ev.text_async([&](const Result<std::string> &r) {
        ++event_calls;
        event_in_drain = system_drag::test_in_drain_phase();
        if (r) {
            event_text = r.value();
        }
    });
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++direct_calls;
        direct_in_drain = system_drag::test_in_drain_phase();
        if (r) {
            direct_bytes = r.value();
        }
    });

    // 登记即返回：同步侧一次都没兑现（发起不阻塞、不谎报）。
    AURORA_TEST_CHECK_EQ(event_calls, 0);
    AURORA_TEST_CHECK_EQ(direct_calls, 0);

    system_drag::pump_reads();

    // 一拍兑现、只一次；回调发生在排空阶段内（帧内兑现而非消息泵内同步回调）。
    AURORA_TEST_CHECK_EQ(event_calls, 1);
    AURORA_TEST_CHECK_EQ(direct_calls, 1);
    AURORA_TEST_CHECK_TRUE(event_in_drain);
    AURORA_TEST_CHECK_TRUE(direct_in_drain);

    // 内容与同步读取全等（大对象走布尔全等，避免失败时把 8 MiB 打进诊断输出）。
    const std::string expected(big.begin(), big.end());
    AURORA_TEST_CHECK(event_text == expected);
    AURORA_TEST_CHECK(direct_bytes == big);

    // 变异自证：同一载荷改走「扣留 + 短超时」——必须拿到 read-failed 而非永远挂起或
    // 假成功。缺了这一段，「pump 一拍兑现」被实现成无条件立即兑现也可能照样全绿。
    system_drag::test_set_read_timeout(std::chrono::milliseconds{20});
    system_drag::test_withhold_format(std::string(AURORA_MIME_TEXT_PLAIN));
    int probe_calls = 0;
    ErrorCode probe_code = ErrorCode::GeneralUnknown;
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++probe_calls;
        if (!r) {
            probe_code = r.error().code_enum;
        }
    });
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(probe_calls, 0);  // 未到位且未超时：留到下一拍
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(probe_calls, 1);
    AURORA_TEST_CHECK_EQ(probe_code, ErrorCode::SystemDragReadFailed);
    system_drag::test_set_read_timeout(std::chrono::milliseconds{0});  // 复位为默认超时
    (void)system_drag::remove_test_backend();  // 清扣留记录，避免污染后续用例
}

AURORA_TEST_CASE(concurrent_async_reads_are_independent) {
    // 并发登记的多个读取互不取消：同一 MIME 的两次读取各自兑现一次；缺失格式按空口径
    // 兑现（Ok 空值而非 Error）——「空」与「败」在异步路上同样分开。
    (void)system_drag::remove_test_backend();

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("shared"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    int text_calls = 0;
    int missing_calls = 0;
    int text_again_calls = 0;
    std::vector<std::uint8_t> first_bytes;
    std::vector<std::uint8_t> second_bytes;
    bool missing_ok = false;
    bool missing_empty = false;

    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++text_calls;
        if (r) {
            first_bytes = r.value();
        }
    });
    payload->read_async(std::string(AURORA_MIME_IMAGE_PNG), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++missing_calls;
        missing_ok = r.ok();
        missing_empty = r.ok() && r.value().empty();
    });
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++text_again_calls;
        if (r) {
            second_bytes = r.value();
        }
    });

    AURORA_TEST_CHECK_EQ(text_calls, 0);
    AURORA_TEST_CHECK_EQ(missing_calls, 0);
    AURORA_TEST_CHECK_EQ(text_again_calls, 0);

    system_drag::pump_reads();

    // 一拍全部兑现、各只一次：后登记的同格式读取没有顶掉前一个。
    AURORA_TEST_CHECK_EQ(text_calls, 1);
    AURORA_TEST_CHECK_EQ(missing_calls, 1);
    AURORA_TEST_CHECK_EQ(text_again_calls, 1);
    AURORA_TEST_CHECK(first_bytes == to_bytes("shared"));
    AURORA_TEST_CHECK(second_bytes == to_bytes("shared"));
    AURORA_TEST_CHECK_TRUE(missing_ok);
    AURORA_TEST_CHECK_TRUE(missing_empty);

    // 再排空一拍：已兑现项不重复投递（回调只调一次）。
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(text_calls, 1);
    AURORA_TEST_CHECK_EQ(missing_calls, 1);
    AURORA_TEST_CHECK_EQ(text_again_calls, 1);
}

AURORA_TEST_CASE(async_callback_runs_in_drain_phase_not_message_pump) {
    // 「帧内排空、不在平台消息泵内同步回调」的可判定形式：登记时不在排空阶段，
    // 回调执行时在排空阶段；宿主不排空 ⇒ 回调不到（不谎报）。
    // 依赖 test_in_drain_phase 钩子，仅在 inert 后端编入时可观测；否则跳过。
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("drain-probe"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    const bool in_drain_at_registration = system_drag::test_in_drain_phase();
    int calls = 0;
    bool in_drain_in_callback = false;
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &) {
        ++calls;
        in_drain_in_callback = system_drag::test_in_drain_phase();
    });

    AURORA_TEST_CHECK_FALSE(in_drain_at_registration);
    AURORA_TEST_CHECK_EQ(calls, 0);

    system_drag::pump_reads();

    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_TRUE(in_drain_in_callback);
    // 排空结束标记即复位：下一轮登记重新落在排空之外（回调内再入排空的嵌套路径同理）。
    AURORA_TEST_CHECK_FALSE(system_drag::test_in_drain_phase());
}

AURORA_TEST_CASE(without_drain_the_callback_never_fires) {
    // 异步结果（读取与终态）只可能经帧循环排空兑现：不排空 ⇒ 永远不到。先等一小段
    // 墙钟再断言零兑现——排除「只是兑现得慢」的解释；随后排空一拍，两者齐至。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("ready"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    std::vector<std::string> ledger;
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN),
                        [&ledger](const Result<std::vector<std::uint8_t>> &) { ledger.emplace_back("read"); });
    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), [&ledger](SystemDragOutcome, DragAction) {
                            ledger.emplace_back("end");
                        }).ok());
    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Copy);

    AURORA_TEST_CHECK(ledger.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    AURORA_TEST_CHECK(ledger.empty());  // 等过一拍墙钟仍零兑现：不是慢，是压根不投递

    system_drag::pump_reads();
    AURORA_TEST_REQUIRE_EQ(ledger.size(), 2U);
    AURORA_TEST_CHECK_EQ(ledger[0], std::string{"read"});  // 先兑现读取、后投递终态
    AURORA_TEST_CHECK_EQ(ledger[1], std::string{"end"});
}

AURORA_TEST_CASE(session_end_settles_in_flight_reads_in_place) {
    // 会话收口（cancel）就地兑现全部在途读取：有数据即兑现数据，扣留未到位即按
    // read-failed 兑现——不等帧循环、不悬挂；终态本身仍走帧循环投递、只投一次。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("ready-bytes"));
    data.emplace(std::string(AURORA_MIME_IMAGE_PNG), to_bytes("never-arrives"));
    const auto payload = DragPayload::from_bytes(std::move(data));
    system_drag::test_withhold_format(std::string(AURORA_MIME_IMAGE_PNG));

    int ready_calls = 0;
    int withheld_calls = 0;
    std::vector<std::uint8_t> ready_bytes;
    ErrorCode withheld_code = ErrorCode::GeneralUnknown;
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++ready_calls;
        if (r) {
            ready_bytes = r.value();
        }
    });
    payload->read_async(std::string(AURORA_MIME_IMAGE_PNG), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++withheld_calls;
        if (!r) {
            withheld_code = r.error().code_enum;
        }
    });

    int end_calls = 0;
    SystemDragOutcome end_outcome = SystemDragOutcome::Failed;
    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), [&](SystemDragOutcome o, DragAction) {
                            ++end_calls;
                            end_outcome = o;
                        }).ok());

    system_drag::cancel();

    // 就地收口：cancel 返回时两个读取都已兑现（同步发生在 cancel 内，而非等帧循环）。
    AURORA_TEST_CHECK_EQ(ready_calls, 1);
    AURORA_TEST_CHECK_EQ(withheld_calls, 1);
    AURORA_TEST_CHECK(ready_bytes == to_bytes("ready-bytes"));
    AURORA_TEST_CHECK_EQ(withheld_code, ErrorCode::SystemDragReadFailed);

    // 终态经帧循环投递一次；投递后会话槽位释放（cancel 真的收了口，不是留着占位）：
    // 二次发起的会话同样走「登记终态 → 帧循环投递」全路，end 第二次落到同一回调上。
    AURORA_TEST_CHECK_EQ(end_calls, 0);
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 1);
    AURORA_TEST_CHECK_EQ(end_outcome, SystemDragOutcome::Cancelled);
    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), [&](SystemDragOutcome o, DragAction) {
                            ++end_calls;
                            end_outcome = o;
                        }).ok());
    system_drag::cancel();
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 2);
    AURORA_TEST_CHECK_EQ(end_outcome, SystemDragOutcome::Cancelled);
}

AURORA_TEST_CASE(withheld_payload_times_out_with_read_failed) {
    // 源侧迟迟不给数据：未超时前 pump 不兑现（不假成功），越过墙钟超时后按
    // system-drag-read-failed 兑现且只兑现一次。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }
    // 先设超时再登记：deadline 在登记时按当前超时算，次序反了超时点就落到默认 5s 上。
    system_drag::test_set_read_timeout(std::chrono::milliseconds{20});
    system_drag::test_withhold_format(std::string(AURORA_MIME_TEXT_PLAIN));

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("late-payload"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    int calls = 0;
    ErrorCode code = ErrorCode::GeneralUnknown;
    payload->read_async(std::string(AURORA_MIME_TEXT_PLAIN), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++calls;
        if (!r) {
            code = r.error().code_enum;
        }
    });

    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(calls, 0);  // 数据未到位且未超时：留到下一拍

    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_EQ(code, ErrorCode::SystemDragReadFailed);

    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(calls, 1);  // 超时兑现只一次，不重复投递

    system_drag::test_set_read_timeout(std::chrono::milliseconds{0});  // 复位默认超时
    (void)system_drag::remove_test_backend();  // 清扣留记录
}

AURORA_TEST_CASE(empty_payload_contract_does_not_regress) {
    // 空口径在异步路上不回归：无载荷时同步按空兑现（没有会话可排空）；有载荷但无该
    // 格式时经 pump 兑现 Ok(空)——「空」与「败」两条路依旧分开。
    (void)system_drag::remove_test_backend();

    int calls = 0;
    bool ok = false;
    bool empty = false;

    SystemDropEvent bare;  // 无载荷（enter / over / leave 态的常态）
    bare.text_async([&](const Result<std::string> &r) {
        ++calls;
        ok = r.ok();
        empty = r.ok() && r.value().empty();
    });
    AURORA_TEST_CHECK_EQ(calls, 1);  // 无载荷：同步兑现，不等帧循环
    AURORA_TEST_CHECK_TRUE(ok);
    AURORA_TEST_CHECK_TRUE(empty);

    std::unordered_map<std::string, std::vector<std::uint8_t>> data;
    data.emplace(std::string(AURORA_MIME_TEXT_PLAIN), to_bytes("present"));
    const auto payload = DragPayload::from_bytes(std::move(data));

    SystemDropEvent ev;
    ev.payload = payload;
    calls = 0;
    ev.image_async([&](const Result<Image> &r) {
        ++calls;
        ok = r.ok();
        empty = r.ok() && r.value().pixels.empty();
    });
    AURORA_TEST_CHECK_EQ(calls, 0);
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_TRUE(ok);
    AURORA_TEST_CHECK_TRUE(empty);

    calls = 0;
    payload->read_async(std::string(AURORA_MIME_IMAGE_PNG), [&](const Result<std::vector<std::uint8_t>> &r) {
        ++calls;
        ok = r.ok();
        empty = r.ok() && r.value().empty();
    });
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(calls, 1);
    AURORA_TEST_CHECK_TRUE(ok);
    AURORA_TEST_CHECK_TRUE(empty);
}

// ---- 事件驱动拖出：发起即返回、终态经帧循环投递 ----

AURORA_TEST_CASE(begin_returns_immediately_and_delivers_on_frame_loop) {
    // begin 的 Ok 只表「会话已发起」：返回时终态尚未投递（Ok 不等于落放成功），
    // 终态经帧循环排空投递且只投一次；会话期内重复发起被槽位约束拒绝。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    int end_calls = 0;
    SystemDragOutcome got_outcome = SystemDragOutcome::Failed;
    DragAction got_action = DragAction::None;
    const auto on_end = [&](SystemDragOutcome o, DragAction a) {
        ++end_calls;
        got_outcome = o;
        got_action = a;
    };

    const auto started = system_drag::begin(make_text_offer(), on_end);
    AURORA_TEST_REQUIRE(started.ok());
    AURORA_TEST_CHECK_EQ(end_calls, 0);  // 发起即返回：此刻没有任何终态

    // 单线程 UI 下一次只能有一个拖出会话（OS 同款约束）：在途发起直接拒绝。
    const auto second = system_drag::begin(make_text_offer(), on_end);
    AURORA_TEST_REQUIRE_FALSE(second.ok());
    AURORA_TEST_CHECK_EQ(second.error().code_enum, ErrorCode::SystemDragSessionFailed);

    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Copy);
    AURORA_TEST_CHECK_EQ(end_calls, 0);  // 仅登记终态：未排空不投递
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 1);
    AURORA_TEST_CHECK_EQ(got_outcome, SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_EQ(got_action, DragAction::Copy);

    // 收口后槽位释放：可再次发起，取消的终态同样经帧循环投递。
    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), on_end).ok());
    system_drag::cancel();
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 2);
    AURORA_TEST_CHECK_EQ(got_outcome, SystemDragOutcome::Cancelled);
    AURORA_TEST_CHECK_EQ(got_action, DragAction::None);
}

AURORA_TEST_CASE(cancel_is_callable_outside_and_inside_a_session) {
    // 程序化取消的调用时机不受限：会话外为 no-op（无终态、不悬挂）；会话内登记
    // Cancelled 并经 pump 投递一次；重复取消不再投递。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    int end_calls = 0;
    SystemDragOutcome got_outcome = SystemDragOutcome::Failed;
    const auto on_end = [&](SystemDragOutcome o, DragAction) {
        ++end_calls;
        got_outcome = o;
    };

    system_drag::cancel();  // 会话外取消：no-op
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 0);

    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), on_end).ok());
    system_drag::cancel();
    AURORA_TEST_CHECK_EQ(end_calls, 0);  // 仅登记：投递在之后的 pump_reads()
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 1);
    AURORA_TEST_CHECK_EQ(got_outcome, SystemDragOutcome::Cancelled);

    system_drag::cancel();  // 会话已收口：再取消是 no-op
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(end_calls, 1);
}

AURORA_TEST_CASE(move_confirmation_precedes_end_and_rollback_on_failure) {
    // Move 落放的两条路：确认成功时「确认严格早于终态投递」可外部断言；确认失败时
    // 终态改写为 (Failed, Move) 并上报 system-drag-move-rollback（目标侧数据不动）。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    std::vector<std::string> ledger;
    SystemDragOutcome got_outcome = SystemDragOutcome::Failed;
    DragAction got_action = DragAction::None;
    const auto record_end = [&ledger, &got_outcome, &got_action](SystemDragOutcome o, DragAction a) {
        ledger.emplace_back(std::string{"end("} + outcome_name(o) + "," + action_name(a) + ")");
        got_outcome = o;
        got_action = a;
    };

    DragOffer ok_offer;
    ok_offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    ok_offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };
    ok_offer.on_move_confirmed = [&ledger]() -> Result<void> {
        ledger.emplace_back("confirm");
        return {};
    };
    AURORA_TEST_REQUIRE(system_drag::begin(ok_offer, record_end).ok());
    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Move);
    system_drag::pump_reads();
    AURORA_TEST_REQUIRE_EQ(ledger.size(), 2U);
    AURORA_TEST_CHECK_EQ(ledger[0], std::string{"confirm"});  // 确认严格早于终态
    AURORA_TEST_CHECK_EQ(ledger[1], std::string{"end(Succeeded,Move)"});
    AURORA_TEST_CHECK_EQ(got_outcome, SystemDragOutcome::Succeeded);

    // 回滚路径：源侧删除失败 ⇒ 终态改写、动作保持 Move、错误上报可读。
    ledger.clear();
    DragOffer bad_offer;
    bad_offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    bad_offer.provide = [](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> { return to_bytes("x"); };
    bad_offer.on_move_confirmed = [&ledger]() -> Result<void> {
        ledger.emplace_back("confirm");
        return make_error(ErrorCode::GeneralUnknown, "source delete failed");
    };
    AURORA_TEST_REQUIRE(system_drag::begin(bad_offer, record_end).ok());
    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Move);
    system_drag::pump_reads();
    AURORA_TEST_REQUIRE_EQ(ledger.size(), 2U);
    AURORA_TEST_CHECK_EQ(ledger[0], std::string{"confirm"});
    AURORA_TEST_CHECK_EQ(ledger[1], std::string{"end(Failed,Move)"});
    AURORA_TEST_CHECK_EQ(got_action, DragAction::Move);
    const auto session_error = system_drag::test_last_session_error();
    AURORA_TEST_REQUIRE(session_error.has_value());
    AURORA_TEST_CHECK_EQ(require_value(session_error).code_enum, ErrorCode::SystemDragMoveRollback);
}

AURORA_TEST_CASE(drag_offer_image_and_hotspot_round_trip_through_backend) {
    // 拖拽影像、热点偏移、缺省动作与 provide 回调经后端登记后原样取回（inert 后端
    // 存的是一份要约副本；字段往返即契约——后端接线方按同一形状消费）。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    DragOffer offer;
    offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
    offer.formats.emplace_back(AURORA_MIME_IMAGE_PNG);
    offer.provide = [](const std::string &mime) -> Result<std::vector<std::uint8_t>> {
        const std::string png{AURORA_MIME_IMAGE_PNG};
        return mime == png ? to_bytes("png-bytes") : to_bytes("text-bytes");
    };
    Image icon;
    icon.width = 2;
    icon.height = 3;
    icon.pixels.assign(static_cast<std::size_t>(2 * 3 * 4), std::uint8_t{0x7F});
    offer.icon = icon;
    offer.icon_hotspot = Point{.x = 3.5F, .y = 4.5F};
    offer.default_action = DragAction::Move;
    AURORA_TEST_REQUIRE(system_drag::begin(offer, nullptr).ok());

    const auto last = system_drag::test_last_offer();
    const auto &lo = require_value(last);
    AURORA_TEST_REQUIRE_EQ(lo.formats.size(), 2U);
    AURORA_TEST_CHECK_EQ(lo.formats[0], std::string(AURORA_MIME_TEXT_PLAIN));
    AURORA_TEST_CHECK_EQ(lo.formats[1], std::string(AURORA_MIME_IMAGE_PNG));  // 顺序即优先级
    AURORA_TEST_CHECK_EQ(lo.icon.width, 2);
    AURORA_TEST_CHECK_EQ(lo.icon.height, 3);
    AURORA_TEST_CHECK(lo.icon.pixels == icon.pixels);
    AURORA_TEST_CHECK_NEAR(lo.icon_hotspot.x, 3.5F, 1.0e-6F);
    AURORA_TEST_CHECK_NEAR(lo.icon_hotspot.y, 4.5F, 1.0e-6F);
    AURORA_TEST_CHECK_EQ(lo.default_action, DragAction::Move);
    AURORA_TEST_REQUIRE(lo.provide);
    const auto provided = lo.provide(std::string(AURORA_MIME_IMAGE_PNG));
    AURORA_TEST_REQUIRE(provided.ok());
    AURORA_TEST_CHECK(provided.value() == to_bytes("png-bytes"));
}

// ---- Widget 适配层：回调序列、失败收尾与销毁取消 ----

AURORA_TEST_CASE(widget_drag_session_callback_sequences_cover_five_paths) {
    // 五种会话路径的回调序列：成功 / 被拒 / 中途异常 / 取消，外加「无后端」与
    // 「无要约」两条发起失败路径。被拒与会话中途异常的序列逐字相同（begin → end），
    // 靠 outcome 区分——故台账把结局名写进条目，逐字比对即可分辨。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    const auto run_path = [](SystemDragOutcome outcome, DragAction action, const char *expect_tail) {
        DragSourceBox box;
        box.offer = make_text_offer();
        AURORA_TEST_REQUIRE(box.begin_system_drag().ok());
        AURORA_TEST_REQUIRE_EQ(box.sequence.size(), 1U);
        AURORA_TEST_CHECK_EQ(box.sequence[0], std::string{"begin"});
        system_drag::emit_session_end(outcome, action);
        system_drag::pump_reads();
        AURORA_TEST_REQUIRE_EQ(box.sequence.size(), 2U);
        AURORA_TEST_CHECK_EQ(box.sequence[1], std::string{expect_tail});
        AURORA_TEST_CHECK_EQ(box.end_calls, 1);
        AURORA_TEST_CHECK_EQ(box.last_outcome, outcome);
        AURORA_TEST_CHECK_EQ(box.last_action, action);
    };

    run_path(SystemDragOutcome::Succeeded, DragAction::Copy, "end(Succeeded,Copy)");
    run_path(SystemDragOutcome::Rejected, DragAction::None, "end(Rejected,None)");
    run_path(SystemDragOutcome::Failed, DragAction::None, "end(Failed,None)");

    // 取消路径：cancel 严格早于 end（同一帧的排空内有序）。
    {
        DragSourceBox box;
        box.offer = make_text_offer();
        AURORA_TEST_REQUIRE(box.begin_system_drag().ok());
        system_drag::emit_session_end(SystemDragOutcome::Cancelled, DragAction::None);
        system_drag::pump_reads();
        AURORA_TEST_REQUIRE_EQ(box.sequence.size(), 3U);
        AURORA_TEST_CHECK_EQ(box.sequence[1], std::string{"cancel"});
        AURORA_TEST_CHECK_EQ(box.sequence[2], std::string{"end(Cancelled,None)"});
        AURORA_TEST_CHECK_EQ(box.end_calls, 1);
    }

    // 无后端：发起失败也收尾（无 begin，直接 end(Failed,None)）——失败同样要收尾。
    AURORA_TEST_REQUIRE(system_drag::remove_test_backend());
    {
        DragSourceBox box;
        box.offer = make_text_offer();
        const auto started = box.begin_system_drag();
        AURORA_TEST_REQUIRE_FALSE(started.ok());
        AURORA_TEST_CHECK_EQ(started.error().code_enum, ErrorCode::SystemDragNotSupported);
        AURORA_TEST_REQUIRE_EQ(box.sequence.size(), 1U);
        AURORA_TEST_CHECK_EQ(box.sequence[0], std::string{"end(Failed,None)"});
        AURORA_TEST_CHECK_EQ(box.end_calls, 1);
        AURORA_TEST_CHECK_EQ(box.last_action, DragAction::None);
    }

    // 无要约：从未发起会话 ⇒ 一个回调都不派发（无可收尾的拖出态）。
    AURORA_TEST_REQUIRE(system_drag::install_test_backend());
    {
        DragSourceBox box;
        box.offer = std::nullopt;
        const auto started = box.begin_system_drag();
        AURORA_TEST_REQUIRE_FALSE(started.ok());
        AURORA_TEST_CHECK_EQ(started.error().code_enum, ErrorCode::SystemDragNotSupported);
        AURORA_TEST_CHECK(box.sequence.empty());
        AURORA_TEST_CHECK_EQ(box.end_calls, 0);
    }
}

AURORA_TEST_CASE(widget_without_offer_dispatches_no_end) {
    // 无要约控件的发起失败只影响自己：不派发终态，也不改写 / 挤掉别的控件持有的
    // 在途会话（失败路径不碰会话槽位）。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    DragSourceBox source;
    source.offer = make_text_offer();
    AURORA_TEST_REQUIRE(source.begin_system_drag().ok());

    DragSourceBox no_offer;
    no_offer.offer = std::nullopt;
    const auto started = no_offer.begin_system_drag();
    AURORA_TEST_REQUIRE_FALSE(started.ok());
    AURORA_TEST_CHECK(no_offer.sequence.empty());
    AURORA_TEST_CHECK_EQ(no_offer.end_calls, 0);

    // 源控件的会话照常走到终态。
    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Copy);
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(source.end_calls, 1);
    AURORA_TEST_CHECK_EQ(source.last_outcome, SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_EQ(no_offer.end_calls, 0);
}

AURORA_TEST_CASE(widget_destruction_cancels_in_flight_session) {
    // 析构即取消：控件在会话期间销毁，终结回调不得解引用已回收对象（双轨判活），
    // provide 也不会再被回调；会话槽位随之释放。计数全部挂在控件外部的台账上——
    // 回调若真的踩已析构对象，计数就会涨（绊线探针）。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    auto end_calls = std::make_shared<int>(0);
    auto begin_calls = std::make_shared<int>(0);
    auto provide_calls = std::make_shared<int>(0);
    const auto make_probe_offer = [provide_calls]() -> DragOffer {
        DragOffer offer;
        offer.formats.emplace_back(AURORA_MIME_TEXT_PLAIN);
        offer.provide = [provide_calls](const std::string & /*mime*/) -> Result<std::vector<std::uint8_t>> {
            ++*provide_calls;
            return to_bytes("x");
        };
        return offer;
    };

    // ① 栈轨：控件非 shared_ptr 持有（weak_from_this 为空）⇒ 存活令牌守卫。
    {
        DragSourceBox box;
        box.offer = make_probe_offer();
        box.on_begin_hook = [begin_calls] { ++*begin_calls; };
        box.on_end_hook = [end_calls] { ++*end_calls; };
        AURORA_TEST_REQUIRE(box.begin_system_drag().ok());
        AURORA_TEST_CHECK_EQ(*begin_calls, 1);  // begin 回调同步派发
        AURORA_TEST_CHECK_EQ(*end_calls, 0);  // 终态未到（帧循环排空）
    }  // ← 控件在此析构：置存活令牌 false + cancel() 登记终态

    // 析构后排空一拍：回调须被令牌挡下，外部计数不涨；provide 零回调。
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(*end_calls, 0);
    AURORA_TEST_CHECK_EQ(*provide_calls, 0);

    // 会话槽位已释放（析构即取消收了口）：可再次发起。
    AURORA_TEST_REQUIRE(system_drag::begin(make_text_offer(), nullptr).ok());
    system_drag::cancel();
    system_drag::pump_reads();

    // ② shared_ptr 轨：守卫走 weak_from_this（回收后 lock 失败即跳过派发）。
    {
        auto box = std::make_shared<DragSourceBox>();
        box->offer = make_probe_offer();
        box->on_begin_hook = [begin_calls] { ++*begin_calls; };
        box->on_end_hook = [end_calls] { ++*end_calls; };
        AURORA_TEST_REQUIRE(box->begin_system_drag().ok());
        AURORA_TEST_CHECK_EQ(*begin_calls, 2);
        box.reset();  // ← 回收：~Widget 取消会话
    }
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(*end_calls, 0);
    AURORA_TEST_CHECK_EQ(*provide_calls, 0);
}

AURORA_TEST_CASE(begin_callback_runs_inline_without_nested_pump) {
    // begin 回调（on_system_drag_begin）在 begin_system_drag 内同步派发，不在排空
    // 阶段内：其内部的树写操作安全（不触发嵌套派发），回调内再入排空也无副作用。
    (void)system_drag::remove_test_backend();
    if (!system_drag::install_test_backend()) {
        AURORA_TEST_SKIP(
            "system drag inert backend unavailable (AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }

    DragSourceBox box;
    box.offer = make_text_offer();
    bool in_drain_at_begin = true;  // 初值取反：钩子内必须置 false 才算证伪
    box.on_begin_hook = [&box, &in_drain_at_begin] {
        in_drain_at_begin = system_drag::test_in_drain_phase();
        box.mark_needs_layout();  // 树写操作：不得触发嵌套派发
        system_drag::pump_reads();  // 嵌套排空：此刻无待投递终态，应无副作用
    };
    AURORA_TEST_REQUIRE(box.begin_system_drag().ok());
    AURORA_TEST_CHECK_FALSE(in_drain_at_begin);
    AURORA_TEST_CHECK_EQ(box.end_calls, 0);
    AURORA_TEST_CHECK_EQ(box.sequence.size(), 1U);  // 嵌套 pump 未提前投递任何终态

    system_drag::emit_session_end(SystemDragOutcome::Succeeded, DragAction::Copy);
    system_drag::pump_reads();
    AURORA_TEST_CHECK_EQ(box.end_calls, 1);
    AURORA_TEST_CHECK_EQ(box.sequence.size(), 2U);
    AURORA_TEST_CHECK_EQ(box.last_outcome, SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_EQ(box.last_action, DragAction::Copy);
}

}  // namespace aurora::test_cases::utest_drag_payload
