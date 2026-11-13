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
target_include_directories(selfcheck PRIVATE
    "${VVOS_ROOT}/src"          # 兜底：即使 VVOS 的 PUBLIC 导出失效也能编
)

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
