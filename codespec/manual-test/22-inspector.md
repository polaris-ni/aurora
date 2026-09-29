# inspector 模块人工测试用例

> **文档状态：临时**
> 本文件按固定格式编写，供程序解析：字段顺序、字段名、值域与分隔符均为约定的一部分，见 §1.5。
> 格式规范与首份示例见 `codespec/manual-test/01-core.md`。

## 1 适用范围与约定

### 1.1 被测模块

`inspector/` —— 运行时 UI 检视层（2 个公共头），在实测依赖分层中位于 L7。

| 头 | 职责 |
|:---|:---|
| `inspector_api.h` | `Inspector` 统一编程接口：树查询（`tree_text` / `tree_rich` / `tree_json` / `tree_json_full`）、`widget_info`、`query`、`get_state`、`find_node`、`get_prop` / `set_prop`、`apply_patch`、`simulate_*`、`subscribe_changes`、`components` |
| `inspector_server.h` | `InspectorServer`：只监听本机回环地址的 HTTP 服务，pimpl 隐藏 Winsock2，后台工作线程处理请求 |

该模块把「运行中的 UI」暴露为可读写的结构化数据，是本库「AI 可直接操作界面」定位的落点。其中 `InspectorServer` 的实现编在**独立静态库** `aurora_inspector_server`（见 §1.2），仅在构建开关开启时存在，因此**默认构建下该能力不可用**——人工用例的第一价值，就是确认开关的开启与关闭两条路径都表现正确。

### 1.2 执行载体

| 载体 | 构建目标 | 用途 |
|:---|:---|:---|
| `examples/demos/demo_inspector_server.cpp`（**新增**） | `demo_inspector_server` | 根树为扁平表单、索引路径稳定，供逐端点核对 `/api/*` 的读写闭环 |
| `examples/app/google_play/demo_google_play.cpp` | `demo_google_play` | 既有应用级演示，仅用于确认服务器亦能挂到 Navigator 驱动的真实应用上；其树随路由变化，不做端点级断言 |

**为何新增载体**：`demo_google_play` 的根是 `NavigatorHost`，其 `child_nodes()` 为空，故 HTTP 查到的树只有 1 个节点、索引路径随路由漂移，无法稳定断言「某个索引对应哪个控件」。新增载体的根树刻意为扁平表单，五个子节点索引 `"0"`–`"4"` 恒定，各端点的读写目标可预期。

新载体窗口标题 `Inspector server · Aurora Demo`，客户区 620×420，根为 `Column`，五个子节点如下：

| 索引 | 控件 | 初始内容 |
|:--:|:---|:---|
| 0 | `Text` | `User info form` |
| 1 | `TextInput` | `value` 为空、`placeholder` 为 `Enter name` |
| 2 | `TextInput` | `value` 为 `alice@example.com`、`placeholder` 为 `Enter email` |
| 3 | `Button` | `label` 为 `Submit`（点击回调把计数 +1 并写入索引 4 的文本） |
| 4 | `Text` | `submit count = 0` |

索引 3 → 4 构成可远程观测的因果：`POST /api/input/click {"path":"3"}` 应使索引 4 的文本递增。

在 `cmake/AuroraDemos.cmake` 中，`demo_inspector_server` 与 `demo_google_play` 一起被条件链接到 `aurora_inspector_server`（仅当构建开关开启时），使消费者经链接自动获得该开关对应的编译宏。

**模块级约定**：本模块载体的构建（开关开启与非开启两条路径）统一置于 `TC-INSPECTOR-001` 的步骤中；其余用例不重复构建，通过依赖用例字段间接依赖其产物。

### 1.3 执行环境

| 项目 | 要求 |
|:---|:---|
| 平台 | Windows 本机（实测环境为 MinGW/GCC + Ninja） |
| 构建 | 需一个**启用构建**目录 `build-inspector/`（配置时置 `-DAURORA_BUILD_INSPECTOR_SERVER=ON`）；载体为按需目标，不在默认构建内 |
| 终端 | 支持 UTF-8 输出；可分离重定向 stdout 与 stderr |
| 网络 | 服务仅绑定回环地址，端口默认 6280；本机需可用 `curl` |
| 端口 | 除 `TC-INSPECTOR-012` 外，执行时 6280 应空闲 |
| 指针 | 交互模拟经 HTTP 合成事件，无需真实鼠标 |

**关于环境变量**：客户端工具（`tools/servers/inspector_client.h`）的默认端口常量同为 6280，并可用环境变量 `AURORA_INSPECTOR_PORT` 覆盖。本模块用例统一使用默认端口，不依赖该变量。

### 1.4 用例编号规则

格式 `TC-<模块段>-<三位序号>`，自 001 起连续编号。
模块段为**模块目录名的大写形式**（`core` → `CORE`、`i18n` → `I18N`；取 `include/aurora/` 下的模块目录名，本目录文件名前缀 `NN-` 是文档序号、不计入模块段，如 `01-core.md` → `CORE`），**不得自创缩写**——目录名含数字时数字原样保留，故正则为 `^TC-[A-Z0-9]+-\d{3}$`。
本模块目录名 `inspector`，即 `TC-INSPECTOR-001` 起。序号在本模块内唯一且**不复用**；用例被删除后其编号作废，新用例取下一个可用序号。
用例在文档中**按编号升序声明**——该顺序即默认执行顺序，也是依赖列表排序的依据（见 §1.5）。

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

**模块级约定**：本模块载体的构建步骤统一置于 `TC-INSPECTOR-001` 的步骤 1 与步骤 6；其余用例不在步骤中重复构建，而是通过依赖用例字段间接依赖其产出的已构建载体。

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

输出中含**时间戳、线程 id、行号**的字段属可变部分，判定时只核对格式与是否出现，不比对具体值。

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

未执行的取值一律**留空单元格**，不写 `-`、`TBD`、`N/A` 等占位符。

### 1.8 覆盖取舍

`Inspector` 的树查询、属性读写、补丁与交互模拟的**进程内语义**已由单测覆盖（`tests/unit/utest_inspector_api.cpp`、`tests/unit/utest_inspector_panel.cpp`）；`InspectorServer` 的 HTTP 解析、路由与并发健壮性亦有 `tests/unit/utest_inspector_server.cpp` 与 `tests/integration/itest_inspector_robustness.cpp` 覆盖。

人工用例只针对「真实进程 + 真实 HTTP + 真实窗口」这条端到端链路：

| 子域 | 人工判定优于程序判定的原因 |
|:---|:---|
| TCP 绑定与端口冲突 | 「端口被占用时新实例启动失败可上报、且不影响既有实例」只有在真实进程与真实端口上才成立 |
| 回环安全边界 | `Host` 头的回环校验属信任边界，须以真实请求验证其确实拒绝非回环来源 |
| 截图落盘 | 「返回的是合法 PNG 且内容非空白」需在文件层面核对签名与像素 |
| 因果可观测 | 「远程点击真的改变了界面状态」（索引 3 → 索引 4 文本递增）是跨请求的端到端事实，进程内断言替代不了 |

**不纳入人工测试的部分及其理由**：

| 范围 | 理由 | 已有覆盖 |
|:---|:---|:---|
| `InspectorPanel` 可视化浏览（`include/aurora/widget/inspector_panel.h`，属 widget 层） | 纯视觉浏览，无程序可断言量；其镜像关系与属性面板键已在 widget 模块覆盖 | `codespec/manual-test/11-widget.md`、`tests/unit/utest_inspector_panel.cpp` |
| 多窗口调试端点（`/api/tree?window=<id>`、`/api/windows` 的枚举回调） | 需宿主注册 `set_window_tree_getter` 与窗口枚举回调，本模块交付的两个载体均**未注册**（故 `/api/windows` 恒为 `{"count":0,"windows":[]}`）；多窗口本身的行为已在 `codespec/manual-test/19-app.md` 覆盖 | app 模块人工测试用例 |
| 消费者工具 `aurora_mcp` / `aurora_cli` | 二者是 inspector 的消费者，各自命令面另有覆盖，本模块不重复 | `codespec/manual-test/11-widget.md` |

## 2 用例清单

### 2.1 载体构建与启动基线

#### TC-INSPECTOR-001 载体构建（开关开启与关闭两条路径）与启动基线

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-001 |
| 测试目的 | 确认载体在构建开关开启与非开启两条路径下均可构建，且开启路径按预期端口启动、非开启路径安全跳过 |
| 前置条件 | 位于仓库根目录；本机工具链（Ninja + MinGW/GCC）可用；端口 6280 空闲；默认构建目录 `build/` 已完成配置 |
| 依赖用例 | 无 |
| 操作步骤 | 1. 配置启用构建目录：`cmake -S . -B build-inspector -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DAURORA_BUILD_INSPECTOR_SERVER=ON -DAURORA_ENABLE_DEBUG=ON -DAURORA_BUILD_TESTS=OFF -DAURORA_BUILD_DEMOS=ON`（纯执行，无预期结果）<br>2. 构建载体：`cmake --build build-inspector --target demo_inspector_server demo_google_play`（纯执行，无预期结果）<br>3. 启动并分离重定向：`./build-inspector/demo_inspector_server.exe 1> out.txt 2> err.txt`（纯执行，无预期结果）<br>4. 查看窗口标题与客户区尺寸<br>5. 查看 `err.txt` 的内容、行数与日志级别<br>6. 在默认构建目录 `build/` 构建非启用路径的同一载体：`cmake --build build --target demo_inspector_server && ./build/demo_inspector_server.exe 1> out_b.txt 2> err_b.txt`（纯执行，无预期结果）<br>7. 查看 `err_b.txt` 的内容与该次运行的退出码 |
| 预期结果 | 4. 标题为 `Inspector server · Aurora Demo`，客户区约 620×420<br>5. 恰 3 行、全部为 `INF` 级：2 行来自 `InspectorServer` 自身（`InspectorServer accept loop started on port 6280`、`InspectorServer started on http://127.0.0.1:6280`），另 1 行来自载体（`[inspector] listening on http://127.0.0.1:6280`）；无 `ERR` / `FTL` 级日志<br>7. 非启用构建下输出恰 1 行 `INF` 级 `[inspector] AURORA_BUILD_INSPECTOR_SERVER 未开启，跳过`，退出码为 0 |

### 2.2 树的只读查询

#### TC-INSPECTOR-002 只读树查询：`/api/tree` 与 `/api/debug/tree`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-002 |
| 测试目的 | 验证只读树查询端点返回完整且稳定的控件树结构，含 JSON 双子端点、YAML 序列化与窗口枚举 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280；本机可用 `curl` |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -i http://127.0.0.1:6280/api/tree`，查看状态行、响应头与响应体结构<br>3. 核对响应体 `children` 数组的元素个数与各子节点的类型、文本<br>4. 请求 `curl -s http://127.0.0.1:6280/api/debug/tree`，与步骤 2 的响应结构比对<br>5. 请求 `curl -s http://127.0.0.1:6280/api/yaml`，查看响应形态<br>6. 请求 `curl -s http://127.0.0.1:6280/api/windows`，查看响应 |
| 预期结果 | 2. HTTP `200`；响应头含 `Connection: close`；响应体顶层含 `"type":"Column"`、`"props":{...}` 与 `"children"` 数组<br>3. `children` 恰 5 个元素，类型自上而下为 `Text`、`TextInput`、`TextInput`、`Button`、`Text`；文本与占位符与 §1.2 的索引表一致（索引 4 为 `submit count = 0`）<br>4. `/api/debug/tree` 返回同一形状的完整 widget 树 JSON（内部走 `aurora::debug::widget_tree`），节点数与类型序列与步骤 2 一致<br>5. HTTP `200`；返回当前树的 YAML 文本，顶层为 `children:`，每项含 `props` / `content` 等键<br>6. HTTP `200`；响应为 `{"count":0,"windows":[]}`——未注册窗口枚举回调时返回空数组而非错误 |

### 2.3 控件属性读写

#### TC-INSPECTOR-003 单控件属性读取：`GET /api/widget/{path}`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-003 |
| 测试目的 | 验证可按索引路径读取单个控件的描述符与属性值 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s http://127.0.0.1:6280/api/widget/1`，查看响应结构<br>3. 核对 `descriptor` 对象<br>4. 核对 `values` 中与索引 1 对应的属性 |
| 预期结果 | 2. HTTP `200`；响应体含 `"descriptor"` 与 `"values"` 两个对象<br>3. `descriptor` 含 `"name":"TextInput"` 与 `property_names` 数组<br>4. `values` 中 `value` 为空串、`placeholder` 为 `Enter name`（与索引 1 的 `TextInput` 一致） |

#### TC-INSPECTOR-004 属性回写：`PUT /api/widget/{path}/{prop}`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-004 |
| 测试目的 | 验证经 HTTP 回写控件属性后该值被真实读取（而非仅回显请求） |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -X PUT http://127.0.0.1:6280/api/widget/2/value -H "Content-Type: application/json" -d "\"bob@example.com\""`，查看响应<br>3. 再请求 `curl -s http://127.0.0.1:6280/api/widget/2`，读取 `values.value` |
| 预期结果 | 2. HTTP `200`；响应体为 `{"property":"value","status":"ok","widget_path":"2"}`<br>3. `values.value` 变为 `bob@example.com`，与步骤 2 写入的值一致（回写已被真实读取到） |

### 2.4 交互模拟

#### TC-INSPECTOR-005 交互模拟（文本）：`POST /api/input/text`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-005 |
| 测试目的 | 验证文本输入模拟能改变控件状态，且该变化可被后续读取观测 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -X POST http://127.0.0.1:6280/api/input/text -H "Content-Type: application/json" -d "{\"path\":\"1\",\"text\":\"typed by AI\"}"`，查看响应<br>3. 请求 `curl -s http://127.0.0.1:6280/api/widget/1`，读取 `values.value` |
| 预期结果 | 2. HTTP `200`；响应体为 `{"action":"text","status":"ok","widget_path":"1"}`<br>3. `values.value` 变为 `typed by AI` |

#### TC-INSPECTOR-006 交互模拟（点击）：`POST /api/input/click`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-006 |
| 测试目的 | 验证点击模拟触发了控件的真实回调，且其副作用可被跨请求观测 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001, TC-INSPECTOR-005 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s http://127.0.0.1:6280/api/widget/4`，记录 `values.content` 的初值<br>3. 请求 `curl -s -X POST http://127.0.0.1:6280/api/input/click -H "Content-Type: application/json" -d "{\"path\":\"3\"}"`，查看响应<br>4. 再次请求 `curl -s http://127.0.0.1:6280/api/widget/4`，读取 `values.content` |
| 预期结果 | 2. `values.content` 为 `submit count = 0`<br>3. HTTP `200`；响应体为 `{"action":"click","status":"ok","widget_path":"3"}`<br>4. `values.content` 为 `submit count = 1`，较步骤 2 恰增 1；若文本不变，说明点击回调未在真实应用中被派发，即 FAIL |

### 2.5 补丁、组件与调试端点

#### TC-INSPECTOR-007 批量属性补丁：`POST /api/patch`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-007 |
| 测试目的 | 验证批量补丁端点能一次提交并按路径应用属性变更 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -X POST http://127.0.0.1:6280/api/patch -H "Content-Type: application/json" -d "[{\"path\":\"/0/content\",\"value\":\"Patched title\"}]"`，查看响应<br>3. 请求 `curl -s http://127.0.0.1:6280/api/widget/0`，读取 `values.content` |
| 预期结果 | 2. HTTP `200`；响应体为 `{"ops":1,"status":"ok"}`<br>3. `values.content` 变为 `Patched title`（补丁路径以根为起点，与 `GET /api/widget/{path}` 的索引路径同源） |

#### TC-INSPECTOR-008 组件 schema 列表：`GET /api/components`

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-008 |
| 测试目的 | 验证组件 schema 端点返回全量控件类型及其元数据 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280；当前目录可写 |
| 依赖用例 | TC-INSPECTOR-001, TC-INSPECTOR-002 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -o components.json -w "%{http_code} %{size_download}" http://127.0.0.1:6280/api/components`，查看状态码与响应体体积<br>3. 在 `components.json` 中清点数组元素个数<br>4. 查看首个元素与任一元素的字段集合 |
| 预期结果 | 2. HTTP `200`；响应体为 JSON 数组、约 124 KB<br>3. 元素个数恰 73<br>4. 每个元素含 `type`、`children_policy`、`container`、`default_props`、`dynamic_children`、`events`、`examples`、`is_clickable` 等字段；首个元素的 `type` 为 `Badge` |

#### TC-INSPECTOR-009 调试端点族：状态、拾取、性能与运行开关

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-009 |
| 测试目的 | 验证调试端点族返回运行时状态、控件拾取与性能/时间线数据，且 flags 只接受 POST |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001, TC-INSPECTOR-002 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s http://127.0.0.1:6280/api/debug/state`，查看关键字段<br>3. 请求 `curl -s "http://127.0.0.1:6280/api/debug/pick?x=10&y=10"`，查看 `hit` 与 `chain`<br>4. 请求 `curl -s "http://127.0.0.1:6280/api/debug/pick?x=300&y=200"`，查看 `hit` 与 `chain`<br>5. 依次请求 `/api/debug/perf`、`/api/debug/timeline`、`/api/debug/why`、`/api/debug/diagnostics`，查看各自响应键<br>6. 请求 `curl -s -X POST http://127.0.0.1:6280/api/debug/flags -H "Content-Type: application/json" -d "{\"layout_guides\":true}"`，查看响应<br>7. 对同一路径改发 `GET /api/debug/flags`，查看状态码与错误体 |
| 预期结果 | 2. HTTP `200`；含 `"available":true`、`"width":620`、`"height":420`（两者即 §1.2 声明的客户区逻辑尺寸，与显示器 DPI 无关）、`"scale_factor"` 等于本机 DPI 缩放（96 DPI 为 `1.0`，144 DPI 为 `1.5`）、`"should_close":false`、`"has_native_window":true` 与 `frame_count` 等字段<br>3. HTTP `200`；`"hit":true`；`chain` 为根→最深命中链，首个元素 `type_name` 为 `Column`（`bounds` 宽 620、高 194），其后为 `Text`<br>4. HTTP `200`；响应为 `{"chain":[],"hit":false}`<br>5. `/api/debug/perf` 键含 `avg_frame_ms` / `fps` / `dropped_frames` / `frame_budget_ms` / `p50_ms` / `p99_ms` 等；`/api/debug/timeline` 键含 `avg_layout_ms` / `avg_paint_ms` / `avg_present_ms` 等；`/api/debug/why` 形如 `{"count":<n>,"entries":[...]}`；`/api/debug/diagnostics` 为 `{"count":0,"diagnostics":[]}`<br>6. HTTP `200`；`status` 为 `ok`，`flags` 中 `layout_guides` 为 `true`，且 `layer_borders` / `layout_guides` / `overdraw` / `relayout_boundaries` / `repaint_highlight` 五字段齐全<br>7. HTTP `405`；错误体为 `{"error":"Method not allowed for /api/debug/flags","status":405}` |

#### TC-INSPECTOR-010 截图端点：`GET /api/debug/snapshot` 返回 PNG

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-010 |
| 测试目的 | 验证截图端点返回合法 PNG，且内容为该窗口的当前帧 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280；当前目录可写 |
| 依赖用例 | TC-INSPECTOR-001, TC-INSPECTOR-002 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -o snapshot.png -w "%{http_code} %{content_type} %{size_download}" http://127.0.0.1:6280/api/debug/snapshot`，查看状态码、内容类型与下载字节数<br>3. 查看 `snapshot.png` 文件头的 8 个字节<br>4. 用图片查看器打开 `snapshot.png` |
| 预期结果 | 2. HTTP `200`；`Content-Type` 为 `image/png`；下载字节数约 2.34 MB（非零）<br>3. 文件头 8 字节为 `89 50 4e 47 0d 0a 1a 0a`（PNG 签名）<br>4. 图像为该窗口当前帧的真实截图，能辨认标题与五个子控件，非全黑或全白（另可用 `?source=win` 改走屏幕窗口截图对照） |

### 2.6 错误语义与端口边界

#### TC-INSPECTOR-011 错误语义与回环安全边界

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-011 |
| 测试目的 | 验证非法方法、缺参、未知路径与非法 Host 均返回可判定的结构化错误 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；已启动并监听 127.0.0.1:6280 |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动载体 `./build-inspector/demo_inspector_server.exe`（纯执行，无预期结果）<br>2. 请求 `curl -s -i http://127.0.0.1:6280/api/input/click`，查看状态与错误体<br>3. 请求 `curl -s -X POST http://127.0.0.1:6280/api/input/text -H "Content-Type: application/json" -d "{}"`，查看状态与错误体<br>4. 请求 `curl -s http://127.0.0.1:6280/api/widget/9`，查看状态与错误体<br>5. 请求 `curl -s http://127.0.0.1:6280/api/nope`，查看状态与错误体<br>6. 请求 `curl -s http://127.0.0.1:6280/api/to_code`，查看状态与错误体<br>7. 带非回环 Host 请求：`curl -s -H "Host: example.com" http://127.0.0.1:6280/api/tree`，查看状态与错误体 |
| 预期结果 | 2. HTTP `405`；错误体为 `{"error":"Method not allowed for /api/input","status":405}`<br>3. HTTP `400`；错误体为 `{"error":"Missing or invalid 'path' (tree index path string, e.g. \"0/1\"; empty string targets the root)","status":400}`<br>4. HTTP `404`；错误体为 `{"error":"Widget not found at path: 9","status":404}`<br>5. HTTP `404`；错误体为 `{"error":"Unknown endpoint: /api/nope","status":404}`<br>6. HTTP `405`；错误体为 `{"error":"Method not allowed for /api/to_code","status":405}`（该端点需 POST）<br>7. HTTP `403`；错误体为 `{"error":"Forbidden: unexpected Host header","status":403}`——服务端只接受回环 Host，非回环请求被拒（客户端侧亦只认 `127.0.0.1` / `localhost` / `::1`，刻意不做 DNS 解析） |

#### TC-INSPECTOR-012 端口被占用时启动失败可上报

| 项目 | 内容 |
|:---|:---|
| 用例编号 | TC-INSPECTOR-012 |
| 测试目的 | 验证端口被占用时新实例启动失败可上报，且不影响既有实例 |
| 前置条件 | 载体 `demo_inspector_server` 已构建成功；可分别捕获两个实例的 stderr |
| 依赖用例 | TC-INSPECTOR-001 |
| 操作步骤 | 1. 启动第 1 个实例 `./build-inspector/demo_inspector_server.exe 1> out1.txt 2> err1.txt`，保持其运行（纯执行，无预期结果）<br>2. 在第 1 个实例仍运行时启动第 2 个实例 `./build-inspector/demo_inspector_server.exe 1> out2.txt 2> err2.txt`（纯执行，无预期结果）<br>3. 查看 `err2.txt` 的内容与日志级别<br>4. 在第 2 个实例退出后，向第 1 个实例请求 `curl -s http://127.0.0.1:6280/api/tree`，查看响应 |
| 预期结果 | 3. 恰 2 行 `ERR` 级：`bind() failed on port 6280, error: 10048` 与 `[inspector] failed to start on port 6280`；第 2 个实例随后退出<br>4. 第 1 个实例的 `/api/tree` 仍返回 HTTP `200` 与完整树——既有实例不受影响 |

## 3 执行记录表

| 用例编号 | 执行日期 | 执行人 | 结果 | 失败步骤号 | 实际现象 | 缺陷编号 | 备注 |
|:---|:---|:---|:---|:---|:---|:---|:---|
| TC-INSPECTOR-001 | 2026-09-24 | Qoder Agent | PASS | | 配置与 `demo_inspector_server`/`demo_google_play` 两目标构建均 rc=0；标题 `Inspector server · Aurora Demo`，窗口外框 635×457（客户区即 620×420）；`err.txt` 恰 3 行且全为 `INF`——`InspectorServer accept loop started on port 6280`、`InspectorServer started on http://127.0.0.1:6280`、`[inspector] listening on http://127.0.0.1:6280`，无 `ERR`/`FTL`，`out.txt` 0 行；`build/`（开关未开）下同载体恰 1 行 `INF` `[inspector] AURORA_BUILD_INSPECTOR_SERVER 未开启，跳过`，退出码 0 | | 执行前以 `netstat` 确证 6280 空闲；非启用路径 `main` 直接返回、不建窗，故无窗口属预期 |
| TC-INSPECTOR-002 | 2026-09-24 | Qoder Agent | PASS | | `/api/tree` HTTP 200、响应头含 `Connection: close`，顶层 `"type":"Column"` 带 `props` 与 `children`；`children` 恰 5 个，自上而下 `Text`/`TextInput`/`TextInput`/`Button`/`Text`，索引 1 `value` 空串 + `placeholder` `Enter name`、索引 2 `alice@example.com`、索引 3 `label` `Submit`、索引 4 `submit count = 0`；`/api/debug/tree` 与之内外同形、类型序列一致；`/api/yaml` 200（3635 B）顶层 `children:`，每项含 `props`/`type`、文本项含 `content`；`/api/windows` 200 且为 `{"count":0,"windows":[]}` | | 树体 2320 B；未注册窗口枚举回调时返回空数组而非错误，与 §1.8 的取舍说明一致 |
| TC-INSPECTOR-003 | 2026-09-24 | Qoder Agent | PASS | | HTTP 200；响应体仅 `descriptor` 与 `values` 两对象；`descriptor.name` 为 `TextInput`、`property_names` 21 项（自 `value`/`placeholder` 起至 `width`/`height`/`show`）；`values.value` 为空串、`placeholder` 为 `Enter name` | | 与 §1.2 索引表的第 1 行逐项对齐 |
| TC-INSPECTOR-004 | 2026-09-24 | Qoder Agent | PASS | | PUT 返回 200 且响应体逐字为 `{"property":"value","status":"ok","widget_path":"2"}`；随后 `GET /api/widget/2` 的 `values.value` 已是 `bob@example.com`（`placeholder` 仍 `Enter email`） | | 写入值经独立读请求取回，排除「仅回显请求体」；本用例与 005/006/007 各自重启全新实例，避免前序写入污染后序初值断言 |
| TC-INSPECTOR-005 | 2026-09-24 | Qoder Agent | PASS | | POST 返回 200 且响应体逐字为 `{"action":"text","status":"ok","widget_path":"1"}`；`GET /api/widget/1` 的 `values.value` 变为 `typed by AI` | | 全新实例上执行，索引 1 初值仍为空串 |
| TC-INSPECTOR-006 | 2026-09-24 | Qoder Agent | PASS | | 点击前 `values.content` 为 `submit count = 0`；POST 返回 200 且响应体逐字为 `{"action":"click","status":"ok","widget_path":"3"}`；再读为 `submit count = 1`，较初值恰增 1 | | 索引 3→4 的因果跨请求可观测，说明合成点击确实派发到真实应用主循环并触发了 `on_click`，而非进程内直调计数 |
| TC-INSPECTOR-007 | 2026-09-24 | Qoder Agent | PASS | | POST `/api/patch` 返回 200 且响应体为 `{"ops":1,"status":"ok"}`；`GET /api/widget/0` 的 `values.content` 变为 `Patched title`，`descriptor.name` 仍为 `Text` | | 补丁路径 `/0/content` 与索引路径 `0` 同源，已核对生效 |
| TC-INSPECTOR-008 | 2026-09-24 | Qoder Agent | PASS | | HTTP 200；响应体为 JSON 数组、124542 字节（约 124 KB）；元素恰 73 个，首个 `type` 为 `Badge`；73 个元素全部含 `type`/`children_policy`/`container`/`default_props`/`dynamic_children`/`events`/`examples`/`is_clickable` 八键 | | 以脚本清点键交集判定「每个元素含」，避免抽样误判；部分元素另有 `invariants` 等附加键，属超集不构成偏差 |
| TC-INSPECTOR-009 | 2026-09-24 | Qoder Agent | PASS | | `/api/debug/state` 200：`available` true、`width` 620、`height` 420、`scale_factor` 1.5（本机 144 DPI）、`should_close` false、`has_native_window` true、`frame_count` 1；`pick?x=10&y=10` 200 且 `hit` true、`chain` 首元素 `Column`（`bounds` 620×194）其后 `Text`；`pick?x=300&y=200` 为 `{"chain":[],"hit":false}`；`perf` 含 `avg_frame_ms`/`fps`/`dropped_frames`/`frame_budget_ms`/`p50_ms`/`p99_ms`，`timeline` 含 `avg_layout_ms`/`avg_paint_ms`/`avg_present_ms`，`why` 为 `{"count":59,"entries":[...]}`，`diagnostics` 为 `{"count":0,"diagnostics":[]}`；POST `flags` 200、`status` ok、五字段齐全且 `layout_guides` 为 true；GET `flags` 405、错误体逐字一致 | | 本用例预期结果 2 原文写 `"height":720`、`"scale_factor":1.0`，与本文 §1.2 声明的客户区 620×420 自相矛盾，且 `scale_factor` 属本机 DPI 而非产品常量（`width`/`height` 取自 `Surface::size()` 的逻辑客户区尺寸、`scale_factor()` 取自系统缩放，见 `src/aurora/debug/debug_backend.cpp` 的 `to_json`），故按运行时订正该条预期而非判 FAIL；`layout_guides` 已在取证后复位为 false，免污染 010 类截图 |
| TC-INSPECTOR-010 | 2026-09-24 | Qoder Agent | PASS | | HTTP 200、`Content-Type: image/png`、下载 2344473 字节（约 2.34 MB）；文件头 8 字节为 `89 50 4e 47 0d 0a 1a 0a`，尾部含 `IEND`，`IHDR` 尺寸 930×630（即 620×420 按 1.5 缩放）；图像为窗口当前帧真实内容——`User info form`、`Enter name` 占位、`alice@example.com`、蓝色 `Submit`、`submit count = 0` 五项均可辨认，非全黑全白；另以 `?source=win` 取屏幕窗口截图对照，同为 200 PNG（2613237 字节、952×686，含窗口边框） | | 内容判读用图片查看器（读取 PNG 后目视），非仅凭文件签名推断 |
| TC-INSPECTOR-011 | 2026-09-24 | Qoder Agent | PASS | | 六条逐项命中：GET `/api/input/click` → 405 `Method not allowed for /api/input`；POST `/api/input/text` 空体 → 400 `Missing or invalid 'path' (tree index path string, e.g. "0/1"; empty string targets the root)`；`/api/widget/9` → 404 `Widget not found at path: 9`；`/api/nope` → 404 `Unknown endpoint: /api/nope`；`/api/to_code` → 405 `Method not allowed for /api/to_code`；带 `Host: example.com` 的 `/api/tree` → 403 `Forbidden: unexpected Host header`；错误体与文档逐字一致 | | 非回环 Host 被拒即回环安全边界在真实请求上生效；服务端未做 DNS 解析，仅比对 Host 字面 |
| TC-INSPECTOR-012 | 2026-09-24 | Qoder Agent | PASS | | 第 2 实例 `err2.txt` 恰 2 行且均为 `ERR`：`bind() failed on port 6280, error: 10048`（`inspector_server.cpp`）与 `[inspector] failed to start on port 6280`（载体），进程随即退出（实测退出码 127，即 `main` 返回 -1）；第 1 实例随后 `/api/tree` 仍返回 200、2320 字节、根 `Column` 带 5 个子节点 | | 第 1 实例沿用 TC-INSPECTOR-001 启动者、全程未关闭且其间无任何写请求，故其树仍为初始态；端口占用另以 `netstat` 复核仅一个 LISTENING 属主 |
