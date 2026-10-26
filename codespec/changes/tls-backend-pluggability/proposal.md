# CHG-022 TLS 后端可插拔化：后端注册表、系统栈与 provider 共存、每次握手覆盖

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-022 |
| 提出日期 | 2026-10-17 |
| 当前状态 | 已归档 |
| 关联需求 | SPEC.PLATFORM.NET.TLS-BACKEND.001 |
| 影响面 | 破坏性重组 TLS 后端装配面：四枚引擎宏 `AURORA_NET_TLS_{SCHANNEL,SECURE_TRANSPORT,OPENSSL,MBEDTLS}` 更名为 `AURORA_ENABLE_NET_TLS_*`（`BUILD_OPTIONS.md` §1 三层命名法归位）；缓存变量新增 `AURORA_ENABLE_NET_TLS_SYSTEM`（与 provider 解耦，系统栈不再被 provider 挤掉）与列表型 `AURORA_NET_TLS_PROVIDERS`（旧单值 `AURORA_NET_TLS_PROVIDER` 保留为别名，两者同设即 configure 期报错）；「恰好编译一个后端 TU」的不变量作废，改为多 TU 共存 + 运行期注册表分派，`tls_unavailable.cpp` 删除；新增公共头 `include/aurora/net/tls_backend.h`（`TlsBackend` 抽象基类、`TlsDataState`、`TlsBackendRegistration` 与注册 / 反查 / 候选链四组自由函数）并纳入伞头；`TlsContext` 新增 `backend` 字段、`TlsTransport` 新增 `backend()` 查询；`Engine::capabilities().tls` 真值源由「被编进来的那个 TU」改为「注册表非空」；四枚后端 TU 的入口符号改唯一名；`src/aurora/net/engine.cpp`、`tests/unit/utest_net_tls.cpp`、`tools/verify/tls_live_probe.cpp`、`.github/workflows/ci.yml`、`codespec/manual-test/23-net.md` 的引用随迁移；不新增错误码，仅改写 `net-tls-engine-unavailable` 的 hint；`aurora_api.json` 随 `--target aurora_api_json` 重建 |

## 动机

1. **系统栈与 provider 是互斥的，而它们本应共存。** `cmake/AuroraNetTls.cmake` 现在只有一条装配分支：`AURORA_NET_TLS_PROVIDER` 为空时按平台定义系统栈宏，非空时**只**定义 provider 宏——系统栈宏连定义机会都没有。后果是 Windows 上给出 OpenSSL 安装根，Schannel 这条腿就整条消失；macOS 给出 homebrew 的 OpenSSL keg，SecureTransport 同样消失。这两台机器上系统栈恰恰是**零依赖、随 servicing 打补丁、由 OS 信任库统一管根**的那一条腿，为了让应用能用 provider 库而把它编译掉，是拿安全默认去换可选能力。
2. **「恰好一个后端」不是需求，是实现约束。** 四枚后端 TU 各自定义同名的 `detail::make_tls_backend()` 与 `detail::tls_backend_available()`，因此多编一枚就是链接期重复定义；`tls_unavailable.cpp` 用「四枚宏全假」的条件补最后一个定义。也就是说今天唯一可行的装配形态是**由符号冲突反推出来的**，而 `codespec/specification/06-app-platform.md` §8.8.18 把它写成了规范性承诺——承诺的方向反了：真正想要的是「调用方能选、能问、能换」，不是「只能存在一个」。
3. **不可插拔的形态与本库既有的可扩展范式不一致。** 图像解码有 `ImageCodecRegistry`（抽象基类 + 进程单例 + 内置实现退为注册项）、音频设备有 `AudioDeviceBackend` 注入、字体支持应用自注册、压缩编解码器同样走注册表。TLS 后端是全库最后一个「能力由编译期唯一选择决定」的子系统：应用既不能注入自研或第三方 TLS 栈，也不能在测试里注入假后端（今天的假后端只在会话层用桩流凑，测不到选择逻辑）。
4. **跨后端选择没有表达口。** 一个真实场景：终端管理软件默认走系统栈（信任库、补丁、智能卡都归 OS），但对连了自签内网 CA 的那台主机需要用 OpenSSL + 显式 PEM 根；mTLS 的 PEM 身份只有 provider 腿支持，系统栈腿今天落 `net-platform-unavailable`。今天的 API 无法表达「这一条连接用那个后端」，也无法表达「这个后端不行就换下一个」。
5. **命名漂移。** 同为「内置实现之一，可被替换/共存」的子系统，音频用 `AURORA_ENABLE_AUDIO_WASAPI/ALSA/WEBAUDIO`、WebView 用 `AURORA_ENABLE_WEBVIEW_WEBVIEW2/WEBKITGTK`，都归 `BUILD_OPTIONS.md` §1 的 `AURORA_ENABLE_*` 组；TLS 四枚却用模块前缀 `AURORA_NET_TLS_*`，读起来像第四个未登记的类别。`AURORA_BACKEND_*` 组不能用来命名它们——该组的语义是「开关名 = PUBLIC feature 宏名、随安装导出给消费者」，而 TLS 引擎宏是 `PRIVATE`（公共头零 `#ifdef`，`SPEC.PLATFORM.ZERO-IFDEF.001`），归错组会让消费者拿到一枚自己的头里根本不出现的宏。

## 变更内容

1. **后端接缝升为公共契约**（新增 `include/aurora/net/tls_backend.h`，命名空间 `aurora::net`）：把 `src/aurora/net/detail/tls_backend.h` 里的 `TlsBackend` 抽象基类与 `TlsDataState` 枚举原样上移（七个纯虚函数、字节级状态机、后端持有密文缓冲的分工边界、不承诺 thread-safe 这三条纪律全部不变——契约的形态已经过四腿实现验证，本次只改可见性）。基类构造 `protected`、五法则禁拷贝禁移动、析构虚。头文件含 `tls.h` 取 `TlsContext` / `PeerCertificate`，`tls.h` 不含本头，无环。本头随伞头导出，`tools/check/umbrella_manifest.txt` 的 `[direct]` 基线随之刷新。
2. **注册表与候选链**（同头声明，实现落新增的 `src/aurora/net/tls_backend.cpp`）：

    ```cpp
    using TlsBackendCreator =
        std::function<Result<std::unique_ptr<TlsBackend>>(const TlsContext &, std::string_view)>;

    struct TlsBackendRegistration {
        std::string id;       ///< 稳定小写标识（"schannel" / "securetransport" / "openssl" / "mbedtls"）
        int priority = 0;     ///< 候选链缺省次序，大者在前
        TlsBackendCreator create;  ///< 造后端；不可置空
    };

    auto register_tls_backend(const TlsBackendRegistration &registration) -> Result<void>;
    auto unregister_tls_backend(std::string_view id) -> bool;
    auto available_tls_backends() -> std::vector<std::string>;
    auto set_default_tls_backends(std::vector<std::string> chain) -> Result<void>;
    auto default_tls_backends() -> std::vector<std::string>;
    ```

    注册项只有三枚字段：**不设第二份名字**（`display_name` 会与原语 id 漂移，而诊断点名的必须就是可以填回 `TlsContext::backend` 的那个串），**不设 `supports()` 谓词**（工厂本来就会在「这条腿表达不了该口径」时落 `net-platform-unavailable`，再加一枚谓词只会造出「谓词说行、工厂说不行」的第二个真相源）。规则：`id` 空或 `create` 空 → `general-invalid-argument`；`id` 已存在 → `net-option-conflict`（**不覆盖**：静默替换别人注册的后端是比失败更糟的结果）。`unregister_tls_backend` 对内置 id 同样允许注销——「这台机器上我不想让某条腿参与」是调用方的合法决定，注销后握手起手的候选链里就没有它。`available_tls_backends()` 按 `priority` 降序返回 id 副本。注册表是进程级单例、内部一枚互斥量串行化，读取即拷出快照，且**绝不持锁调用工厂**（工厂是调用方代码，它在回调里注册或注销后端是一处自死锁）；注销不影响**已在途**的会话（它们持有自己的 `unique_ptr`），这条是本类型生命周期唯一的硬承诺。
3. **`set_default_tls_backends` 只校验形态、不校验存在性**：入参去重后逐项要求非空，否则 `general-invalid-argument`；空链是合法值，语义为「起手不选任何后端，除非每次握手显式指定」。不在置入时校验 id 是否已注册，因为第三方后端的注册次序与配置次序没有先后保证——置入即拒会把「先配链、后加载插件」这一常见启动顺序变成不可用形态。未知 id 在**解析时**跳过并留一条 `warning`；整链解析不出可用后端时落 `net-tls-engine-unavailable`，detail 点名链里的项与实际在案的 id。
4. **每次握手覆盖**：`TlsContext` 新增 `std::optional<std::string> backend`。未置 → 用全局候选链（Auto 语义，可回落）；置值 → 只用那一枚后端，**不回落**（显式指定即决定，悄悄换成别的后端等于推翻调用方的决定，尤其当决定里含合规口径时）。`TlsTransport` 新增 `[[nodiscard]] auto backend() const -> Result<std::string>`，回报实际生效的后端 id；空态或已关闭为 `net-socket-failed`（与 `peer_certificate()` 同口径：关掉通道后再报「当前后端」会诱导复用）。
5. **解析与回落语义（规范性）**：解析发生在握手起手、一次成型，产出的后端实例与 id 一并固化进会话状态。可跳过的下一步只有两种确定形态——候选未注册、或工厂落 `net-tls-engine-unavailable` / `net-platform-unavailable`（后者说的就是「这一枚表达不了这份口径」，如系统栈腿收到 PEM 形态的 mTLS 身份）；跳过时留 `warning` 点名 id 与原因。其余任何失败（口令错、PEM 读不开、版本界不合法）**终止整条链**并以该原因收尾：一次配置错误被另一条腿「凑成功」接走，比失败更难排查。后端一旦造出并开始握手，**永不换后端**——信任根在各腿各自实现里，换腿即换一次校验的口径，那等于把证书拒绝的处置权交给调度器。
6. **能力位改源**：`Engine::capabilities().tls` 由「注册表非空」回答。`detail::tls_backend_available()` 保留为注册表包装（`src/aurora/net/engine.cpp` 与 `tests/unit/utest_net_tls.cpp` 的既有引用点零改动即可继续成立），语义随之扩宽一格：注入自研后端后能力位为真，这与「调用真的能成功」仍然同源。内置后端在注册表首次访问时就地安装，安装集合由被编译进来的 TU 决定，因此能力位仍与构建面同源。
7. **构建面：系统栈与 provider 解耦**（`cmake/AuroraNetTls.cmake`）：新增 `AURORA_ENABLE_NET_TLS_SYSTEM`（`option`，Win32 / APPLE 缺省 `ON`，其余平台缺省 `OFF`；在不支持系统栈的平台上显式置 `ON` 即 configure 期 `FATAL_ERROR`——静默无后端就是把「拼错平台」伪装成「构建成功」）。新增 `AURORA_NET_TLS_PROVIDERS`（字符串列表，每项 `<static|shared>:<prefix>`，探测与链接规则逐字沿用现有实现）；**同一后端种类至多一枚 provider**（两个 OpenSSL 根没有可区分的语义，猜哪一个都是错），重复即 `FATAL_ERROR`；库定位改用**该种类自己的库名**（`NAMES ssl` / `NAMES mbedtls`，不再是 `NAMES ssl mbedtls` 的并集），于是「头推导出 mbedTLS、却链上 OpenSSL 的库」这类错配在安装根混挂两套库时被构造性排除（该危害此前只写在文档告诫里）。旧 `AURORA_NET_TLS_PROVIDER` 保留为单值别名；两者同时置值即 `FATAL_ERROR`（口径冲突不猜）。导入目标改为按种类建 `aurora::net::tls.openssl` / `aurora::net::tls.mbedtls`，并保留 `aurora::net::tls` 作为聚合 `INTERFACE` 目标——`cmake/AuroraVerify.cmake`、`tools/verify/tls_live_probe.cpp` 与安装侧引用点的名字不变。系统栈的链接面改由 `AURORA_ENABLE_NET_TLS_SYSTEM` 单独决定（`secur32 crypt32 bcrypt` / `-framework Security -framework CoreFoundation`），与 provider 是否存在无关。
8. **宏命名归位**：四枚引擎宏改名 `AURORA_ENABLE_NET_TLS_{SCHANNEL,SECURE_TRANSPORT,OPENSSL,MBEDTLS}`，归 `BUILD_OPTIONS.md` §1 的 ENABLE 组（`PRIVATE` 注入、不 `EXPORT`、不进 `aurora::debug::feature_flags()`，三条现状不变）。provider 侧宏仍由探测推导、不接受手工设置。旧宏名不做兼容别名：它们 `PRIVATE` 且不导出，消费者编译里从不存在，改名无迁移成本；库内四个 `#ifdef` 点与文档一次改齐。
9. **后端 TU 改唯一入口、删无后端桩**：四枚 TU 的 `detail::make_tls_backend()` 改为 `detail::make_tls_backend_schannel()` / `_securetransport()` / `_openssl()` / `_mbedtls()`，`detail::tls_backend_available()` 从各 TU 移除（改由注册表提供）；`src/aurora/net/detail/tls_backend.h` 退为内部声明头（声明四枚唯一入口与注册表需要的内置安装钩子），`tls_unavailable.cpp` 删除。多宏同真不再是链接冲突，而是「多条腿同时在注册表里」。
10. **已定案裁决的更替**：`codespec/changes/xplat-network-stack/proposal.md` 里「Windows / macOS 仍默认系统栈，且可被 provider 覆盖」这条已定案表述被本变更**推翻**（覆盖 → 共存）；`codespec/specification/06-app-platform.md` §8.8.18 的「装配模块因此只保证恰好编译一个后端 TU」同步作废并改写。旧编号与旧条目就地改写、不删除，避免制造指向不存在条目的伪死链。
11. **非目标**：不提供服务端 TLS（`TlsListener` 仍不存在，客户端-only 裁决不变）；不引入任何 TLS 的**动态二进制**依赖（`shared:` 腿的运行期寻址交宿主部署）——原先「`third_party/` 无新增、不引入任何 TLS 源码」这条已被**后续构建裁决更替**：mbedTLS 改由仓库内置源码 `third_party/mbedtls` 装配（`AURORA_ENABLE_NET_TLS_MBEDTLS=ON` → `add_subdirectory`，见 `cmake/AuroraNetTls.cmake` 与 `BUILD_OPTIONS.md` §1.2），故 `third_party/` 有新增，但「断网可构建」不变（源码在库、不联网），OpenSSL 仍不内置、仍走 provider 安装根；不做后端配置的持久化；不承诺跨编译器 / 跨 CRT 的插件二进制兼容——插件契约承诺的是**源码级 C++ 抽象接口**，同工具链同 CRT 静态或动态装配由调用方保证，这条如实写进公共头注释与文档而不是含糊过去。

## 验收判据

1. **共存性**：Windows 上同时给 `-DAURORA_ENABLE_NET_TLS_SYSTEM=ON -DAURORA_NET_TLS_PROVIDERS=shared:<vcpkg OpenSSL 根>`，配置日志两条腿各记一次，`available_tls_backends()` 同时含 `schannel` 与 `openssl`，且两条腿各能完成一次真握手（`utest_net_tls` 逐后端遍历，非只测缺省那条）。macOS 同理（`securetransport` + homebrew OpenSSL）。
2. **不互斥的构建矩阵**：CI 的 `tls-providers` 作业新增「系统栈 + provider 同腿」矩阵项，Windows 与 macOS 各一；Linux 侧保持 OpenSSL 与 mbedTLS 两枚 provider 同腿共存（旧形态只能各测一条）。反极性腿（只给 provider、显式关系统栈）保留，证明 `AURORA_ENABLE_NET_TLS_SYSTEM=OFF` 真的能关。
3. **回落只发生在握手开始前**：以假后端注册进表（不触网）断言四条——候选一工厂落 `net-platform-unavailable` ⇒ 候选二接手且成功；候选一工厂落 `net-option-conflict`（非可跳过码）⇒ 整链终止、错误原样上报、候选二的 `create` 一次都不被调用；`context.backend` 置值 ⇒ 只用那一枚、绝不换；已造出后端后失败 ⇒ 不再尝试下一个。每例都断言回调恰一次抵达。
4. **诚实失败**：空注册表（注销全部内置后端）时 `capabilities().tls` 为假，`handshake()` 恰一次落 `net-tls-engine-unavailable`，detail 含实际在案 id 列表；不存在任何退回明文的通路。
5. **注册表纪律**：重复 id 注册落 `net-option-conflict` 且原实现仍在案；注销内置 id 后该 id 不再出现在 `available_tls_backends()`；`set_default_tls_backends` 的入参含空串或重复项时按规则判定；链中未知 id 被跳过并留 `warning`，全链未知时落 `net-tls-engine-unavailable`。
6. **实际后端可查**：握手成功后 `backend()` 回报的 id 与 `context.backend` 指定值逐字相同；Auto 形态下回报的是链里第一个**能**表达该口径的 id。
7. **能力位单源**：`capabilities().tls == detail::tls_backend_available()` 仍成立；注入第三枚自研后端后能力位为真，且缺省链里能选中它。
8. **门禁全绿**：`check_api_schema_sync`（先跑 `--target aurora_api_json`）、`check_naming_conventions`、`check_api_budget`、`check_doc_comments`、`check_umbrella_header`（新头入 `[direct]`）、`check_core_layer_boundary`、`check_arch_module_map`、`check_platform_macros`、`check_codespec_xref`、`check_code_doc_sync`、`check_change_proposals`、`check_no_cjk_literals`、`check_no_hardcoded_paths`、`check_manual_test_format`、`check_version_consistency`、`format-check`；改动文件逐个过 `lint`。
9. **宏改名无残留**：全仓 `AURORA_NET_TLS_{SCHANNEL,SECURE_TRANSPORT,OPENSSL,MBEDTLS}` 旧名零命中（`build*/` 除外），`BUILD_OPTIONS.md` §1.2 与 `06-app-platform.md` §8.8.18 的表与新名一致。
10. **别名可用性**：只给旧 `AURORA_NET_TLS_PROVIDER` 单值的既有命令行（现 CI 与 manual-test 写法）配置结果与改名前语义一致；新旧同设时 configure 期硬失败并打印两个实际取值。

## 回写落点

1. `codespec/BUILD_OPTIONS.md`：§1.2 整节改写（宏名表更名、`AURORA_ENABLE_NET_TLS_SYSTEM` 与 `AURORA_NET_TLS_PROVIDERS` 两条新入、别名与冲突口径、共存装配、多后端与注册表语义、`shared:` 的加载期限制如实记账）；§1 总览表的 ENABLE 组语义补「内置 TLS 后端引擎」一项；§1.1 模块表里 `cmake/AuroraNetTls.cmake` 的职责描述随改；§依赖传导段（`aurora::aurora` 自动传递链接）里「三枚系统库只在系统栈缺省装配时挂上」随 `AURORA_ENABLE_NET_TLS_SYSTEM` 改述。
2. `codespec/specification/06-app-platform.md`：§8.8.18 后端接缝一节改写为公共契约 + 注册表 + 候选链，删除「恰好编译一个后端 TU」的规范承诺，补「握手开始前才可回落」与「换腿即换信任根口径」两条不变量；§8.8.18 的 provider 探测段与回归记录随构建面更新。
3. `codespec/SPECIFICATIONS.md`：§5 表 `SPEC.PLATFORM.NET.TLS-BACKEND.001` 行的名称与判据改写为「可插拔后端注册表 + 系统栈与 provider 共存 + 每次握手覆盖」，验收判据补「可注入后端、共存装配、回落只发生在握手前」。
4. `codespec/errors.toml`：`net-tls-engine-unavailable` 的 hint 改写（现文案教的是「配 provider」，新形态下还应教「查 `available_tls_backends()`、必要时注册或指定后端」）；`net-platform-unavailable` 的 hint 补一句「该后端不表达此口径时候选链会继续下一枚，显式指定后端则直接落此码」。改后跑生成器刷新 `codespec/ERROR_CATALOG.md`。
5. `codespec/manual-test/23-net.md`：TLS 段补「系统栈 + provider 共存」与「指定不可用后端 id」两项目的、步骤与期望，`AURORA_NET_TLS_PROVIDER` 写法更新为 `AURORA_NET_TLS_PROVIDERS`。
6. `codespec/changes/xplat-network-stack/proposal.md`：§16 里「系统栈可被 provider 覆盖」那条已定案就地标注被本变更更替（保留原编号与原文，追加裁决指向本目录），§8 宏表的四枚宏名随更名。
7. `codespec/ARCHITECTURE.md`：模块映射与后端矩阵中涉及 TLS 后端装配形态的表述随改（若该文档未描述后端唯一性承诺，则只在其网络子系统的装配段补一句注册表形态）。
8. 代码与数据面（不属 codespec 树，故不逐项列为回写落点，此处只记必做动作）：根目录 CHANGELOG.md 记破坏性条目（宏名、缓存变量、无后端桩 TU 删除、公共头新增）与迁移路径；根目录 aurora_api.json 经 `--target aurora_api_json` 重建；tools/check/umbrella_manifest.txt 经 `--write-manifest` 刷新。
