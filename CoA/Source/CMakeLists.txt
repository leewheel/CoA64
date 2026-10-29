file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(CoA STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(CoA
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# FontRenderClient.cpp: FreeType 2.14.1 (AscFontRender).
target_link_libraries(CoA
    PRIVATE
        freetype
)

# 中文化前提：源码/执行字符集按 UTF-8 编入，否则 MSVC 按 GBK 误读中文串（By leewheel 2026-10-29）
if (MSVC)
    target_compile_options(CoA PRIVATE /utf-8)
endif ()
