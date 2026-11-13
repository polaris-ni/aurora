# CHG-059 剪贴板：测试后端下沉测试侧 + 变更监听多播化

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-059 |
| 提出日期 | 2026-11-04 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.FEAT.APP.CLIPBOARD-RICH-FORMATS.001 |
| 影响面 | 公共 API（破坏性：删除 `on_change` / `on_ownership_lost` 与公共头 test-only 段，新增 `on_event` 与 `ClipboardEventKind`）；内部接缝 [`clipboard_backend.h`](../../../src/aurora/app/detail/clipboard_backend.h) 由"仅富格式"扩为全通道表；memory 后端与模拟入口整体迁至 `tests/framework/`；`src/aurora/app/clipboard.cpp` 内 22 处 `#ifdef AURORA_CLIPBOARD_TEST_BACKEND` 与内存后端本体删除；平台桥（Win32 / macOS / X11 / Wayland）与后端矩阵**零改动** |

## 动机

### 1 测试后端把测试设施焊进了产品代码

[`src/aurora/app/clipboard.cpp`](../../../src/aurora/app/clipboard.cpp) 共 1038 行，其中约 400 行是进程内 memory 后端，且**每个公共入口的第一个分支都是 `#ifdef AURORA_CLIPBOARD_TEST_BACKEND` 的提前返回**（22 处），与 Win32 / Linux / macOS 三套平台实现交织在同一函数体里。该宏还在源文件内自我定义（`AURORA_ENABLE_DEBUG && AURORA_ENABLE_TEST_HOOKS`），既不是 CMake 选项、也不进 toggles 矩阵，属隐形开关。公共头另有 6 个 test-only 方法常驻（`install_test_backend` / `reset_test_backend` / `remove_test_backend` / `emit_ownership_lost` / `emit_change` / `set_listen_supported`），生产 API 面被测试设施污染。

成本不只是可读性：任何改动剪贴板门面的提交都必须在"平台路径 / 测试路径"双线同步推理；测试后端的行为（惰性要约缓存、立即写接管）与平台语义存在第二实现，二者偏差靠测试自证。而仓库已有成熟接缝——[`clipboard_backend.h`](../../../src/aurora/app/detail/clipboard_backend.h) 的进程级后端表，所有平台实现都经它注入门面，"被测的就是产品代码本身而不是它的复刻"这条判据已有落点。memory 后端本可作为**同表上的另一个后端**存在，而非门面里的 `#ifdef` 分支。

### 2 监听单槽无法承载"每个对象自己的监听"

`on_change` / `on_ownership_lost` 各只有一个槽位，**再注册即替换**（[`include/aurora/app/clipboard.h`](../../../include/aurora/app/clipboard.h)）。这带来三个后果：多对象无法各自订阅；对象销毁后其回调仍留在槽位，形成悬垂 `std::function` 捕获；仓内没有主动注销入口（只能靠"传空回调"覆盖，含义是注销而非本对象退订）。

## 变更内容

### 一 测试后端迁出门面（公共 API 收敛）

1. **接缝扩为全通道表**：`clipboard::detail::PlatformBackend` 在现有 `set_payload` / `get_payload` / `set_files` / `get_files` / `set_html` / `get_html` / `set_offer` / `available_formats` / `set_change_listener` 之外补齐 `set_text` / `get_text` / `set_image` / `get_image` / `clear`。字段缺席的语义不变：缺席即"该子能力本后端没有"，门面回落原生平台路径。
2. **门面入口统一为一次委托判断**：[`clipboard.cpp`](../../../src/aurora/app/clipboard.cpp) 的每个公共入口改为"后端表填了该字段 → 直接委托；未填 → 走既有原生平台路径"，删除 22 处 `#ifdef AURORA_CLIPBOARD_TEST_BACKEND` 与 `TestBackend` 结构体、`to_bytes` / `to_text` / `decode_or_empty` / `offer_declares` / `take_over_from_offer` / `cache_rendered` / `provide_for` 等辅助体，以及源文件内的宏定义。
3. **memory 后端整体迁至 tests/framework/clipboard_memory_backend.cpp（待建，配同名头）**：实现上面补齐的五个字段与既有富格式字段，经内部头注册到同一张后端表；同时把 `emit_change` / `emit_ownership_lost` / `reset` 等模拟入口作为**测试侧自由函数**保留（不再挂公共类）。
4. **"有无监听机制"由字段是否存在表达**：memory 后端填 `set_change_listener` 即模拟"有机制"，不填即模拟"无机制"，因此 `set_listen_supported` 开关整体消失——能力判据回到"表里有这一项"，与平台路径同一条规则。
5. **删除公共头 test-only 段**：`include/aurora/app/clipboard.h` 的 6 个方法整段移除；[`tests/framework/isolation.cpp`](../../../tests/framework/isolation.cpp) 的用例兜底清理改调内部 `clipboard::detail::remove_backend()`。
6. **测试宏面收窄**：剪贴板不再消费 `AURORA_ENABLE_TEST_HOOKS`（其余模块如热键 / 存储 / 系统拖放不受影响，开关保留）。

### 二 变更监听多播化（单入口 + 两种注册形态）

1. **新增事件种类与订阅入口**（[`include/aurora/app/clipboard.h`](../../../include/aurora/app/clipboard.h)）：

   ```cpp
   enum class ClipboardEventKind : std::uint8_t {
       None = 0, ContentChanged = 1, OwnershipLost = 2
   };
   // 位掩码：operator| / operator& / operator|= / clipboard_event_has 按既有 enums.h 惯例提供

   static auto on_event(ClipboardEventKind kinds,
                        std::function<void(ClipboardEventKind)> callback)
       -> Result<Subscription>;
   static auto on_event(Widget &owner,
                        ClipboardEventKind kinds,
                        std::function<void(ClipboardEventKind)> callback)
       -> Result<Subscription>;
   ```

   `kinds` **必填**，不提供"省略即全订阅"的重载；`callback` 空指针为参数错 `GeneralInvalidArgument`。返回类型复用既有 `aurora::Subscription`（[`include/aurora/state/subscription.h`](../../../include/aurora/state/subscription.h)，析构即取消、`reset()` 显式取消、`active()` 查询），与 `Storage::on_change` 同款惯例，不新造句柄类型。

2. **删除旧入口**：`on_change` / `on_ownership_lost` 不复存在，"再注册即替换"的语义随之消失。

3. **注册失败口径**：请求含 `ContentChanged` 且当前无监听机制（平台后端未填 `set_change_listener`）→ `ClipboardListenUnsupported`（既有错误码，`retryable = false`）；只请求 `OwnershipLost` 时不做能力校验、恒成功（所有权丢失本就不经平台监听槽位）。owner 形态另行校验：`weak_from_this()` 取不到非空守卫（对象非 `shared_ptr` 托管或传入空引用场景）→ `GeneralInvalidArgument`，detail 写明 owner 必须由 `shared_ptr` 托管。

4. **纯多播语义**：每次注册都是一条独立条目，同一对象注册两次收到两次通知，各条由各自的 `Subscription` 注销。守卫条目额外在派发与入口清扫时按 `expired()` 自动剔除。

5. **派发规则**：注册序 FIFO；派发前在锁内做条目快照（含各条目的存活标记与 kinds 掩码），锁外逐条调用，调用前跳过已取消 / 守卫已失效的条目；**本轮内新增的注册从下一次通知开始生效**，本轮内取消的立即停止接收；回调中再次注册或取消不改变本轮快照。

6. **平台层零改动，门面只做多播分发**：`PlatformBackend::set_change_listener` 在门面里是**纯能力探针**（只判字段是否存在，从不调用它）；真正的变更接线是平台桥经既有 Hooks 直接调 `clipboard::detail::fire_change()`（Win32 `WM_CLIPBOARDUPDATE`、X11 / Wayland selection 事件），`OwnershipLost` 同理直接调 `fire_ownership_lost()`。门面不向平台登记 trampoline、也不维护首尾引用计数——那样会与既有 Hooks 双接线、可能双触发；平台桥与后端矩阵零改动。

7. **消亡清扫与频率控制**：维护弱守卫条目计数。**计数为 0 时任何入口都不清扫**（纯句柄形态零开销）；计数大于 0 时，所有公共入口（`set_text` / `get_text` / `set_image` / `get_image` / `set_payload` / `get_payload` / `set_files` / `get_files` / `set_html` / `get_html` / `set_offer` / `available_formats` / `clear` / `on_event`）在入口处按 **5 秒**时间窗口节流整表清扫一次（时间源复用 [`core/time.h`](../../../include/aurora/core/time.h) 的 `current_timestamp()`）。派发永远逐条检查存活，不受该窗口影响。

8. **`clear()` 与监听的关系不变**：监听注册不是内容，`clear()` 只清内容与要约，订阅留存。

### 三 兼容与迁移

1. `CHANGELOG.md` 记一条 Breaking：旧 `on_change(cb)` → `on_event(ClipboardEventKind::ContentChanged, cb)`；旧 `on_ownership_lost(cb)` → `on_event(ClipboardEventKind::OwnershipLost, cb)`；旧"传空回调即注销" → 丢弃或 `reset()` 返回的 `Subscription`；旧"再注册替换" → 再注册新增（需自行先 `reset()` 旧句柄再注册，以保持单订阅效果）。
2. 依赖公共头 test-only 方法的下游（若有）改走 tests/framework/clipboard_memory_backend.h（待建）的内部注册入口。

## 验收判据

1. `src/aurora/app/clipboard.cpp` 中 `AURORA_CLIPBOARD_TEST_BACKEND` 出现次数为 0，且文件中不含 memory 后端实现。
2. `include/aurora/app/clipboard.h` 不含 `install_test_backend` / `reset_test_backend` / `remove_test_backend` / `emit_change` / `emit_ownership_lost` / `set_listen_supported`。
3. `tests/framework/clipboard_memory_backend.*` 存在，且 `utest_clipboard` / `itest_text_selection` / 其他既有消费用例全部迁移后可编译并通过。
4. 多播语义：两个订阅同时收到一次变更；一条 `reset()` 后另一条仍收到；回调内新注册不在本轮被调用而在下一轮被调用；回调内 `reset()` 自身后本轮不再收到剩余事件。
5. 守卫语义：`shared_ptr` 托管的 Widget 被释放后，其条目在下一次派发或 5 秒窗口后的首个入口调用时被剔除；栈上 Widget 注册返回 `GeneralInvalidArgument`。
6. 能力口径：memory 后端不填 `set_change_listener` 时，请求 `ContentChanged` 返回 `ClipboardListenUnsupported`；只请求 `OwnershipLost` 仍成功并能在 `fire_ownership_lost` 后收到。
7. 平台层零改动：`git diff` 中 `src/aurora/window/detail/*clipboard*` 与 `src/aurora/app/detail/macos_clipboard.cpp` 无改动；门面不新增任何平台登记 / 注销调用（`set_change_listener` 仅作字段存在性探针）。
8. 门禁全绿：`aurora_api_json`（刷新后 `check_api_schema_sync`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_arch_module_map`、`check_codespec_xref`、`lint`、`format-check`；`ctest --preset ninja-test` 全量通过。

## 回写落点

1. [`codespec/specification/06-app-platform.md`](../../specification/06-app-platform.md) §8.2：`on_ownership_lost` / `on_change` 两条改写为 `on_event` 单入口（含 kinds、双注册形态、多播与派发规则、清扫与 5 秒窗口、能力探针）；"测试注入点（test-only）"整段改写为"测试后端位于 tests/framework/，经内部接缝注册"；失败口径表补 `on_event` 两条（能力缺失 / owner 非托管）。
2. [`codespec/specification/06-app-platform.md`](../../specification/06-app-platform.md) 的 Linux/macOS 实现段：`install_test_backend()` 的表述改为测试侧注册入口。
3. 本文件：实施完成后状态流转至已归档。
