/* 光标形状 —— macOS 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 验收范围（务必读清，避免过度解读结果）：
//   ✅ 本探针验收的是 `aurora::MacOSSurface::set_cursor(CursorShape)` 的**接线正确性**：
//      11 个语义形状是否各自派发到文档所声明的 `NSCursor` 单例。
//   ❌ 本探针**不**验收 macOS 后端的窗口/上屏路径——`src/aurora/window/macos_surface.cpp`
//      的 `create_window()` / `present()` 目前仍是骨架 TODO（见该文件头部注记）。因此本
//      探针不需要可见窗口，也不需要事件循环：它只读回「`set` 之后当前 `NSCursor` 是哪个」。
//
// 原理：
//   1) 建 `MacOSSurface`（构造在 macOS 上恒成功，不依赖窗口是否真创建）；
//   2) 对 11 个 `CursorShape` 逐个 `set_cursor`；
//   3) 读回 `[NSCursor currentCursor]`，与本探针按同一映射表取得的期望单例做**指针同一性**
//      比对（`NSCursor` 类方法返回进程级单例，故指针相等即语义相等）。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_MACOS=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_macos_cursor
//   ./build-verify/aurora_verify_macos_cursor
// 构建（方式 ② clang++ 直编，须先有一个已构建好的 macOS 后端构建目录 build-macos）：
//   clang++ -std=c++20 -ObjC++ -fobjc-arc -DAURORA_BACKEND_MACOS -DAURORA_BACKEND_HEADLESS \
//     -I include -I src -I third_party -I tools/verify \
//     -I build-macos/third_party/freetype/include -I third_party/harfbuzz/src \
//     tools/verify/macos_cursor_live_probe.mm -o macos_cursor_live_probe \
//     build-macos/libaurora.a build-macos/third_party/harfbuzz/libharfbuzz.a \
//     build-macos/third_party/freetype/libfreetype.a \
//     -framework Cocoa -framework CoreGraphics -framework CoreText -lz -lpthread -lm
//   ./macos_cursor_live_probe
//   （若链接报缺符号/框架，按 `cmake --build build-macos --verbose` 打印的实际链接行补齐。）
//
// 退出码：
//   0  11 个形状读回皆等于其期望单例 —— 真机验收通过
//   4  读回恒为同一光标（`set_cursor` 未生效）
//   5  部分形状读回与期望不符 —— 见逐行表格 match 列
//   6  环境判据不适用：AppKit 未返回单例（本探针的指针同一性判据失效）→ 请改人工目视
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_macos_cursor 只能在 macOS 上构建（AURORA_PLATFORM_MACOS）"
#endif
#if !defined(AURORA_BACKEND_MACOS)
#error "须开启 AURORA_BACKEND_MACOS"
#endif

#include "aurora/window/macos_surface.h"

#import <AppKit/AppKit.h>

#include <string>

#include "aurora/window/cursor_map.h"
#include "verify_print.h"

namespace {

/// 期望映射表：与 `src/aurora/window/macos_surface.cpp` 的 `MacOSSurface::set_cursor` 逐项对齐。
/// 探针的逐行比对（match 列）即「实现与文档声明是否一致」的漂移检测。
///
/// 注意 `ResizeNWSE` / `ResizeNESW`：macOS **没有**公开的对角缩放光标（仅有 private
/// `_windowResize*` 系列），后端文档声明二者回退 `arrowCursor`；故本表同样映射到箭头，
/// 这也使得「11 个形状只应出现 9 个互异单例」成为可断言的期望值。
/// `busyButClickableCursor` 与后端保持一致直接取用（该 selector 自 macOS 10.14 提供，
/// 后端未加可用性守卫）。
auto expected_ns_cursor(aurora::CursorShape shape) -> NSCursor * {
    switch (shape) {
        case aurora::CursorShape::Arrow:
            return [NSCursor arrowCursor];
        case aurora::CursorShape::IBeam:
            return [NSCursor IBeamCursor];
        case aurora::CursorShape::PointingHand:
            return [NSCursor pointingHandCursor];
        case aurora::CursorShape::ResizeNS:
            return [NSCursor resizeUpDownCursor];
        case aurora::CursorShape::ResizeEW:
            return [NSCursor resizeLeftRightCursor];
        case aurora::CursorShape::ResizeNWSE:
        case aurora::CursorShape::ResizeNESW:
            return [NSCursor arrowCursor];  // 无公开对角缩放光标 → 回退箭头
        case aurora::CursorShape::Move:
            return [NSCursor openHandCursor];
        case aurora::CursorShape::Crosshair:
            return [NSCursor crosshairCursor];
        case aurora::CursorShape::NotAllowed:
            return [NSCursor operationNotAllowedCursor];
        case aurora::CursorShape::Wait:
            return [NSCursor busyButClickableCursor];
    }
    return nullptr;
}

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// ObjC 对象 → 裸指针（仅供打印；不改所有权）。ARC 下须用 `__bridge`、非 ARC 下该关键字
/// 不可用，故按 `__has_feature(objc_arc)` 三分支给出，避免「返回后仍有可达代码」的死分支。
inline auto raw_ptr(id object) -> const void * {
#if defined(__has_feature)
#if __has_feature(objc_arc)
    return (__bridge const void *)object;
#else
    return (const void *)object;
#endif
#else
    return (const void *)object;
#endif
}

}  // namespace

auto main() -> int {
    int rc = 0;
    @autoreleasepool {
        (void)[NSApplication sharedApplication];  // 初始化 AppKit 与窗口服务器连接

        // ---- 前置自检：本探针的判据（类方法返回进程级单例）是否成立 ----
        // 若 AppKit 每次返回新实例，则「指针同一性」判据失效，此时如实报告 env 不适用，
        // 而不是给出满屏假失败。
        NSCursor *probe_a = [NSCursor arrowCursor];
        NSCursor *probe_b = [NSCursor arrowCursor];
        if (probe_a == nil || probe_a != probe_b) {
            AURORA_LOG_ERROR("verify", "环境判据不适用：AppKit 未返回 NSCursor 单例（指针同一性失效）");
            rc = 6;
        } else {
            aurora::MacOSSurface surface(240, 160, "aurora-verify-i1-cursor-macos");
            const int total = static_cast<int>(aurora::kCursorShapeCount);

            emit(aurora_verify::pad_right("#", 3) + aurora_verify::pad_right("shape(rfc name)", 20) +
                 aurora_verify::pad_right("expect@NSCursor", 20) + aurora_verify::pad_right("readback@current", 20) +
                 "match");

            int hits = 0;
            int distinct = 0;
            NSCursor *previous = nil;
            for (int i = 0; i < total; ++i) {
                const auto shape = static_cast<aurora::CursorShape>(i);
                NSCursor *want = expected_ns_cursor(shape);
                surface.set_cursor(shape);
                NSCursor *got = [NSCursor currentCursor];
                const bool match = (got != nil && got == want);
                if (match) {
                    ++hits;
                }
                if (i == 0 || got != previous) {
                    ++distinct;
                }
                previous = got;

                emit(aurora_verify::pad_right(aurora_verify::format_int(i), 3) +
                     aurora_verify::pad_right(aurora::cursor_rfc_name(shape), 20) +
                     aurora_verify::pad_right(aurora_verify::format_handle(raw_ptr(want)), 20) +
                     aurora_verify::pad_right(aurora_verify::format_handle(raw_ptr(got)), 20) +
                     (match ? "YES" : "no"));
            }

            emit(std::string("命中 ") + aurora_verify::format_int(hits) + "/" + aurora_verify::format_int(total) +
                 "，读回互异 " + aurora_verify::format_int(distinct) + "/" + aurora_verify::format_int(total) +
                 "（期望互异 9：ResizeNWSE/NESW 与 Arrow 同为箭头）");

            if (distinct <= 1) {
                AURORA_LOG_ERROR("verify", "读回恒为同一光标 —— set_cursor 未生效（FAIL 4）");
                rc = 4;
            } else if (hits != total) {
                AURORA_LOG_ERROR("verify", "部分形状读回与期望单例不符（FAIL 5）");
                rc = 5;
            } else {
                emit("PASS: 11 个 CursorShape 皆派发到文档声明的 NSCursor 单例");
            }
        }
    }
    return rc;
}
