# net 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`net/` —— 通用网络栈（提案 [`changes/xplat-network-stack/proposal.md`](../changes/xplat-network-stack/proposal.md) §17 的落地件），在实测依赖分层中位于 L0（`app::EventSourceRegistry`）之上、L3（HTTP）之下：

| 头 | 职责 |
|:---|:---|
| `net/engine.h` + `net/capabilities.h` | 常驻 poll 线程引擎与九枚能力位（`tls` 由被编译进来的后端探测口回答） |
| `net/transport.h` | 异步流式传输 `StreamTransport` / `StreamListener`（TLS 之下的那条流） |
| `net/tls.h` | `TlsContext` / `TlsVersion` / `PeerCertificate` / `TlsTransport`：TLS 通道，握手成功后与 L1 流同构 |
| `net/resolver.h`、`net/proxy.h` | 名称解析与代理配置（本文件的用例只作前置，不单独出题） |
| `net/http_binding.h` | 声明式交付面：`deliver` / `bind_resource` / `deliver_stream` / `image_fetcher`（把一次交换接成 `Task` 与三态资源，并把取消接回套接口） |

本文档覆盖两类判据：**① 必须出站才能成立的**——真实公网证书、真实对端的 ALPN 协商、真实 TLS 记录层上的往返；**② 必须有宿主帧循环参与的**——三态资源在真实帧循环里的逐支可视落地（`TC-NET-007`）。第二类离线单测做不到：`tests/unit/` 里没有装帧循环的宿主，回投与唤醒这一段在测试里一律走「无投递器即同线程直调」那条腿，于是取消、失败与迟到结果这些「要看得见」的结局只剩运行期证据。离线可跑的判据（自签桩下的握手、未知 CA 被拒、过期证书、危险开关、pin 判定、ALPN 无交集、取消与关闭结局）全部在 `tests/unit/utest_net_tls.cpp` 里，**不在本文档重复**——重复记录会让「手工跑过」与「CI 跑过」两种证据在读者眼里混成一种。本口径自声明式交付面切片起追加第 ② 类，第 ① 类的取舍不变。

### 1.2 执行载体

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `tools/verify/tls_live_probe.cpp` | `aurora_verify_tls_live` | 对真实公网端点跑六段自动判据：能力位 → 缺省口径握手 → 证书四项摘要 → ALPN → 加密通道往返 → pin 反向 |

探针的命令行口径（`aurora_verify::parse_command_line`）：

| 选项 | 值域 | 缺省 | 用途 |
|:---|:---|:---|:---|
| `--host` | DNS 名 | `www.example.com` | SNI 与主机名核对的输入 |
| `--port` | `1`–`65535` | `443` | TLS 端口的 TCP 端口 |
| `--alpn` | 单条 ASCII 标识 | 不发 ALPN 扩展 | 宣告一枚 ALPN 标识（`http/1.1` 在目标端点上协商得出，别的标识会被对端拒绝） |
| `--interactive` | 开关 | 关 | 在核对证书字段后暂停，供与浏览器逐项比对 |

**退出码即结论**：`0` 全部判据通过、`1` 有 `[FAIL]`、`2` 环境不可用（`[SKIP]`，两种成因：本腿没有 TLS 后端，或 DNS／出站路径不可用）。探针**不进 CTest**（`aurora_add_verify_probe` 的 `EXCLUDE_FROM_ALL` 与出站要求，见 §1.8），产品输出经 `AURORA_LOG_RAW` 走 stdout，因此 `[PASS]` / `[FAIL]` 行可直接重定向成文本比对。

**引擎生命周期是载体的一部分**：探针没有宿主帧循环，`main` 的第一条语句构造一枚 `EngineScope`，其析构显式调用 `Engine::instance().shutdown()`。少了这一步，常驻线程会跨过 `main` 的返回，在静态拆除阶段撞上已经不可依赖的加锁与 join（机制与裁决见 [`specification/06-app-platform.md`](../specification/06-app-platform.md) §8.8.4 与提案 §16 第 18 项）。执行时**退出码 139 不是判据失败**，而是这条收敛缺失或平台运行时拆除次序出问题，须按缺陷处理。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | 三条腿：Windows 本机（MinGW/GCC + Ninja，系统栈 Schannel）、WSL Ubuntu（POSIX `poll`，分别以 mbedTLS 与 OpenSSL 作 provider） |
| 构建 | 每腿一个独立构建目录；WSL 两腿需 `-DAURORA_BUILD_TESTS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON` 与对应的 `-DAURORA_NET_TLS_PROVIDER=<static\|shared>:<安装根>` |
| 终端 | 支持 UTF-8 输出；可分离重定向 stdout 与 stderr |
| 网络 | **需出站访问 443**（本文档是全仓唯一允许出站的测试面）；`--host` 换成别的公网端点时 §2 的字段值断言须随之重取 |
| 对照 | 证书字段的独立对照源为浏览器证书查看器或 `openssl s_client -connect <host>:443 -servername <host>`（后者不受本库代码影响，故是有效的外部对照） |

**信任库差异**：Linux 两腿读的是 provider 所配置的 CA 束（mbedTLS 腿由本层读入系统常规 CA 束文件，OpenSSL 腿走 `SSL_CTX_set_default_verify_paths`），Windows 腿读系统 ROOT 存储。因此同一张证书在两处「被谁信任」的成因不同，§2 只断言**结论**（成功 / 被拒及其错误码），不断言链的中间节点。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，本模块段为 `NET`，自 001 起连续编号。

### 1.5 用例字段清单与格式规范

每条用例由**六个固定字段**组成，排布于同一张 Markdown 表格内。**字段顺序固定、字段名一字不差、每字段独占一行、单元格内不换行**（需要多条内容时用 `<br>` 分隔）：

| 序号 | 字段 | 格式 | 正则约束 | 说明 |
|:--:|:---|:---|:---|:---|
| 1 | 用例编号 | 单行 | `^TC-[A-Z0-9]+-\d{3}$` | 模块内唯一且不复用 |
| 2 | 测试目的 | 单行 | 自由文本 | 一句话，指向**单一**验证目标 |
| 3 | 前置条件 | 单行 | 自由文本 | 只描述**环境状态**；「必须先执行某用例」属依赖用例字段，不在此重复 |
| 4 | 依赖用例 | 单行 | `^无$` 或 `^TC-[A-Z0-9]+-\d{3}(, TC-[A-Z0-9]+-\d{3})*$` | 见下方规范 |
| 5 | 操作步骤 | 条目列表 | 每条目以 `^\d+\. ` 开头，条目间以 `<br>` 分隔 | 自 1 起连续编号 |
| 6 | 预期结果 | 条目列表 | 同操作步骤；编号是步骤号的**子集** | 见 §1.6 |

**依赖用例字段规范（四条硬性要求）：**

1. 无依赖时**必须**写 `无`，不得留空、不得写 `-`、`N/A`、`none` 等变体。
2. 有依赖时写**用例编号全称**（含模块段），不写省略形式；跨模块依赖同样写全称。
3. 多项依赖以**半角逗号加一个空格**（`, `）分隔；逗号前后不得出现空格以外的字符。
4. 多项依赖的**排列顺序即依赖先后**——先决者在前。因用例按编号升序声明，等价于按被依赖用例的**编号升序**排列。

**模块级约定**：三腿的 configure 与构建统一置于 `TC-NET-001`；其余用例不重复构建，通过依赖用例字段间接依赖该用例产出的探针可执行文件。

**为什么必须统一格式**：本文件由 `tools/check/check_manual_test_format.py` 解析守护（CTest 用例 `check_manual_test_format`，校验字段名与顺序、取值域、依赖拓扑、步骤-预期同号映射与执行记录表格式）。上述约定使每个字段可用**单条正则**校验，无需自然语言推断。

### 1.6 操作步骤与预期结果编号规则

**预期结果与操作步骤同号一一对应**：`预期结果 N` 即「步骤 N 的对应结果」。

1. 预期结果的序号**等于**其对应步骤的序号，两者严格相等——不得因省略而重排、压缩或重新连续编号。
2. 纯执行步骤**不产生预期结果**，其序号在预期结果列表中直接跳过。
3. 故预期结果的序号**不要求连续**，条数也无需等于步骤数。
4. 步骤列表中，纯执行步骤在文本末尾标注「（纯执行，无预期结果）」，供与预期结果列表逐号对照。

示例：某用例有步骤 1、2、3、4，其中步骤 3 为纯执行步骤，则该用例的预期结果共 3 条，编号为 1、2、4。

**该规则解决的问题**：执行者按步骤顺序推进时，编号本身即是映射关系，无需自行推断「这条预期结果对应哪一步」；执行记录可借步骤号精确落到失败点。

### 1.7 判定与记录

| 结果 | 含义 |
|:---|:---|
| PASS | 全部非空预期结果均满足 |
| FAIL | 任一非空预期结果不满足；须在记录表登记该预期结果对应的**步骤号**、实际现象与缺陷编号 |
| BLOCKED | 前置条件无法满足（如载体构建失败），未开始执行 |
| SKIP | 主动判定不适用于本环境（如被测平台与用例限定平台不符） |

探针输出里的**字节数、耗时、证书有效期与序列号**属可变部分，判定时只核对形态与是否出现，不比对具体值（`TC-NET-006` 的跨腿一致性除外——它比的是同一时刻同一端点的同一摘要）。

**执行记录表格式**（列顺序固定）：

| 列 | 格式 | 允许值 / 正则 |
|:---|:---|:---|
| 用例编号 | 单行 | 与 §2 用例编号一致 |
| 执行日期 | 单行 | `^\d{4}-\d{2}-\d{2}$` |
| 执行人 | 单行 | 自由文本 |
| 结果 | 单行 | `PASS` / `FAIL` / `BLOCKED` / `SKIP` |
| 失败步骤号 | 单行 | 留空（未失败或未执行），或 `^\d+(, \d+)*$`（多个以半角逗号加一个空格分隔） |
| 实际现象 | 单行 | 自由文本 |
| 缺陷编号 | 单行 | 留空或缺陷单号 |
| 备注 | 单行 | 自由文本 |

### 1.8 覆盖取舍

| 不做的项 | 理由 |
|:---|:---|
| 探针不进 CTest | 出站的成败不由本库决定（公网端点的证书轮换、限速、DNS 都可能让一条判据变红），把这种噪声接进门禁会让红灯失去归因价值；提案 §17 的切片纪律「不跨切片留红灯」因此要求这类用例只能手工触发 |
| 不做公网服务端互操作矩阵（各站点的实现偏差） | 判据是「本库的客户端在真实链上走得通」，不是「本库能通过任意服务器的考验」；后者要维护一枚不断漂移的站点集合，属另一件事 |
| 吊销检查（CRL / OCSP）不出题 | 三条腿一律不做吊销判定（见 [`specification/06-app-platform.md`](../specification/06-app-platform.md) §8.8.18），已知边界而非疏漏 |
| 会话恢复 / 0-RTT / 重协商的服务端侧 | 接缝只暴露「数据阶段可外发握手字节」这一条例外通路，服务端发起的形态需要一枚可控服务器，属离线用例范围 |
| 逐平台等待集并入宿主主循环 | 提案 §16 第 12 项把它收窄为宿主可选项，仓内宿主一律走常驻引擎线程 |

## 2 用例清单

### 2.1 三腿装配与构建基线

#### TC-NET-001 三腿 configure 与探针构建，含 provider 探测的两类失败

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-001 |
| 测试目的 | 验证 TLS 后端的装配唯一决策点：系统栈缺省、provider 探测推导引擎宏、路径非法时 configure 期即失败，以及 Linux 无 provider 时的诚实降级 |
| 前置条件 | 本机有 Windows 与 WSL 两条腿；WSL 内已安装 mbedTLS 与 OpenSSL 的开发文件（各自构成一枚「安装根」） |
| 依赖用例 | 无 |
| 操作步骤 | 1. Windows 腿：不给任何 provider，`cmake --preset ninja` 后 `cmake --build build --target aurora_verify_tls_live`（纯执行，无预期结果）<br>2. 查看该次 configure 的输出中 `TLS backend:` 那一行，并确认产物存在<br>3. WSL mbedTLS 腿：`cmake -S <仓> -B <新目录> -G Ninja -DAURORA_BUILD_VERIFY_TOOLS=ON -DAURORA_NET_TLS_PROVIDER=static:<mbedTLS 安装根>` 后构建同一目标（纯执行，无预期结果）<br>4. 查看该次 configure 的 `TLS backend:` 行与构建退出码，再从生成的 `build.ninja` 里取三枚 mbedTLS 归档在链接行上的先后次序<br>5. WSL OpenSSL 腿：同法以 `-DAURORA_NET_TLS_PROVIDER=shared:/usr` configure 并构建（纯执行，无预期结果）<br>6. 查看该次 configure 的 `TLS backend:` 行与构建退出码<br>7. 把 provider 的安装根改成一枚不存在 `include/` 的目录再 configure 一次<br>8. Linux 腿不给任何 provider 再 configure 一次，并跑一条只查能力位的用例 |
| 预期结果 | 2. configure 日志为 `TLS backend: system stack (AURORA_NET_TLS_SCHANNEL)`；构建 rc=0 且产出 `aurora_verify_tls_live.exe`<br>4. configure 日志为 `TLS backend: provider AURORA_NET_TLS_MBEDTLS (static) from <安装根>`；构建 rc=0；链接行上三枚归档的次序是 `libmbedtls.a` 在 `libmbedx509.a` 之前、`libmbedx509.a` 在 `libmbedcrypto.a` 之前（GNU ld 左到右求解，被依赖者须在右）<br>6. configure 日志为 `TLS backend: provider AURORA_NET_TLS_OPENSSL (shared) from /usr`；构建 rc=0<br>7. configure **失败**（非零退出），诊断打印实际检查过的 `include/` 与 `lib/` 组合；不得出现「configure 成功但后端为 none」的静默降级<br>8. configure 成功且日志为 `TLS backend: none`，能力位用例回报 `capabilities().tls == false`（提案判据 7 的可执行形态） |

### 2.2 真实公网链上的握手与摘要

#### TC-NET-002 缺省口径对真实证书的握手与证书四项摘要

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-002 |
| 测试目的 | 验证「校验默认开启且只用系统信任库」这一缺省口径在真实公网证书上成立，且 `peer_certificate()` 的四项字段来自真实证书 |
| 前置条件 | `TC-NET-001` 的三腿产物齐备；本机可出站访问 443；浏览器或 `openssl s_client` 可用于独立对照 |
| 依赖用例 | TC-NET-001 |
| 操作步骤 | 1. 三腿各运行一次 `aurora_verify_tls_live`（不带 `--alpn`）<br>2. 核对首段的能力位判据行<br>3. 核对缺省口径握手那一条判据行<br>4. 核对 `subject` / `issuer` / `valid` / `spki` 四行注记都非空<br>5. 用独立对照源取同一端点的证书，逐项比对主体、签发者与有效期<br>6. 核对 SPKI 摘要的形态 |
| 预期结果 | 1. 三腿退出码均为 `0`，输出末尾为 `result: PASS (exit 0)`<br>2. `[PASS] capabilities().tls reports the assembled backend`<br>3. `[PASS] handshake against the real chain in the system trust store succeeds`——真实证书在**未经任何追加**的系统信任库下被接受<br>4. 四行注记均有值：`subject` 含该端点的 `CN=`，`issuer` 是公网 CA 而非自签，`valid` 是两个 ISO-8601 时刻的区间且当前时刻落在其中<br>5. 主体与签发者与对照源逐字一致；有效期与对照源相符（秒级偏差不计）<br>6. `spki` 是 44 字符的 base64（结尾一枚 `=`）且只含 base64 字符集，即 `base64(SHA-256(DER(SubjectPublicKeyInfo)))` 的形态，不是十六进制指纹也不是整证摘要 |

### 2.3 ALPN 与加密通道上的往返

#### TC-NET-003 ALPN 宣告与协商回报

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-003 |
| 测试目的 | 验证 ALPN 在真实服务器上的两种结局都有确定形态：协商出标识时如实回报，对端不选时如实回报而不是伪造空串 |
| 前置条件 | 三腿产物齐备且可出站；目标端点在 `--alpn http/1.1` 下的行为已知（公网端点可能选中也可能拒绝该标识） |
| 依赖用例 | TC-NET-001 |
| 操作步骤 | 1. 三腿各运行 `aurora_verify_tls_live --alpn http/1.1`（纯执行，无预期结果）<br>2. 核对 ALPN 那一条判据行的三种可能形态<br>3. 三腿各运行不带 `--alpn` 的探针，与步骤 1 的判定行数对照<br>4. 任一腿以 `openssl s_client -alpn http/1.1 -connect <host>:443` 观察服务器是否选用该标识，作为独立对照 |
| 预期结果 | 2. 该行的结局只能是三种之一，且各腿各自成立即可（不做跨腿强制）：`[PASS] ...` 携协商出的 `http/1.1`；或 `[PASS] peer selected no ALPN id` 后一句说明通道仍可用；或 `[PASS]` 那条形为 `net-option-conflict` 的回报——**绝不允许**把「未协商」呈现为空串<br>3. 两次运行的判据行数相同，差别只在 ALPN 那一条的文案：不带 `--alpn` 时该行是「未宣告 ⇒ 回报形态不符而不是伪造空串」，带时才是协商结果本身（空集合必须不产缓冲，带着空表调用会让 ClientHello 发不出去，故这条判据在无宣告时同样要落一次确定性回报而不是缺席）<br>4. 对照源的结论与本腿回报一致：对照显示服务器未选择该标识时，本腿必须报「对端未选」而不是报协商成功 |

#### TC-NET-004 加密通道上与明文流同构的 HTTP/1.1 往返

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-004 |
| 测试目的 | 验证握手成功后的通道与 L1 流逐字同构：一条 `GET` 写得出去、响应读得回来，且记录层的分帧对上层不可见 |
| 前置条件 | 三腿产物齐备且可出站 |
| 依赖用例 | TC-NET-001 |
| 操作步骤 | 1. 三腿各运行探针（不带 `--alpn`）（纯执行，无预期结果）<br>2. 核对往返那一条判据行与其后的字节数注记<br>3. 观察 `received` 注记的字节数量级<br>4. 三腿各运行一次离线 TLS 套件的载荷跨记录用例（`aurora_test_runner --run utest_net_tls`，只看那一条） |
| 预期结果 | 2. `[PASS] response over the encrypted channel starts with HTTP/1.`，且 `received <N> bytes` 中 `N > 0`<br>3. `N` 与该端点一次真实首页响应的量级相符（数百字节至数千字节，不是 0、不是被截断的一枚记录大小）<br>4. 载荷跨记录往返那条为 OK：响应分片落在任意记录边界上都不影响上层拿到完整字节 |

#### TC-NET-005 SPKI pin 的反向判定与危险开关不遮蔽 pin

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-005 |
| 测试目的 | 验证 pin 集合外的公钥一定被拒，且拒因来自 pin 而不是别的分支——「不设防」与「防错对象」必须能分别表达 |
| 前置条件 | 三腿产物齐备且可出站 |
| 依赖用例 | TC-NET-001 |
| 操作步骤 | 1. 三腿各运行探针——其第六段会另起一条连接，用**改过一字符的真实摘要**作 pin（纯执行，无预期结果）<br>2. 核对 pin 那一条判据行<br>3. 核对该行紧随的 `observed:` 注记<br>4. 离线套件里跑「危险开关不遮蔽 pin」那条用例作正向配套 |
| 预期结果 | 2. `[PASS] a public key outside the pin set is rejected with net-tls-pin-mismatch`<br>3. `observed:` 里的错误码逐字为 `net-tls-pin-mismatch`，且消息体携带命中的摘要值（便于运维定位是哪一枚 pin 过期）；不得是 `net-tls-cert-rejected` 或握手类码——那说明判定发生在链之后或根本没发生<br>4. 该用例为 OK：`allow_insecure_certificate` 置真时 pin 仍生效，链校验跳过只影响链与主机名 |

### 2.4 跨腿一致性

#### TC-NET-006 同一真实端点的跨腿逐值一致性

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-006 |
| 测试目的 | 验证三条后端腿对同一真实端点给出同一结论与同一摘要，即跨平台一致行为在 TLS 层成立 |
| 前置条件 | 三腿产物齐备且可出站；三腿在同一次维护窗口内运行（证书轮换会让摘要合理地不同，属时间差而不是不一致） |
| 依赖用例 | TC-NET-001, TC-NET-002 |
| 操作步骤 | 1. 三腿各运行一次探针并把 stdout 分别重定向到三个文件（纯执行，无预期结果）<br>2. 比对三段的 `[PASS]` / `[FAIL]` 计数与退出码<br>3. 比对三段的 `spki` 行<br>4. 比对三段的 `subject` 与 `issuer` 行<br>5. 各腿再跑一次离线 TLS 套件（`aurora_test_runner --run utest_net_tls`），核对通过数与跳过数 |
| 预期结果 | 2. 三腿退出码同为 `0`，`[FAIL]` 计数同为 0<br>3. 三腿的 `spki` 逐字相同（同一张真实证书的 SPKI 摘要与后端无关；这条是 pin 计算跨腿可移植的唯一证据）<br>4. 三腿的 `subject` 与 `issuer` 文本逐字相同<br>5. 每腿 TLS 套件的失败数为 0；跳过只允许两枚，一是「本构建有后端，故无后端落点那例按环境缺失跳过」（每腿都有），二是 provider 腿缺少该能力对应的原生件（如 mbedTLS 3.6.5 无 PKCS#12 读取器 ⇒ 客户端身份那条，见 [`../specification/06-app-platform.md`](../specification/06-app-platform.md) §8.8.18）；离线用例不得因缺出站而跳过 |

### 2.5 声明式交付面在宿主帧循环里的三态

#### TC-NET-007 `Resource` 的三态逐支可视，且 `reload()` 的取消落到套接口上

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-NET-007 |
| 测试目的 | 验证一次真实 HTTP 交换经 `net::http::bind_resource` 接成三态资源后，在宿主帧循环里把 Loading / Ready / Failed 三支逐支落地，且 `reload()` 的取消是真的掐断在途连接而不只是丢弃结果 |
| 前置条件 | `ninja` 预设的构建目录已 configure 且能建 `demo_net_http`；本机可绑 `127.0.0.1` 的临时端口（**全程不出站、可在断网环境执行**）；stderr 未被丢弃——判据的一半在日志里 |
| 依赖用例 | 无 |
| 操作步骤 | 1. `cmake --build build --target demo_net_http` 并运行 `build/demo_net_http.exe`，同时观察其 stderr（纯执行，无预期结果）<br>2. 起手立刻读窗口里的文本块与日志<br>3. 不做任何输入，等文本块自己变化后再读同一处<br>4. 按一次 `reload`，并在文本块仍是 `loading…` 的那段窗口内再按一次（桩的停顿就是应答前那次带超时的探针读，这一枚按得到）<br>5. 按 `stop / resume serving` 关掉供答，再按一次 `reload`<br>6. 再按一次 `stop / resume serving` 恢复供答，然后按 `reload` |
| 预期结果 | 2. 文本块为 `loading… (reload again within this pause to cut the exchange mid-flight)`，日志同文本一行，并先于它出现 `accepted a connection`<br>3. 文本块变成 `seq=1  status=200  the socket is the truth`，日志里 `answered, closing the connection` 早于这一行——即「对端已写出」与「宿主看见」之间确有间隔，但这一变化不依赖任何输入事件<br>4. 日志出现 `request vanished before the answer (that exchange was cancelled)` 或 `peer vanished before the answer (that exchange was cancelled)`（桩读请求那一枚、与应答前那枚探针读，任一读到对端收摊都算；只丢结果的取消两枚都读不到东西）：前一次那条连接被掐断，这是取消落到套接口上的唯一硬证据；文本块最终落到更大的序号上，前一次的迟到结果没有盖住这一次<br>5. 文本块进 Failed 支 `failed: http-protocol-error - peer closed before the body was complete`，日志给出 `serving halted: connection closed without an answer`；窗口不崩、不白屏，`Resource` 的错误侧就是这一行的全部来源<br>6. 文本块回到 Ready 支且序号比第 5 步之前更大：失败不是终态，同一枚资源可继续重发 |

**本用例只核对形态，不比对时刻**：第 3 步「对端已写出 → 宿主看见」之间的间隔由帧循环的唤醒时机决定，本机实测在数百毫秒到数秒之间浮动（宿主回投与唤醒属 `app` 层的既有行为，不是本交付面的判据），因此它只记为观察、不作红灯；若该间隔变成「直到下一次输入才落地」，才算唤醒丢失，按缺陷处理。

**桩在写出应答之前先做一次探针读**（带 1.5 s 超时的第二次 `read_some`），而不是睡一段固定时长：睡出来的停顿留下的窗口是「对端已经写完一份没人收的应答」，客户端那一侧照常收到完整字节，取消只发生在结果投递上——第 4 步要看的「对端读到收摊」在那种形态里根本不可能出现，用例退化成「日志里有没有那一行」的赌博。探针读把「客户端还在等」与「客户端已经收摊」分成两种**线上事实**：还在等的只会让这次读超时，被取消的那一条交出一次真实的读端收摊。于是这段窗口既是 Loading 分支的可见时间，也是人工按得到 `reload` 的取消窗口（1.5 s），一物两用。

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-NET-001 | 2026-10-14 | Qoder Agent | PASS | | Windows 腿 configure 日志 `TLS backend: system stack (AURORA_NET_TLS_SCHANNEL)`，构建 rc=0 且产出 `aurora_verify_tls_live.exe`；WSL mbedTLS 腿日志 `TLS backend: provider AURORA_NET_TLS_MBEDTLS (static) from <该腿的 mbedTLS 安装根>`（安装根是本机路径，按门禁纪律不入仓库文本，`-D` 传入）、rc=0，`build.ninja` 链接行三枚归档次序为 `libmbedtls.a` → `libmbedx509.a` → `libmbedcrypto.a`；WSL OpenSSL 腿日志 `TLS backend: provider AURORA_NET_TLS_OPENSSL (shared) from /usr`、rc=0；安装根改成不存在 `include/` 的目录时 configure rc=1 并打印实际检查过的 `include/` 与 `lib/` 组合；Linux 腿不给 provider 时 configure 成功且日志 `TLS backend: none`，该构建内 `utest_net_tls.absent_backend_reports_engine_unavailable` 为 OK（能力位为假、请求 TLS 通道落 `net-tls-engine-unavailable`） | | 无 provider 的那次运行该套件 20 例中 4 跑 16 跳过，跳过原因逐条为 `this build carries no tls backend (capabilities().tls is false)`，即诚实降级而不是静默放行 |
| TC-NET-002 | 2026-10-14 | Qoder Agent | PASS | | 三腿（Windows Schannel、WSL OpenSSL、WSL mbedTLS）退出码均为 0 且末尾 `result: PASS (exit 0)`，判据行各 7 条 `[PASS]` 无 `[FAIL]`；首条为 `capabilities().tls reports the assembled backend`，缺省口径条目为 `handshake against the real chain in the system trust store succeeds`；四行注记均有值——`subject CN=example.com`、`issuer C=US, O=SSL Corporation, CN=Cloudflare TLS Issuing ECC CA 3`、`valid 2026-09-26T22:49:11Z .. 2026-12-25T22:56:35Z`、`spki toyMGpiThuoVaTygu6QeCcnum2PKJknnGOsHalH7BzU=`；`openssl s_client` 独立对照给出逐字相同的 `subject=` 与 `issuer=`，`openssl x509 -pubkey` 到 SPKI DER 再到 SHA-256 base64 的独立算得值与 `spki` 行逐字相同（44 字符、结尾一枚 `=`、纯 base64 字符集） | | 三腿全程未追加任何信任根，真实证书由系统信任库直接接受；SPKI 形态即 `base64(SHA-256(DER(SPKI)))`，不是十六进制指纹也不是整证摘要 |
| TC-NET-003 | 2026-10-14 | Qoder Agent | PASS | | 三腿 `--alpn http/1.1` 运行均 rc=0，ALPN 判据行为 `[PASS] negotiated ALPN is the advertised id (a peer may only pick from the advertisement)`；不带 `--alpn` 的运行判据行数仍为 7 条 `[PASS]`，那一条换成 `[PASS] no ALPN advertised -> negotiated_alpn() reports the shape mismatch, not an empty id`；`openssl s_client -alpn http/1.1` 独立对照输出 `ALPN protocol: http/1.1`，与本腿回报一致 | | 本端点确实选中该标识，故三腿都落在「协商出标识」这一分支；「未协商」与「未宣告」两分支的可执行形态由离线套件覆盖，不依赖公网端点行为 |
| TC-NET-004 | 2026-10-14 | Qoder Agent | PASS | | 三腿均报 `[PASS] response over the encrypted channel starts with HTTP/1.` 且注记 `received 931 bytes`（三腿同一数量级、逐字节相同），非 0 也不是单枚记录大小；离线套件 `utest_net_tls` 的载荷跨记录往返用例在三腿均为 OK（分片落在任意记录边界上上层仍拿到完整字节） | | 931 字节与 `https://www.example.com/` 一次真实首页响应的量级相符 |
| TC-NET-005 | 2026-10-14 | Qoder Agent | PASS | | 三腿探针第六段（另起一条连接、以改过一字符的真实摘要作 pin）均报 `[PASS] a public key outside the pin set is rejected with net-tls-pin-mismatch`，紧随的 `observed:` 注记逐字为 `net-tls-pin-mismatch (TLS certificate pin mismatch: 'peer public key pin toyMGpiThuoVaTygu6QeCcnum2PKJknnGOsHalH7BzU= is not in the configured pin set')`，携带命中的摘要值且不是链拒绝或握手类码；离线套件的「危险开关不遮蔽 pin」用例在三腿均为 OK | | 拒因来自 pin 判定本身；`allow_insecure_certificate` 置真只跳过链与主机名校验，pin 仍生效 |
| TC-NET-006 | 2026-10-14 | Qoder Agent | PASS | | 三腿 stdout 逐字节比对：退出码同为 0，`[FAIL]` 计数同为 0，`subject`、`issuer`、`valid`、`spki` 四行与 `received 931 bytes` 三腿逐字相同；离线 TLS 套件失败数为 0，Windows 腿 19 跑 1 跳、WSL OpenSSL 腿 19 跑 1 跳、WSL mbedTLS 腿 18 跑 2 跳，跳过集合为 `absent_backend_reports_engine_unavailable`（三腿皆有后端故按环境缺失跳过）加 mbedTLS 腿的 `client_identity_satisfies_a_verified_peer_request`（3.6.5 无 PKCS#12 读取器）；`utest_net_test_double` 三腿各 14 例全跑全绿 | | 三条腿在同一次维护窗口内运行，故摘要一致性与证书轮换无关；`subject`/`issuer` 的文本形态自本批起按 DER 正序统一（OpenSSL 腿此前用 `XN_FLAG_RFC2253` 会反序，实测暴露后已改），跨腿可逐字比对的唯一值仍是 `spki` |
| TC-NET-007 | 2026-10-16 | Qoder Agent | BLOCKED | | 第 2、3 步已见：起手文本块与日志同为 `loading… (reload again within this pause to cut the exchange mid-flight)`，且 `accepted a connection` 先于它；不做任何输入即落到 `seq=1  status=200  the socket is the truth`，日志里 `answered, closing the connection` 早于这一行约 4 s。第 4 步未按受控形态执行：运行期间本机桌面进入锁屏，Windows 侧自动化在锁屏下不投递输入，两次 `reload` 的间隔因此无从控制（其间一次非受控的取消确实让桩读到 `request vanished before the answer (that exchange was cancelled)`，但那不是按步骤按出来的证据）。第 5、6 步未执行 | | 判据既未成立也未推翻，按未执行对待；桌面解锁后照 §2.5 的步骤重跑即可。目前「取消落到套接口」的硬证据只有离线那枚 `utest_net_http_binding.a_reload_cuts_the_socket_of_the_previous_request`（证据取在串行对端：读到收摊 + 监听口照常受理下一条） |
