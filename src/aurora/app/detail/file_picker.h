#pragma once

// 文件 / 目录选择器（框架自绘腿）的内部实现：L0 目录数据源、L1 状态机、L2 装配与模态会话。
//
// 为何是内部头而非公共头：选择器复合体**不进 WidgetRegistry、不新增公共 widget**
// （这是 `aurora_api.json` 零差异的前提），三入口的公共契约仍在
// `include/aurora/app/file_dialog.h`。平台中性的纯逻辑（数据源接口、状态机、路径折算）
// 放本头，可被无头单测直接断言；std::filesystem 默认数据源与 widget 装配放在
// `src/aurora/app/file_dialog_selfdrawn.cpp`（Linux / macOS 桌面腿）。

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/app/file_dialog.h"
#include "aurora/core/log.h"
#include "aurora/widget/dialog.h"
#include "aurora/widget/widget.h"

namespace aurora {

class Application;  // 前置声明：模态会话只持指针，免把 application.h 拖进每个包含者
class Button;
class FocusManager;
class ListView;
class OverlayHost;
class Text;
class TextInput;

namespace file_dialog::detail {

/// @brief 选择器形态（三个公共入口各自的路由目标）。
enum class PickerMode : std::uint8_t {
    OpenFile,  ///< 打开文件（可多选）
    SaveFile,  ///< 保存文件（文件名由输入框给出）
    OpenFolder,  ///< 选择目录
};

// ---------------------------------------------------------------------------
// L0：目录数据源
// ---------------------------------------------------------------------------

/// @brief 目录枚举数据源：状态机与 UI 都不直接碰文件系统，测试注入 mock 即可无头证。
/// @note Thread: main-thread only
class DirectorySource {  // NOLINT(cppcoreguidelines-special-member-functions):
                         // 抽象数据源基类，派生以值/引用注入，无需复制/移动语义
  public:
    /// @brief 一个目录直属条目。
    struct Entry {
        std::string name;  ///< 条目名（不含路径分隔符）
        bool is_dir = false;  ///< 是否为目录
    };

    virtual ~DirectorySource() = default;

    /// @brief 枚举目录直属条目（不含 `.` / `..`）。
    /// @param path 目标目录（调用方保证为绝对路径）。
    /// @return 条目列表；目录不存在 / 不可读时返回 `std::nullopt`（调用方降级并留痕）。
    [[nodiscard]] virtual auto list_dir(const std::string &path) const -> std::optional<std::vector<Entry>> = 0;

    /// @brief 路径是否为目录（不存在 / 不可判定时为 false）。
    /// @param path 目标路径。
    [[nodiscard]] virtual auto is_dir(const std::string &path) const -> bool = 0;

    /// @brief 归一为绝对路径（已是绝对路径时语义上原样返回）。
    /// @param path 待归一路径。
    [[nodiscard]] virtual auto to_absolute(const std::string &path) const -> std::string = 0;

    /// @brief 数据源根：初始目录为空或不可用时的落点。
    [[nodiscard]] virtual auto root() const -> std::string = 0;
};

/// @brief 通配匹配（`*` 匹配任意段、`?` 匹配单字符；大小写敏感，与 POSIX 文件名语义一致）。
/// @param pattern 通配模式（如 `*.png`）。
/// @param text 待匹配文本（文件名）。
/// @return 匹配为 true。
[[nodiscard]] inline auto wildcard_match(std::string_view pattern, std::string_view text) -> bool {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p;
            ++p;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            ++star_t;
            t = star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

/// @brief 路径拼接（`base` 以 `/` 收尾则不重复分隔符；`base` 为空时原样返回 `name`）。
/// @param base 目录部分。
/// @param name 条目名。
[[nodiscard]] inline auto join_path(const std::string &base, const std::string &name) -> std::string {
    if (base.empty()) {
        return name;
    }
    if (base.back() == '/') {
        return base + name;
    }
    return base + "/" + name;
}

/// @brief 父目录（根目录或相对路径无上级时返回空串——调用方据此判「无上级」）。
/// @param path 绝对路径。
[[nodiscard]] inline auto parent_path(const std::string &path) -> std::string {
    std::size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;  // 剥掉尾部分隔符；根目录 "/" 剥至 end == 0
    }
    if (end == 0) {
        return {};  // 根目录：无父目录
    }
    const std::size_t slash = path.find_last_of('/', end - 1);
    if (slash == std::string::npos) {
        return {};  // 无分隔符（相对路径）：无父目录
    }
    if (slash == 0) {
        return "/";  // 父目录即根
    }
    return path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// L1：状态机（纯逻辑，无 UI）
// ---------------------------------------------------------------------------

/// @brief 选择器状态机：当前目录、条目、过滤匹配、选择与确认 / 取消。
///
/// 与 UI 完全解耦：无头单测以 mock 数据源断「初始目录定位 / 进出目录 / 过滤匹配 /
/// 确认返回绝对路径 / 取消返回空」全链；UI 只作它的镜像（选中行经 `select` 回写）。
/// @note Thread: main-thread only
class FilePickerModel {
  public:
    /// @brief 构造并解析初始目录（空 → 数据源根；不存在 / 不可读 → 降级根 + WARN）。
    ///        目录形态（OpenFolder）不收窄文件条目——目录无类型语义，`filters` 被忽略。
    /// @param mode 选择器形态。
    /// @param opts 公共选项（消费 `initial_dir` 与 `filters`；保存形态另消费 `suggested_name` 播种
    ///        保存名、`default_extension` 在确认时补；`title` 归 UI）。
    /// @param source 目录数据源（生命周期须长于本模型）。
    FilePickerModel(PickerMode mode, file_dialog::Options opts, const DirectorySource &source);

    [[nodiscard]] auto mode() const -> PickerMode { return mode_; }
    /// @brief 当前目录（构造后即为解析出的初始目录；恒为绝对路径）。
    [[nodiscard]] auto current_dir() const -> const std::string & { return current_dir_; }
    /// @brief 过滤 + 排序后的条目（目录在前，各块内按名字节序）。
    [[nodiscard]] auto entries() const -> const std::vector<DirectorySource::Entry> & { return entries_; }
    /// @brief 第 i 行的显示名（目录带 `/` 尾缀；下标与 `entries()` 对齐，越界为空串）。
    /// @param i 行号。
    [[nodiscard]] auto display_name(std::size_t i) const -> std::string;
    /// @brief 当前选中行集合（升序；单选形态至多一项）。
    [[nodiscard]] auto selection() const -> const std::vector<int> & { return selection_; }
    /// @brief 选中 / 反选一行（多选形态切换、单选形态替换；越界忽略）。
    /// @param row 行号。
    auto select(int row) -> void;
    /// @brief 清空选中集合。
    auto clear_selection() -> void;
    /// @brief 进入指定行（仅目录项可进入；目标列不出时留在原地并返回 false）。
    /// @param row 行号。
    auto enter(int row) -> bool;
    /// @brief 回父目录（已在根或父目录列不出时返回 false）。
    auto go_up() -> bool;
    /// @brief 写入保存文件名（仅 SaveFile 形态的确认消费；初值取自 `Options::suggested_name` 末段）。
    /// @param name 文件名（可为空）。
    auto set_save_name(std::string name) -> void { save_name_ = std::move(name); }
    /// @brief 当前保存文件名。
    [[nodiscard]] auto save_name() const -> const std::string & { return save_name_; }
    /// @brief 确认：返回选中的绝对路径列表（无有效选择时为空——取消语义）。
    ///
    /// 保存形态在确认这一刻按 `default_extension` 补全（名字里没打点才补），故中途改写过的名字
    /// 也吃同一条规则；补全规则本身在折算层，两腿共用一份。
    [[nodiscard]] auto confirm() const -> std::vector<std::string>;

  private:
    /// @brief 按当前目录重枚举 + 过滤 + 排序；列不出时**不改动**既有状态并返回 false。
    auto refresh() -> bool;
    /// @brief 文件名是否通过筛选器（筛选器为空 = 全放行）。
    [[nodiscard]] auto matches_filters(const std::string &name) const -> bool;

    PickerMode mode_;
    file_dialog::Options opts_;
    const DirectorySource *source_;
    std::string current_dir_;
    std::vector<DirectorySource::Entry> entries_;
    std::vector<int> selection_;
    std::string save_name_;
};

// ---------------------------------------------------------------------------
// L2：选择器复合 widget（声明；装配定义在自绘 TU）
// ---------------------------------------------------------------------------

/// @brief 选择器复合 widget：以 `Dialog` 为体的自绘模态面板，**自带零帧循环**。
///
/// 形态：`Dialog`（居中内容 + 遮罩 + 焦点作用域 push/pop）承载一个纵向 `Column`——
/// 标题、当前路径、上一级按钮、条目 `ListView`（`Scroll` 限高视口内滚动）、
/// 保存形态另加文件名 `TextInput`、底部确认 / 取消按钮行。选中行的唯一真源是
/// `FilePickerModel`，ListView 只作镜像（回写经 `syncing_` 防递归）。
/// @note Thread: main-thread only
class FilePickerView : public Dialog {
  public:
    /// @brief 构造并装配面板（初始为关闭态，须 `show_modal` 才渲染）。
    /// @param mode 选择器形态。
    /// @param opts 公共选项（标题 / 初始目录 / 筛选器；保存形态另取建议名作输入框初值）。
    /// @param source 目录数据源（生命周期须长于本视图）。
    FilePickerView(PickerMode mode, const file_dialog::Options &opts, const DirectorySource &source);
    /// @brief 析构定义在自绘 TU：成员持 Dialog/ListView 等不完整类型的 shared_ptr。
    ~FilePickerView() override;

    FilePickerView(const FilePickerView &) = delete;
    auto operator=(const FilePickerView &) -> FilePickerView & = delete;
    FilePickerView(FilePickerView &&) = delete;
    auto operator=(FilePickerView &&) -> FilePickerView & = delete;

    /// @brief 控件类型名（结构快照 / 诊断用）。
    /// @return 字面量 `"FilePickerView"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "FilePickerView"; }
    /// @brief 实例级自描述：选择器面板的描述表（不进注册表，仅供诊断 / 结构快照）。
    /// @return 名为 "FilePickerView" 的描述表（无属性、`on_close` 事件、单子节点策略）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override;

    /// @brief 打开模态面板：自包装当前焦点管理器后调 `Dialog::show`——调用点未必在派发栈内
    ///        （选择器由菜单 / 帧回调打开），而 `Dialog::show` 的焦点作用域压入只在
    ///         `current_focus_manager()` 非空时发生；不包装则模态失焦陷阱静默缺失。
    /// @param fm 面板所在宿主的焦点管理器。
    auto show_modal(FocusManager &fm) -> void;
    /// @brief 关闭模态面板（与 `show_modal` 严格配对；重复调用幂等）。
    /// @param fm 面板所在宿主的焦点管理器。
    auto hide_modal(FocusManager &fm) -> void;

    /// @brief 确认（与确认按钮回调同一条路径）：写结果并置完成位。
    auto confirm() -> void;
    /// @brief 取消（与取消按钮同一条路径）：空结果 + 完成位。
    auto cancel() -> void;
    /// @brief 上一级（与上一级按钮回调同一条路径）。
    auto go_up() -> void;

    /// @brief 用户是否已作答（确认 / 取消 / 模态被外部终止）。
    [[nodiscard]] auto finished() const { return finished_; }
    /// @brief 确认产物的绝对路径列表（取消 / 无有效选择时为空）。
    [[nodiscard]] auto result() const -> const std::vector<std::string> & { return result_; }
    /// @brief 内部状态机（测试观测当前目录 / 条目 / 选中行）。
    [[nodiscard]] auto model() -> FilePickerModel & { return model_; }
    /// @brief 文件名输入框（仅保存形态存在，其余形态为空指针）。
    ///
    /// 存在的理由：`Options::suggested_name` 要同时喂状态机的保存名（决定确认结果）与输入框初值
    /// （决定用户看得见的字），只喂其一都算不得消费——本访问口让两处各自可观测。
    [[nodiscard]] auto name_input() const -> const std::shared_ptr<TextInput> & { return name_input_; }

  private:
    auto refresh_ui() -> void;
    auto on_row_selected(int row) -> void;
    auto sync_list_selection() -> void;
    [[nodiscard]] auto has_confirmable_selection() const -> bool;

    FilePickerModel model_;
    std::shared_ptr<ListView> list_;
    std::shared_ptr<Text> path_label_;
    std::shared_ptr<TextInput> name_input_;
    std::shared_ptr<Button> confirm_button_;
    bool syncing_ = false;  ///< ListView 回写期的递归护栏
    bool finished_ = false;  ///< 完成位（run_modal 的退出判据）
    std::vector<std::string> result_;  ///< 确认产物（绝对路径列表）
};

// ---------------------------------------------------------------------------
// L3：模态会话（挂载 / 驱动 / 摘除）
// ---------------------------------------------------------------------------

/// @brief 模态会话：把选择器挂上主窗口场景根、驱动 `Application::run_modal`、摘除并交还结果。
///
/// 挂载两路：场景根本就是 `OverlayHost` → 直接追加浮层（应用树零重挂）；
/// 否则包一层 `OverlayHost`（原根作基础内容）并在退出时还原——原根 widget 实例不变，
/// 挂载幂等判据（宿主 id）保证应用子树不重挂、状态不丢。
/// @note Thread: main-thread only
class FilePickerSession {
  public:
    /// @brief 构造会话（此间不挂载、不弹UI；挂载发生在 `run()` 首步）。
    /// @param app 应用上下文（持主窗口场景根与帧循环）。
    /// @param mode 选择器形态。
    /// @param opts 公共选项。
    /// @param source 目录数据源。
    FilePickerSession(Application &app, PickerMode mode, const file_dialog::Options &opts,
                      const DirectorySource &source);
    /// @brief 析构定义在自绘 TU：成员持 `FilePickerView`（不完整类型的 unique_ptr 亦可）。
    ~FilePickerSession();

    FilePickerSession(const FilePickerSession &) = delete;
    auto operator=(const FilePickerSession &) -> FilePickerSession & = delete;
    FilePickerSession(FilePickerSession &&) = delete;
    auto operator=(FilePickerSession &&) -> FilePickerSession & = delete;

    /// @brief 进入嵌套帧循环直到用户作答（或宿主全部请求关闭 / 帧预算耗尽）。
    ///        返回时场景根已还原、选择器已摘除。
    /// @return 确认产物的绝对路径列表（取消语义时为空）。
    [[nodiscard]] auto run() -> std::vector<std::string>;

    /// @brief 内部选择器视图（测试经此脚本化触发确认 / 取消）。
    [[nodiscard]] auto picker() -> FilePickerView & { return *picker_; }

  private:
    auto attach() -> void;
    auto detach() -> void;

    Application &app_;
    std::shared_ptr<FilePickerView> picker_;
    Node saved_root_;  ///< 包壳路径挂载前的场景根（共享句柄，非拥有）
    OverlayHost *overlay_host_ = nullptr;  ///< 直挂路径的浮层宿主（场景根 widget，非拥有）
    std::size_t overlay_index_ = 0;  ///< 直挂路径的浮层序号
    bool wrapped_ = false;  ///< 是否走了包壳路径
    bool attached_ = false;  ///< 当前是否已挂载（析构兜底摘除用）
};

// ---------------------------------------------------------------------------
// 测试接缝（无头用例观测 / 脚本化驱动；默认空或零，不影响生产路径）
// ---------------------------------------------------------------------------

/// @brief 选择器视图构造计数：钩子优先 / 非交互 / 无上下文路径不得构造选择器（计数不变）。
/// @return 进程级计数引用（测试读后自行清零）。
[[nodiscard]] inline auto picker_construct_count() -> std::size_t & {
    static std::size_t count = 0;
    return count;
}

/// @brief 会话观察者：自绘腿构造选择器会话后调用一次（默认空）。
///        供无头用例在 `on_frame_` 里脚本化触发确认 / 取消（`run_modal` 循环内）。
/// @return 观察者引用（测试装配后须清空，避免污染同进程后续用例）。
[[nodiscard]] inline auto &session_observer() {
    // 测试接缝：函数内 static 惰性初始化（首次调用才构造，无模块加载期副作用）。仅 libc++
    // （Emscripten）的 std::function 默认构造非 constexpr，故动态初始化告警只在该口径下触发。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static std::function<void(FilePickerView &)> observer;
    return observer;
}

/// @brief 模态期派发抑制标志：`Application::run_modal` 入口置真、出口还原。
///        嵌套模态循环里的点击经 `WindowHost::dispatch` 派发，不得翻转活动窗口记账
///        （记账点在本标志为真时跳过）；无模态时恒假。
/// @return 标志引用。
[[nodiscard]] inline auto modal_dispatch_suppressed() -> bool & {
    static bool suppressed = false;
    return suppressed;
}

/// @brief 进程级「当前帧循环所属 Application」槽：`Application` 帧循环起手安装、收尾摘除。
///        自绘三入口据此判「有无上下文」——无上下文即等价非交互（WARN 回空，不挂起）。
/// @return 槽位引用（空 = 无上下文）。
[[nodiscard]] inline auto current_application_slot() -> Application *& {
    static Application *slot = nullptr;
    return slot;
}

}  // namespace file_dialog::detail
}  // namespace aurora
