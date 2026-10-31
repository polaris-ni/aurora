// file_dialog_selfdrawn.cpp — 文件 / 目录选择器的桌面自绘实现。
//
// 架构（对应内部头 src/aurora/app/detail/file_picker.h 的四层）：
//   L0 数据源  —— FsDirectorySource（std::filesystem 默认实现；测试可注入 mock）
//   L1 状态机  —— FilePickerModel（纯逻辑，当前目录 / 条目 / 过滤 / 选择 / 确认）
//   L2 装配    —— FilePickerView（Dialog 承载的自绘模态面板，零帧循环）
//   L3 入口    —— 三个公共函数的路由：钩子优先 → 非交互回空 → 无上下文 WARN 回空
//                 → 有上下文走 FilePickerSession（挂场景根 + Application::run_modal）
//
// 选路理由（不引外部依赖）：GTK / Qt 会把链接期桌面依赖加到每个使用方；zenity 一类子进程
// 把「装了这个二进制」变成运行期前提；xdg-desktop-portal 的 FileChooser 是异步 DBus 信号，
// 本库无 DBus 客户端。框架自绘零新增依赖，且状态机层可完全无头证。
//
// 门控：L0–L2 在 Linux 与 macOS 上都编译（模型 / 视图可在无头用例里直接断言）；L3 的公共
// 入口只在**自绘腿真实生效**的组合上定义——Linux，以及「macOS 平台但未启用 AppKit 后端」
// （GLFW / X11 / Headless）。macOS + AppKit 后端由 `file_dialog_macos.cpp` 提供原生面板，
// 两处定义互斥，避免同符号重复定义。其余平台（Win32 走 file_dialog_win32.cpp；WASM /
// Android / BSD 保留 file_dialog.h 内联回退）本文件不含任何定义。
#include "aurora/core/platform.h"
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/detail/file_dialog_plan.h"
#include "aurora/app/detail/file_picker.h"
#include "aurora/app/file_dialog.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/event/focus.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/data_widgets.h"
#include "aurora/widget/dialog.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

namespace fs = std::filesystem;

namespace aurora::file_dialog {
namespace {

/// @brief 选择器条目列表的视口高度（dp）：Scroll 限高，超长目录走容器内滚动。
constexpr float AURORA_PICKER_LIST_HEIGHT = 240.0F;

/// @brief 内容纵向间距（dp）。
constexpr float AURORA_PICKER_GAP = 8.0F;

/// @brief L0 默认实现：std::filesystem 目录枚举（错误码形态，不抛）。
class FsDirectorySource final : public detail::DirectorySource {
  public:
    [[nodiscard]] auto list_dir(const std::string &path) const -> std::optional<std::vector<Entry>> override {
        std::vector<Entry> entries;
        std::error_code ec;
        fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            return std::nullopt;  // 不存在 / 不可读 / 不是目录：交由调用方降级
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                return std::nullopt;
            }
            const auto &entry_path = it->path();
            entries.push_back(Entry{.name = entry_path.filename().string(), .is_dir = it->is_directory(ec)});
            if (ec) {
                return std::nullopt;
            }
        }
        return entries;
    }

    [[nodiscard]] auto is_dir(const std::string &path) const -> bool override {
        std::error_code ec;
        return fs::is_directory(path, ec);
    }

    [[nodiscard]] auto to_absolute(const std::string &path) const -> std::string override {
        std::error_code ec;
        const fs::path absolute = fs::absolute(path, ec);
        if (ec || absolute.empty()) {
            return path;  // 归一提不出绝对形态时原样返回（由 list_dir 的成败决定可用性）
        }
        return absolute.string();
    }

    [[nodiscard]] auto root() const -> std::string override {
        std::error_code ec;
        const fs::path current = fs::current_path(ec);
        return ec ? std::string{"/"} : current.string();
    }
};

/// @brief 形态缺省标题（`Options::title` 为空时的自绘腿默认值）。
auto default_title(detail::PickerMode mode) -> std::string {
    switch (mode) {
        case detail::PickerMode::OpenFile:
            return "Open File";
        case detail::PickerMode::SaveFile:
            return "Save File";
        case detail::PickerMode::OpenFolder:
            return "Select Folder";
    }
    return "Select";
}

/// @brief 确认按钮文案（随形态变化）。
auto confirm_label(detail::PickerMode mode) -> std::string {
    switch (mode) {
        case detail::PickerMode::OpenFile:
            return "Open";
        case detail::PickerMode::SaveFile:
            return "Save";
        case detail::PickerMode::OpenFolder:
            return "Select";
    }
    return "OK";
}

/// @brief 筛选器摘要行（如 `Images (*.png *.jpg)`；空筛选器返回空串）。
auto filter_summary(const std::vector<file_dialog::Filter> &filters) -> std::string {
    std::string out;
    for (const auto &filter : filters) {
        if (!out.empty()) {
            out += "; ";
        }
        out += filter.name;
        out += " (";
        for (std::size_t i = 0; i < filter.extensions.size(); ++i) {
            if (i != 0U) {
                out += ' ';
            }
            out += filter.extensions[i];
        }
        out += ')';
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// L1：状态机
// ---------------------------------------------------------------------------

namespace detail {

FilePickerModel::FilePickerModel(PickerMode mode, file_dialog::Options opts, const DirectorySource &source)
    : mode_(mode), opts_(std::move(opts)), source_(&source) {
    // 保存形态的名字栏初值：`suggested_name` 过折算层取末段后播种（Win32 腿投给 OS 的名字栏，本腿
    // 投给状态机——同一份换算，两腿才落出同一个名字）。非保存形态一概不消费该格。
    if (mode_ == PickerMode::SaveFile) {
        save_name_ = aurora::file_dialog_plan::suggested_file_name(opts_.suggested_name);
    }
    // 初始目录三级口径：未指定 → 数据源根（文档化默认，不算降级）；指定不可用 → 降级根并留痕；
    // 显式指定的目录列不出 → 同样降级根并留痕；连根都列不出 → 空条目启动（仍留痕）。
    const bool specified = !opts_.initial_dir.empty();
    std::string dir;
    std::string reason;  // 非空 = 已发生的降级原因（退出前留痕）
    if (!specified) {
        dir = source_->root();
    } else if (!source_->is_dir(opts_.initial_dir)) {
        dir = source_->root();
        reason = "initial_dir is not a directory: " + opts_.initial_dir;
    } else {
        dir = source_->to_absolute(opts_.initial_dir);
    }
    current_dir_ = dir;
    if (refresh()) {
        if (!reason.empty()) {
            AURORA_LOG_WARN("file_dialog", reason + "; falling back to source root");
        }
        return;
    }
    if (specified && reason.empty()) {
        AURORA_LOG_WARN("file_dialog",
                        "initial_dir is not readable: " + current_dir_ + "; falling back to source root");
        current_dir_ = source_->root();
        if (refresh()) {
            return;
        }
    }
    AURORA_LOG_WARN("file_dialog", "source root is not readable; picker starts with no entries");
}

auto FilePickerModel::refresh() -> bool {
    const auto listed = source_->list_dir(current_dir_);
    if (!listed.has_value()) {
        return false;  // 列不出：不改动既有状态（进入失败时调用方据此留在原地）
    }
    std::vector<DirectorySource::Entry> entries;
    entries.reserve(listed->size());
    for (const auto &entry : *listed) {
        // 目录恒列（目录无类型语义；目录选择器形态连文件也不按筛选器收窄）；
        // 其余形态的文件受筛选器约束（空筛选器 = 全放行）。
        if (entry.is_dir || mode_ == PickerMode::OpenFolder || opts_.filters.empty() || matches_filters(entry.name)) {
            entries.push_back(entry);
        }
    }
    std::ranges::sort(entries, [](const DirectorySource::Entry &a, const DirectorySource::Entry &b) -> bool {
        if (a.is_dir != b.is_dir) {
            return a.is_dir;  // 目录在前
        }
        return a.name < b.name;
    });
    entries_ = std::move(entries);
    selection_.clear();
    return true;
}

auto FilePickerModel::display_name(std::size_t i) const -> std::string {
    if (i >= entries_.size()) {
        return {};
    }
    return entries_[i].is_dir ? entries_[i].name + "/" : entries_[i].name;
}

auto FilePickerModel::matches_filters(const std::string &name) const -> bool {
    for (const auto &filter : opts_.filters) {
        for (const auto &pattern : filter.extensions) {
            if (wildcard_match(pattern, name)) {
                return true;
            }
        }
    }
    return false;
}

auto FilePickerModel::select(int row) -> void {
    if (row < 0 || std::cmp_greater_equal(row, entries_.size())) {
        return;
    }
    if (mode_ == PickerMode::OpenFile) {
        const auto it = std::ranges::find(selection_, row);
        if (it != selection_.end()) {
            selection_.erase(it);
        } else {
            selection_.push_back(row);
            std::ranges::sort(selection_);
        }
        return;
    }
    selection_.assign(1, row);  // 单选形态：替换
}

auto FilePickerModel::clear_selection() -> void { selection_.clear(); }

auto FilePickerModel::enter(int row) -> bool {
    if (row < 0 || std::cmp_greater_equal(row, entries_.size()) || !entries_[static_cast<std::size_t>(row)].is_dir) {
        return false;
    }
    const std::string previous = current_dir_;
    current_dir_ = join_path(current_dir_, entries_[static_cast<std::size_t>(row)].name);
    if (!refresh()) {
        current_dir_ = previous;  // 进不去的目录：留在原地（既有条目与选中原样保留）
        return false;
    }
    return true;
}

auto FilePickerModel::go_up() -> bool {
    const std::string parent = parent_path(current_dir_);
    if (parent.empty() || parent == current_dir_) {
        return false;
    }
    const std::string previous = current_dir_;
    current_dir_ = parent;
    if (!refresh()) {
        current_dir_ = previous;
        return false;
    }
    return true;
}

auto FilePickerModel::confirm() const -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto is_file_row = [this](int row) -> bool {
        return row >= 0 && std::cmp_less(row, entries_.size()) && !entries_[static_cast<std::size_t>(row)].is_dir;
    };
    const auto is_dir_row = [this](int row) -> bool {
        return row >= 0 && std::cmp_less(row, entries_.size()) && entries_[static_cast<std::size_t>(row)].is_dir;
    };
    switch (mode_) {
        case PickerMode::OpenFile:
            for (const int row : selection_) {
                if (is_file_row(row)) {
                    out.push_back(join_path(current_dir_, entries_[static_cast<std::size_t>(row)].name));
                }
            }
            break;
        case PickerMode::SaveFile:
            if (!save_name_.empty()) {
                // 扩展名在确认时补（用户没打点才补），故改写过的名字同样吃这条规则；
                // Win32 腿由 OS 在名字栏里补，两腿结果同一句：选定路径以该扩展名结尾。
                out.push_back(join_path(current_dir_, aurora::file_dialog_plan::with_default_extension(
                                                          save_name_, opts_.default_extension)));
            }
            break;
        case PickerMode::OpenFolder:
            if (!selection_.empty() && is_dir_row(selection_.front())) {
                const auto row = static_cast<std::size_t>(selection_.front());
                out.push_back(join_path(current_dir_, entries_[row].name));
            }
            break;
    }
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// L2：装配
// ---------------------------------------------------------------------------

namespace detail {

FilePickerView::FilePickerView(PickerMode mode, const file_dialog::Options &opts, const DirectorySource &source)
    : model_(mode, opts, source) {
    ++picker_construct_count();

    auto title = std::make_shared<Text>(opts.title.empty() ? default_title(mode) : opts.title);
    title->font_size(18).bold();

    path_label_ = std::make_shared<Text>(model_.current_dir());
    path_label_->font_size(12);

    list_ = std::make_shared<ListView>(std::vector<std::string>{}, mode == PickerMode::OpenFile);
    list_->set_on_select([this](int row) -> void { on_row_selected(row); });
    auto list_scroll = std::make_shared<Scroll>(ScrollProps{.child = Node{list_}});
    list_scroll->height(Length::fixed(AURORA_PICKER_LIST_HEIGHT));

    auto up_button = std::make_shared<Button>(ButtonProps{.label = "Up"});
    up_button->set_on_click([this]() -> void { go_up(); });

    confirm_button_ = std::make_shared<Button>(ButtonProps{.label = confirm_label(mode)});
    confirm_button_->set_on_click([this]() -> void { confirm(); });
    confirm_button_->set_enabled(has_confirmable_selection());

    auto cancel_button = std::make_shared<Button>(ButtonProps{.label = "Cancel"});
    cancel_button->set_on_click([this]() -> void { cancel(); });

    // 路径行：上一级 + （保存形态）文件名输入框。
    std::vector<Node> path_row;
    path_row.emplace_back(up_button);
    if (mode == PickerMode::SaveFile) {
        name_input_ = std::make_shared<TextInput>();
        name_input_->set_placeholder("File name");
        // 输入框初值与状态机同源（只喂其一不算闭合：状态机管确认结果、输入框管用户看得见的字）。
        name_input_->set_value(model_.save_name());
        name_input_->set_on_changed([this](const std::string &value) -> void { model_.set_save_name(value); });
        path_row.emplace_back(name_input_);
    }

    std::vector<Node> column;
    column.emplace_back(title);
    column.emplace_back(path_label_);
    column.emplace_back(Row(RowProps{.children = std::move(path_row), .gap = AURORA_PICKER_GAP}));
    column.emplace_back(list_scroll);
    if (!opts.filters.empty()) {
        auto filter_label = std::make_shared<Text>(filter_summary(opts.filters));
        filter_label->font_size(12);
        column.emplace_back(filter_label);
    }
    column.emplace_back(
        Row(RowProps{.children = {Node{confirm_button_}, Node{cancel_button}}, .gap = AURORA_PICKER_GAP}));

    set_content(Node{Column(ColumnProps{.children = std::move(column), .gap = AURORA_PICKER_GAP})});
    refresh_ui();
}

FilePickerView::~FilePickerView() = default;

auto FilePickerView::describe() const -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "FilePickerView",
        .properties = {},
        .events = {"on_close"},
        .children_policy = "single",
        .examples = {"internal self-drawn file picker panel (not registered)"},
    };
}

auto FilePickerView::show_modal(FocusManager &fm) -> void {
    // Dialog::show / close 的焦点作用域压入 / 弹出以 current_focus_manager() 非空为条件，
    // 而选择器的打开点（菜单 / 帧回调 / 脚本化直调）未必在派发栈内——自包装保证配对。
    FocusManager *previous = current_focus_manager();
    set_current_focus_manager(&fm);
    show();
    set_current_focus_manager(previous);
}

auto FilePickerView::hide_modal(FocusManager &fm) -> void {
    FocusManager *previous = current_focus_manager();
    set_current_focus_manager(&fm);
    close();
    set_current_focus_manager(previous);
}

auto FilePickerView::confirm() -> void {
    result_ = model_.confirm();
    finished_ = true;
}

auto FilePickerView::cancel() -> void {
    result_.clear();
    finished_ = true;
}

auto FilePickerView::go_up() -> void {
    if (model_.go_up()) {
        refresh_ui();
    }
}

auto FilePickerView::refresh_ui() -> void {
    // ListView 无 clear-all：逐行移除后按状态机重填（on_remove 未注册，无副作用）。
    while (list_->item_count() > 0) {
        list_->remove(0);
    }
    const std::size_t count = model_.entries().size();
    for (std::size_t i = 0; i < count; ++i) {
        list_->append(model_.display_name(i));
    }
    path_label_->set_content(model_.current_dir());
    sync_list_selection();
}

auto FilePickerView::on_row_selected(int row) -> void {
    if (syncing_) {
        return;  // 镜像回写触发的回调：模型已是真源，不再二次进入
    }
    model_.select(row);
    sync_list_selection();
}

auto FilePickerView::sync_list_selection() -> void {
    syncing_ = true;
    list_->clear_selection();
    for (const int row : model_.selection()) {
        list_->select(row);  // 会再触发 on_select，被 syncing_ 拦下
    }
    syncing_ = false;
    confirm_button_->set_enabled(has_confirmable_selection());
}

auto FilePickerView::has_confirmable_selection() const -> bool {
    if (model_.mode() == PickerMode::SaveFile) {
        return !model_.save_name().empty();
    }
    return !model_.selection().empty();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// L3：模态会话与公共入口
// ---------------------------------------------------------------------------

namespace detail {

FilePickerSession::FilePickerSession(Application &app, PickerMode mode, const file_dialog::Options &opts,
                                     const DirectorySource &source)
    : app_(app), picker_(std::make_shared<FilePickerView>(mode, opts, source)) {
    if (session_observer()) {
        session_observer()(*picker_);  // 测试接缝：拿到视图即可脚本化触发确认 / 取消
    }
}

// 抛出面：`detach()` 内是场景根的 shared_ptr 赋值与浮层摘除，模态收尾期无分配需求，
// 实际不可抛（同 `Application::SingletonRestore::~SingletonRestore` 的处置口径）；
// 析构又隐式 noexcept，逐层 noexcept 化会把调用面的异常语义改掉，故点名豁免。
// NOLINTNEXTLINE(bugprone-exception-escape)
FilePickerSession::~FilePickerSession() { detach(); }

auto FilePickerSession::attach() -> void {
    Scene &scene = app_.scene();
    Node &root = scene.root_node();
    // 直挂路径：场景根已是浮层宿主且带基础内容 → 直接追加（应用树零重挂）。
    if (auto *host = dynamic_cast<OverlayHost *>(root.operator->()); host != nullptr && !host->child_nodes().empty()) {
        if (const auto index = host->add_overlay(Node{picker_}); index.has_value()) {
            overlay_host_ = host;
            overlay_index_ = *index;
            attached_ = true;
            return;
        }
    }
    // 包壳路径：原根作基础内容、选择器作浮层；原根 widget 实例不变（shared 句柄移交），
    // 退出时整体还原——挂载幂等判据（宿主 id）保证应用子树不重挂、状态不丢。
    saved_root_ = root;
    auto wrapper = std::make_shared<OverlayHost>(Node{saved_root_});
    (void)wrapper->add_overlay(Node{picker_});
    root = Node{wrapper};
    wrapped_ = true;
    attached_ = true;
}

auto FilePickerSession::detach() -> void {
    if (!attached_) {
        return;
    }
    if (wrapped_) {
        app_.scene().root_node() = saved_root_;  // 还原场景根（包壳节点随之释放）
    } else if (overlay_host_ != nullptr) {
        overlay_host_->remove_overlay(overlay_index_);
    }
    attached_ = false;
    wrapped_ = false;
    overlay_host_ = nullptr;
}

auto FilePickerSession::run() -> std::vector<std::string> {
    attach();
    FocusManager &fm = app_.focus();
    picker_->show_modal(fm);
    app_.run_modal([this]() -> bool { return picker_->finished(); });
    picker_->hide_modal(fm);
    detach();
    return picker_->result();
}

}  // namespace detail

namespace {

// 自绘腿的公共入口只在「本腿真实生效」的组合上定义：Linux，或 macOS 未启用 AppKit 后端。
// macOS + AppKit 的入口在 file_dialog_macos.cpp（原生面板），两处互斥。
#if defined(AURORA_PLATFORM_LINUX) || (defined(AURORA_PLATFORM_MACOS) && !defined(AURORA_BACKEND_MACOS))

/// @brief 自绘腿统一路由：无 Application 上下文即等价非交互（WARN 回空，不挂起）。
auto run_selfdrawn(detail::PickerMode mode, const Options &opts, const detail::DirectorySource &source)
    -> std::vector<std::string> {
    Application *app = detail::current_application_slot();
    if (app == nullptr) {
        AURORA_LOG_WARN("file_dialog", "no running Application context; returning empty result (equivalent to cancel)");
        return {};
    }
    detail::FilePickerSession session{*app, mode, opts, source};
    return session.run();
}

}  // namespace

auto open_file(const Options &opts) -> Result<std::vector<std::string>> {
    headless_last_options = opts;
    if (!headless_open_result.empty()) {
        return headless_open_result;  // 钩子优先：未构造选择器（计数器可证）
    }
    if (!interactive) {
        return std::vector<std::string>{};  // 非交互 = 取消（不弹 UI）
    }
    const FsDirectorySource source;
    return Result<std::vector<std::string>>{run_selfdrawn(detail::PickerMode::OpenFile, opts, source)};
}

auto save_file(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_save_result.empty()) {
        return headless_save_result;
    }
    if (!interactive) {
        return std::string{};
    }
    const FsDirectorySource source;
    const std::vector<std::string> picked = run_selfdrawn(detail::PickerMode::SaveFile, opts, source);
    return Result<std::string>{picked.empty() ? std::string{} : picked.front()};
}

auto open_folder(const Options &opts) -> Result<std::string> {
    headless_last_options = opts;
    if (!headless_folder_result.empty()) {
        return headless_folder_result;
    }
    if (!interactive) {
        return std::string{};
    }
    const FsDirectorySource source;
    const std::vector<std::string> picked = run_selfdrawn(detail::PickerMode::OpenFolder, opts, source);
    return Result<std::string>{picked.empty() ? std::string{} : picked.front()};
}

#else

// macOS + AppKit：入口在 file_dialog_macos.cpp。这里留一枚编译期锚点，防止将来误把
// 本 TU 的门控改宽后出现「两条腿都没定义」的静默链接错误而无从归因。
static_assert(true, "self-drawn entries are disabled on macOS with the AppKit backend");

}  // namespace

#endif  // 自绘腿入口生效条件

}  // namespace aurora::file_dialog

#endif  // defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_MACOS)
