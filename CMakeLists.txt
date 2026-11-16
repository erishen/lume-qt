cmake_minimum_required(VERSION 3.21)

project(lume-qt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
# Qt6 依赖：Widgets + WebEngineWidgets + WebChannel + Positioning
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS
    Widgets
    WebEngineWidgets
    WebChannel
    Network
    Core
    Positioning
    Multimedia
    OpenGLWidgets
)

# DICOM 解析（DCMTK 3.7.x，brew install dcmtk）
find_package(DCMTK REQUIRED)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# 前端构建产物输出目录（与 frontend/vite.config.ts 保持一致）
# ---------------------------------------------------------------------------
set(WEB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/resources/web")

# ---------------------------------------------------------------------------
# 前端构建：npm run build → resources/web
# CUSTOM_TARGET 挂在 ALL 上，只保证它进入默认构建；先于宿主目标编译的顺序
# 由下面 qt_add_executable 之后的 add_dependencies 负责，二者缺一不可。
# ---------------------------------------------------------------------------
find_program(NPM_EXECUTABLE npm REQUIRED)
find_program(BASH_EXECUTABLE bash REQUIRED)

# 增量构建：scripts/frontend-build.sh 会在前端源码无变化时跳过 npm run build，
# 避免每次 cmake --build 都强制重跑整次前端构建（含全量 tsc 类型检查）。
add_custom_target(frontend-build ALL
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/frontend-build.sh"
    COMMENT "Building React frontend (Vite) → resources/web (skipped if unchanged)"
    VERBATIM
)

# 若 Qt 安装中能找到 qwebchannel.js（QWebChannel 的 JS 运行时），
# 拷贝到 web 根目录供前端 <script> 直接引用。
find_file(QWEBCHANNEL_JS
    NAMES qwebchannel.js
    PATHS "${Qt6_DIR}/../../../libexec" "${Qt6_DIR}/../../libexec"
          "/opt/homebrew/opt/qt/libexec"
          "/opt/homebrew/opt/qtwebchannel"
          "/opt/homebrew/Cellar/qtwebchannel"
    PATH_SUFFIXES libexec share/qt6/webchannel
    NO_DEFAULT_PATH
)
if(QWEBCHANNEL_JS)
    add_custom_command(TARGET frontend-build POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QWEBCHANNEL_JS}"
                "${WEB_ROOT}/qwebchannel.js"
        COMMENT "Copying qwebchannel.js → resources/web"
        VERBATIM
    )
    message(STATUS "qwebchannel.js found: ${QWEBCHANNEL_JS}")
else()
    message(WARNING "qwebchannel.js not found — 前端将无法与 C++ 通信。"
                    "请手动将 Qt 安装目录下 libexec/qwebchannel.js 复制到 resources/web/")
endif()

# ---------------------------------------------------------------------------
# 可执行目标
# ---------------------------------------------------------------------------
qt_add_executable(lume-qt
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/bridge.cpp
    src/bridge.h
    src/alarmconfig.cpp
    src/auditquery.cpp
    src/auditsink.cpp
    src/lumesink.cpp
    src/auditsink.h
    src/pacsclient.cpp
    src/pacsclient.h
    src/pacstls.cpp
    src/pacstls.h
    src/deviceprovider.h
    src/simulatorprovider.cpp
    src/simulatorprovider.h
    src/dicomreader.cpp
    src/dicomreader.h
    src/appschemehandler.cpp
    src/appschemehandler.h
    src/schemepath.cpp
    src/schemepath.h
    src/consolepage.cpp
    src/consolepage.h
    src/vitalstrip.cpp
    src/vitalstrip.h
    src/volumeview.cpp
    src/volumeview.h
    src/volumeprefs.cpp
    src/volumeprefs.h
    src/volumestats.cpp
    src/volumestats.h
)

# 显式依赖边：ALL 只表示"参与默认构建"，不表示"先于本目标完成"。
# 没有这一行，ninja 可以先把宿主目标编译完再跑前端构建，产物一致性又回到靠人。
add_dependencies(lume-qt frontend-build)

# 设置编译器警告（目标定义后设置）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lume-qt PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -Wshadow
        -Wformat=2
        -Wconversion
        -Wnull-dereference
        -Wdouble-promotion
    )
endif()

# 检查并设置 C++ 标准
target_compile_features(lume-qt PRIVATE cxx_std_17)

# 添加医疗模拟支持
target_compile_definitions(lume-qt PRIVATE
    HAS_MEDICAL_SIMULATION=1
    QT_WEBENGINE_IMPLEMENTATION
)

target_link_libraries(lume-qt PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
    Qt6::Multimedia
    Qt6::OpenGLWidgets
    DCMTK::dcmdata
    DCMTK::dcmnet
    DCMTK::dcmtls
    DCMTK::oflog
)

# 让 C++ 侧能在运行时定位 resources/web 目录
target_compile_definitions(lume-qt PRIVATE
    WEB_ROOT_PATH="${WEB_ROOT}"
)

# DICOM 样例数据目录（经 QWebChannel 暴露给前端，用于演示体积加载）
target_compile_definitions(lume-qt PRIVATE
    DICOM_SAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/dicom/brain-t1"
)

# macOS 应用打包（可选，未启用 bundle）
# set_target_properties(lume-qt PROPERTIES
#     MACOSX_BUNDLE TRUE
#     MACOSX_BUNDLE_BUNDLE_NAME "lume-qt"
# )

# ---------------------------------------------------------------------------
# 测试配置
# ---------------------------------------------------------------------------
enable_testing()
include(CTest)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# 性能基准测试目标
add_custom_target(perf-benchmark
    COMMAND ${CMAKE_COMMAND} -E echo "性能基准测试：构建时间 = ${CMAKE_BUILD_TYPE}"
    COMMENT "运行性能基准测试"
)

# 辅助目标：运行所有测试
add_custom_target(run-all-tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMENT "运行所有单元测试"
    VERBATIM
)
