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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
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
# robomind-headless：无窗口接收端（控制台子系统）
#
# 为什么单独一个可执行文件而不是给 GUI 加 --headless：
#   GUI 是 WIN32 子系统，stdout 默认不接父控制台 —— 塞进去会重蹈 M2 覆辙：
#   输出静默丢失而退出码仍为 0（"看起来通过"）。理由同 selfcheck。
# ---------------------------------------------------------------------------
add_executable(robomind-headless src/headless_main.cpp src/headless.cpp src/headless.h)
target_link_libraries(robomind-headless PRIVATE Qt6::Core Qt6::Network vr_base_framework)
target_include_directories(robomind-headless PRIVATE "${VVOS_ROOT}/src")

# ---------------------------------------------------------------------------
# M1 · GUI 壳
#
# WIN32_EXECUTABLE ON ⇒ 生成 GUI 子系统（PE Subsystem = 2），双击不弹控制台。
# 判据：objdump -p 看 Subsystem 字段，应为 Windows GUI (2)。
# ---------------------------------------------------------------------------
add_executable(robomind-console WIN32
    src/main.cpp src/mainwindow.cpp src/mainwindow.h
    src/lanchat_sink.cpp src/lanchat_sink.h
)
target_link_libraries(robomind-console PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    vr_base_framework
)
target_include_directories(robomind-console PRIVATE "${VVOS_ROOT}/src")

set_target_properties(robomind-console PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "RoboMindConsole"
    # 直接产出到 build/dist/，而不是 build/ 根下。
    #
    # 原因：`deploy` 是**就地** windeployqt —— 依赖落在 exe 旁边。
    #   若 exe 在 build/ 根下，而分发目录又是 build/dist/，
    #   就会出现两份 exe：一份带新代码、一份是上次手动拷过去的旧版，
    #   用户双击哪份完全看运气，症状是"明明改了却看不到效果"。
    #   本工程已实际踩到（dist 里那份比 build/ 落后一整个里程碑）。
    #   把输出目录钉在 dist 后，构建、部署、双击的位置是同一个。
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)

# ---------------------------------------------------------------------------
# 应用图标：把 assets/app.ico 编进 exe 的资源段
#
# 为什么必须有：没有这段资源，RoboMindConsole.exe 在任务栏、Alt-Tab、
#   资源管理器里一律显示系统默认空白图标。setWindowIcon() 设的是**窗口**图标，
#   exe 文件本身的图标只认 PE 资源段，两者是两套机制。
#
# ⚠️ 为什么不用 enable_language(RC) + target_sources(app.rc)：
#   MinGW 的 windres 在把 **-I 路径**转交给预处理器 cc1 时**不加引号**。
#   本工程路径是 "E:/Github Project/..."，含空格，于是 cc1 收到
#   `-IE:/Github` + `Project/RoboMind-Console/...` 两个碎参数，直接报
#       cc1.exe: fatal error: Project/RoboMind-Console/...: No such file or directory
#       windres.exe: preprocessing failed.
#   这是 CMake RC 规则的已知缺陷，与本工程配置无关。
#   实测：**完全不给 -I 时 windres 成功**（.rc 里只有 ICON 语句，无需任何
#   #include），且输入/输出路径含空格也不影响。故这里自建 custom command。
#   （代价：将来 .rc 若需要 #include，必须换成无空格路径或改用 --preprocessor-arg）
#
# 判据（改完必须实测，不能只看"构建成功"）：
#   windres 失败经常只打印 warning 而不中断链接 —— exe 照样产出、图标静默缺失。
#   用 `objdump -h <exe> | grep rsrc`（应有 .rsrc 段）或
#   PowerShell `[System.Drawing.Icon]::ExtractAssociatedIcon(<exe>)` 实际取一次图标来证明。
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(WINDRES_EXE windres HINTS "${_mingw_bin}" NO_DEFAULT_PATH)

    if(WINDRES_EXE)
        configure_file(assets/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
        set(_rc_obj "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc.obj")

        add_custom_command(
            OUTPUT "${_rc_obj}"
            COMMAND "${WINDRES_EXE}" -O coff
                    "${CMAKE_CURRENT_BINARY_DIR}/app.rc" "${_rc_obj}"
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico"
            COMMENT "编译图标资源 app.rc → app_icon.rc.obj"
            VERBATIM
        )
        # EXTERNAL_OBJECT：告诉 CMake 这是**已编译好**的对象文件，
        # 直接进链接行，不要试图用 CXX 规则再编一遍。
        set_source_files_properties("${_rc_obj}" PROPERTIES
            EXTERNAL_OBJECT TRUE
            GENERATED TRUE
        )
        target_sources(robomind-console PRIVATE "${_rc_obj}")
    else()
        message(WARNING "未找到 windres，exe 将没有图标；检查 MinGW 工具链是否完整")
    endif()
endif()

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
