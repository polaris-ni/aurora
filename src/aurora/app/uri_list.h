#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aurora::uri_list {

/// @brief 把本地路径清单编码为 `text/uri-list` 载荷。
///
/// 每条路径产出一行 `file://` + 百分号编码后的路径，行以 CRLF 结尾（含末行），空路径不产出条目。
/// 入参**只接受本地路径**：URL 类内容（`http://` 等）不是本地路径，调用方应改走 `set_payload`
/// 直接给原始字节，不要塞进本函数——本函数不会把入参当 URI 解析，只会把它当路径编码。
///
/// 编码字符集：字母数字与 `-._~/:` 原样保留，其余字节（含空格、`%`、`#`、`?`、`\` 与 UTF-8 中文的
/// 多字节序列）一律 `%XX` 大写十六进制。保留 `\` 需编码正是为了与解码构成**逐字节逆变换**——
/// 编码侧不做任何路径分隔符归一化，解码侧也就无需反向猜测。
/// @param paths 本地路径清单（UTF-8）；空串条目被跳过。
/// @return `text/uri-list` 载荷文本；清单为空时为空串。
[[nodiscard]] auto encode(const std::vector<std::string> &paths) -> std::string;

/// @brief 把 `text/uri-list` 载荷解码为本地路径清单。
///
/// 行切分容错 CRLF / LF / CR 三种分隔符并忽略空行（末行的行尾分隔符因此不产生幽灵条目）。
/// `file://` 开头的行（scheme 大小写不敏感）取其后全部内容百分号解码为本地路径；**其余 scheme 的行
/// （`http://` 之类）不映射为路径、静默跳过**——调用方需要完整原始行时自行持有载荷文本，本函数
/// 只回答「哪些条目能落成文件」。全部条目都被跳过时返回空清单：空内容不是失败。
///
/// 解码出口另有一道结构性闸门：带内嵌 NUL / 控制字节（含百分号编码后现形的 CRLF）、独立 `..`
/// 段、空条目或非空 authority 形态的条目按同一口径静默跳过；反斜杠、盘符与 UTF-8 多字节序列
/// 属合法路径字符，照常返回，故 `decode(encode(paths)) == paths` 的逐字节往返契约不变。
/// **过滤不等于授权**：本函数不触碰文件系统，也不校验路径是否落在允许的目录内；符号链接、
/// 允许目录、权限与「打开前是否复用为 URI」都由调用方负责。
/// @param payload `text/uri-list` 载荷文本（UTF-8 字节流，可含任意平台的分隔符）。
/// @return 结构性合法、可映射为本地路径的条目清单（按载荷中的出现顺序）；无路径行时为空清单。
[[nodiscard]] auto decode(std::string_view payload) -> std::vector<std::string>;

}  // namespace aurora::uri_list
