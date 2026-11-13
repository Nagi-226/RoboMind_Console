cmake_minimum_required(VERSION 3.16)
project(RoboMindConsole VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 依赖：VVOS（add_subdirectory 本地路径）
#
# ⚠️ 这是**路径依赖**，不是版本约束。README 里登记了基线 commit，
#    升级 VVOS 时必须同步更新那一行，否则"同一份代码在别人机器上行为不同"。
#
# 为什么关掉 ENABLE_TESTS：VVOS 自带 19 个测试可执行文件，我们只要库。
#    开着会把它们全部拖进本工程的构建目标里。
# ---------------------------------------------------------------------------
set(VVOS_ROOT "E:/Github Project/Virtual-Vehicle-OS" CACHE PATH "VVOS 仓库根目录")
if(NOT EXISTS "${VVOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "找不到 VVOS：${VVOS_ROOT}（用 -DVVOS_ROOT=... 指定）")
endif()

set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SANITIZER OFF CACHE BOOL "" FORCE)
# VVOS 默认 -Werror，我们不想被它的严格阈值卡住本工程的构建
set(ENABLE_STRICT_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory("${VVOS_ROOT}" vvos_build)

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# M2 · selfcheck（控制台程序）
#
# 单独一个**控制台**目标，而不是给 GUI 可执行文件加 --selfcheck：
#   GUI 是 WIN32 子系统，stdout 默认不接到父控制台，要么 AttachConsole 折腾，
#   要么输出丢失。分成两个目标后，selfcheck 的输出和退出码都是天然的，
#   而 GUI 那个仍然保持"双击不弹窗"。两条判据互不干扰。
# ---------------------------------------------------------------------------
add_executable(selfcheck src/selfcheck.cpp)
target_link_libraries(selfcheck PRIVATE vr_base_framework)
# 兜底，正常不需要：VVOS 的 vr_base_framework 已 PUBLIC 导出 include（master 13c46b0 起）。
# 保留它是为了"哪天 PUBLIC 导出被人误删，至少还能编过"——用 0 成本把故障从编译失败降级。
# ⚠️ 这不是标准做法，不要复制到别处：正确的做法是依赖目标的 PUBLIC 导出。
target_include_directories(selfcheck PRIVATE
    "${VVOS_ROOT}/src"
)


# ---------------------------------------------------------------------------
# framecheck：用**真实 MessageCodec::Decode** 校验板端抓到的帧文件
#
# 存在理由：字节数对 ≠ 能被解开。板端曾出现双层长度头 bug，
# 「440 帧、残留 0」全部正常但 Decode 必然失败 —— 只有真解码才能抓到。
# ---------------------------------------------------------------------------
add_executable(framecheck src/framecheck.cpp)
target_link_libraries(framecheck PRIVATE vr_base_framework)
target_include_directories(framecheck PRIVATE "${VVOS_ROOT}/src")

# ---------------------------------------------------------------------------
# M1 · GUI 壳
#
# WIN32_EXECUTABLE ON ⇒ 生成 GUI 子系统（PE Subsystem = 2），双击不弹控制台。
# 判据：objdump -p 看 Subsystem 字段，应为 Windows GUI (2)。
# ---------------------------------------------------------------------------
add_executable(robomind-console WIN32 src/main.cpp)
target_link_libraries(robomind-console PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    vr_base_framework
)
target_include_directories(robomind-console PRIVATE "${VVOS_ROOT}/src")

set_target_properties(robomind-console PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "RoboMindConsole"
)

# ---------------------------------------------------------------------------
# 部署目标：把 Qt 运行时依赖复制到 exe 旁边，让产物**不依赖 PATH** 也能双击运行
#
# 为什么要做成 CMake 目标而不是 README 里的手动步骤：
#   手动步骤会退化成"每次都要记得跑"，而忘跑的症状是"双击没反应、无报错、
#   进程短暂存在"——极易被误判成 UI 代码缺陷（我们已踩过一次）。
#
# windeployqt 不在 PATH 里，路径从 Qt6::qmake 反推，不写死。
# 就地部署（不加 --dir）：exe 本身已在输出目录，只需补齐依赖。
#   （注：若改用 --dir <新目录>，windeployqt **不会**复制 exe，需额外拷贝一步）
# ---------------------------------------------------------------------------
get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
if(_qt_qmake)
    get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin_dir}" NO_DEFAULT_PATH)
endif()

if(WINDEPLOYQT_EXE)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT_EXE}" --release --no-translations
                "$<TARGET_FILE:robomind-console>"
        DEPENDS robomind-console
        COMMENT "部署 Qt 运行时依赖到输出目录（部署后不依赖 PATH）"
        VERBATIM
    )
else()
    message(WARNING "未找到 windeployqt，deploy 目标不可用；分发版需手动部署")
endif()
