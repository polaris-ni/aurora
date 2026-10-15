#pragma once

/// @brief HTTP/1.1 发起侧的流式接收面：`DataSink` 抽象、逐段交付与 `StreamOutcome` 完成事实（CHG-018 §6 / §12）。
/// @file net/http_stream.h
///
/// 本头只回答一个问题：**「应答体还在路上时，把字节一段段交给宿主，并且整条消息不必同时存在于内存里」**。
/// 物化路径（`Client::send`）把消息体收进 `Response::body()` 才交出；本头是它的对偶：分帧解出的字节每到手
/// 一段就写进 `DataSink`，透明解压同样以流式串联在交付之前完成，峰值内存与载荷总量无关。大文件下载、边下
/// 边显示的进度面、超过可用内存的载荷走这条路。
///
/// 四条形态约束（规范性，落码与文档回写以此为准）：
/// - **完成事实是 `StreamOutcome`，不是 `Response`**：流式路径的体不在库里留存，一份带体的 `Response` 只会
///   让调用方误判「再存一份就拿到了全文」。本值类型交出的恒是**头部块 + 三条字节 / 停读事实**，与 `DataSink`
///   已经收到的段数可对照。
/// - **接收侧是抽象基类，不是三枚 `std::function`**：`on_head` / `on_data` / `on_end` 要共享的是同一个宿主
///   对象的状态（打开的文件句柄、已累计的偏移、要不要中途收摊），拆成三个回调就得让调用方自己把那份状态缝
///   三次；且生命周期由宿主对象表达——`open_stream` 的协程帧按引用持有 Sink，其有效期必须覆盖到协程收尾。
/// - **`SinkFlow::Stop` 是正常结局而不是失败**：调用方主动收摊（够了、用户取消了、校验和不符）时，本次交换
///   依然成功——它拿到了一份完整头部与「已交付多少」的事实，失败只用于「本库做不到」。故停读走
///   `StreamOutcome::stopped_by_sink`，不落成取消类错误码。
/// - **线上字节不设限，解压产物照旧设限**：`Client::max_body_bytes()` 是物化路径的内存闸，流式路径不物化，
///   没有内存可闸（设了它也不会让 `open_stream` 提前失败，那只会把「收大文件」这件事变成必须调大上限）；
///   `Client::max_decompressed_bytes()` 管的仍是解压炸弹，流式路径逐层照它限，越限落 `http-body-too-large`。
///
/// 内容与重定向语义与 `send` 同源：`Accept-Encoding` 缺省宣告同一支持集、编码链反序逐层还原、按 `Location`
/// 跟随重定向（`open_stream` 在头部判定为重定向后即弃该跳消息体，不为一跳注定被丢弃的载荷继续读字节）、
/// 三档超时与进度节流同判据。`deflate` 的两型歧义在流式路径按**包装头判定**而非「先试再回落」——增量交付
/// 无法重播已交出的字节，判据见 `Client::open_stream()` 的注释。
///
/// @code
/// struct Downloader final : aurora::net::http::DataSink {
///     auto on_data(std::span<const char> body) -> aurora::net::http::SinkFlow {
///         write(handle_, body);            // 边到边写：全程不物化
///         return keep_going ? aurora::net::http::SinkFlow::Continue
///                           : aurora::net::http::SinkFlow::Stop;
///     }
/// };
/// Downloader sink;
/// auto task = client.open_stream(aurora::net::http::Request::get("https://example.com/big.bin"), sink);
/// // 宿主帧内泵至 task.is_done()，再读 task.result() 得 Result<StreamOutcome>
/// @endcode
/// @note Thread: `DataSink` 的三个入口恒在主线程同步调用（续体经主线程投递器恢复）；实现方可在其中触碰
/// 响应式状态，但不得阻塞事件循环
/// @note Side-effects: none（本头只定义类型；收发在 `Client::open_stream()`）

#include <cstdint>
#include <span>

#include "aurora/net/http.h"

namespace aurora::net::http {

/// @brief 宿主对「还要不要继续收」的答复（`DataSink` 的两个可判定入口共用）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
enum class SinkFlow : std::uint8_t {
    Continue,  ///< 本段已收下，继续交付下一段
    Stop,  ///< 就此收摊：剩余字节不再读取、也不再交付，本次交换以成功结局返回
};

/// @brief 流式接收侧的宿主对象：逐段收下应答字节并回报是否继续。
///
/// 一次 `open_stream` 对应一个实例（由调用方持有，生命周期须覆盖协程收尾）。三个入口的调用次序恒为
/// `on_head`（一次）→ `on_data`（零次或多次）→ `on_end`（恰一次）；失败结局**不**调 `on_end`——那时字节流
/// 断在半途，收尾该由调用方按 `Result` 分流，而不是库替它假装「收完了」。
/// @note Thread: 三个入口恒在主线程同步调用，且由同一条协程串行驱动（不并发、不重入）
/// @note Side-effects: 由实现方决定（写文件、更新信号等）；本库只约定调用时机，不干预实现内容
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，按值拷贝 / 移动无意义
class DataSink {
  public:
    /// @brief 虚析构（default）：保证经基类指针销毁派生的接收侧宿主，本类自身不持有资源。
    virtual ~DataSink() = default;

    /// @brief 应答头部块已定（状态码与字段可读、消息体尚未开始）时调用一次。
    ///
    /// 形参的 `body()` 恒为空：体的字节属于 `on_data`。`head` 是本次调用的只借用，需要留用请自行拷贝分量。
    /// 回 `Stop` 即整条消息都不再读取，`on_data` 一发都不会有。
    /// @param head 已装配的头部块（状态 / 原因 / 字段）。
    /// @return 是否继续交付。
    virtual auto on_head(const Response &head) -> SinkFlow {
        (void)head;  // 缺省实现只关心体，头部是观测口而非判定口
        return SinkFlow::Continue;
    }

    /// @brief 消息体的一段到达时调用（零次或多次；恒非空）。
    ///
    /// 交出的是**已按 `Content-Encoding` 还原**的字节（声明了编码却没编入解压能力的应答根本走不到这里）。
    /// 段的切分由分帧边界、读缓冲与解码器内部窗口共同决定，不是一段的应用语义单元；需要按行或按记录切分
    /// 请宿主自己缓冲残余（本库不猜载荷的分隔形态）。`body` 只在本次调用内有效。
    /// @param body 本段明文字节。
    /// @return 是否继续交付。
    virtual auto on_data(std::span<const char> body) -> SinkFlow = 0;

    /// @brief 本次交付正常结束（体读全，或宿主 `Stop` 提前收摊）时调用一次。
    ///
    /// 它是「把缓冲刷下去、把句柄关掉」的时机：完整还是停读由 `StreamOutcome` 表达，本入口不重复那个判定。
    virtual auto on_end() -> void {}
};

/// @brief 一次流式交换的完成事实（`Client::open_stream()` 的成功出口值）。
///
/// 两枚字节数是**不同口径**且刻意并存：`wire_bytes` 是分帧解出后线上真到达的字节（透明解压前），
/// `delivered_bytes` 是实际交给 `on_data` 的字节（解压后）。无编码环节时二者相等；有环节时后者是前者的
/// 还原量，进度回调只能报前者（见 `Progress`）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct StreamOutcome {
    Response response;  ///< 最终一跳的头部块；`body()` 恒为空——体的字节已逐段交给 `DataSink`
    std::uint64_t wire_bytes{0};  ///< 分帧解出的线上字节累计（跨全部跳；解压前口径）
    std::uint64_t delivered_bytes{0};  ///< 交给 `DataSink::on_data` 的字节累计（解压后口径）
    bool stopped_by_sink{false};  ///< 是否因宿主回 `SinkFlow::Stop` 而提前收摊（成功结局的一种，不是失败）
};

}  // namespace aurora::net::http
