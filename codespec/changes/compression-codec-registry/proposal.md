# CHG-020 压缩解压能力可扩展化：编解码器抽象与注册表

| 字段 | 值 |
|:---|:---|
| 变更编号 | CHG-020 |
| 提出日期 | 2026-10-16 |
| 当前状态 | 已提议 |
| 关联需求 | 无 |
| 影响面 | 原地重写 include/aurora/core/compression.h 与 src/aurora/core/compression.cpp（字符串格式 ID、编解码器抽象、注册表、内置 wuffs 实现退为库内部）；迁移 src/aurora/net/detail/http_encoding.{h,cpp} 的格式标识与持有类型、include/aurora/net/http_client.h 的宣告条件注释；tests/unit/utest_core_compression.cpp 与 tests/integration/itest_known_enums.cpp 迁移；tools/include/known_enums.h 移除 CompressionFormat 登记、aurora_api.json 随 --target aurora_api_json 重建（enums 段 -1）；cmake/AuroraCompression.cmake 注释同步；codespec 回写见「回写落点」（另 CHANGELOG.md 记破坏性条目与迁移路径，该文档不属 codespec 树故不列为回写落点） |

## 动机

1. **零扩展点**：现状实现焊死在三方件 wuffs 上——应用不能替换解码器（更快的 zlib 绑定、平台原生 API）、不能注入测试替身、不能提供编码器；`compress()` 是恒失败的桩。「库当前不能压缩」是三方件事实，但不该以「库永远不能压缩」的架构形态固化下来。
2. **压缩方向缺一条实现路径**：wuffs 无编码器是事实，正确解法不是永久留桩，而是把编码能力的供给权经注册表交给应用（zlib / zstd / lz4 绑定或自研 deflate 压缩机），库只定义契约、调度与护栏。本批仍不内置编码器（不引新三方依赖），但接口形状令「注册即用」成立。
3. **对齐项目既有范式**：「自定义实现」在本仓已三立范式——`ImageCodecRegistry`（抽象基类 + 能力位 + 进程单例 + 内置懒注册）、`AudioDeviceBackend` 经 unique_ptr 注入、应用自注册字体。压缩子系统对齐同一范式，不另造机制，调用方与 AI 的学习成本最低。
4. **注入路径不随内置开关消失**：既有口径「关闭内置实现后自定义注入路径恒在」（自定义 Surface / AudioDeviceBackend / 自注册字体）尚未覆盖压缩——现状宏关时全部入口恒落 unavailable，未来的注入面也随宏一起消失。本批把 `AURORA_ENABLE_COMPRESSION` 的语义收窄为「只门控内置实现」。
5. **格式标识改为字符串 + 预定义常量**：格式 ID 是小写字符串，与 HTTP `Content-Encoding` 令牌一一对应（HTTP 层映射零成本）；内置三格式经头文件预定义常量暴露，保留可发现性与拼写安全；自定义格式（zstd / brotli / 应用私有 ID）由应用自选字符串注册，表达力无上限。取舍如实记账：失去编译期穷尽性，未知或拼错的 ID 在运行期落 `compression-unavailable` 且 detail 点名（诚实失败，不静默回落）；格式仍显式入参、不做内容嗅探。HTTP 侧格式集仍封闭（见变更内容 9）——线上协商面不随核心 API 开放而扩大。

## 变更内容

1. **格式 ID 与预定义常量**（include/aurora/core/compression.h，命名空间 `aurora`）：格式 ID 为 `std::string_view`，按字节精确匹配（大小写敏感；内置三枚取小写，与 HTTP 令牌规范写法一致，自定义 ID 建议同一风格）。内置三格式以预定义常量暴露（先例：`net/detail/http_encoding.h` 的 `AURORA_HTTP_ACCEPT_ENCODING`）：

    ```cpp
    inline constexpr std::string_view AURORA_COMPRESS_FORMAT_GZIP = "gzip";      ///< gzip 包装（RFC 1952）
    inline constexpr std::string_view AURORA_COMPRESS_FORMAT_ZLIB = "zlib";      ///< zlib 包装（RFC 1950）
    inline constexpr std::string_view AURORA_COMPRESS_FORMAT_DEFLATE = "deflate";  ///< 裸 DEFLATE 流（RFC 1951）
    ```

    注册表内部以 `std::string` 持存格式 ID，故调用方的 `string_view` 不要求长命（注册即拷贝）。错误 detail 直接点名格式 ID 字符串，不再需要枚举到文本的映射表。
2. **新增 `CompressionCodec` 抽象基类**：`name() -> std::string_view`（纯虚，实现方稳定标识，诊断用）；`can_compress() -> bool`（默认 false）；`compress(input, options) -> Result<std::vector<char>>`（默认落 `general-not-supported`，detail 带 name()）；`create_decompressor(options) -> Result<std::unique_ptr<Decompressor>>`（默认落 `compression-unavailable`——流式是可选能力，不实现即没有）。全部方法 const、实现须线程安全（同 `ImageCodec` 契约）；五法则禁拷贝移动，只经 shared_ptr 注入注册表。编解码器不自带格式字段——格式到实现的绑定发生在注册点（同一实现可注册到多个格式 ID，内置 wuffs 实现即此形态）。用户侧接入形如：

    ```cpp
    class ZstdCodec : public aurora::CompressionCodec {
        [[nodiscard]] auto name() const -> std::string_view override { return "my-zstd"; }
        [[nodiscard]] auto can_compress() const -> bool override { return true; }
        [[nodiscard]] auto compress(std::span<const char> in, const aurora::CompressOptions &opt) const
            -> aurora::Result<std::vector<char>> override;
        [[nodiscard]] auto create_decompressor(const aurora::DecompressOptions &opt) const
            -> aurora::Result<std::unique_ptr<aurora::Decompressor>> override;
    };
    aurora::register_codec("zstd", std::make_shared<ZstdCodec>());  // 一行接入，自定义格式 ID 自选
    auto raw = aurora::decompress("zstd", packed);                  // 调用侧与内置格式同一套入口
    ```

    流式契约的最小正确实现：`push` 缓存全部输入、`finish` 一次性解出并经 Sink 交出（不增量但合法）；关心峰值内存的实现自作真增量。编解码器只写流式即自动获得一次性入口（框架派生），无需实现第二个解压方法。编解码器方法返回的错误原样透传（不包装、不改码）。

3. **`Decompressor` 由具体类改为抽象基类**：`push` / `finish` / `DecompressSink` 契约逐字保留（含 const 限定与单条流作用域纪律）；静态 `create()` 移除，创建入口移至注册表；实例改经 `unique_ptr` 持有。内置 wuffs 实现退为库内部类型，头文件不再暴露其形状。**破坏性变更**：按值持有 `Decompressor` 与 `Decompressor::create` 的调用点迁移至 `create_decompressor(format, options)` 加 `unique_ptr`（仓内一处：src/aurora/net/detail/http_encoding.cpp；迁移路径记 CHANGELOG.md）。
4. **新增 `CompressOptions`**：单旋钮 `std::optional<int> level`（nullopt = 编解码器自定义默认；语义由实现定义，如 zlib 0–9 / zstd 1–22），框架只透传不解释——不为猜测中的编码器预造旋钮阵。
5. **新增 `CompressionCodecRegistry` 进程单例**：`instance()` 首次访问时按 `AURORA_ENABLE_COMPRESSION` 懒注册内置 wuffs 三族解码器（键为三枚预定义常量）；`register_codec(format, codec)` 为**覆盖语义**——同格式 ID 后注册者替换先注册者（「替换实现」正是本批开放的核心能力），空指针忽略；`unregister_codec(format)` 清除该格式 ID 的覆盖注册并回落内置（若有），无内置亦无覆盖时该格式不可用——测试隔离因此确定：注销即回默认态，不会把后续用例留在自定义实现上；注销不影响已创建的在途解压器（实例由调用方持有，与注册表槽位脱钩）；`compress` / `decompress` / `create_decompressor` 按格式 ID 分派，无实现落 `compression-unavailable`——格式无任何编解码器与「有编解码器但 `can_compress()` 为假」是两种 detail 文案（前者点名格式 ID，后者点名实现名与缺失的方向）；`registered()` 诊断列表逐格式给出当前生效实现（覆盖优先），条目形态 `struct RegisteredCodec { std::string format; std::string name; };`。注册表内部持「内置表 + 覆盖表」两张表，查找 = 覆盖优先、回落内置；互斥量护槽位，全部方法 const（与 `ImageCodecRegistry` 同形态）；分派时锁内取 `shared_ptr` 副本、锁外调用实现——不在持锁期间运行实现方代码，与实现内重入注册也不会互锁。
6. **自由函数转调单例**：`register_codec` / `unregister_codec` / `compress` / `decompress` / `create_decompressor`，第一参数均为格式 ID（`std::string_view`）。一次性 `decompress` 在框架侧由流式派生（create → push 全量 → finish 收集），语义与现状逐字等价——编解码器作者只写流式即自动获得一次性入口。
7. **解压炸弹护栏双层**：框架层——注册表交出的每个解压器经计数装饰器包裹（库内部，不占公共 API），越 `max_output_bytes` 即止交并落 `compression-output-too-large`，不依赖实现是否自觉；同一轮里越限标志优先于解码器错误（与现状 `emit_output` 先于 transform 状态检查的次序一致）；实现层——`create_decompressor` 收到的 options 即产出上限，实现应自行止步（CPU 视角不白烧），内置 wuffs 实现保留现状的提前止交。限额的单一事实源是 `DecompressOptions`，装饰器是兜底而非第二真源。
8. **`AURORA_ENABLE_COMPRESSION` 语义收窄**：只门控内置 wuffs 编解码器的注册；抽象、注册表、自由函数与自定义注入路径恒编入——关闭宏的构建里注册自定义编解码器即可用。cmake/AuroraCompression.cmake 的 wuffs OBJECT 库创建条件不变（内置实现仍需它），注释同步收窄后的口径。
9. **HTTP 层迁移（格式集封闭，零语义变化）**：src/aurora/net/detail/http_encoding.{h,cpp} 的格式标识由 `CompressionFormat` 改格式 ID 字符串——`Coding` 枚举保持不动（支持集仍封闭为 gzip / deflate 两枚），`known_format` / `probe_deflate_format` 改返回预定义常量（`string_view` 指向静态存储，可安全持于 `std::optional`），`Stage::format` 改 `std::optional<std::string_view>`；`StreamingDecoder` 的 Stage 持有由 `std::optional<Decompressor>` 改 `std::unique_ptr<Decompressor>`，`open()` 的 `Decompressor::create` 改走注册表工厂；`compression_available()` 由编译期宏判定改**注册表查询**——gzip、zlib、deflate **三槽均可服务方为真**（Deflate 环节内部先试 zlib 再回落裸流，两型都须有着落；任缺其一即整组不宣告，不做部分宣告），这是「宣告与支持集同源」在运行期的落实：应用替换实现后宣告照常（覆盖实现仍可服务）；在无内置的构建里注销任一必需槽位则连宣告都不发（诚实降级）；关闭宏且应用自注册了三槽的实现时 HTTP 即恢复透明解压（注入的实现参与到既有两枚格式的服务中，不扩大格式集）。include/aurora/net/http_client.h 文件头「构建编入解压能力时补 `Accept-Encoding`、未编入连宣告都不发」的注释判据同步改为注册表可服务（comment-only）。物化路径的 `decode_stage` / `decode_content_encoding` 结构不动，仅换格式 ID 实参；`utest_net_http_encoding` 可为此新增一例（见验收判据 7），既有用例零改动。
10. **测试与工具链迁移**：tests/unit/utest_core_compression.cpp 的 `CompressionFormat::X` 全部改 `AURORA_COMPRESS_FORMAT_X`、`Decompressor::create` 改注册表工厂，并按验收判据扩充；tools/include/known_enums.h 移除 CompressionFormat 登记、tests/integration/itest_known_enums.cpp 移除对应编译期锚点（该锚点是枚举成员求和 + `>= 0` 断言，纯删除即可、无期望值需同步）；aurora_api.json 随 `--target aurora_api_json` 重建（enums 段 -1，`check_api_schema_sync` 复核零漂移）；aurora_cli / aurora_lsp / aurora_mcp 对 known_enums 是泛化迭代，无需逐工具改动；公共 API 注释按 `CODING_STANDARDS.md` §13 写全。**不新增 demo**：examples/demos/demo_core.cpp 的职责是输出通道与编译期判定且逐组对应 codespec/manual-test/01-core.md 的人工用例，压缩既不在其 charter 也无人工用例，接入示例由头文件注释与提案承载（有意决策，非遗漏）。
11. **无新增文件**：全部落点均为既有文件原地改写。

## 验收判据

1. **路由与替换**：测试桩编解码器注册到自定义格式 ID（如 `"test-format"`）后，`compress` / `decompress` / `create_decompressor` 全部路由到桩（调用计数证据）；以桩覆盖 `AURORA_COMPRESS_FORMAT_GZIP` 槽后，同一入口改走桩不走内置 wuffs；三枚预定义常量的取值逐一断言（`== "gzip"` / `"zlib"` / `"deflate"`）。变异自证：把注册表查找改回直连内置 ⇒ 覆盖用例转红。
2. **方向独立可选**：只实现压缩的桩注册后 `compress` 成功而 `decompress` 落 `compression-unavailable`；只实现解压的桩反之——两个能力位独立，不连坐。
3. **不可用口径**：未知格式 ID（未注册的自定义名或拼写变体）落 `compression-unavailable` 且 detail 点名该 ID；大小写敏感（`"GZIP"` 与 `"gzip"` 是两个 ID，前者未注册即不可用，不静默匹配）；关闭 `AURORA_ENABLE_COMPRESSION` 的构建里内置路径全部落 unavailable；覆盖用例执行后注销，内置三格式解压逐字节恢复正确（证明注销回落内置而非留空）。
4. **护栏在框架层**：故意不实现限额的桩编解码器经注册表包装后，越 `max_output_bytes` 仍落 `compression-output-too-large`，且越限之后的字节不交调用方；一次性与流式两条路径同判据。变异自证：摘除计数装饰器 ⇒ 该例转红（证明护栏不依赖实现自觉）。
5. **等价与零回归**：同一桩编解码器，一次性 `decompress` 与 `create_decompressor` + push + finish 产物逐字节一致；内置三格式既有 golden 向量（含跨轮大载荷）与改前逐字节一致。
6. **注入路径恒在（关闭宏的构建）**：`AURORA_ENABLE_COMPRESSION=OFF` 构建下注册自定义编解码器（自定义格式 ID）后 `compress` / `decompress` 照常工作——内置缺席不影响注入路径，这是动机 4 架构口径的直接判据。
7. **HTTP 零回归**：`utest_net_http_encoding` / `utest_net_http_client` / `utest_net_http_stream` 既有用例全绿且零改动（迁移只换格式标识与持有类型，不换语义）；关闭宏的构建里同名 skip 登记不变。三槽判据（gzip ∧ zlib ∧ deflate 任一不可服务即不宣告）在 `utest_net_http_encoding` 以「自注册三槽后能力转真」一例验证：该例只在能力关闭的构建里断言（内置三槽恒在使它在能力在场的构建里空洞），按 TEST-R6 纪律两腿同名登记——关闭腿执行、在场腿同名跳过；注册的桩用完即注销，回落空注册表，不污染后续用例。
8. **门禁**：`check_naming_conventions` / `check_doc_comments` / `check_core_layer_boundary` / `check_umbrella_header` / `check_api_budget` / `check_api_schema_sync` / `check_platform_macros` / `check_no_hardcoded_paths` / `format-check` 全绿；`lint` 仅改动 TU；全量 ctest 最高并行度在宏 ON / OFF 两种构建下均绿。

## 回写落点

- `codespec/specification/01-core.md`（§12 整节重写：字符串格式 ID 与预定义常量、编解码器抽象、注册表与覆盖 / 注销语义、双层护栏、宏语义收窄、注入路径恒在）
- `codespec/ARCHITECTURE.md`（核心文件清单行 compression.h 的括注由「gzip / zlib / deflate 通用解压」改述为「压缩解压编解码器与注册表」，锚点 01-core.md §12 不变）
- `codespec/BUILD_OPTIONS.md`（**两处**：cmake 模块表 `AuroraCompression.cmake` 行的职责描述、`-D` 清单 `AURORA_ENABLE_COMPRESSION` 行的注释——均改注为「门控内置实现；关闭后自定义注入路径恒在」）
- `codespec/errors.toml`（三枚 `compression-*` 的 hint 文本随语义更新：压缩方向从「未实现」改为「无已注册编码器」、不可用口径点名格式 ID；无新增码，连带重建生成物 ERROR_CATALOG.md）
- `codespec/specification/06-app-platform.md`（§8.8.20：`Accept-Encoding` 注入条件由「编入宏」改为「注册表可服务三槽」，宣告与支持集同源的口径不变）
- 发布记录 CHANGELOG.md（破坏性条目：`CompressionFormat` 枚举移除（迁移：枚举值改 `AURORA_COMPRESS_FORMAT_*` 常量）、`Decompressor` 改抽象基类、`Decompressor::create` 移除、`compress()` 从恒失败改为注册表调度；迁移路径逐项）——该文档不属 codespec 树，故按 AGENTS.md 硬规则 5 不列为回写落点，实际已同步。
