// 场景枚举与渲染小工具（examples/demos/scene_tool.cpp）。
//
// 场景注册表（scenes/scene_registry.h）的 CLI 面：
//   scene_tool --list                            列出全部场景（id / 画布 / 来源 demo）
//   scene_tool --render <id> <out.png> [w h]     以 HeadlessSurface 软件路径渲染场景为 PNG
//                                                （缺省尺寸取注册表推荐值）
//
// 用途：E2E golden 基线的软件 SSOT 生成、场景内容人工核对、demo 抽取改造的渲染结果一致性
// 抽查。与 demo 同被 CMake GLOB 成目标（EXCLUDE_FROM_ALL：--target scene_tool 单建，
// --target demos 随全量 demo 一起建）；不参与默认构建。
// 退出码：0 成功；1 运行失败（渲染出错）；2 用法错误（未知场景 / 参数缺失）。
#include <stdexcept>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "scenes/scene_registry.h"

namespace {

auto print_usage() -> void {
    AURORA_LOG_RAW("scene_tool",
                   "usage:\n"
                   "  scene_tool --list\n"
                   "  scene_tool --render <id> <out.png> [width height]\n");
}

[[nodiscard]] auto find_scene(const std::string &id) -> const aurora::demo_scenes::SceneEntry * {
    for (const auto &entry : aurora::demo_scenes::scene_registry()) {
        if (id == entry.id) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char *argv[]) -> int {
    // argv 指针算术：工具入口的参数收集，与 aurora_cli.cpp 同口径豁免（Es.49 边界例外）。
    // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
    const std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    // NOLINTEND(*-pro-bounds-pointer-arithmetic)

    if (args.empty()) {
        print_usage();
        return 2;
    }
    if (args[0] == "--help") {
        print_usage();
        return 0;
    }

    if (args[0] == "--list") {
        AURORA_LOG_RAW("scene_tool", "id | canvas | demo title\n");
        for (const auto &entry : aurora::demo_scenes::scene_registry()) {
            AURORA_LOG_RAW("scene_tool", entry.id, " | ", std::to_string(static_cast<int>(entry.width)), "x",
                           std::to_string(static_cast<int>(entry.height)), " | ",
                           *entry.title != '\0' ? entry.title : "(no demo twin)", "\n");
        }
        return 0;
    }

    if (args[0] == "--render") {
        if (args.size() < 3 || args.size() == 4 || args.size() > 5) {
            print_usage();
            return 2;
        }
        const auto *entry = find_scene(args[1]);
        if (entry == nullptr) {
            AURORA_LOG_RAW("scene_tool", "unknown scene: ", args[1], "\n");
            print_usage();
            return 2;
        }
        float width = entry->width;
        float height = entry->height;
        if (args.size() == 5) {
            try {
                width = std::stof(args[3]);
                height = std::stof(args[4]);
            } catch (const std::exception &) {
                AURORA_LOG_RAW("scene_tool", "invalid size: ", args[3], " ", args[4], "\n");
                return 2;
            }
        }

        aurora::Scene scene{entry->build()};
        // 基线底色对齐真实后端 Surface::clear_color()（{245,245,247,255}）：窗口渲染前由 Surface
        // 清屏，无头渲染默认不清（零初始化透明黑）——E2E golden 把无头基线与真实窗口读回帧
        // 做像素比对，底色必须同口径，否则控件未覆盖区域两侧不一致。
        const auto r = scene.render_to_png(args[2].c_str(), static_cast<int>(width), static_cast<int>(height),
                                           aurora::Color{245, 245, 247, 255});
        if (!r) {
            AURORA_LOG_RAW("scene_tool", "render failed: ", r.error().message, "\n");
            return 1;
        }
        AURORA_LOG_RAW("scene_tool", "rendered ", args[2], " (", std::to_string(static_cast<int>(width)), "x",
                       std::to_string(static_cast<int>(height)), ")\n");
        return 0;
    }

    print_usage();
    return 2;
}
