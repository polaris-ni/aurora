# URL 黄金语料（`Url::parse` / `Url::resolve`）

由 `tests/unit/utest_core_url.cpp` 的两个语料驱动用例读取：

| 文件 | 列 | 判据 |
|:---|:---|:---|
| `parse.tsv` | 输入文本 → 期望 `to_string()`（`ERR` = 必须以 `net-url-invalid` 拒绝） | 解析即规范化，且规范化结果再解析必须逐分量同色（幂等） |
| `resolve.tsv` | 引用 → 解析后的绝对 URL 文本 | 基址固定为 `http://a/b/c/d;p?q`（RFC 3986 §5.4 的场景基址） |

格式：UTF-8、制表符分隔、`#` 开头行与空行跳过、按字节比较（语料含裸 UTF-8 与 `%` 三元组，任何文本模式转换都会破坏判据）。每行必须恰好两列，缺列或多列直接判红而不是被静默忽略。

## 来源（可核）

- **RFC 3986 §5.4**：`resolve.tsv` 的「正常」与「异常」两张示例表逐条照录（含 `http:g` 这类「带 scheme 的引用整体胜出、基址不参与」的口径）。
- **RFC 3986 §6.2.2 / §6.2.3**：语法规范化用例（scheme 与主机小写、`%41` → `A` 这类无保留还原、`%2F` **不**还原、三元组大写化、端口前导零去除、空路径保留）。
- **RFC 3986 §3.2.2 / §7.4**：主机三型判定与 IPv4 点分四段（禁前导零、非四段的 `1.2.3` 按注册名，不做 `inet_aton` 式缩写）。
- **RFC 3986 §4.2**：冒号与定界符的先后决定整段是不是 scheme（`g:x` → scheme `g`；`../g:x`、`./g:x` → 冒号留在路径里）。
- **RFC 3490 / WHATWG URL**：主机尾点（DNS 根标签）视为同一名字的另一种拼写，`example.com` 与 `example.com.` 判等。
- **RFC 3492 §6.1–§6.3**：punycode 的编码/解码与 `adapt` 阈值函数。`parse.tsv` 里的多次插入向量（`naïve-café` → `xn--nave-caf-i1a7c`、`送别到東京` → `xn--1lqz8fpa878sml0c`、`αβγδεπανζ` → `xn--mxaacefgi9cza`）卡住 §6.3 三个极易抄错的分支：`adapt` 里「首次除 `damp`、此后除 2」、阈值比较用 `tmax` 而不是码点数、以及**每轮 while 末尾的 `delta++` / `n++` 且插入后不中断本轮扫描**——漏掉最后这条时 `日本語` 依然编得对，更长的串才露馅。

比对基准（本机可复现）：Node 的 `url.domainToASCII` 与 CPython 的 `encodings/punycode.py`。`parse.tsv` 每条 IDN 行都同时与这两个 oracle 逐字符对齐过，唯一的成体系差异见下面第 1 条口径。

## 本库口径（RFC 沉默或分歧处，均已裁决并写进 `codespec/specification/01-core.md` §11）

1. **不做 Unicode 归一化**（无 NFC/NFD、无 Unicode 大小写折叠）：主机只做 ASCII 小写 + punycode。后果分两面看：`ß` 按码点编码仍是 `xn--zca`，与浏览器 nontransitional 处理同色（`node -e "require('url').domainToASCII('ß.test')"` 即此值）；但 Latin-1 大写字母不折叠，`MÜNCHEN.test` 得 `xn--mnchen-psa.test`，而浏览器先按 UTS #46 把 `Ü` 折成 `ü` 再编码，得 `xn--mnchen-3ya.test`——语料里这两行成对存在，就是为了把这个差异钉成事实而不是传说。取舍理由：归一化表随 Unicode 版本变动，而本库要求各平台逐值同色，代价是极少数含大写字母的非 ASCII 域名与浏览器判等不一致。
2. **主机段拒绝百分号转义**（`http://m%C3%BCnchen.test/` → `ERR`）：转义在 DNS 上没有 1:1 对应，且 `%2e%2e` 式写法会被利用做前缀仿冒。
3. **点段消除只在绝对 URL 上做**：`../g` 这类引用原样保留点段，`resolve` 在合并后才消除一次——否则「相对引用解析结果」与「引用本身」会被混为同一形态。
4. **`%2e%2e` / `%2e` 在解析时先还原成 `.` / `..` 再走点段消除**，因此 `http://a/b/c/%2e%2e/g` → `http://a/b/g`，与 WHATWG 同色；`%2F` 永不还原成 `/`，故它不会变成新的分隔符。
5. **控制字节与空格一律拒绝**（`detail=` 带偏移），不做「浏览器式自动补 `%20`」：静默改写会把用户手抖的空格变成对端可见的资源身份。
6. **`+` 不等于空格**：`?q=a+b` 解码为 `a+b`。form-urlencoding 的历史约定不外溢到 URL 层。

## 加用例

直接追加一行（制表符分隔，两列），必要时同步在 `codespec/specification/01-core.md` §11 补一句口径。改的是数据不是代码：`utest_core_url` 的两个语料用例按行数下限（`parse ≥ 70`、`resolve ≥ 40`）防止文件被截空，具体判据逐行独立报告，一条失配不会遮住其余各行。
