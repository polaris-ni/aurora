#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"

namespace aurora {

// 分段说明：以下事件类型均为纯数据载荷，只在主线程构造与派发
// （契约 Thread: main-thread only、Side-effects: pure 逐类成立，此处统一声明）。

/// @brief 连击判定的**默认时间窗**（毫秒）：两次点击间隔不超过该值才可能累加连击序号。
/// 单一真源：`EventDispatcher::click_window_ms` 以它为初值，窗口装饰层（Wayland CSD 标题栏
/// 双击最大化）直接引用同一常量，避免「库内两套阈值」漂移。
/// @note 该值是库内约定常量，**未接线到系统双击速度设置**（Windows `GetDoubleClickTime` /
///       X11 `Xkb` 等），跨平台才有一致手感；后续接系统值时须同步调整本注释。
inline constexpr std::uint32_t AURORA_DEFAULT_CLICK_WINDOW_MS = 500;

/// @brief 连击判定的**默认位移半径**（逻辑 dp）：两次点击的落点距离不超过该值才可能累加连击序号。
/// 单一真源：`EventDispatcher::click_radius_dp` 以它为初值，窗口装饰层与派发器共用。
/// @note 与 `AURORA_DEFAULT_CLICK_WINDOW_MS` 同口径：库内约定常量，未接线到系统设置。
inline constexpr float AURORA_DEFAULT_CLICK_RADIUS_DP = 4.0F;

/// @brief 连击序号上限：达到后继续快速点击仍记该值（供「三击选整段」语义使用）。
inline constexpr std::uint8_t AURORA_MAX_CLICK_COUNT = 3;

/// @brief 鼠标/触摸按键。
enum class MouseButton : std::uint8_t { Left, Right, Middle };

/// @brief 指针动作。
enum class MouseAction : std::uint8_t {
    Press,  ///< 按下（激活/点击）
    Release,  ///< 抬起
    Move,  ///< 移动（悬停/拖拽）
};

/// @brief 键盘动作。
enum class KeyAction : std::uint8_t { Down, Up };

/// @brief 键盘修饰键位（位掩码，可组合）。
enum class ModifierKey : std::uint8_t {
    None = 0,
    Shift = 1U << 0U,
    Control = 1U << 1U,
    Alt = 1U << 2U,
    Meta = 1U << 3U,
    // 数字小键盘锁（NumLock）当前状态；建模为修饰位而非键码（切换键无发送意义），消费方据此
    // 自行决定 KP_Prior 的语义——框架不做这层二次翻译，详见 keycode.h 的小键盘口径。
    NumLock = 1U << 4U
};

/// @brief 可按住的修饰位并集（Shift / Control / Alt / Meta）。
/// @note 这是 `ModifierKey` 里**语义为「此刻被按住」**的那部分位。消费方做「某修饰键是否按下」
///       判定时用单个位（`e.modifiers & ModifierKey::Shift`）即可，无需本掩码；本掩码用于
///       **两处位集的整体比较**——此时必须先摘掉锁定态位，否则键盘锁定态会污染比较结果
///       （`KeyCombo::matches` 即按它取子集，见 `app/shortcuts.h`）。
inline constexpr std::uint8_t AURORA_MODIFIER_PRESSABLE_MASK =
    static_cast<std::uint8_t>(ModifierKey::Shift) | static_cast<std::uint8_t>(ModifierKey::Control) |
    static_cast<std::uint8_t>(ModifierKey::Alt) | static_cast<std::uint8_t>(ModifierKey::Meta);

/// @brief 键盘锁定态位的并集（当前只有 `NumLock`）。
/// @note 这部分位语义为「键盘的锁定灯态」而非「手指按住」：切换键按下的瞬间翻转，抬起不恢复，
///       故与 `AURORA_MODIFIER_PRESSABLE_MASK` 是两类不可混用的位。**新增锁定类位（`CapsLock` /
///       `ScrollLock` 等）时必须并入本掩码**，否则该位会落进「两掩码之外」的缝隙、在按位比较中
///       继续污染结果；`AURORA_MODIFIER_PRESSABLE_MASK | AURORA_MODIFIER_LOCK_MASK` 应恒等于
///       `ModifierKey` 的全部已定义位，该恒等式由 `utest_shortcuts` 的位集自证用例钉住。
inline constexpr std::uint8_t AURORA_MODIFIER_LOCK_MASK = static_cast<std::uint8_t>(ModifierKey::NumLock);

/// @brief 修饰键位按位或（便于组合 `modifiers`）。
/// @param a 左侧掩码。
/// @param b 右侧掩码。
/// @return 两者位并集对应的 `ModifierKey`（组合值，非单个枚举量）。
[[nodiscard]] inline auto operator|(ModifierKey a, ModifierKey b) noexcept -> ModifierKey {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
    return static_cast<ModifierKey>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @brief 修饰键位按位与（判定某个修饰键是否按下）。
/// @param a 待测组合（如 `KeyEvent::modifiers`）。
/// @param b 掩码（单个修饰键位）。
/// @return 交集的原始位掩码；非 0 表示 `b` 落在 `a` 内。
[[nodiscard]] inline auto operator&(ModifierKey a, ModifierKey b) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b);
}

/// @brief 焦点到达方式：决定基类统一焦点环是否画出（specification/05-event-navigation.md §4.4）。
///
/// 由 `FocusManager::set_focus` 随焦点一并写入控件（`Widget::focus_arrival()`），控件与基类绘制
/// 路径据此判定可见性；本枚举刻意定义在本头而非 `event/focus.h`，因为 `Widget` 需在包含关系上
/// 先于焦点管理器看到它（`focus.h` 包含 `widget.h`，反向不可）。
///
/// - `Pointer`：指针 / 触摸按下把焦点交给控件。控件此刻已有 pressed / hover 反馈，再补一圈环会
///   被知觉归组成「控件自带的边框」，故不画环——与浏览器 `:focus-visible`、WinUI 的 `FocusVisual`、
///   Qt 的 `TabFocusReason` vs `MouseFocusReason` 同口径。
/// - `Keyboard`：经 Tab / Shift+Tab / 方向键移动而来。键盘可达性的可观测停点，必须画环。
/// - `Programmatic`：显式聚焦（`set_focus` / `request_focus` / 焦点作用域进出恢复等），无从判断
///   用户所处模态，保守按可见处理，因而既有调用点行为逐位不变。
enum class FocusArrival : std::uint8_t { Pointer, Keyboard, Programmatic };

/// @brief 输入事件基类。
///
/// 持有 `is_handled` 标志：响应链中某级消费事件后置 true，派发器据此停止冒泡（specification/05-event-navigation.md
/// §2.1）。
struct Event {
    bool is_handled = false;  ///< 是否已被消费（停止冒泡）

    Event() = default;
    virtual ~Event() = default;

    // Rule of Five：虚析构一旦由用户声明，拷贝/移动便不再隐式生成，故显式全部声明。
    // 派生事件在派发与手势处理中会被按值复制（如 `TouchEvent copy = e;`、
    // `MouseEvent release = press;`），这里保留默认语义（仅基类子对象参与，不发生多态切片）。
    Event(const Event &) = default;
    Event(Event &&) = default;
    auto operator=(const Event &) -> Event & = default;
    auto operator=(Event &&) -> Event & = default;
};

/// @brief 指针（鼠标/触摸）事件：坐标为相对根坐标系。
struct MouseEvent : Event {
    Point position{.x = 0.0F, .y = 0.0F};  ///< 全局（窗口逻辑）坐标，由 Surface 后端写入
    Point local_position{.x = 0.0F, .y = 0.0F};  ///< 相对当前控件的本地坐标，由 EventDispatcher 在命中链冒泡时写入
    MouseButton button = MouseButton::Left;  ///< 触发的鼠标键（后端派发时写入；由触点合成的事件一律记左键）
    MouseAction action = MouseAction::Press;  ///< 指针动作（按下/抬起/移动）
    /// @brief 触点 ID（多点触控场景由 TouchEvent 合成时写入，标识该次手势归属的指针；
    ///        鼠标/真实 MouseEvent 为 nullopt，表示「任意指针」）。用于 Draggable/LongPress
    ///        在并发触控下绑定到具体指针，避免同控件被第二根手指误触发。
    std::optional<int> pointer_id;
    /// @brief 连击序号：本次 Press 是同一指针、同一按键上的第几次连续点击（1 = 单击，2 = 双击，3 = 三击）。
    ///        由 `EventDispatcher::dispatch_mouse` 在派发前**集中**计算并写入（后端不参与判定）；
    ///        Release / Move 事件恒为 1（不参与连击计数）。判别口径见 specification/05-event-navigation.md §2.2。
    ///        上限 3：更快的连续点击仍记 3，供「三击选整段」这类语义使用。
    std::uint8_t click_count = 1;
    /// @brief 事件产生那一刻的修饰键位组合（由 Surface 后端在事件构造处写入）。
    ///
    /// 与 `KeyEvent::modifiers` 同一个位掩码枚举、同一份真值源，判读方式一致
    /// （`e.modifiers & ModifierKey::Shift` 非 0 即「按下时 Shift 有效」）。默认 `None` 有两义：
    /// 既可能是「用户确实没按任何修饰键」，也可能是「本事件不由真实指针产生」——触控合成、
    /// 可达性动作、诊断注入的程序化事件一律 `None`，因为不存在对应的物理修饰态。**需要区分
    /// 来源的调用方用既有的 `pointer_id`**（触控合成有值、鼠标为 `nullopt`），不要靠本字段推断。
    ///
    /// **派发器只透传、不推断**：`EventDispatcher::dispatch_mouse` 不读「最后一次 `KeyEvent`
    /// 的 `modifiers`」兜底，也不在派发时刻轮询物理按键态（两种做法的失效场景见
    /// codespec/specification/05-event-navigation.md §2.2.2）。逐后端真值源同见该节。
    ModifierKey modifiers = ModifierKey::None;
};

/// @brief 键盘事件：键码为平台无关的逻辑键码（见 event/keycode.h 的 KeyCode）。
struct KeyEvent : Event {
    int key = 0;  ///< 逻辑键码的整数值（按 `KeyCode` 解释，见 event/keycode.h）；0 == KeyCode::Unknown（未映射）
    KeyAction action = KeyAction::Down;  ///< 键盘动作（按下/抬起）
    ModifierKey modifiers =
        ModifierKey::None;  ///< 修饰键位组合（Shift/Ctrl/Alt/Meta/NumLock；指针与滚轮事件同用此枚举，见 §2.2.2）
};

/// @brief 滚轮事件（specification/05-event-navigation.md §2.2）。delta 为设备无关增量，y 正方向为向上滚动。
struct ScrollEvent : Event {
    Point position;  ///< 事件发生的逻辑坐标（鼠标所在处）
    float delta_x = 0;  ///< 水平滚动增量（右为正）
    float delta_y = 0;  ///< 垂直滚动增量（上为正）
    /// @brief 消费后未用尽的垂直余量（与 delta_y 同单位、同号；嵌套滚动协调的回传通道）。
    /// 消费方在 `on_scroll` 内写入本字段声明「我只吃了这么多」，派发器据此沿命中链
    /// 向更浅可滚动祖先继续派发（见 `EventDispatcher::dispatch(Widget&, ScrollEvent&)`）。
    /// **默认 0 = 全量消费**：不写本字段的既有自定义 handler 行为与「最深可滚动者
    /// 一次性消费、不冒泡」的旧约定逐位一致。
    float remaining_y = 0;
    /// @brief 事件产生那一刻的修饰键位组合（由 Surface 后端在事件构造处写入）。
    ///
    /// 语义与 `MouseEvent::modifiers` 完全一致：同枚举、判读方式相同、真值源在后端、派发器
    /// 只透传。默认 `None` 同样两义——真实滚轮事件里是「没按修饰键」，程序化合成里是「不由
    /// 真实滚轮产生」（`Widget::scroll_by` / `Scroll::scroll_by` 这类程序化滚动恒 `None`，
    /// 不存在对应的物理修饰态）。本结构体无 `pointer_id`，故合成来源不由字段区分：消费方按
    /// 「`None` 即无修饰」使用即可。逐后端真值源见
    /// codespec/specification/05-event-navigation.md §2.2.2。
    ModifierKey modifiers = ModifierKey::None;
};

/// @brief 文本输入事件（specification/05-event-navigation.md §2.2）：由键盘/输入法产生的 Unicode 文本片段。
struct TextInputEvent : Event {
    std::string text;  ///< UTF-8 文本片段
};

/// @brief 输入法（IME）组合事件（CJK 攻坚）。
///
/// 与 `TextInputEvent` 并存、职责分离：
/// - **已提交文本**（上屏）走 `TextInputEvent`；
/// - **组合态**（preedit / 候选 / 组合内选区）走本事件。
///
/// 一次中文输入的典型序列（`TextInput` / `RichTextEdit` 均按此消费）：
/// 1. `preedit="ni hao"` → 显示带下划线的预编辑串；
/// 2. `preedit="你好"`、`cursor_index=2` → 候选替换预编辑串，光标停在候选插入点；
/// 3. `preedit=""`、`committed="你好"` → 预编辑串落字为正式文本，组合结束。
///
/// `sel_start` / `sel_end` 描述 **preedit 内部** 的选中区间（含头含尾，码点下标），
/// 用于输入法高亮「待转换的拼音片段」；无区间时 `sel_end == AURORA_NO_SELECTION`。
struct TextCompositionEvent : Event {
    /// @brief preedit 内「无选区」哨兵（与 `TextInput::AURORA_NO_SEL` 同语义，独立定义以免跨头依赖）。
    static constexpr std::size_t AURORA_NO_SELECTION = static_cast<std::size_t>(-1);

    std::string preedit;  ///< 预编辑串（UTF-8）；空串 = 组合结束 / 取消
    std::size_t cursor_index = 0;  ///< 光标在 preedit 内的码点下标（候选插入点）
    std::size_t sel_start = 0;  ///< preedit 内选中区间起点（码点下标）
    std::size_t sel_end = AURORA_NO_SELECTION;  ///< preedit 内选中区间终点（含尾）；AURORA_NO_SELECTION = 无
    std::string committed;  ///< 本次随组合一并上屏的文本（UTF-8），可为空

    /// @brief preedit 内是否存在选中区间。
    /// @return `sel_end != AURORA_NO_SELECTION` 时为 true（区间含头含尾）。
    [[nodiscard]] auto has_preedit_selection() const -> bool { return sel_end != AURORA_NO_SELECTION; }
};

/// @brief 系统拖放动作：源与目标之间协商出的「这一次拖放要做什么」。
///
/// 对标 Qt `Qt::DropAction` / GTK4 `GdkDragAction` / X11 Xdnd 的 action 三态：`None` 是**不接受**
/// （也是默认值与「会话被取消 / 建立失败」的返回值），其余三态是真实动作。**调用方不得把 `None`
/// 当作成功**：拖出会话被取消、被目标拒绝或建立失败一律回 `None`。
enum class DragAction : std::uint8_t {
    None,  ///< 不接受 / 无动作（默认；取消与失败也归此态）
    Copy,  ///< 复制（源保留其数据）
    Move,  ///< 移动（源在放置后移除其数据）
    Link,  ///< 建立链接 / 快捷方式
};

/// @brief 系统拖出会话的**结局**：会话「怎么完的」，与 `DragAction`「谈成了什么」分工。
///
/// 事件驱动模型（`system_drag::begin` 发起即返回、终态经 `on_end` 回调投递）下，四种结局必须
/// 可分辨：把「被取消」「被拒绝」「建立失败」折叠成同一个 `DragAction::None` 时，控件层三条路径
/// 的回调序列逐字相同（都是 begin → end(None)），等于没补——本枚举与 `on_system_drag_end` 的
/// 签名一起换来「五种路径全部可辨」。
enum class SystemDragOutcome : std::uint8_t {
    Succeeded,  ///< 落放成功（`DragAction` 有效）
    Cancelled,  ///< 被 Esc 或系统取消（用户意图，不是故障）
    Rejected,  ///< 目标不接受该拖放
    Failed,  ///< 会话中途异常终止，或 Move 落放后源侧删除失败（回滚）
};

/// @brief 系统拖放的四个阶段（specification/05-event-navigation.md §5.2）。
///
/// 四态共用同一个事件结构（与 `MouseEvent` / `MouseAction` 同款惯例，不造四个事件类型）：
/// `Enter` 指针携带数据首次进入窗口、`Over` 在其上移动、`Leave` 离开或未放置即结束、`Drop` 落下。
/// **只有 `Drop` 态保证带载荷**：`Enter` / `Over` 的载荷句柄可为空（数据延迟到 drop 才读取），
/// 控件不得在非 drop 态假定有数据。
enum class SystemDropPhase : std::uint8_t {
    Enter,  ///< 进入：指针携带数据首次进入窗口
    Over,  ///< 悬停：在窗口内移动（按指针采样频率到达，处理须轻量）
    Leave,  ///< 离开：移出窗口或会话被放弃（不产生放置）
    Drop,  ///< 落下：用户放下数据（唯一保证带载荷的态）
};

/// @brief 系统拖放载荷：按 MIME 取原始字节的**只读句柄**（pimpl，实现藏于 `src/aurora/event/`）。
///
/// 形态上刻意「只暴露读取面」：数据的真实来源是平台会话（Win32 `IDataObject`、X11 Xdnd、
/// Wayland `wl_data_offer`、macOS dragging pasteboard），落到本库时要么已全部读入内存、要么仍
/// 挂在会话上按需拉取——把「怎么取」藏进实现，事件与控件只认「按 MIME 取字节」这一条契约。
///
/// **空口径（与 `Clipboard` 同款）**：格式不存在 = `Ok` 空值，**空内容不是失败**；只有「读取这个
/// 动作本身失败」（会话已失效、字节不可得）才是 `Error`。
///
/// **同步 / 非阻塞双通道**：`bytes()` 一族同步读取，用于小载荷、无头测试与同步后端；
/// `read_async()` 是非阻塞并列通道——跨进程会话上的一次读取可能触发跨进程 IO，同步读完一个大
/// 载荷即是一帧的整段停顿（本库是单线程 UI + 帧循环）。非阻塞入口登记在途项，结果经
/// `system_drag::pump_reads()` 在帧内排空兑现（宿主不排空 ⇒ 回调不到，不谎报），回调只被调用
/// 一次；会话收口（`system_drag::cancel()`）时就地兑现未决读取，不悬挂。
/// @note Thread: main-thread only（载荷随会话，会话在派发线程上）
/// @note Side-effects: 读取可能触发平台会话 IO（`from_bytes` 构造的内存载荷除外）
class DragPayload : public std::enable_shared_from_this<DragPayload> {
  public:
    /// @brief 由内存字节表构造载荷：平台后端把会话数据一次性读入时的唯一工厂，也是无头测试的注入口。
    /// @param data MIME → 原始字节的映射（键为 MIME 字符串，空键被丢弃）。
    /// @return 只读载荷句柄；`data` 为空时同样返回非空句柄（读取任何格式都得 `Ok` 空值）。
    [[nodiscard]] static auto from_bytes(std::unordered_map<std::string, std::vector<std::uint8_t>> data)
        -> std::shared_ptr<const DragPayload>;

    /// @brief 是否持有指定 MIME 的载荷。
    /// @param mime 待查询的 MIME（如 "text/plain"）。
    /// @return 该格式存在时 true；否则 false（含载荷句柄为空时）。
    [[nodiscard]] auto has(const std::string &mime) const -> bool;

    /// @brief 按 MIME 读取原始字节（通用入口，同步）。
    /// @param mime 待读取的 MIME。
    /// @return `Ok(字节)`——格式不存在时为 `Ok(空)`（空内容不是失败）；会话失效等读取失败为结构化 `Error`。
    [[nodiscard]] auto bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>>;

    /// @brief 按 MIME 非阻塞读取原始字节：登记在途项后立即返回，读取期间不阻塞帧循环。
    ///
    /// 结果经 `system_drag::pump_reads()` 在帧内排空兑现：**宿主不排空 ⇒ 回调不到**（不谎报，
    /// 与 `NotificationCenter` 同款口径）。同一 const 句柄可被并发读取——每次调用各自独立登记、
    /// 各自兑现一次，不共享结果、不互相取消。源侧迟迟不给数据时按墙钟超时兑现
    /// `system-drag-read-failed`；会话收口（`system_drag::cancel()`）时就地兑现，不悬挂。
    /// @param mime 待读取的 MIME。
    /// @param callback 兑现回调（`Ok(字节)` 或结构化 `Error`；只被调用一次）。
    auto read_async(const std::string &mime, std::function<void(Result<std::vector<std::uint8_t>>)> callback) const
        -> void;

    /// @brief 便利读取：文件清单（解码 `text/uri-list`，只返回可映射为本地路径的条目）。
    /// @return `Ok(路径清单)`；无该格式时为 `Ok(空清单)`。非 `file` scheme 的行静默跳过
    ///         （完整原始行经 `bytes("text/uri-list")` 取得）。
    [[nodiscard]] auto files() const -> Result<std::vector<std::string>>;

    /// @brief 便利读取：纯文本（读 `text/plain` 并按 UTF-8 原样返回）。
    /// @return `Ok(文本)`；无该格式时为 `Ok("")`。
    [[nodiscard]] auto text() const -> Result<std::string>;

    /// @brief 便利读取：图像（读 `image/png` 字节并解码为 RGBA8）。
    /// @return `Ok(图像)`；无该格式时为 `Ok(空 Image)`（width == 0）；字节存在但解码失败为结构化 `Error`。
    [[nodiscard]] auto image() const -> Result<Image>;

    /// @brief 析构（pimpl：在 `src/aurora/event/` 中定义，避免实现体进入公共头）。
    ~DragPayload();

    /// @brief 禁止拷贝：载荷以 `shared_ptr<const ...>` 共享，拷贝一个句柄没有语义。
    DragPayload(const DragPayload &) = delete;
    /// @brief 禁止拷贝赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const DragPayload &) -> DragPayload & = delete;
    /// @brief 禁止移动（理由同拷贝构造）。
    DragPayload(DragPayload &&) = delete;
    /// @brief 禁止移动赋值（理由同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(DragPayload &&) -> DragPayload & = delete;

  private:
    /// @brief 私有构造：实例只能经 `from_bytes` 产出（实现体内填充）。
    DragPayload();

    /// @brief pimpl 实现体（定义在 `src/aurora/event/drag_payload.cpp`）。
    struct Impl;
    std::unique_ptr<Impl> impl_;  ///< 实现体（唯一所有权）
};

/// @brief 拖出要约：控件声明「我能提供哪些格式、按 MIME 怎么取字节」。
///
/// 与拖入侧的 `DragPayload` 对称：那边是**读**（会话推送后按需取），这边是**提供**（会话期内由源
/// 按需回调）。`provide` 为空回调而 `formats` 非空 = 声明了格式却给不出数据，会话按失败处理。
///
/// 字段形态与剪贴板侧 `ClipboardOffer`（`app/clipboard.h`）同构：两条通道共享同一套 MIME 词汇
/// （`app/mime_types.h` 是单一真源），同一个 `provide` 回调可同时交给两条通道；两侧不合并——
/// `event/` 不依赖 `app/` 的分层约束使统一数据对象无从落地。
struct DragOffer {
    std::vector<std::string> formats;  ///< 可提供的 MIME 清单（顺序即优先级）
    /// @brief 按 MIME 提供字节的回调；返回 `Error` 视为该次提供失败（会话按失败结束）。
    std::function<Result<std::vector<std::uint8_t>>(const std::string &mime)> provide;
    /// @brief 可选拖拽影像（RGBA8；空图像 = 交给 OS 用默认影像）。
    ///
    /// 跨进程拖拽时跟随光标的影像由**源应用提供给 OS**、OS 合成显示，本库不绘制跟随层：该字段
    /// 只经 `Widget::begin_system_drag()` 交给 OS。不支持影像的后端静默忽略该字段、不改变会话语义
    /// （装饰性提示缺失不致语义受损，故不设能力位、不报错）。
    Image icon;
    /// @brief 影像热点相对指针的偏移（逻辑 dp；仅 `icon` 非空时有意义）。
    Point icon_hotspot{.x = 0.0F, .y = 0.0F};
    /// @brief 源侧缺省动作：无有效修饰键时 `SystemDropEvent::proposed` 取该值（对标
    /// `QDrag::setDefaultAction`）；缺省 `Copy`（未设置与显式 `Copy` 行为一致）。
    ///
    /// 存在的理由：`ModifierKey::None` 是**两义**的（既可能是没按修饰键，也可能是程序化合成事件
    /// 不由真实指针产生），故不能靠修饰键缺席推定默认动作。控件也借此表达「我不支持 Move」——
    /// 写进本字段即可，无需另开门面。
    DragAction default_action = DragAction::Copy;
    /// @brief Move 完成确认：目标侧成功接收并落盘后由宿主调用，源在此回调内删除原数据。
    ///
    /// 返回 `Error` 即源删除失败：宿主据此把会话终态改写为 Move 失败（`on_end` 收到
    /// `Failed` + `Move`）并上报 `system-drag-move-rollback`，目标侧数据保持不动。契约：
    /// **源不得在收到确认前删除任何数据**；**目标侧接收失败时不会调用本回调**；调用点严格早于
    /// `on_end(Succeeded, Move)` 的投递（事件驱动下该顺序从外部可断言）。
    std::function<Result<void>()> on_move_confirmed;
};

/// @brief 系统拖放事件：跨进程拖放的四个阶段共用一个结构（specification/05-event-navigation.md §5.2）。
///
/// 与既有应用内拖拽（`event/drag_drop.h`）分工：那条是进程内控件重排 / 手势，本条是**跨出进程边界**
/// 的 OS 拖放（资源管理器 ↔ 应用）。坐标契约沿用「位置类事件一律整幅表面逻辑坐标」。
///
/// `formats` 用裸 `std::string` 而非 MIME 常量表：`event/` 不依赖 `app/`；MIME 词汇的单一真源在
/// `app/mime_types.h`，两侧取值一致由该表的常量保证。
///
/// **派发规则**：四态一律不冒泡、只交给命中目标（既有文件拖放派发规则原样延续）；消费时置
/// `e.is_handled`。`allowed` 由派发器在 `Enter` / `Over` 态按命中目标的 accept 查询与 `proposed`
/// 求交后写定，宿主据此决定 OS 光标反馈。`modifiers` 由**宿主 / 后端**在构造事件时从 OS 会话写入
/// （与 `MouseEvent::modifiers` 同一 `ModifierKey` 位掩码、同一份真值源）；修饰键 → `proposed`
/// 的翻译**由宿主在派发前完成**，控件只读 `proposed` 与 `allowed`、不做翻译（翻译前按
/// `AURORA_MODIFIER_PRESSABLE_MASK` 摘除锁定态位；无有效修饰键时 `proposed` 取源侧缺省动作，
/// 见 `DragOffer::default_action`）。
struct SystemDropEvent : Event {
    SystemDropPhase phase = SystemDropPhase::Drop;  ///< 阶段（默认 `Drop`，便于程序化构造落放事件）
    Point position;  ///< 落点（整幅表面逻辑坐标，契约见 收口后的位置类事件口径）
    std::vector<std::string> formats;  ///< 本次拖放可用格式的 MIME 清单
    DragAction proposed = DragAction::None;  ///< 源建议的动作
    DragAction allowed = DragAction::None;  ///< 目标允许的动作（派发器按 accept 查询与 proposed 求交后写定）
    /// @brief 事件产生时的修饰键位掩码（宿主从 OS 会话写入；`None` 两义，见结构体说明）。
    ModifierKey modifiers = ModifierKey::None;
    /// @brief 载荷句柄；`Enter` / `Over` / `Leave` 可为空（数据延迟到 drop 才读取）。
    std::shared_ptr<const DragPayload> payload;

    /// @brief 便利读取：文件清单（委托 `DragPayload::files`；无载荷时为空清单）。
    /// @return `Ok(路径清单)`；无载荷或无该格式时为 `Ok(空清单)`。
    [[nodiscard]] auto files() const -> Result<std::vector<std::string>>;

    /// @brief 便利读取：纯文本（委托 `DragPayload::text`；无载荷时为空串）。
    /// @return `Ok(文本)`；无载荷或无该格式时为 `Ok("")`。
    [[nodiscard]] auto text() const -> Result<std::string>;

    /// @brief 便利读取：图像（委托 `DragPayload::image`；无载荷时为空 `Image`）。
    /// @return `Ok(图像)`；无载荷或无该格式时为 `Ok(空 Image)`。
    [[nodiscard]] auto image() const -> Result<Image>;

    /// @brief 便利读取：按 MIME 取原始字节（委托 `DragPayload::bytes`；无载荷时为空）。
    /// @param mime 待读取的 MIME。
    /// @return `Ok(字节)`；无载荷或无该格式时为 `Ok(空)`。
    [[nodiscard]] auto bytes(const std::string &mime) const -> Result<std::vector<std::uint8_t>>;

    /// @brief 便利读取（非阻塞）：文件清单（委托 `DragPayload::read_async` 后解码）。
    ///
    /// 与同步 `files()` 并列而非替换：大载荷走本入口，读取期间帧循环不停顿。无载荷时**同步**按
    /// 空口径兑现（没有会话可排空）；有载荷时经 `system_drag::pump_reads()` 在帧内排空兑现，
    /// 宿主不排空 ⇒ 回调不到。回调只被调用一次。
    /// @param callback 兑现回调（`Ok(路径清单)` 或结构化 `Error`）。
    auto files_async(std::function<void(Result<std::vector<std::string>>)> callback) const -> void;

    /// @brief 便利读取（非阻塞）：纯文本（委托 `DragPayload::read_async`）。
    /// @param callback 兑现回调（`Ok(文本)` 或结构化 `Error`）。
    auto text_async(std::function<void(Result<std::string>)> callback) const -> void;

    /// @brief 便利读取（非阻塞）：图像（委托 `DragPayload::read_async` 后解码）。
    ///
    /// 字节已取到但按 MIME 解码失败 ⇒ `system-drag-decode-failed`（同一份字节重试无意义）。
    /// @param callback 兑现回调（`Ok(图像)` 或结构化 `Error`）。
    auto image_async(std::function<void(Result<Image>)> callback) const -> void;

    /// @brief 便利读取（非阻塞）：按 MIME 取原始字节（委托 `DragPayload::read_async`）。
    /// @param mime 待读取的 MIME。
    /// @param callback 兑现回调（`Ok(字节)` 或结构化 `Error`）。
    auto bytes_async(const std::string &mime, std::function<void(Result<std::vector<std::uint8_t>>)> callback) const
        -> void;
};

/// @brief 系统**拖出**会话入口：把控件给出的 `DragOffer` 交给 OS 拖放会话（事件驱动模型）。
///
/// **执行模型（形态二，GTK4 口径）**：`begin` **发起即返回**，不阻塞到会话结束——本库是单线程
/// UI + 帧循环，阻塞期间不泵帧循环则会话回调无从派发、泵则引入嵌套循环的重入面；既有规格
/// （`codespec/specification/06-app-platform.md` §8.9）已明文「不引入嵌套消息泵」，故事业级后端
/// 接线后必然是「发起即返回 + 终态经回调投递」的形态（Win32 `DoDragDrop`、Qt `QDrag::exec()` 的
/// 嵌套模态循环不取）。返回值语义收窄为**「会话是否发起成功」**，最终结局与动作改由 `on_end`
/// 投递，经 `pump_reads()` 在帧循环上兑现（宿主不排空 ⇒ 回调不到，不谎报）。
///
/// 系统拖出后端按平台接线（Win32 / D3D11 / X11 / Wayland 均已注册；Headless / Glfw 恒无后端）；
/// 当无可用后端或能力位为 false 时返回 `system-drag-not-supported`（`retryable = false`，调用方不该重试）。
namespace system_drag {
/// @brief 发起一次系统拖出会话（发起即返回，不等待会话结束）。
/// @param offer 控件提供的格式与取字节回调；`formats` 为空或 `provide` 为空回调视为要约不合法。
/// @param on_end 终结回调：会话终结时投递 `(SystemDragOutcome, DragAction)`。四种结局
/// （`Succeeded` / `Cancelled` / `Rejected` / `Failed`）可分辨；传空回调表示不关心终态（会话照常）。
/// @return `Ok` = 会话已发起；无后端 / 能力位为 false / 要约不合法时为结构化 `Error`
/// （`Ok` **不等于**落放成功——终态看 `on_end`）。
[[nodiscard]] auto begin(const DragOffer &offer, std::function<void(SystemDragOutcome, DragAction)> on_end)
    -> Result<void>;

/// @brief 修饰键 → 源建议动作的翻译（宿主 / 后端在派发前调用的**唯一翻译点**）。
///
/// 控件只读 `SystemDropEvent::proposed` 与 `allowed`、不做翻译——「谁翻译」的唯一答案
/// 就是本函数，控件侧不存在第二处翻译逻辑。两条既有约定必须沿用，否则会踩坑：
/// 比对前按 `AURORA_MODIFIER_PRESSABLE_MASK` 摘除锁定态位（NumLock 常开的实现上，
/// Win32 `GetKeyState` 一类返回含锁定态位的位集，不摘会让 Ctrl 组合静默失配）；
/// `ModifierKey::None` 是**两义**的（用户确实没按 / 事件由程序化合成产生），故**不能靠
/// 修饰键缺席推定默认动作**，无有效修饰键时取 `default_action` 参数。
/// @param modifiers 事件修饰键位域（与 `MouseEvent::modifiers` 同一枚举、同一份真值源）。
/// @param default_action 源侧缺省动作（`DragOffer::default_action`；无有效修饰键时取它）。
/// @return Ctrl → `Copy`、Shift → `Move`、Ctrl+Shift → `Link`（Windows 惯例）；
///         无有效修饰键 → `default_action`（其为 `None` 时按 `Copy` 兜底）。
[[nodiscard]] inline auto proposed_action_for(ModifierKey modifiers, DragAction default_action) -> DragAction {
    const auto pressable = static_cast<std::uint8_t>(modifiers) & AURORA_MODIFIER_PRESSABLE_MASK;
    const bool ctrl = (pressable & static_cast<std::uint8_t>(ModifierKey::Control)) != 0U;
    const bool shift = (pressable & static_cast<std::uint8_t>(ModifierKey::Shift)) != 0U;
    if (ctrl && shift) {
        return DragAction::Link;  // Ctrl+Shift → Link（Windows 惯例）
    }
    if (shift) {
        return DragAction::Move;
    }
    if (ctrl) {
        return DragAction::Copy;
    }
    // 无有效修饰键：取源侧缺省动作；缺省自身未表态（None）时按 Copy 兜底。
    return default_action == DragAction::None ? DragAction::Copy : default_action;
}

/// @brief 取消在途拖出会话（程序化取消：无头测试与 Esc 语义共用同一条路径）。
///
/// 调用时机不受限——事件驱动下可在任意时刻调用：有在途会话则取消（终态 `Cancelled` 经帧循环
/// 投递 `on_end`），无则 no-op。同时**就地收口全部在途读取**（`DragPayload::read_async` 登记的
/// 未决项）：有数据即兑现数据，否则按 `system-drag-read-failed` 兑现，不悬挂。
auto cancel() -> void;

/// @brief 排空：投递终结回调与在途读取结果（宿主每帧调用一次；无在途项时近零开销）。
///
/// 与 `NotificationCenter::pump_events()` 同款：异步结果在帧内排空、**不在平台消息泵内同步回调**
/// （回调可能重建页面 / 触发重排，在消息泵内重入会把布局与绘制切到半途的状态）。
/// **宿主不排空 ⇒ 回调不到**（不虚假成功）。
auto pump_reads() -> void;

/// @brief 安装进程内 inert 后端：此后 `begin` 不触碰平台、登记在途会话（终态经测试接缝驱动）。
/// @return 注入是否生效（双宏 `AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS` 未齐备时为 false）。
[[nodiscard]] auto install_test_backend() -> bool;

/// @brief 复位 inert 后端（清会话、在途读取与扣留记录；未安装时为 no-op）。
auto reset_test_backend() -> void;

/// @brief 卸载 inert 后端，恢复「无后端」语义。
/// @return 是否确有后端被卸载（双宏未齐备时为 false）。
[[nodiscard]] auto remove_test_backend() -> bool;

/// @brief 模拟「OS 侧会话终结事件」（平台后端 / 测试接缝，**应用代码不应调用**）。
///
/// inert 后端下由测试驱动：登记在途会话的终态，经下一拍 `pump_reads()` 投递 `on_end`（动作为
/// `Move` 时先调 `DragOffer::on_move_confirmed` 确认，失败则终态改写为 `Failed`）。无在途会话时
/// 为 no-op。
/// @param outcome 会话结局。
/// @param action 协商出的动作（`Succeeded` 时有效；其余结局传 `DragAction::None`）。
auto emit_session_end(SystemDragOutcome outcome, DragAction action) -> void;

/// @brief 扣留指定 MIME 的载荷读取（测试专用）：被扣留格式的在途读取保持未决，直到墙钟超时。
///
/// 用于覆盖「源侧迟迟不给数据 ⇒ 超时兑现 `system-drag-read-failed`」判据；随 inert 后端复位清除。
/// @param mime 待扣留的 MIME。
auto test_withhold_format(const std::string &mime) -> void;

/// @brief 设置在途读取的墙钟超时（测试专用；默认 5s，生产路径不可调）。
/// @param timeout 超时时长（≤ 0 复位为默认值）。
auto test_set_read_timeout(std::chrono::milliseconds timeout) -> void;

/// @brief 当前是否处于 `pump_reads()` 排空阶段（测试专用）：断言「回调在帧内排空兑现、而非在
/// 平台消息泵内同步回调」时在回调里读它。
/// @return 排空进行中为 true；否则 false。
[[nodiscard]] auto test_in_drain_phase() -> bool;

/// @brief 最近一次会话终结的结构化错误（测试专用）：Move 确认失败时宿主上报的
/// `system-drag-move-rollback`；无则为 `std::nullopt`。
/// @return 最近一次会话错误（随 inert 后端复位清除）。
[[nodiscard]] auto test_last_session_error() -> std::optional<Error>;

/// @brief 最近一次 `begin` 登记的要约（测试专用）：断言拖拽影像 / 热点偏移 / 缺省动作等字段往返。
/// @return 最近一次登记的要约（随 inert 后端复位清除；无则为 `std::nullopt`）。
[[nodiscard]] auto test_last_offer() -> std::optional<DragOffer>;
}  // namespace system_drag

/// @brief 单个触点（多点触控）。
struct TouchPoint {
    int id = 0;  ///< 触点唯一 ID（平台分配）
    Point position;  ///< 当前位置（逻辑坐标）
    Point prev_position;  ///< 上一帧位置
    bool is_active = true;  ///< 是否按下（false = 已抬起）
};

/// @brief 多点触控事件：携带所有活跃触点。
struct TouchEvent : Event {
    std::vector<TouchPoint> points;  ///< 当前所有触点（含已抬起的，active=false）

    /// @brief 活跃触点数。
    /// @return `points` 中 `is_active == true` 的个数（无活跃触点时为 0）。
    [[nodiscard]] auto active_count() const -> int {
        int n = 0;
        for (const auto &p : points) {
            if (p.is_active) {
                ++n;
            }
        }
        return n;
    }

    /// @brief 双指距离（仅 active_count>=2 时有意义）。
    /// @return `points[0]` 与 `points[1]` 当前位置（逻辑坐标）的欧氏距离；不足两个活跃触点时 0.0F。
    [[nodiscard]] auto pinch_distance() const -> float {
        // 同时判 points.size()<2：既保证下标访问不越界，又让 GCC 的 -Warray-bounds 能据此
        // 证明 points[1] 合法（active_count>=2 已蕴含 size>=2，二者语义等价，故不改变返回值）。
        if (points.size() < 2 || active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        return std::sqrt((dx * dx) + (dy * dy));
    }

    /// @brief 双指角度（弧度，仅 active_count>=2 时有意义）。
    /// @return `atan2(dy, dx)`：从 `points[0]` 指向 `points[1]` 的连线与 +x 轴夹角；不足两个活跃触点时 0.0F。
    [[nodiscard]] auto pinch_angle() const -> float {
        // 同 pinch_distance：先判 points.size()<2 以向 -Warray-bounds 证明下标合法。
        if (points.size() < 2 || active_count() < 2) {
            return 0.0F;
        }
        const auto &[x1, y1] = points[0].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        const auto &[x2, y2] = points[1].position;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        return std::atan2(y2 - y1, x2 - x1);
    }

    /// @brief 按 id 查找触点（不存在返回 nullopt），用于手势在并发场景下锁定特定指针对。
    /// @param id 触点唯一 ID（平台分配，见 `TouchPoint::id`）。
    /// @return 首个 `id` 相同的触点副本；无匹配时为 `std::nullopt`。
    [[nodiscard]] auto point_by_id(int id) const -> std::optional<TouchPoint> {
        for (const auto &p : points) {
            if (p.id == id) {
                return p;
            }
        }
        return std::nullopt;
    }
};

}  // namespace aurora
