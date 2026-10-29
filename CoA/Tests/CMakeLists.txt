file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(CoATests ${TEST_SOURCES})

target_include_directories(CoATests
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(CoATests
    PRIVATE
        CoA
        Base
        Storm
        BlizzardCore
        lua
        Os
        ConsoleBase
)

# 中文化断言串需 UTF-8 源字符集（By leewheel 2026-10-29）
if (MSVC)
    target_compile_options(CoATests PRIVATE /utf-8)
endif ()

add_test(NAME CoATests COMMAND CoATests)
