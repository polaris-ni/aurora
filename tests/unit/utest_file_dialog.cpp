/// @file utest_file_dialog.cpp
/// 测试类型: unit
/// 目标单元: include/aurora/app/file_dialog.h, include/aurora/app/application.h,
///           src/aurora/app/detail/file_picker.h, src/aurora/app/file_dialog_selfdrawn.cpp
/// 测试说明: 覆盖 Filter/Options 配置结构体默认值与聚合构造、headless 注入钩子
/// （open/save/folder 预设返回）与 interactive=false 取消语义、钩子优先级、`headless_last_options`
/// 观察面（三入口逐一记录、排在钩子判据之前、非 ASCII 逐字节回读）；
/// 桌面自绘腿（Linux，或未启用 AppKit 后端的 macOS；其余组合记 skip）：L1 状态机无头全覆盖
/// （初始目录三级
/// 定位与降级 WARN、进出目录失败驻留、筛选与排序、选择语义、确认绝对路径、取消为空、
/// SaveFile 形态按 `suggested_name` 末段播种与 `default_extension` 确认时补）、
/// L2 视图的确认 / 取消 / 上一级与 Options 传导（含输入框初值与状态机同源）、L3 路由三态（钩子优先不构造选择器、
/// 非交互回空、无上下文 WARN 回空不挂起）、Application::run_modal 的单例安装与逐项
/// 还原 / 有限帧驱动 / 帧预算退出 / 一枚叠一枚与第三层拒绝，以及脚本化确认 / 取消 /
/// 保存文件名 / 目录选择的端到端会话（含两枚选择器叠一层、名字改写以改写后为准）。
/// 真实系统对话框路径不测（原生 API），全部走钩子 / 非交互 / 自绘腿无头路径；
/// Win32 腿的 COM 投递由真机探针 `aurora_verify_win32_file_dialog` 取证（见同目录探针头注），
/// macOS 原生面板属真机交互路径，无自动化落点——其入口口径由头文件契约与钩子 / 非交互用例覆盖。

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/detail/file_picker.h"
#include "aurora/app/file_dialog.h"
#include "aurora/core/log.h"
#include "aurora/core/main_queue.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "framework/aurora_test.h"

// 自绘选择器腿的生效条件：Linux，或「macOS 但未启用 AppKit 后端」（macOS + AppKit 的入口是
// file_dialog_macos.cpp 里的原生面板）。L0–L2（数据源 / 状态机 / 视图）在所有桌面平台仍编译，
// 故 L1/L2 用例继续按「Linux || macOS」守护；只有入口路由与会话端到端用例跟着入口实现走。
#if defined(AURORA_PLATFORM_LINUX) || (defined(AURORA_PLATFORM_MACOS) && !defined(AURORA_BACKEND_MACOS))
#define AURORA_UTEST_FILE_DIALOG_SELFDRAWN 1
#endif

namespace aurora::test_cases::utest_file_dialog {

namespace {

using aurora::testing::require_value;

/// RAII 恢复 headless 钩子与交互开关，避免污染同进程内后续用例。
struct HookGuard {
    HookGuard() = default;
    HookGuard(const HookGuard &) = delete;
    auto operator=(const HookGuard &) -> HookGuard & = delete;
    HookGuard(HookGuard &&) = delete;
    auto operator=(HookGuard &&) -> HookGuard & = delete;
    ~HookGuard() {
        aurora::file_dialog::headless_open_result.clear();
        aurora::file_dialog::headless_save_result.clear();
        aurora::file_dialog::headless_folder_result.clear();
        aurora::file_dialog::headless_last_options = std::nullopt;
        aurora::file_dialog::interactive = true;
    }
};

/// @brief RAII：接管 Logger sink 捕获日志行；析构恢复级别与默认 stderr，
///        不把 Logger 全局状态泄漏给其他用例（与 utest_window_geometry 同构）。
class CapturedLog {
  public:
    CapturedLog() : level_{aurora::Logger::instance().level()} {
        aurora::Logger::instance().set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
    }
    ~CapturedLog() {
        auto &logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_sink(nullptr);  // 恢复默认 stderr
    }
    CapturedLog(const CapturedLog &) = delete;
    auto operator=(const CapturedLog &) & = delete;
    CapturedLog(CapturedLog &&) = delete;
    auto operator=(CapturedLog &&) & = delete;

    /// @brief 已捕获行里是否有一条同时含全部给定片段（级别标签 + 分类 + 文案）。
    [[nodiscard]] auto has_line_with(std::initializer_list<const char *> fragments) const -> bool {
        for (const std::string &line : lines) {
            bool all = true;
            for (const char *f : fragments) {
                if (line.find(f) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> lines;  // NOLINT(*-non-private-member-variables-in-classes) 捕获到的日志行

  private:
    aurora::LogLevel level_;
};

/// @brief RAII：还原自绘腿测试接缝（会话观察者 / 构造计数 / 应用上下文槽 / 模态抑制标志），
///        避免污染同进程内后续用例。
struct PickerSeamGuard {
    PickerSeamGuard() = default;
    PickerSeamGuard(const PickerSeamGuard &) = delete;
    auto operator=(const PickerSeamGuard &) -> PickerSeamGuard & = delete;
    PickerSeamGuard(PickerSeamGuard &&) = delete;
    auto operator=(PickerSeamGuard &&) -> PickerSeamGuard & = delete;
    ~PickerSeamGuard() {
        aurora::file_dialog::detail::session_observer() = nullptr;
        aurora::file_dialog::detail::picker_construct_count() = 0;
        aurora::file_dialog::detail::current_application_slot() = nullptr;
        aurora::file_dialog::detail::modal_dispatch_suppressed() = false;
    }
};

/// @brief RAII：用例末把进程级单例还原到本 guard 构造时的快照——用例故意预装「外层」
///        单例以证 run_modal 的逐项还原，不把全局态泄漏给后续用例。
///        值班租约不在此列：全局队列只有一个，run_modal 自领自还、无槽可快照。
struct SingletonSnapshot {
    SingletonSnapshot()
        : sched(Scheduler::current()), anim(Animator::current()), app(file_dialog::detail::current_application_slot()),
          focus(current_focus_manager()), strict(aurora::strict_mode()) {}
    SingletonSnapshot(const SingletonSnapshot &) = delete;
    auto operator=(const SingletonSnapshot &) & -> SingletonSnapshot & = delete;
    SingletonSnapshot(SingletonSnapshot &&) = delete;
    auto operator=(SingletonSnapshot &&) & -> SingletonSnapshot & = delete;
    ~SingletonSnapshot() {
        Scheduler::set_current(sched);
        Animator::set_current(anim);
        set_current_focus_manager(focus);
        aurora::set_strict_mode(strict);
        file_dialog::detail::current_application_slot() = app;
        file_dialog::detail::modal_dispatch_suppressed() = false;
    }

    Scheduler *sched = nullptr;
    Animator *anim = nullptr;
    Application *app = nullptr;
    FocusManager *focus = nullptr;
    StrictMode strict = StrictMode::Off;
};

#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)

using PickEntry = aurora::file_dialog::detail::DirectorySource::Entry;

/// @brief L0 mock 数据源：目录树在内存里，`unreadable` 模拟「is_dir 为真但列不出」。
class MockSource final : public aurora::file_dialog::detail::DirectorySource {
  public:
    explicit MockSource(std::string root) : root_(std::move(root)) {}

    [[nodiscard]] auto list_dir(const std::string &path) const -> std::optional<std::vector<PickEntry>> override {
        const auto it = tree.find(path);
        if (it == tree.end()) {
            return std::nullopt;  // 不存在 / 列不出：交由调用方降级
        }
        return it->second;
    }

    [[nodiscard]] auto is_dir(const std::string &path) const -> bool override {
        return tree.contains(path) || unreadable.contains(path);
    }

    [[nodiscard]] auto to_absolute(const std::string &path) const -> std::string override {
        if (path.empty()) {
            return root_;
        }
        return path.front() == '/' ? path : aurora::file_dialog::detail::join_path(root_, path);
    }

    [[nodiscard]] auto root() const -> std::string override { return root_; }

    std::map<std::string, std::vector<PickEntry>> tree;
    std::set<std::string> unreadable;

  private:
    std::string root_;
};

/// @brief 选中状态机里第一个文件行（OpenFile 确认只收文件行）。
auto select_first_file_row(file_dialog::detail::FilePickerModel &model) -> bool {
    const auto &entries = model.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].is_dir) {
            model.select(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

/// @brief 选中状态机里第一个目录行（OpenFolder 确认只收目录行）。
auto select_first_dir_row(file_dialog::detail::FilePickerModel &model) -> bool {
    const auto &entries = model.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].is_dir) {
            model.select(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

#if defined(AURORA_UTEST_FILE_DIALOG_SELFDRAWN) && defined(AURORA_BACKEND_HEADLESS)

/// @brief 无头驱动一轮自绘会话的观测结果。
struct SelfdrawnRun {
    bool ok = false;  ///< 入口返回 Ok（取消也是 Ok，不是 Error）
    std::vector<std::string> paths;  ///< 入口产物（open_file 为列表，另两类为首元素）
    std::size_t construct_count = 0;  ///< 选择器构造计数（进 UI 路径即 >= 1）
    bool root_restored = false;  ///< 退出后场景根 widget 实例与进入前同一
};

/// @brief 无头驱动一轮自绘会话：帧 1 调 `entry`（进入自绘腿），模态帧内按 `answer`
///        脚本化作答（与确认 / 取消按钮回调同一条路径）。
auto run_selfdrawn_case(const std::function<void(file_dialog::detail::FilePickerView &)> &answer,
                        const std::function<Result<std::vector<std::string>>()> &entry) -> SelfdrawnRun {
    SelfdrawnRun out;
    WindowOptions o;
    o.size = Size{.width = 320.0F, .height = 240.0F};
    o.max_frames = 8;
    o.power_saving = false;  // 无 OS 等待通道：忙轮询，帧数确定
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto window = create_window(h);
    if (!window.ok()) {
        return out;  // 无 Headless 后端：ok 保持 false，调用方的 REQUIRE 会给出可归因失败
    }
    Application app{Scene{Node{std::make_shared<Text>("root")}}, std::move(window.value()), o};
    const aurora::Widget *const root_before = app.scene().root_node().operator->();

    file_dialog::detail::FilePickerView *picker = nullptr;
    int frames = 0;
    app.set_on_frame([&]() -> void {
        ++frames;
        if (frames == 1) {
            const Result<std::vector<std::string>> r = entry();
            out.ok = r.ok();
            if (r.ok()) {
                out.paths = r.value();
            }
            return;
        }
        if (picker != nullptr && !picker->finished()) {
            answer(*picker);  // 模态帧内脚本化作答
        }
    });
    file_dialog::detail::session_observer() = [&picker](file_dialog::detail::FilePickerView &view) -> void {
        picker = &view;
    };
    app.run();
    // 观察者槽是进程级静态，其 lambda 捕获了**栈上**的 `picker`；不复位即留下悬垂引用
    // （下一次会话构造会向已销毁的栈对象回调）。此处就地切断，与 PickerSeamGuard 同口径。
    file_dialog::detail::session_observer() = nullptr;

    out.construct_count = file_dialog::detail::picker_construct_count();
    out.root_restored = app.scene().root_node().operator->() == root_before;
    return out;
}

#endif  // AURORA_UTEST_FILE_DIALOG_SELFDRAWN && AURORA_BACKEND_HEADLESS

#endif  // defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)

}  // namespace

AURORA_TEST_CASE(filter_and_options_struct_defaults) {
    // Filter：名称 + 扩展名列表的聚合结构。
    const aurora::file_dialog::Filter f{.name = "Images", .extensions = {"*.png", "*.jpg"}};
    AURORA_TEST_CHECK_STREQ(f.name, "Images");
    AURORA_TEST_CHECK_EQ(f.extensions.size(), 2U);
    AURORA_TEST_CHECK_STREQ(f.extensions[0], "*.png");
    AURORA_TEST_CHECK_STREQ(f.extensions[1], "*.jpg");

    // Options：全空默认值（四枚字符串格 + 过滤器表；空串一律 = 不表达意图）。
    const aurora::file_dialog::Options def;
    AURORA_TEST_CHECK_TRUE(def.title.empty());
    AURORA_TEST_CHECK_TRUE(def.initial_dir.empty());
    AURORA_TEST_CHECK_TRUE(def.suggested_name.empty());
    AURORA_TEST_CHECK_TRUE(def.default_extension.empty());
    AURORA_TEST_CHECK_TRUE(def.filters.empty());

    // Options 聚合构造携带过滤器。
    const aurora::file_dialog::Options opts{.title = "Open file", .initial_dir = "C:/", .filters = {f}};
    AURORA_TEST_CHECK_STREQ(opts.title, "Open file");
    AURORA_TEST_CHECK_STREQ(opts.initial_dir, "C:/");
    AURORA_TEST_CHECK_TRUE(opts.suggested_name.empty());  // 跳过式指定初始化：未点名的格仍取默认
    AURORA_TEST_REQUIRE_EQ(opts.filters.size(), 1U);
    AURORA_TEST_CHECK_STREQ(opts.filters[0].name, "Images");

    // 保存形态的两格各自独立可指定（次序在 initial_dir 之后、filters 之前）。
    const aurora::file_dialog::Options save{
        .title = "Save file", .suggested_name = "notes.txt", .default_extension = "txt", .filters = {f}};
    AURORA_TEST_CHECK_STREQ(save.suggested_name, "notes.txt");
    AURORA_TEST_CHECK_STREQ(save.default_extension, "txt");
    AURORA_TEST_CHECK_TRUE(save.initial_dir.empty());
}

AURORA_TEST_CASE(headless_last_options_records_every_entry) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;
    aurora::file_dialog::headless_last_options = std::nullopt;

    // 首次调用前是 nullopt（与「调过但没表达意图」的空 Options 不是同一件事）。
    AURORA_TEST_CHECK_FALSE(aurora::file_dialog::headless_last_options.has_value());

    const aurora::file_dialog::Options open_opts{
        .title = "Open", .initial_dir = "/preset-dir", .filters = {{.name = "Images", .extensions = {"*.png"}}}};
    const aurora::file_dialog::Options save_opts{
        .title = "Save", .initial_dir = "/preset-dir", .suggested_name = "report.txt", .default_extension = "txt"};
    const aurora::file_dialog::Options folder_opts{.title = "Pick folder", .initial_dir = "/preset-dir"};

    (void)aurora::file_dialog::open_file(open_opts);
    const auto opened_rec = require_value(aurora::file_dialog::headless_last_options);
    AURORA_TEST_CHECK_STREQ(opened_rec.title, "Open");
    AURORA_TEST_CHECK_STREQ(opened_rec.initial_dir, "/preset-dir");
    AURORA_TEST_CHECK_TRUE(opened_rec.suggested_name.empty());
    AURORA_TEST_REQUIRE_EQ(opened_rec.filters.size(), 1U);
    AURORA_TEST_CHECK_STREQ(opened_rec.filters[0].extensions[0], "*.png");

    (void)aurora::file_dialog::save_file(save_opts);
    const auto saved_rec = require_value(aurora::file_dialog::headless_last_options);
    AURORA_TEST_CHECK_STREQ(saved_rec.suggested_name, "report.txt");
    AURORA_TEST_CHECK_STREQ(saved_rec.default_extension, "txt");

    (void)aurora::file_dialog::open_folder(folder_opts);
    const auto folder_rec = require_value(aurora::file_dialog::headless_last_options);
    AURORA_TEST_CHECK_STREQ(folder_rec.title, "Pick folder");

    // 全空那一次也被记录（空 Options 记成「调过但没表达意图」，不是回到 nullopt）。
    (void)aurora::file_dialog::open_file();
    const auto empty_rec = require_value(aurora::file_dialog::headless_last_options);
    AURORA_TEST_CHECK_TRUE(empty_rec.title.empty());
    AURORA_TEST_CHECK_TRUE(empty_rec.filters.empty());
}

AURORA_TEST_CASE(headless_last_options_records_before_hook_check) {
    HookGuard guard;
    // 钩子置值：入口走钩子分支，但记录排在判据之前，故仍读得到本次传入的原文。
    aurora::file_dialog::headless_open_result = {"preset.png"};
    aurora::file_dialog::headless_save_result = "preset.txt";
    aurora::file_dialog::headless_folder_result = "/preset";
    aurora::file_dialog::headless_last_options = std::nullopt;

    const aurora::file_dialog::Options opts{
        .title = "Hooked", .initial_dir = "/preset-dir", .suggested_name = "hooked.txt", .default_extension = "log"};
    const auto opened = aurora::file_dialog::open_file(opts);
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_REQUIRE_EQ(opened.value().size(), 1U);  // 走的是钩子，不是投递
    AURORA_TEST_CHECK_STREQ(require_value(aurora::file_dialog::headless_last_options).suggested_name, "hooked.txt");

    (void)aurora::file_dialog::save_file(opts);
    AURORA_TEST_CHECK_STREQ(require_value(aurora::file_dialog::headless_last_options).title, "Hooked");
    (void)aurora::file_dialog::open_folder(opts);
    AURORA_TEST_CHECK_STREQ(require_value(aurora::file_dialog::headless_last_options).default_extension, "log");

    // 记录的是原文，不是折算结果：带目录的建议名照原样回读（截断发生在投递前，由各腿做）。
    const aurora::file_dialog::Options raw{.suggested_name = "dir/sub/report.txt"};
    (void)aurora::file_dialog::save_file(raw);
    AURORA_TEST_CHECK_STREQ(require_value(aurora::file_dialog::headless_last_options).suggested_name,
                            "dir/sub/report.txt");
}

AURORA_TEST_CASE(headless_last_options_keeps_non_ascii_bytes) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;
    aurora::file_dialog::headless_last_options = std::nullopt;

    // 转义写死的 UTF-8 字节（「中文报告」+「报告」），不经 argv、不经活动码页。
    const std::string utf8_name = "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a.txt";
    const std::string utf8_dir = "\xe7\x9b\xae\xe5\xbd\x95/sub";
    const aurora::file_dialog::Options opts{
        .title = "\xe6\xa0\x87\xe9\xa2\x98", .initial_dir = utf8_dir, .suggested_name = utf8_name};
    (void)aurora::file_dialog::save_file(opts);

    // 判据 5：逐字节相等，不是「看起来能用」——本机活动码页 936 下 ANSI 解法会把 E4 B8 AD 折成花码。
    const auto rec = require_value(aurora::file_dialog::headless_last_options);
    AURORA_TEST_CHECK_TRUE(rec.suggested_name == utf8_name);
    AURORA_TEST_CHECK_EQ(rec.suggested_name.size(), utf8_name.size());
    AURORA_TEST_CHECK_TRUE(rec.initial_dir == utf8_dir);
    AURORA_TEST_CHECK_TRUE(rec.title == opts.title);
}

AURORA_TEST_CASE(open_file_returns_headless_preset) {
    HookGuard guard;
    aurora::file_dialog::headless_open_result = {"a.png", "b.jpg"};

    // 钩子非空 → 直接返回预设（opts 仅作配置载体，不触发真实对话框）。
    const aurora::file_dialog::Options opts{.title = "Open", .filters = {{.name = "Images", .extensions = {"*.png"}}}};
    const auto r = aurora::file_dialog::open_file(opts);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().size(), 2U);
    AURORA_TEST_CHECK_STREQ(r.value()[0], "a.png");
    AURORA_TEST_CHECK_STREQ(r.value()[1], "b.jpg");
}

AURORA_TEST_CASE(save_and_folder_headless_presets) {
    HookGuard guard;

    aurora::file_dialog::headless_save_result = "out.txt";
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_STREQ(saved.value(), "out.txt");

    aurora::file_dialog::headless_folder_result = "C:/work";
    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_STREQ(folder.value(), "C:/work");
}

AURORA_TEST_CASE(non_interactive_behaves_as_cancel) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;

    // 钩子为空 + 非交互 → 等价取消（空结果，不抛错、不弹窗）。
    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_CHECK_TRUE(opened.value().empty());

    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_TRUE(saved.value().empty());

    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_TRUE(folder.value().empty());
}

AURORA_TEST_CASE(headless_hook_takes_priority_over_interactive) {
    HookGuard guard;
    aurora::file_dialog::interactive = false;

    // 钩子优先于 interactive 开关：预设值照常返回。
    aurora::file_dialog::headless_open_result = {"preset.bin"};
    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_CHECK_EQ(opened.value().size(), 1U);
    AURORA_TEST_CHECK_STREQ(opened.value()[0], "preset.bin");

    aurora::file_dialog::headless_save_result = "hooked.log";
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_STREQ(saved.value(), "hooked.log");
}

AURORA_TEST_CASE(hook_priority_skips_picker_construction) {
    HookGuard hooks;
    PickerSeamGuard seams;
    aurora::file_dialog::interactive = true;  // 交互开 + 钩子置值：仍应走钩子

    aurora::file_dialog::headless_open_result = {"preset.png"};
    aurora::file_dialog::headless_save_result = "preset.txt";
    aurora::file_dialog::headless_folder_result = "/preset";

    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_REQUIRE_EQ(opened.value().size(), 1U);
    AURORA_TEST_CHECK_STREQ(opened.value()[0], "preset.png");
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_STREQ(saved.value(), "preset.txt");
    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_STREQ(folder.value(), "/preset");

    // 钩子路径未构造选择器（计数器证未进入 UI 路径）。
    AURORA_TEST_CHECK_EQ(file_dialog::detail::picker_construct_count(), 0U);

    // 钩子优先的判别式：交互关 + 钩子置值 → 仍走钩子（先回落空即优先级被移动）。
    aurora::file_dialog::interactive = false;
    aurora::file_dialog::headless_open_result = {"still-hooked.bin"};
    const auto reopened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(reopened.ok());
    AURORA_TEST_REQUIRE_EQ(reopened.value().size(), 1U);
    AURORA_TEST_CHECK_STREQ(reopened.value()[0], "still-hooked.bin");
    AURORA_TEST_CHECK_EQ(file_dialog::detail::picker_construct_count(), 0U);
}

// ---------------------------------------------------------------------------
// 桌面自绘腿（Linux / macOS）：L0/L1 纯逻辑无头全覆盖
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(path_helper_semantics) {
    namespace detail = aurora::file_dialog::detail;

    // join_path：尾部分隔符不重复；空 base 原样返回 name。
    AURORA_TEST_CHECK_STREQ(detail::join_path("/data", "a.txt"), "/data/a.txt");
    AURORA_TEST_CHECK_STREQ(detail::join_path("/data/", "a.txt"), "/data/a.txt");
    AURORA_TEST_CHECK_STREQ(detail::join_path("", "a.txt"), "a.txt");

    // parent_path：嵌套、根目录（无父）、无分隔符相对路径（无父）；尾部分隔符不改变归属。
    AURORA_TEST_CHECK_STREQ(detail::parent_path("/data/docs"), "/data");
    AURORA_TEST_CHECK_STREQ(detail::parent_path("/data/"), "/");
    AURORA_TEST_CHECK_STREQ(detail::parent_path("/"), "");
    AURORA_TEST_CHECK_STREQ(detail::parent_path("data"), "");
}

AURORA_TEST_CASE(wildcard_match_semantics) {
    namespace detail = aurora::file_dialog::detail;

    // `*` 匹配任意段（含空）、`?` 匹配单字符；大小写敏感（POSIX 文件名语义）。
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("*.png", "a.png"));
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("*.png", ".png"));
    AURORA_TEST_CHECK_FALSE(detail::wildcard_match("*.png", "a.PNG"));
    AURORA_TEST_CHECK_FALSE(detail::wildcard_match("*.png", "a.pngx"));
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("a?c", "abc"));
    AURORA_TEST_CHECK_FALSE(detail::wildcard_match("a?c", "ac"));
    AURORA_TEST_CHECK_FALSE(detail::wildcard_match("a?c", "abbc"));

    // 多星回溯：`*a*b` 命中 "xab"（第一星吞 "x"、第二星吞空），不命中 "xb"。
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("*a*b", "xaxb"));
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("*a*b", "xab"));
    AURORA_TEST_CHECK_FALSE(detail::wildcard_match("*a*b", "xb"));
    AURORA_TEST_CHECK_TRUE(detail::wildcard_match("*", "anything"));
}

AURORA_TEST_CASE(model_initial_dir_resolution) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    // 空 initial_dir → 数据源根（文档化默认，不算降级，无 WARN）。
    {
        CapturedLog log;
        MockSource source("/");
        source.tree["/"] = {PickEntry{.name = "x", .is_dir = false}};
        file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                                   aurora::file_dialog::Options{}, source);
        AURORA_TEST_CHECK_STREQ(model.current_dir(), "/");
        AURORA_TEST_CHECK_FALSE(log.has_line_with({"WRN", "file_dialog"}));
    }
    // 显式有效 → 定位到该目录（判据 3：Options 传到状态机当前目录字段）。
    {
        MockSource source("/");
        source.tree["/data"] = {PickEntry{.name = "a.txt", .is_dir = false}};
        file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                                   aurora::file_dialog::Options{.initial_dir = "/data"}, source);
        AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data");
    }
    // 不存在 → 降级根 + 可归因 WARN（判据 2）。
    {
        CapturedLog log;
        MockSource source("/");
        source.tree["/"] = {PickEntry{.name = "x", .is_dir = false}};
        file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                                   aurora::file_dialog::Options{.initial_dir = "/nope"}, source);
        AURORA_TEST_CHECK_STREQ(model.current_dir(), "/");
        AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "initial_dir is not a directory: /nope"}));
    }
    // 存在但列不出 → 降级根 + 可归因 WARN（判据 2）。
    {
        CapturedLog log;
        MockSource source("/");
        source.tree["/"] = {PickEntry{.name = "x", .is_dir = false}};
        source.unreadable = {"/secret"};
        file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                                   aurora::file_dialog::Options{.initial_dir = "/secret"}, source);
        AURORA_TEST_CHECK_STREQ(model.current_dir(), "/");
        AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "initial_dir is not readable: /secret"}));
    }
    // 连根都列不出 → 空条目启动 + WARN，不挂起不抛。
    {
        CapturedLog log;
        MockSource source("/none");
        file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                                   aurora::file_dialog::Options{}, source);
        AURORA_TEST_CHECK_STREQ(model.current_dir(), "/none");
        AURORA_TEST_CHECK_TRUE(model.entries().empty());
        AURORA_TEST_CHECK_TRUE(model.display_name(0).empty());
        AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "source root is not readable"}));
    }
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(model_enter_go_up_stays_on_failure) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/");
    source.tree["/"] = {PickEntry{.name = "data", .is_dir = true}, PickEntry{.name = "home", .is_dir = true}};
    source.tree["/data"] = {PickEntry{.name = "docs", .is_dir = true}, PickEntry{.name = "locked", .is_dir = true},
                            PickEntry{.name = "a.txt", .is_dir = false}, PickEntry{.name = "b.txt", .is_dir = false}};
    source.tree["/data/docs"] = {PickEntry{.name = "readme.txt", .is_dir = false}};

    file_dialog::detail::FilePickerModel model(file_dialog::detail::PickerMode::OpenFile,
                                               aurora::file_dialog::Options{.initial_dir = "/data"}, source);
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data");
    AURORA_TEST_REQUIRE_EQ(model.entries().size(), 4U);
    AURORA_TEST_CHECK_STREQ(model.display_name(0), "docs/");
    AURORA_TEST_CHECK_STREQ(model.display_name(1), "locked/");
    AURORA_TEST_CHECK_STREQ(model.display_name(2), "a.txt");

    model.select(0);
    model.select(3);
    AURORA_TEST_REQUIRE_EQ(model.selection().size(), 2U);

    // 文件行不可进入：原地驻留，条目与选中原样保留。
    AURORA_TEST_CHECK_FALSE(model.enter(3));
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data");
    AURORA_TEST_REQUIRE_EQ(model.selection().size(), 2U);

    // 目录行可进入：目录切换、条目刷新、选中清空。
    AURORA_TEST_CHECK_TRUE(model.enter(0));
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data/docs");
    AURORA_TEST_REQUIRE_EQ(model.entries().size(), 1U);
    AURORA_TEST_CHECK_TRUE(model.selection().empty());
    AURORA_TEST_CHECK_FALSE(model.enter(0));  // readme.txt 是文件

    // 回父目录。
    AURORA_TEST_CHECK_TRUE(model.go_up());
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data");
    AURORA_TEST_REQUIRE_EQ(model.entries().size(), 4U);

    // 列不出的目录（is_dir 为真但 list_dir 失败）：进不去，留在原地。
    model.select(2);
    AURORA_TEST_CHECK_FALSE(model.enter(1));
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/data");
    AURORA_TEST_REQUIRE_EQ(model.entries().size(), 4U);
    AURORA_TEST_REQUIRE_EQ(model.selection().size(), 1U);
    AURORA_TEST_CHECK_EQ(model.selection()[0], 2);

    // 一路回到根；根无父目录，go_up  false。
    AURORA_TEST_CHECK_TRUE(model.go_up());
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/");
    AURORA_TEST_REQUIRE_EQ(model.entries().size(), 2U);
    AURORA_TEST_CHECK_FALSE(model.go_up());
    AURORA_TEST_CHECK_STREQ(model.current_dir(), "/");
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(model_filters_and_sorting) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/m");
    source.tree["/m"] = {PickEntry{.name = "z.txt", .is_dir = false}, PickEntry{.name = "b.png", .is_dir = false},
                         PickEntry{.name = "a.png", .is_dir = false}, PickEntry{.name = "c.jpg", .is_dir = false},
                         PickEntry{.name = "sub", .is_dir = true}};
    const aurora::file_dialog::Filter images{.name = "Images", .extensions = {"*.png"}};

    // 目录恒列 + 文件按筛选器收窄；目录在前、各块内按名字节序。
    file_dialog::detail::FilePickerModel filtered(
        file_dialog::detail::PickerMode::OpenFile,
        aurora::file_dialog::Options{.initial_dir = "/m", .filters = {images}}, source);
    AURORA_TEST_REQUIRE_EQ(filtered.entries().size(), 3U);
    AURORA_TEST_CHECK_STREQ(filtered.display_name(0), "sub/");
    AURORA_TEST_CHECK_STREQ(filtered.display_name(1), "a.png");
    AURORA_TEST_CHECK_STREQ(filtered.display_name(2), "b.png");

    // 空筛选器 = 全放行。
    file_dialog::detail::FilePickerModel all(file_dialog::detail::PickerMode::OpenFile,
                                             aurora::file_dialog::Options{.initial_dir = "/m"}, source);
    AURORA_TEST_REQUIRE_EQ(all.entries().size(), 5U);
    AURORA_TEST_CHECK_STREQ(all.display_name(0), "sub/");
    AURORA_TEST_CHECK_STREQ(all.display_name(1), "a.png");

    // 目录选择器形态忽略筛选器（目录无类型语义）。
    file_dialog::detail::FilePickerModel folder(file_dialog::detail::PickerMode::OpenFolder,
                                                aurora::file_dialog::Options{.initial_dir = "/m", .filters = {images}},
                                                source);
    AURORA_TEST_CHECK_EQ(folder.entries().size(), 5U);
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(model_selection_semantics) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/d");
    source.tree["/d"] = {PickEntry{.name = "a", .is_dir = false}, PickEntry{.name = "b", .is_dir = false},
                         PickEntry{.name = "c", .is_dir = false}};

    // 打开文件：多选切换，集合保持升序。
    file_dialog::detail::FilePickerModel open(file_dialog::detail::PickerMode::OpenFile,
                                              aurora::file_dialog::Options{.initial_dir = "/d"}, source);
    open.select(1);
    open.select(2);
    AURORA_TEST_REQUIRE_EQ(open.selection().size(), 2U);
    AURORA_TEST_CHECK_EQ(open.selection()[0], 1);
    AURORA_TEST_CHECK_EQ(open.selection()[1], 2);
    open.select(1);  // 反选
    AURORA_TEST_REQUIRE_EQ(open.selection().size(), 1U);
    AURORA_TEST_CHECK_EQ(open.selection()[0], 2);
    open.select(-1);  // 越界忽略
    open.select(99);
    AURORA_TEST_REQUIRE_EQ(open.selection().size(), 1U);
    open.clear_selection();
    AURORA_TEST_CHECK_TRUE(open.selection().empty());

    // 单选形态（保存 / 目录）：替换而非切换。
    file_dialog::detail::FilePickerModel single(file_dialog::detail::PickerMode::OpenFolder,
                                                aurora::file_dialog::Options{.initial_dir = "/d"}, source);
    single.select(0);
    single.select(2);
    AURORA_TEST_REQUIRE_EQ(single.selection().size(), 1U);
    AURORA_TEST_CHECK_EQ(single.selection()[0], 2);
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(model_confirm_absolute_paths) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/k");
    source.tree["/k"] = {PickEntry{.name = "docs", .is_dir = true}, PickEntry{.name = "a.txt", .is_dir = false},
                         PickEntry{.name = "b.txt", .is_dir = false}};

    // 打开文件：多选文件 → 绝对路径；目录行被跳过。
    file_dialog::detail::FilePickerModel open(file_dialog::detail::PickerMode::OpenFile,
                                              aurora::file_dialog::Options{.initial_dir = "/k"}, source);
    open.select(0);  // 目录行
    open.select(1);
    auto picked = open.confirm();
    AURORA_TEST_REQUIRE_EQ(picked.size(), 1U);
    AURORA_TEST_CHECK_STREQ(picked[0], "/k/a.txt");
    open.select(2);
    picked = open.confirm();
    AURORA_TEST_REQUIRE_EQ(picked.size(), 2U);
    AURORA_TEST_CHECK_STREQ(picked[0], "/k/a.txt");
    AURORA_TEST_CHECK_STREQ(picked[1], "/k/b.txt");

    // 无有效选择 → 空（取消语义，Ok 而非 Error）。
    file_dialog::detail::FilePickerModel none(file_dialog::detail::PickerMode::OpenFile,
                                              aurora::file_dialog::Options{.initial_dir = "/k"}, source);
    AURORA_TEST_CHECK_TRUE(none.confirm().empty());

    // 保存文件：路径 = 当前目录 + 输入框文件名；空名 = 取消。
    file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile,
                                              aurora::file_dialog::Options{.initial_dir = "/k"}, source);
    save.set_save_name("out.txt");
    picked = save.confirm();
    AURORA_TEST_REQUIRE_EQ(picked.size(), 1U);
    AURORA_TEST_CHECK_STREQ(picked[0], "/k/out.txt");
    save.set_save_name("");
    AURORA_TEST_CHECK_TRUE(save.confirm().empty());

    // 选择目录：仅目录行可确认，文件行视作取消。
    file_dialog::detail::FilePickerModel folder(file_dialog::detail::PickerMode::OpenFolder,
                                                aurora::file_dialog::Options{.initial_dir = "/k"}, source);
    folder.select(1);
    AURORA_TEST_CHECK_TRUE(folder.confirm().empty());
    folder.select(0);
    picked = folder.confirm();
    AURORA_TEST_REQUIRE_EQ(picked.size(), 1U);
    AURORA_TEST_CHECK_STREQ(picked[0], "/k/docs");
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(model_seeds_save_name_and_applies_extension) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/s");
    source.tree["/s"] = {PickEntry{.name = "docs", .is_dir = true}, PickEntry{.name = "a.txt", .is_dir = false}};

    // 判据 6（状态机这一处）：SaveFile 形态构造后 `save_name()` 即等于建议名的末段。
    {
        CapturedLog log;
        const aurora::file_dialog::Options opts{.initial_dir = "/s", .suggested_name = "dir/sub/report.txt"};
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile, opts, source);
        AURORA_TEST_CHECK_STREQ(save.save_name(), "report.txt");
        // 截断是改写用户输入，必须留痕（折算层在两条腿之前，故这里也读得到同一条 WARN）。
        AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "suggested_name carries a path"}));
    }
    // 未带分隔符的名字不留痕；空串与不传该字段同形（保存名保持空 = 空名即取消）。
    {
        CapturedLog log;
        file_dialog::detail::FilePickerModel save(
            file_dialog::detail::PickerMode::SaveFile,
            aurora::file_dialog::Options{.initial_dir = "/s", .suggested_name = "report.txt"}, source);
        AURORA_TEST_CHECK_STREQ(save.save_name(), "report.txt");
        AURORA_TEST_CHECK_FALSE(log.has_line_with({"WRN", "file_dialog"}));
    }
    {
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile,
                                                  aurora::file_dialog::Options{.initial_dir = "/s"}, source);
        AURORA_TEST_CHECK_TRUE(save.save_name().empty());
        AURORA_TEST_CHECK_TRUE(save.confirm().empty());  // 空名 = 取消语义不变
    }
    // 另外两枚入口如实不消费该格（打开态预选名字与「选一个已存在的文件」冲突，目录形态没有名字概念）。
    {
        const aurora::file_dialog::Options opts{.initial_dir = "/s", .suggested_name = "report.txt"};
        file_dialog::detail::FilePickerModel open(file_dialog::detail::PickerMode::OpenFile, opts, source);
        AURORA_TEST_CHECK_TRUE(open.save_name().empty());
        file_dialog::detail::FilePickerModel folder(file_dialog::detail::PickerMode::OpenFolder, opts, source);
        AURORA_TEST_CHECK_TRUE(folder.save_name().empty());
    }
    // 判据 4 在本腿的形态：扩展名在**确认时**补，名字里没打点才补。
    {
        const aurora::file_dialog::Options opts{
            .initial_dir = "/s", .suggested_name = "report", .default_extension = "txt"};
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile, opts, source);
        AURORA_TEST_CHECK_STREQ(save.save_name(), "report");  // 初值不被补全改写
        const auto picked = save.confirm();
        AURORA_TEST_REQUIRE_EQ(picked.size(), 1U);
        AURORA_TEST_CHECK_STREQ(picked[0], "/s/report.txt");
    }
    {  // 已打过扩展名 ⇒ 原样（含前导点写法同解）
        const aurora::file_dialog::Options a{
            .initial_dir = "/s", .suggested_name = "report.md", .default_extension = "txt"};
        const aurora::file_dialog::Options b{
            .initial_dir = "/s", .suggested_name = "report", .default_extension = ".txt"};
        file_dialog::detail::FilePickerModel save_a(file_dialog::detail::PickerMode::SaveFile, a, source);
        file_dialog::detail::FilePickerModel save_b(file_dialog::detail::PickerMode::SaveFile, b, source);
        AURORA_TEST_REQUIRE_EQ(save_a.confirm().size(), 1U);
        AURORA_TEST_CHECK_STREQ(save_a.confirm()[0], "/s/report.md");
        AURORA_TEST_REQUIRE_EQ(save_b.confirm().size(), 1U);
        AURORA_TEST_CHECK_STREQ(save_b.confirm()[0], "/s/report.txt");
    }
    {  // default_extension 为空 ⇒ 逐字节原样
        const aurora::file_dialog::Options opts{.initial_dir = "/s", .suggested_name = "report"};
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile, opts, source);
        AURORA_TEST_REQUIRE_EQ(save.confirm().size(), 1U);
        AURORA_TEST_CHECK_STREQ(save.confirm()[0], "/s/report");
    }
    // 用户改写后仍以改写后的值为准（建议值是预选而不是答案），改写串同样吃补全规则。
    {
        const aurora::file_dialog::Options opts{
            .initial_dir = "/s", .suggested_name = "report", .default_extension = "txt"};
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile, opts, source);
        save.set_save_name("handwritten");
        AURORA_TEST_CHECK_STREQ(save.save_name(), "handwritten");
        AURORA_TEST_REQUIRE_EQ(save.confirm().size(), 1U);
        AURORA_TEST_CHECK_STREQ(save.confirm()[0], "/s/handwritten.txt");
        save.set_save_name("");
        AURORA_TEST_CHECK_TRUE(save.confirm().empty());
    }
    // 非 ASCII 建议名在本腿落到同一份字节（判据 5 的自绘腿一侧）。
    {
        const std::string utf8_name = "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a";
        const aurora::file_dialog::Options opts{.initial_dir = "/s",
                                                .suggested_name = std::string("/\xe7\x9b\xae\xe5\xbd\x95/") + utf8_name,
                                                .default_extension = "txt"};
        file_dialog::detail::FilePickerModel save(file_dialog::detail::PickerMode::SaveFile, opts, source);
        AURORA_TEST_CHECK_TRUE(save.save_name() == utf8_name);
        AURORA_TEST_REQUIRE_EQ(save.confirm().size(), 1U);
        AURORA_TEST_CHECK_STREQ(save.confirm()[0], "/s/" + utf8_name + ".txt");
    }
#else
    AURORA_TEST_SKIP("FilePickerModel is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(view_seeds_name_input_from_suggested_name) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    MockSource source("/w");
    source.tree["/w"] = {PickEntry{.name = "docs", .is_dir = true}, PickEntry{.name = "a.txt", .is_dir = false}};
    const aurora::file_dialog::Options opts{.initial_dir = "/w", .suggested_name = "dir/sub/report.txt"};

    // 判据 6 的另一处：输入框初值与状态机同源——只喂其一不算闭合（状态机管确认结果、输入框管
    // 用户看得见的字，两处不同源就会出现「显示的名字和存下来的名字不是一回事」）。
    file_dialog::detail::FilePickerView save_view(file_dialog::detail::PickerMode::SaveFile, opts, source);
    AURORA_TEST_REQUIRE_TRUE(save_view.name_input() != nullptr);
    AURORA_TEST_CHECK_STREQ(save_view.name_input()->value(), "report.txt");
    AURORA_TEST_CHECK_STREQ(save_view.model().save_name(), save_view.name_input()->value());

    // 另两形态根本没有名字输入框（目录形态无名字概念、打开态预选名字与多选列表冲突）。
    file_dialog::detail::FilePickerView open_view(file_dialog::detail::PickerMode::OpenFile, opts, source);
    AURORA_TEST_CHECK_TRUE(open_view.name_input() == nullptr);
    AURORA_TEST_CHECK_TRUE(open_view.model().save_name().empty());
    file_dialog::detail::FilePickerView folder_view(file_dialog::detail::PickerMode::OpenFolder, opts, source);
    AURORA_TEST_CHECK_TRUE(folder_view.name_input() == nullptr);

    // 未给建议名 ⇒ 输入框保持空（与今日同形，不凭空造一个名字）。
    file_dialog::detail::FilePickerView plain(file_dialog::detail::PickerMode::SaveFile,
                                              aurora::file_dialog::Options{.initial_dir = "/w"}, source);
    AURORA_TEST_REQUIRE_TRUE(plain.name_input() != nullptr);
    AURORA_TEST_CHECK_TRUE(plain.name_input()->value().empty());
#else
    AURORA_TEST_SKIP("FilePickerView is the Linux/macOS self-drawn leg");
#endif
}

AURORA_TEST_CASE(view_confirm_cancel_and_go_up) {
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
    PickerSeamGuard seams;
    MockSource source("/v");
    source.tree["/v"] = {PickEntry{.name = "docs", .is_dir = true}, PickEntry{.name = "a.txt", .is_dir = false}};
    source.tree["/"] = {PickEntry{.name = "v", .is_dir = true}};
    Application app{Scene{Node{std::make_shared<Text>("root")}}, 320, 240};

    const aurora::file_dialog::Options opts{.title = "utest view", .initial_dir = "/v"};

    // 装配与 Options 传导：标题 / 初始目录进状态机当前目录字段（判据 3）。
    file_dialog::detail::FilePickerView view(file_dialog::detail::PickerMode::OpenFile, opts, source);
    AURORA_TEST_CHECK_STREQ(view.type_name(), "FilePickerView");
    AURORA_TEST_CHECK_STREQ(view.describe().name, "FilePickerView");
    AURORA_TEST_CHECK_STREQ(view.model().current_dir(), "/v");
    AURORA_TEST_CHECK_FALSE(view.finished());
    AURORA_TEST_CHECK_FALSE(view.is_open());

    // 模态显隐：show_modal / hide_modal 与 Dialog 开合严格配对。
    view.show_modal(app.focus());
    AURORA_TEST_CHECK_TRUE(view.is_open());
    view.model().select(1);  // a.txt
    view.confirm();
    AURORA_TEST_CHECK_TRUE(view.finished());
    AURORA_TEST_REQUIRE_EQ(view.result().size(), 1U);
    AURORA_TEST_CHECK_STREQ(view.result()[0], "/v/a.txt");
    view.hide_modal(app.focus());
    AURORA_TEST_CHECK_FALSE(view.is_open());

    // 取消语义：空结果 + 完成位（run_modal 的退出判据）。
    file_dialog::detail::FilePickerView cancel_view(file_dialog::detail::PickerMode::OpenFile, opts, source);
    cancel_view.show_modal(app.focus());
    cancel_view.cancel();
    AURORA_TEST_CHECK_TRUE(cancel_view.finished());
    AURORA_TEST_CHECK_TRUE(cancel_view.result().empty());
    cancel_view.hide_modal(app.focus());

    // 上一级：状态机目录跟随；根目录无上级时原地不动。
    file_dialog::detail::FilePickerView up_view(file_dialog::detail::PickerMode::OpenFile, opts, source);
    up_view.go_up();
    AURORA_TEST_CHECK_STREQ(up_view.model().current_dir(), "/");
    up_view.go_up();
    AURORA_TEST_CHECK_STREQ(up_view.model().current_dir(), "/");
#else
    AURORA_TEST_SKIP("FilePickerView is the Linux/macOS self-drawn leg");
#endif
}

// ---------------------------------------------------------------------------
// L3 路由：钩子三态 / 无上下文回空
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(no_context_warns_and_returns_empty) {
#ifdef AURORA_UTEST_FILE_DIALOG_SELFDRAWN
    HookGuard hooks;
    PickerSeamGuard seams;
    aurora::file_dialog::interactive = true;  // 钩子空 + 交互开 + 无 Application 上下文
    AURORA_TEST_REQUIRE_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    CapturedLog log;

    const auto opened = aurora::file_dialog::open_file();
    AURORA_TEST_REQUIRE_TRUE(opened.ok());
    AURORA_TEST_CHECK_TRUE(opened.value().empty());
    const auto saved = aurora::file_dialog::save_file();
    AURORA_TEST_REQUIRE_TRUE(saved.ok());
    AURORA_TEST_CHECK_TRUE(saved.value().empty());
    const auto folder = aurora::file_dialog::open_folder();
    AURORA_TEST_REQUIRE_TRUE(folder.ok());
    AURORA_TEST_CHECK_TRUE(folder.value().empty());

    // WARN 留痕；未构造选择器；同步返回（不挂起）。
    AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "file_dialog", "no running Application context"}));
    AURORA_TEST_CHECK_EQ(file_dialog::detail::picker_construct_count(), 0U);
#else
    AURORA_TEST_SKIP("self-drawn no-context routing needs the self-drawn leg (Linux / non-AppKit macOS)");
#endif
}

// ---------------------------------------------------------------------------
// Application::run_modal：单例还原 / 帧驱动 / 嵌套层级
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(run_modal_installs_and_restores_singletons) {
    // 入口预装「外层」单例（模拟外层 Application / TestController 的上下文）。
    SingletonSnapshot snapshot;
    Application outer{Scene{Node{std::make_shared<Text>("outer")}}, 100, 100};
    Application app{Scene{Node{std::make_shared<Text>("main")}}, 320, 240};
    Scheduler outer_sched;
    Animator outer_anim;
    Scheduler::set_current(&outer_sched);
    Animator::set_current(&outer_anim);
    file_dialog::detail::current_application_slot() = &outer;
    set_current_focus_manager(&outer.focus());
    aurora::set_strict_mode(StrictMode::On);

    bool inside_ok = true;
    std::atomic<bool> queue_ran{false};
    app.run_modal([&]() -> bool {
        inside_ok = inside_ok && (file_dialog::detail::current_application_slot() == &app);
        inside_ok = inside_ok && file_dialog::detail::modal_dispatch_suppressed();
        inside_ok = inside_ok && (Scheduler::current() != &outer_sched) && (Scheduler::current() != nullptr);
        inside_ok = inside_ok && (Animator::current() != &outer_anim) && (Animator::current() != nullptr);
        // 模态自领值班租约后（无槽可快照，共享同一全局队列），投递当帧排水执行。
        inside_ok = inside_ok && MainQueue::has_on_duty();
        if (const auto epoch = MainQueue::current_epoch(); epoch != 0) {
            static_cast<void>(
                MainQueue::try_post([&queue_ran]() -> void { queue_ran.store(true, std::memory_order_release); }));
        }
        return true;
    });
    AURORA_TEST_CHECK_TRUE(inside_ok);
    AURORA_TEST_CHECK_TRUE(queue_ran.load(std::memory_order_acquire));  // 模态帧头排空了全局队列

    // 出口逐项还原（与入口快照相等）——置空式收尾会把外层打成半身不遂。
    AURORA_TEST_CHECK_TRUE(Scheduler::current() == &outer_sched);
    AURORA_TEST_CHECK_TRUE(Animator::current() == &outer_anim);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == &outer);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
    AURORA_TEST_CHECK_TRUE(current_focus_manager() == &outer.focus());
    AURORA_TEST_CHECK_TRUE(aurora::strict_mode() == StrictMode::On);
    // 值班租约自还：模态外无循环，队列回到「无值班」默认态。
    AURORA_TEST_CHECK_FALSE(MainQueue::has_on_duty());
}

AURORA_TEST_CASE(run_modal_drives_frames_until_done) {
    PickerSeamGuard seams;
    AURORA_TEST_REQUIRE_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    Application app{Scene{Node{std::make_shared<Text>("root")}}, 320, 240};

    int frames = 0;
    bool done = false;
    app.set_on_frame([&]() -> void {
        ++frames;
        if (frames >= 3) {
            done = true;
        }
    });
    app.run_modal([&]() -> bool { return done; });

    // 第 3 帧内置完成位，第 4 次轮询退出——嵌套循环真在推进帧。
    AURORA_TEST_CHECK_EQ(frames, 3);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

AURORA_TEST_CASE(run_modal_stops_on_frame_budget) {
#ifdef AURORA_BACKEND_HEADLESS
    PickerSeamGuard seams;
    AURORA_TEST_REQUIRE_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    WindowOptions o;
    o.size = Size{.width = 200.0F, .height = 150.0F};
    o.max_frames = 2;
    o.power_saving = false;  // 无 OS 等待通道：忙轮询，帧数确定
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto window = create_window(h);
    AURORA_TEST_REQUIRE_TRUE(window.ok());
    Application app{Scene{Node{std::make_shared<Text>("root")}}, std::move(window.value()), o};

    int frames = 0;
    app.set_on_frame([&frames]() -> void { ++frames; });
    // 用户永不作答：帧预算耗尽即退，不挂起。
    app.run_modal([]() -> bool { return false; });
    AURORA_TEST_CHECK_EQ(frames, 2);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
#else
    AURORA_TEST_SKIP("frame-budget escape needs the Headless backend");
#endif
}

AURORA_TEST_CASE(run_modal_allows_one_stacked_modal) {
    PickerSeamGuard seams;
    AURORA_TEST_REQUIRE_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    Application app{Scene{Node{std::make_shared<Text>("root")}}, 320, 240};

    bool inner_ran = false;
    bool inner_context_ok = false;
    app.run_modal([&]() -> bool {
        // 外层模态的轮询点内再开一枚（恰好一枚叠一枚）：应被允许。
        app.run_modal([&]() -> bool {
            inner_ran = true;
            inner_context_ok = file_dialog::detail::current_application_slot() == &app;
            return true;
        });
        AURORA_TEST_CHECK_TRUE(inner_ran);
        AURORA_TEST_CHECK_TRUE(inner_context_ok);
        // 内层出口还原的是它入口看到的快照：外层上下文原样在位。
        AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == &app);
        return true;  // 外层随即退出
    });
    AURORA_TEST_CHECK_TRUE(inner_ran);
    AURORA_TEST_CHECK_TRUE(inner_context_ok);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

AURORA_TEST_CASE(run_modal_refuses_third_level) {
    PickerSeamGuard seams;
    AURORA_TEST_REQUIRE_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    Application app{Scene{Node{std::make_shared<Text>("root")}}, 320, 240};

    bool innermost_ran = false;
    app.run_modal([&]() -> bool {  // 第 1 层
        app.run_modal([&]() -> bool {  // 第 2 层（一枚叠一枚，允许）
            CapturedLog log;
            app.run_modal([&]() -> bool {  // 第 3 层：拒绝并 WARN
                innermost_ran = true;
                return true;
            });
            AURORA_TEST_CHECK_TRUE(log.has_line_with({"WRN", "app", "refusing deeper nesting"}));
            AURORA_TEST_CHECK_FALSE(innermost_ran);
            return true;
        });
        return true;
    });
    AURORA_TEST_CHECK_FALSE(innermost_ran);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

// ---------------------------------------------------------------------------
// 端到端会话：脚本化确认 / 取消 / 保存 / 目录选择 / 两枚叠一层
// ---------------------------------------------------------------------------

#if defined(AURORA_UTEST_FILE_DIALOG_SELFDRAWN) && defined(AURORA_BACKEND_HEADLESS)

AURORA_TEST_CASE(session_confirm_returns_picked_paths) {
    HookGuard hooks;
    PickerSeamGuard seams;
    const aurora::file_dialog::Options opts{.title = "utest open", .initial_dir = "."};
    const SelfdrawnRun run = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void {
            AURORA_TEST_CHECK_TRUE(select_first_file_row(view.model()));
            view.confirm();  // 与确认按钮回调同一条路径
        },
        [&opts]() -> Result<std::vector<std::string>> { return aurora::file_dialog::open_file(opts); });

    AURORA_TEST_REQUIRE_TRUE(run.ok);
    AURORA_TEST_REQUIRE_EQ(run.paths.size(), 1U);
    AURORA_TEST_CHECK_TRUE(run.paths[0].starts_with('/'));  // 绝对路径
    AURORA_TEST_CHECK_EQ(run.construct_count, 1U);  // 有上下文：真进了 UI 路径
    AURORA_TEST_CHECK_TRUE(run.root_restored);  // 场景根已还原
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

AURORA_TEST_CASE(session_cancel_returns_empty_and_keeps_globals) {
    HookGuard hooks;
    PickerSeamGuard seams;
    const aurora::file_dialog::Options opts{.title = "utest cancel", .initial_dir = "."};
    const SelfdrawnRun run = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void { view.cancel(); },
        [&opts]() -> Result<std::vector<std::string>> { return aurora::file_dialog::open_file(opts); });

    AURORA_TEST_REQUIRE_TRUE(run.ok);  // 取消不是失败：Ok(空)
    AURORA_TEST_CHECK_TRUE(run.paths.empty());
    AURORA_TEST_CHECK_EQ(run.construct_count, 1U);
    AURORA_TEST_CHECK_TRUE(run.root_restored);
    // 取消无副作用：全局钩子 / interactive / 接缝零改动。
    AURORA_TEST_CHECK_TRUE(aurora::file_dialog::headless_open_result.empty());
    AURORA_TEST_CHECK_TRUE(aurora::file_dialog::headless_save_result.empty());
    AURORA_TEST_CHECK_TRUE(aurora::file_dialog::headless_folder_result.empty());
    AURORA_TEST_CHECK_TRUE(aurora::file_dialog::interactive);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

AURORA_TEST_CASE(session_save_file_uses_name_input) {
    HookGuard hooks;
    PickerSeamGuard seams;
    const aurora::file_dialog::Options opts{.title = "utest save", .initial_dir = "."};
    const SelfdrawnRun run = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void {
            view.model().set_save_name("utest-note.txt");  // 文件名输入框的同一写口
            view.confirm();
        },
        [&opts]() -> Result<std::vector<std::string>> {
            const auto r = aurora::file_dialog::save_file(opts);
            if (!r.ok()) {
                return Result<std::vector<std::string>>{r.error()};
            }
            return std::vector<std::string>{r.value()};
        });

    AURORA_TEST_REQUIRE_TRUE(run.ok);
    AURORA_TEST_REQUIRE_EQ(run.paths.size(), 1U);
    AURORA_TEST_CHECK_TRUE(run.paths[0].starts_with('/'));
    AURORA_TEST_CHECK_TRUE(run.paths[0].ends_with("utest-note.txt"));
    AURORA_TEST_CHECK_EQ(run.construct_count, 1U);
    AURORA_TEST_CHECK_TRUE(run.root_restored);
}

AURORA_TEST_CASE(session_save_file_suggested_name_reaches_result) {
    HookGuard hooks;
    PickerSeamGuard seams;
    // 端到端：建议名（带目录分隔）经会话落在最终路径里，缺省扩展名在确认时补。
    const aurora::file_dialog::Options opts{.title = "utest save seeded",
                                            .initial_dir = ".",
                                            .suggested_name = "dir/sub/utest-seeded",
                                            .default_extension = "txt"};
    const SelfdrawnRun run = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void {
            AURORA_TEST_REQUIRE_TRUE(view.name_input() != nullptr);
            AURORA_TEST_CHECK_STREQ(view.name_input()->value(), "utest-seeded");  // 输入框看得见同一个名字
            view.confirm();  // 不作答改写：预选值原样确认
        },
        [&opts]() -> Result<std::vector<std::string>> {
            const auto r = aurora::file_dialog::save_file(opts);
            if (!r.ok()) {
                return Result<std::vector<std::string>>{r.error()};
            }
            return std::vector<std::string>{r.value()};
        });

    AURORA_TEST_REQUIRE_TRUE(run.ok);
    AURORA_TEST_REQUIRE_EQ(run.paths.size(), 1U);
    AURORA_TEST_CHECK_TRUE(run.paths[0].ends_with("utest-seeded.txt"));
    AURORA_TEST_CHECK_EQ(run.construct_count, 1U);
    AURORA_TEST_CHECK_TRUE(run.root_restored);

    // 用户改写仍以改写后的值为准，且已带扩展名时不再补。
    const aurora::file_dialog::Options override_opts{.title = "utest save override",
                                                     .initial_dir = ".",
                                                     .suggested_name = "utest-seeded",
                                                     .default_extension = "txt"};
    const SelfdrawnRun overridden = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void {
            view.model().set_save_name("utest-handwritten.md");  // 输入框的同一写口
            view.confirm();
        },
        [&override_opts]() -> Result<std::vector<std::string>> {
            const auto r = aurora::file_dialog::save_file(override_opts);
            if (!r.ok()) {
                return Result<std::vector<std::string>>{r.error()};
            }
            return std::vector<std::string>{r.value()};
        });
    AURORA_TEST_REQUIRE_TRUE(overridden.ok);
    AURORA_TEST_REQUIRE_EQ(overridden.paths.size(), 1U);
    AURORA_TEST_CHECK_TRUE(overridden.paths[0].ends_with("utest-handwritten.md"));
}

AURORA_TEST_CASE(session_open_folder_selects_directory) {
    HookGuard hooks;
    PickerSeamGuard seams;
    // 目录形态忽略 filters（目录无类型语义）——仍应列出全部条目供选择。
    const aurora::file_dialog::Options opts{
        .title = "utest folder", .initial_dir = ".", .filters = {{.name = "Docs", .extensions = {"*.md"}}}};
    const SelfdrawnRun run = run_selfdrawn_case(
        [](file_dialog::detail::FilePickerView &view) -> void {
            AURORA_TEST_CHECK_TRUE(select_first_dir_row(view.model()));
            view.confirm();
        },
        [&opts]() -> Result<std::vector<std::string>> {
            const auto r = aurora::file_dialog::open_folder(opts);
            if (!r.ok()) {
                return Result<std::vector<std::string>>{r.error()};
            }
            return std::vector<std::string>{r.value()};
        });

    AURORA_TEST_REQUIRE_TRUE(run.ok);
    AURORA_TEST_REQUIRE_EQ(run.paths.size(), 1U);
    AURORA_TEST_CHECK_TRUE(run.paths[0].starts_with('/'));
    AURORA_TEST_CHECK_EQ(run.construct_count, 1U);
    AURORA_TEST_CHECK_TRUE(run.root_restored);
}

AURORA_TEST_CASE(nested_pickers_stack_one_level) {
    HookGuard hooks;
    PickerSeamGuard seams;
    const aurora::file_dialog::Options opts{.title = "utest nested", .initial_dir = "."};
    WindowOptions o;
    o.size = Size{.width = 320.0F, .height = 240.0F};
    o.max_frames = 10;
    o.power_saving = false;
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto window = create_window(h);
    AURORA_TEST_REQUIRE_TRUE(window.ok());
    Application app{Scene{Node{std::make_shared<Text>("root")}}, std::move(window.value()), o};
    const aurora::Widget *const root_before = app.scene().root_node().operator->();

    file_dialog::detail::FilePickerView *view_a = nullptr;
    file_dialog::detail::FilePickerView *view_b = nullptr;
    bool a_answered = false;
    bool b_answered = false;
    bool a_ok = false;
    bool b_ok = false;
    std::vector<std::string> result_a;
    std::vector<std::string> result_b;
    int frames = 0;

    app.set_on_frame([&]() -> void {
        ++frames;
        if (frames == 1) {
            const auto ra = aurora::file_dialog::open_file(opts);  // A：外层选择器
            a_ok = ra.ok();
            if (ra.ok()) {
                result_a = ra.value();
            }
            return;
        }
        if (view_b != nullptr && !b_answered) {
            AURORA_TEST_CHECK_TRUE(select_first_file_row(view_b->model()));
            view_b->confirm();  // B 叠在 A 的内层，先作答
            b_answered = true;
            return;
        }
        if (view_a != nullptr && !a_answered) {
            AURORA_TEST_CHECK_TRUE(select_first_file_row(view_a->model()));
            view_a->confirm();
            a_answered = true;
        }
    });
    file_dialog::detail::session_observer() = [&](file_dialog::detail::FilePickerView &view) -> void {
        if (view_a == nullptr) {
            view_a = &view;
            // A 的「确认回调」：在其会话内再开 B（恰好一枚叠一枚）。
            const auto rb = aurora::file_dialog::open_file(opts);
            b_ok = rb.ok();
            if (rb.ok()) {
                result_b = rb.value();
            }
            return;
        }
        view_b = &view;
    };
    app.run();

    // 两枚选择器各自作答、结果互不串。
    AURORA_TEST_REQUIRE_TRUE(a_ok);
    AURORA_TEST_REQUIRE_TRUE(b_ok);
    AURORA_TEST_REQUIRE_EQ(result_a.size(), 1U);
    AURORA_TEST_REQUIRE_EQ(result_b.size(), 1U);
    AURORA_TEST_CHECK_TRUE(view_a != view_b);
    AURORA_TEST_CHECK_EQ(file_dialog::detail::picker_construct_count(), 2U);
    AURORA_TEST_CHECK_TRUE(app.scene().root_node().operator->() == root_before);
    AURORA_TEST_CHECK_TRUE(file_dialog::detail::current_application_slot() == nullptr);
    AURORA_TEST_CHECK_FALSE(file_dialog::detail::modal_dispatch_suppressed());
}

#else

AURORA_TEST_CASE(session_confirm_returns_picked_paths) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}
AURORA_TEST_CASE(session_cancel_returns_empty_and_keeps_globals) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}
AURORA_TEST_CASE(session_save_file_uses_name_input) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}
AURORA_TEST_CASE(session_save_file_suggested_name_reaches_result) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}
AURORA_TEST_CASE(session_open_folder_selects_directory) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}
AURORA_TEST_CASE(nested_pickers_stack_one_level) {
    AURORA_TEST_SKIP("self-drawn session e2e needs the self-drawn leg + the Headless backend");
}

#endif

}  // namespace aurora::test_cases::utest_file_dialog
