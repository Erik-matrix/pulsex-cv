cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)                      # CMAKE_MSVC_RUNTIME_LIBRARY
project(pulsex_cv LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# static C runtime: the programs run without the Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Qualcomm QAIRT SDK: only its Genie HEADERS are needed (the built-in Qwen engine loads Genie.dll at run time, from
# the model bundle). Nothing is linked from the SDK.
set(QNN_SDK "" CACHE PATH "Qualcomm QAIRT SDK root (the folder that has include/Genie)")
if(NOT EXISTS "${QNN_SDK}/include/Genie/GenieDialog.h")
    message(FATAL_ERROR "Set -DQNN_SDK=<path to the Qualcomm QAIRT SDK> (the folder that has include/Genie/GenieDialog.h).")
endif()
set(JSON_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/json")
set(IMGUI_DIR    "${CMAKE_SOURCE_DIR}/third_party/imgui")

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/pcore
    ${CMAKE_SOURCE_DIR}/include/pcore/runtime
    ${CMAKE_SOURCE_DIR}/src/tools
    ${JSON_INCLUDE}
    ${QNN_SDK}/include
    ${QNN_SDK}/include/QNN
)

add_library(pulse_cv_core STATIC
    src/pcore/runtime/genie_pipeline.cpp
    src/pcore/runtime/mem_util.cpp          # reclaims standby RAM around a model load
)
target_link_libraries(pulse_cv_core PRIVATE winhttp crypt32)
target_compile_definitions(pulse_cv_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# the tailor: ad + your details -> texts -> PDFs
add_executable(pulse_cv_tailor src/tools/pulsecore_tailor.cpp)
target_link_libraries(pulse_cv_tailor PRIVATE pulse_cv_core)
target_compile_definitions(pulse_cv_tailor PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(pulse_cv_tailor PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_sources(pulse_cv_tailor PRIVATE src/utf8.manifest)   # ANSI code page = UTF-8

# the window
add_executable(pulse_cv_gui
    src/pcore/gui/pulse_cv_gui.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(pulse_cv_gui PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(pulse_cv_gui PRIVATE pulse_cv_core d3d11 dxgi d3dcompiler comdlg32)
target_compile_definitions(pulse_cv_gui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(pulse_cv_gui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
target_sources(pulse_cv_gui PRIVATE src/gui.manifest)       # UTF-8 + per-monitor DPI
