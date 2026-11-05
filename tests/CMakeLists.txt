# tests/CMakeLists.txt - 单元测试配置

# 查找 Qt6 测试支持
find_package(Qt6 REQUIRED COMPONENTS Test)

# 查找 Google Test（可选，若不存在则跳过）
find_package(GTest QUIET)
if (GTest_FOUND)
    include(GoogleTest)

    # 创建测试可执行文件。
    # 注意：Bridge 的实现位于 src/bridge.cpp（并调用 src/dicomreader.cpp 的
    # DicomReader::loadVolume），必须一并编译进来，否则链接期会出现
    # Bridge 相关符号未定义的错误。
    add_executable(bridge_test
        bridge_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/bridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/simulatorprovider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/bridge.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/auditsink.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/auditsink.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/deviceprovider.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dicomreader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dicomreader.h
    )

# 链接 Qt、DCMTK 和 GTest
    target_link_libraries(bridge_test
        Qt6::Core
        Qt6::Test
        Qt6::WebChannel
        DCMTK::dcmdata
        DCMTK::oflog
        GTest::gtest
        GTest::gtest_main
    )

    # Bridge 的实现引用了 DICOM_SAMPLE_PATH 宏（src/bridge.cpp:145），
    # 该宏原本只在主可执行目标上定义，测试目标必须同步定义，否则
    # QStringLiteral(DICOM_SAMPLE_PATH) 会展开成 QStringLiteral() 而编译失败。
    target_compile_definitions(bridge_test PRIVATE
        DICOM_SAMPLE_PATH="${CMAKE_SOURCE_DIR}/resources/dicom/brain-t1"
    )

    # 设置编译选项
    target_compile_features(bridge_test PRIVATE cxx_std_17)

    # 与主目标保持同一套质量门：测试代码不该比产品代码更宽松，
    # 否则测试里的类型转换 / 变量遮蔽问题没人拦。
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(bridge_test PRIVATE
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

    # 添加测试
    add_test(NAME BridgeUnitTests
        COMMAND bridge_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../
    )

    # 设置测试属性
    set_target_properties(bridge_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # 添加运行测试的目标
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS bridge_test
        COMMENT "运行所有单元测试"
        VERBATIM
    )

    # 安装测试
    install(TARGETS bridge_test
        RUNTIME DESTINATION bin
        OPTIONAL
    )
else()
    message(WARNING "GTest not found, skipping tests. Install with: brew install googletest")
endif()