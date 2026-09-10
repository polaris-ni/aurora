# ============================================================
# AuroraFeatures.cmake — feature 宏定义单一入口（归一化）
# ------------------------------------------------------------
# 全部「以 feature 宏形态注入 aurora（或指定目标）的编译定义」统一经
# aurora_define_feature() 声明：定义注入与导出登记二合一，杜绝
# target_compile_definitions 与 AURORA_FEATURE_DEFINES 手工双列表漂移。
#
# 登记语义（与归一化前行为一致，AuroraInstall 依赖）：
#   - AURORA_FEATURE_DEFINES 仅含随安装导出的宏（后端 + 架构优化，EXPORT 显式声明）；
#     SIMD / 编解码等纯内部宏不进——安装后消费者以无该宏的口径编译 aurora.h，
#     强行导出反而制造剪裁不一致。
#   - AURORA_ENABLE_DEBUG 例外：它改变类内存布局（Widget 新增数据成员），故由
#     AuroraInstall.cmake 按**安装产物的实际取值**条件导出（见该模块），不在此登记。
#
# 选项语义以 codespec/BUILD_OPTIONS.md 为唯一权威来源；运行时查询入口为
# aurora::debug::feature_flags()（include/aurora/debug/feature_flags.h）。
# ⚠️ 必须在 add_library(aurora ...) 之后、首个调用点（AuroraImageCodecs）之前 include。
# ============================================================

# 导出登记列表：随安装导出给消费者的 feature 宏（由 EXPORT 调用点累积，AuroraInstall 消费）。
set(AURORA_FEATURE_DEFINES "")

# aurora_define_feature(<MACRO> [SCOPE PUBLIC|PRIVATE] [TARGET <tgt>] [RAW <definition>] [EXPORT])
#   MACRO   feature 宏名（导出登记名）
#   SCOPE   编译定义作用域，默认 PUBLIC
#   TARGET  接收定义的目标，默认 aurora（如 inspector server 用其独立目标）
#   RAW     传给 target_compile_definitions 的原始定义文本，默认 = MACRO；
#           三态开关（AUTO）的生成器表达式定义走此处
#   EXPORT  追加进 AURORA_FEATURE_DEFINES（随安装导出给消费者）
function(aurora_define_feature _macro)
    cmake_parse_arguments(_f "EXPORT" "" "SCOPE;TARGET;RAW" ${ARGN})
    if (NOT _f_SCOPE)
        set(_f_SCOPE PUBLIC)
    endif ()
    if (NOT _f_TARGET)
        set(_f_TARGET aurora)
    endif ()
    set(_def "${_macro}")
    if (_f_RAW)
        set(_def "${_f_RAW}")
    endif ()
    target_compile_definitions(${_f_TARGET} ${_f_SCOPE} "${_def}")
    if (_f_EXPORT)
        # 读回主作用域现值，追加后写回（function 局部作用域不会自动上浮）。
        set(AURORA_FEATURE_DEFINES "${AURORA_FEATURE_DEFINES};${_macro}" PARENT_SCOPE)
    endif ()
endfunction()
