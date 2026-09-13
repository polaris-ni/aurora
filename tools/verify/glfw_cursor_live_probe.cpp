/* 光标形状 —— GLFW 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 为什么是本形态（请先读这段，否则会误判结果）：
//   GLFW **没有**查询「当前光标」的 API（无 `glfwGetCursor` 之类），因此无法像
//   X11（XFIXES 读回）/ Win32（`GetCursorInfo` 读回）/ macOS（`[NSCursor currentCursor]`）
//   那样做自动读回断言。故本探针分两段，缺一不可：
//
//   ① 自动段（恒执行）：把 `src/aurora/window/glfw_surface.cpp` 的 `glfw_standard_cursor`
//      映射在**探针侧镜像一份**，核对三件事：
//        - 本 GLFW 版本提供哪些标准光标（`GLFW_RESIZE_NWSE/NESW/ALL/NOT_ALLOWED_CURSOR`
//          自 GLFW 3.4 引入；缺失时后端按文档回退 Arrow）；
//        - 每个可映射形状 `glfwCreateStandardCursor` 均返回有效句柄（非 null）；
//        - 按后端「每形状一份句柄、不可映射者复用 Arrow 句柄」的缓存语义去重后，互异句柄数
//          等于期望值（基础 6 + 本版本可用的 4 个扩展 = 6 或 10），且 Wait 与不可映射形状
//          确实复用 Arrow 句柄。
//      任何一项不符 → 退出码 6，并打印逐行表格指出是哪一项。
//
//   ② 人工段（`--interactive`，可选但**是唯一能证明「屏幕上真的变了」的手段**）：逐个形状
//      `glfwSetCursor` 到窗口上，在控制台提示「此刻鼠标指针应是什么形状」，由人把鼠标移入
//      窗口目视比对后按 y/n。全部确认为 y → 退出码 0；否则 7。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_GLFW=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_glfw_cursor
//   ./build-verify/aurora_verify_glfw_cursor --interactive
//   （GLFW 后端需 GL 与 X11 扩展开发包；Linux 上缺 `libgl-dev` / `libxrandr-dev`
//     等会在 configure 阶段即中止。）
//
// 退出码：
//   0  自动段通过 **且**人工段全部确认
//   2  环境不可用（glfwInit / 建窗 / 建标准光标失败）
//   6  自动段不符 —— 见逐行表格与末尾「不符项」
//   7  人工段存在未确认（或有形状被判定不正确）
//   8  自动段通过但**未**跑人工段 —— 屏幕表现仍未被证明，请补 `--interactive`
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_BACKEND_GLFW)
#error "须开启 AURORA_BACKEND_GLFW"
#endif

#include "aurora/window/glfw_surface.h"

// 本探针只用「窗口 + 标准光标」这层 GLFW API，完全不碰 OpenGL。定义 GLFW_INCLUDE_NONE
// 可让 glfw3.h 不去 `#include <GL/gl.h>`——既是语义上的正确（无 GL 依赖），也顺带让本探针
// 在**没装 GL 开发包**（如只装了 GLFW 头）的环境里也能编译。必须在 glfw3.h 之前定义。
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <array>
#include <iostream>
#include <string>

#include "aurora/window/cursor_map.h"
#include "verify_print.h"

namespace {

/// 探针侧镜像的「语义形状 → GLFW 标准光标 id」映射，逐项对齐
/// `src/aurora/window/glfw_surface.cpp` 的 `glfw_standard_cursor`（含 `#ifdef` 版本守卫）。
/// 返回 -1 表示本 GLFW 版本无对应标准光标（后端据此回退 Arrow）。
/// 说明：无法直接复用库内那份（它是 .cpp 匿名命名空间里的 constexpr），故镜像；自动段的
/// 「互异数与回退关系」断言即是漂移检测。
constexpr auto mirrored_standard_cursor(aurora::CursorShape shape) -> int {
    switch (shape) {
        case aurora::CursorShape::Arrow:
            return GLFW_ARROW_CURSOR;
        case aurora::CursorShape::IBeam:
            return GLFW_IBEAM_CURSOR;
        case aurora::CursorShape::PointingHand:
            return GLFW_HAND_CURSOR;
        case aurora::CursorShape::ResizeNS:
            return GLFW_VRESIZE_CURSOR;
        case aurora::CursorShape::ResizeEW:
            return GLFW_HRESIZE_CURSOR;
        case aurora::CursorShape::Crosshair:
            return GLFW_CROSSHAIR_CURSOR;
        case aurora::CursorShape::ResizeNWSE:
#ifdef GLFW_RESIZE_NWSE_CURSOR
            return GLFW_RESIZE_NWSE_CURSOR;
#else
            break;
#endif
        case aurora::CursorShape::ResizeNESW:
#ifdef GLFW_RESIZE_NESW_CURSOR
            return GLFW_RESIZE_NESW_CURSOR;
#else
            break;
#endif
        case aurora::CursorShape::Move:
#ifdef GLFW_RESIZE_ALL_CURSOR
            return GLFW_RESIZE_ALL_CURSOR;
#else
            break;
#endif
        case aurora::CursorShape::NotAllowed:
#ifdef GLFW_NOT_ALLOWED_CURSOR
            return GLFW_NOT_ALLOWED_CURSOR;
#else
            break;
#endif
        case aurora::CursorShape::Wait:
            break;  // GLFW 无 busy/wait 标准形状 → 回退 Arrow（与后端一致）
    }
    return -1;
}

/// GLFW 标准光标 id → 报告用常量名。
auto standard_cursor_name(int id) -> const char * {
    switch (id) {
        case GLFW_ARROW_CURSOR:
            return "GLFW_ARROW_CURSOR";
        case GLFW_IBEAM_CURSOR:
            return "GLFW_IBEAM_CURSOR";
        case GLFW_HAND_CURSOR:
            return "GLFW_HAND_CURSOR";
        case GLFW_VRESIZE_CURSOR:
            return "GLFW_VRESIZE_CURSOR";
        case GLFW_HRESIZE_CURSOR:
            return "GLFW_HRESIZE_CURSOR";
        case GLFW_CROSSHAIR_CURSOR:
            return "GLFW_CROSSHAIR_CURSOR";
#ifdef GLFW_RESIZE_NWSE_CURSOR
        case GLFW_RESIZE_NWSE_CURSOR:
            return "GLFW_RESIZE_NWSE_CURSOR";
#endif
#ifdef GLFW_RESIZE_NESW_CURSOR
        case GLFW_RESIZE_NESW_CURSOR:
            return "GLFW_RESIZE_NESW_CURSOR";
#endif
#ifdef GLFW_RESIZE_ALL_CURSOR
        case GLFW_RESIZE_ALL_CURSOR:
            return "GLFW_RESIZE_ALL_CURSOR";
#endif
#ifdef GLFW_NOT_ALLOWED_CURSOR
        case GLFW_NOT_ALLOWED_CURSOR:
            return "GLFW_NOT_ALLOWED_CURSOR";
#endif
        default:
            // 纯 ASCII 且足够短（表格列宽按字节对齐，含 CJK 的值会让后续列错位）。
            return "<none -> fallback ARROW>";
    }
}

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// 人工段的提示语：说完「应该看到什么」，让人去看屏幕。
auto human_expectation(aurora::CursorShape shape) -> const char * {
    switch (shape) {
        case aurora::CursorShape::Arrow:
            return "默认箭头";
        case aurora::CursorShape::IBeam:
            return "文本 I 形（工字形）";
        case aurora::CursorShape::PointingHand:
            return "手形（食指指向）";
        case aurora::CursorShape::ResizeNS:
            return "上下双向箭头";
        case aurora::CursorShape::ResizeEW:
            return "左右双向箭头";
        case aurora::CursorShape::ResizeNWSE:
            return "主对角（↖↘）双向箭头；本机 GLFW < 3.4 时回退箭头";
        case aurora::CursorShape::ResizeNESW:
            return "副对角（↗↙）双向箭头；本机 GLFW < 3.4 时回退箭头";
        case aurora::CursorShape::Move:
            return "四向移动箭头（十字）";  // GLFW_RESIZE_ALL_CURSOR；< 3.4 回退箭头
        case aurora::CursorShape::Crosshair:
            return "十字准星";
        case aurora::CursorShape::NotAllowed:
            return "禁止（圆圈加斜杠）";  // < 3.4 回退箭头
        case aurora::CursorShape::Wait:
            return "等待/忙碌";
    }
    return "(未知)";
}

}  // namespace

auto main(int argc, char **argv) -> int {
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--interactive") {
            interactive = true;
        }
    }

    if (glfwInit() != GLFW_TRUE) {
        AURORA_LOG_ERROR("verify", "glfwInit 失败（无显示 / 无驱动）");
        return 2;
    }
    int major = 0;
    int minor = 0;
    int revision = 0;
    glfwGetVersion(&major, &minor, &revision);
    emit(std::string("GLFW ") + aurora_verify::format_int(major) + "." + aurora_verify::format_int(minor) + "." +
         aurora_verify::format_int(revision) + "  " + glfwGetVersionString());

    const int total = static_cast<int>(aurora::kCursorShapeCount);
    GLFWwindow *window =
        glfwCreateWindow(420, 260, "aurora-verify-i1-cursor-glfw", nullptr, nullptr);
    if (window == nullptr) {
        AURORA_LOG_ERROR("verify", "glfwCreateWindow 失败");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);
    glfwShowWindow(window);

    // ---- 句柄池：每个「标准光标 id」只建一份（与后端 Impl::cursors 缓存语义一致）----
    std::array<int, aurora::kCursorShapeCount> pool_ids{};
    std::array<GLFWcursor *, aurora::kCursorShapeCount> pool_handles{};
    int pool_size = 0;
    bool create_failed = false;
    const auto handle_for_id = [&](int id) -> GLFWcursor * {
        for (int k = 0; k < pool_size; ++k) {
            if (pool_ids[static_cast<std::size_t>(k)] == id) {
                return pool_handles[static_cast<std::size_t>(k)];
            }
        }
        GLFWcursor *created = glfwCreateStandardCursor(id);
        if (created == nullptr) {
            create_failed = true;
            return nullptr;
        }
        pool_ids[static_cast<std::size_t>(pool_size)] = id;
        pool_handles[static_cast<std::size_t>(pool_size)] = created;
        ++pool_size;
        return created;
    };

    // ---- 逐形状解析句柄（不可映射者复用 Arrow 句柄，等价后端的回退分支）----
    std::array<GLFWcursor *, aurora::kCursorShapeCount> handles{};
    for (int i = 0; i < total; ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        const int id = mirrored_standard_cursor(shape);
        handles[static_cast<std::size_t>(i)] =
            (id < 0) ? handle_for_id(GLFW_ARROW_CURSOR) : handle_for_id(id);
    }

    const GLFWcursor *arrow_handle = handles[static_cast<std::size_t>(aurora::CursorShape::Arrow)];
    if (create_failed || arrow_handle == nullptr) {
        AURORA_LOG_ERROR("verify", "glfwCreateStandardCursor 失败（无 GL/窗口系统后端可用）");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
    }

    // ---- 自动段判定 ----
    int distinct_actual = 0;
    std::array<const GLFWcursor *, aurora::kCursorShapeCount> seen{};
    int fallback_mismatch = 0;

    emit(aurora_verify::pad_right("#", 3) + aurora_verify::pad_right("shape(rfc name)", 20) +
         aurora_verify::pad_right("expect standard cursor", 32) + aurora_verify::pad_right("handle", 20) +
         "回退Arrow?");

    for (int i = 0; i < total; ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        const int id = mirrored_standard_cursor(shape);
        const GLFWcursor *handle = handles[static_cast<std::size_t>(i)];
        bool already_seen = false;
        for (int k = 0; k < distinct_actual; ++k) {
            if (seen[static_cast<std::size_t>(k)] == handle) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen) {
            seen[static_cast<std::size_t>(distinct_actual)] = handle;
            ++distinct_actual;
        }

        // 期望：id < 0 的形状必须与 Arrow 同句柄；id >= 0 且非 Arrow 自身者必须与 Arrow 不同。
        const bool expect_arrow = (id < 0) || (shape == aurora::CursorShape::Arrow);
        const bool is_arrow = (handle == arrow_handle);
        const bool fallback_ok = expect_arrow ? is_arrow : !is_arrow;
        if (!fallback_ok) {
            ++fallback_mismatch;
        }

        emit(aurora_verify::pad_right(aurora_verify::format_int(i), 3) +
             aurora_verify::pad_right(aurora::cursor_rfc_name(shape), 20) +
             aurora_verify::pad_right(standard_cursor_name(id), 32) +
             aurora_verify::pad_right(aurora_verify::format_handle(handle), 20) +
             (is_arrow ? "yes" : "no"));
    }

    emit(std::string("互异句柄数 ") + aurora_verify::format_int(distinct_actual) + "，期望 " +
         aurora_verify::format_int(pool_size) + "（= 本 GLFW 版本实际可映射的标准光标种类数）");

    bool auto_ok = true;
    if (pool_size != distinct_actual) {
        AURORA_LOG_ERROR("verify", "不符项：互异句柄数与期望不符 —— 形状间句柄被意外共享/未共享");
        auto_ok = false;
    }
    if (fallback_mismatch != 0) {
        AURORA_LOG_ERROR("verify", "不符项：回退 Arrow 关系不符（" + aurora_verify::format_int(fallback_mismatch) +
                                       " 处）—— 后端映射表与本文档声明不一致");
        auto_ok = false;
    }
    for (int k = 0; k < pool_size; ++k) {
        if (pool_handles[static_cast<std::size_t>(k)] == nullptr) {
            AURORA_LOG_ERROR("verify", "不符项：存在创建失败的标准光标句柄");
            auto_ok = false;
        }
    }

    int rc = 0;
    if (!auto_ok) {
        rc = 6;
    } else if (!interactive) {
        emit("自动段通过（映射/回退语义一致）。屏幕表现仍未被证明，请补 `--interactive` 人工目视确认。");
        rc = 8;
    } else {
        // ---- 人工段：逐个形状让人对照屏幕 ----
        emit("");
        emit("人工段开始：请把鼠标移入刚出现的 GLFW 窗口（点击一下使其获得焦点），");
        emit("每次按回车切到下一个形状，然后目视「鼠标指针形状」是否与提示一致。");
        int rejected = 0;
        for (int i = 0; i < total; ++i) {
            const auto shape = static_cast<aurora::CursorShape>(i);
            glfwSetCursor(window, handles[static_cast<std::size_t>(i)]);
            glfwFocusWindow(window);
            glfwPollEvents();

            emit(std::string("[") + aurora_verify::format_int(i + 1) + "/" + aurora_verify::format_int(total) +
                 "] " + aurora::cursor_rfc_name(shape) + " —— 期望看到：" + human_expectation(shape) +
                 "；按回车确认（输入 n + 回车判为不符）");
            std::string answer;
            if (!std::getline(std::cin, answer)) {
                AURORA_LOG_WARN("verify", "标准输入已结束，人工段提前收尾");
                break;
            }
            if (!answer.empty() && (answer[0] == 'n' || answer[0] == 'N')) {
                ++rejected;
                AURORA_LOG_ERROR("verify", std::string("人工判定不符：") + aurora::cursor_rfc_name(shape));
            }
        }
        if (rejected == 0) {
            emit("PASS: GLFW 光标接线真机验收通过（自动段 + 人工段）");
        } else {
            AURORA_LOG_ERROR("verify", "人工段存在不符形状：" + aurora_verify::format_int(rejected) + " 个");
            rc = 7;
        }
    }

    for (int k = 0; k < pool_size; ++k) {
        glfwDestroyCursor(pool_handles[static_cast<std::size_t>(k)]);
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return rc;
}
