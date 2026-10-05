cmake_minimum_required(VERSION 3.20)
project(Watermark VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Watermark requires Windows (WIC and BCrypt).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(watermark_core STATIC
    src/core/image.cpp
    src/core/image_wic.cpp
    src/core/crypto_win.cpp
    src/core/dct.cpp
    src/core/payload.cpp
    src/core/pattern.cpp
    src/core/embed.cpp
    src/core/detect.cpp
)
target_include_directories(watermark_core PUBLIC src)
target_compile_definitions(watermark_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(watermark_core PUBLIC windowscodecs ole32 bcrypt)
if(MSVC)
    target_compile_options(watermark_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(watermark_app STATIC src/app/files.cpp src/app/jobs.cpp)
target_link_libraries(watermark_app PUBLIC watermark_core)
add_library(watermark_cli STATIC src/cli/common.cpp)
target_link_libraries(watermark_cli PUBLIC watermark_app)
add_executable(wmembed src/cli/wmembed.cpp)
target_link_libraries(wmembed PRIVATE watermark_cli)
add_executable(wmdetect src/cli/wmdetect.cpp)
target_link_libraries(wmdetect PRIVATE watermark_cli)
if(MSVC)
    target_compile_options(watermark_cli PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(watermark_app PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(wmgui WIN32 src/gui/main.cpp src/gui/dialogs.cpp src/gui/wmgui.manifest)
target_link_libraries(wmgui PRIVATE watermark_app comctl32 shell32 uuid)
if(MSVC)
    target_compile_options(wmgui PRIVATE /W4 /permissive- /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(watermark_selftest tests/selftest.cpp)
    target_link_libraries(watermark_selftest PRIVATE watermark_core)
    if(MSVC)
        target_compile_options(watermark_selftest PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME watermark_selftest COMMAND watermark_selftest)
    add_executable(watermark_cli_test tests/cli_test.cpp)
    target_link_libraries(watermark_cli_test PRIVATE watermark_core)
    add_dependencies(watermark_cli_test wmembed wmdetect)
    if(MSVC)
        target_compile_options(watermark_cli_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME watermark_cli_test COMMAND watermark_cli_test)
    add_executable(watermark_gui_test tests/gui_test.cpp)
    target_link_libraries(watermark_gui_test PRIVATE watermark_app comctl32)
    add_dependencies(watermark_gui_test wmgui)
    if(MSVC)
        target_compile_options(watermark_gui_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME watermark_gui_test COMMAND watermark_gui_test)
    set_tests_properties(watermark_selftest watermark_cli_test watermark_gui_test PROPERTIES TIMEOUT 240)
endif()
