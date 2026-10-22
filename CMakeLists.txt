cmake_minimum_required(VERSION 3.22)
project(ProjectCane VERSION 0.4.0 LANGUAGES C CXX)

# CMake 4 no longer enables compatibility policies older than 3.5 by default.
# Some pinned third-party projects (currently FAudio 25.02) still declare an
# older minimum, so provide the supported policy floor to those subprojects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Minimum policy compatibility for bundled third-party projects")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Rebuilds retail level files into the PC format: a desktop tool, and a JNI library for the Android importer.
set(brx_rebuilder_sources
    "File Extractor GUI/NativeTools/BrxRebuilder/AssetWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/BinaryInputStream.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/BrxWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/GeometryWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/main.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/ObjectWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/OptionWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/SpliceWalker.cpp"
    "File Extractor GUI/NativeTools/BrxRebuilder/TriangleList.cpp")
if(NOT ANDROID)
add_executable(Sly1BrxRebuilder ${brx_rebuilder_sources})
target_compile_definitions(Sly1BrxRebuilder PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
# Android: the same rebuilder as a JNI library, called by the on-device importer (extract/BrxRebuild.java).
set(brx_rebuilder_lib_sources ${brx_rebuilder_sources})
list(FILTER brx_rebuilder_lib_sources EXCLUDE REGEX "/main[.]cpp$")
add_library(brxrebuild SHARED ${brx_rebuilder_lib_sources} platform/android/brx_rebuild_jni.cpp)
target_include_directories(brxrebuild PRIVATE "File Extractor GUI/NativeTools/BrxRebuilder")
target_link_libraries(brxrebuild PRIVATE log)
endif()

set(pal_extractor_sources
    "File Extractor GUI/NativeTools/PalExtractor/AssetWalker.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/BinaryInputStream.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/BrxWalker.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/GeometryWalker.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/main.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/ObjectWalker.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/OptionWalker.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/PalIsoExtractor.cpp"
    "File Extractor GUI/NativeTools/PalExtractor/SpliceWalker.cpp")
if(NOT ANDROID)
add_executable(Sly1PalExtractor ${pal_extractor_sources})
target_compile_definitions(Sly1PalExtractor PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(CANE_FFMPEG_ROOT "" CACHE PATH "FFmpeg development prefix")
if(CANE_FFMPEG_ROOT AND NOT WIN32)
    list(PREPEND CMAKE_PREFIX_PATH "${CANE_FFMPEG_ROOT}")
endif()

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)
find_package(Threads REQUIRED)

# Android / OpenGL ES 3.2 build. Uses SDL2 (supplied by the Gradle project's
# SDL source tree) behind a small GLFW-compatible shim (platform/android).
if(ANDROID)
    set(CANE_GLES ON)
endif()
option(CANE_DIAG "Android bring-up diagnostics (extra logcat output). Turn OFF for release builds." ON)
option(CANE_GLES "Build against OpenGL ES 3.2 via the SDL2 GLFW shim (implied on Android)" OFF)

if(NOT CANE_GLES)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL https://codeload.github.com/glfw/glfw/tar.gz/refs/tags/3.4
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01)
FetchContent_MakeAvailable(glfw)
endif()

if(NOT CANE_GLES)
find_package(SDL2 2.24 CONFIG QUIET)
if(NOT SDL2_FOUND)
    if(WIN32)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl2
        URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-2.30.11
        URL_HASH SHA256=cc6136dd964854e8846c679703322f3e2a341d27a06a53f8b3f642c26f1b0cfd)
    FetchContent_MakeAvailable(sdl2)
endif()
endif()

add_library(cane_platform INTERFACE)
if(CANE_GLES)
    # Must precede Libraries/include so <GLFW/glfw3.h> and
    # "imgui/imgui_impl_glfw.h" resolve to the SDL2-backed shims.
    target_include_directories(cane_platform INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/android")
endif()
target_include_directories(cane_platform INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform")
if(WIN32)
    target_link_libraries(cane_platform INTERFACE shell32)
endif()

set(imgui_root "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include/imgui")
set(cane_ui_sources
    "${imgui_root}/imgui.cpp"
    "${imgui_root}/imgui_draw.cpp"
    "${imgui_root}/imgui_tables.cpp"
    "${imgui_root}/imgui_widgets.cpp"
    "${imgui_root}/imgui_impl_opengl3.cpp"
    "${imgui_root}/ImGuiFileDialog.cpp"
    Sly1-Release/glad.c)
if(CANE_GLES)
    list(APPEND cane_ui_sources platform/android/glfw_sdl.cpp platform/android/touch_overlay.cpp platform/android/crash_handler.cpp)
else()
    list(APPEND cane_ui_sources "${imgui_root}/imgui_impl_glfw.cpp")
endif()
add_library(cane_ui STATIC ${cane_ui_sources})
if(CANE_GLES)
    # The ImGui GL backend uses the system GLES3 headers directly (it never
    # sees glad), so give it the ES3 code path.
    set_source_files_properties("${imgui_root}/imgui_impl_opengl3.cpp"
        PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_OPENGL_ES3)
    # Android ships libGLESv3.so; desktop Mesa/ANGLE expose ES 3.x through libGLESv2.
    if(ANDROID)
        set(CANE_GLES_LIB GLESv3)
    else()
        set(CANE_GLES_LIB GLESv2)
    endif()
    set(CANE_SDL2_DIR "" CACHE PATH "SDL2 source tree (Android: supplied by the Gradle project)")
    if(CANE_SDL2_DIR)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${CANE_SDL2_DIR}" sdl2)
    else()
        find_package(SDL2 REQUIRED CONFIG)
    endif()
    target_link_libraries(cane_ui PUBLIC cane_platform SDL2::SDL2 ${CANE_GLES_LIB} EGL ${CMAKE_DL_LIBS})
    target_compile_definitions(cane_ui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
else()
    target_link_libraries(cane_ui PUBLIC cane_platform glfw ${CMAKE_DL_LIBS})
    target_compile_definitions(cane_ui PUBLIC IMGUI_DEFINE_MATH_OPERATORS GLFW_INCLUDE_NONE)
endif()

# Keep Visual Studio's source list authoritative while the two build systems coexist.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/Sly1-Release/Sly1-Release.vcxproj"
    source_lines REGEX "<ClCompile Include=")
set(game_sources "")
foreach(line IN LISTS source_lines)
    string(REGEX REPLACE ".*Include=\"([^\"]+)\".*" "\\1" source "${line}")
    string(REPLACE "\\" "/" source "${source}")
    if(NOT source MATCHES "^\\.\\." AND NOT source STREQUAL "glad.c")
        list(APPEND game_sources "Sly1-Release/${source}")
    endif()
endforeach()

if(NOT WIN32)
    list(REMOVE_ITEM game_sources "Sly1-Release/spu_reverb_xapo.cpp")
endif()

if(ANDROID)
    # SDL's Java activity loads libmain.so and calls SDL_main.
if(CANE_GLES)
    list(APPEND game_sources "Sly1-Release/resume_state.cpp")
    list(APPEND game_sources "platform/android/art_extract.cpp")   # Android: second-screen artwork from the cutscenes
endif()
    add_library(ProjectCane SHARED ${game_sources} "${imgui_root}/imgui_demo.cpp")
    set_target_properties(ProjectCane PROPERTIES OUTPUT_NAME main)
else()
    add_executable(ProjectCane ${game_sources} "${imgui_root}/imgui_demo.cpp")
endif()
if(CANE_GLES)
    target_compile_definitions(ProjectCane PRIVATE CANE_GLES)
    if(CANE_DIAG)
        target_compile_definitions(ProjectCane PRIVATE CANE_DIAG)
        target_compile_definitions(cane_ui PRIVATE CANE_DIAG)
    endif()
    # Maps the few desktop-only GL calls onto ES (platform/gles_compat.h).
    if(MSVC)
        target_compile_options(ProjectCane PRIVATE /FI"${CMAKE_CURRENT_SOURCE_DIR}/platform/gles_compat.h")
    else()
        target_compile_options(ProjectCane PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/platform/gles_compat.h")
    endif()
endif()
target_link_libraries(ProjectCane PRIVATE cane_ui Threads::Threads SDL2::SDL2)
target_compile_definitions(ProjectCane PRIVATE
    CANE_PORTABLE_SHADERS
    _CRT_SECURE_NO_WARNINGS
    _ENABLE_EXTENDED_ALIGNED_STORAGE)
target_include_directories(ProjectCane PRIVATE "${CMAKE_BINARY_DIR}/generated")

if(NOT MSVC)
    target_compile_options(ProjectCane PRIVATE -fno-strict-aliasing -Wno-invalid-offsetof)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(GLOB shader_inputs CONFIGURE_DEPENDS
    "Sly1-Release/*.vert" "Sly1-Release/*.frag" "Sly1-Release/*.geom")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/generated/embedded_shaders.h"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Sly1-Release
        -DOUTPUT=${CMAKE_BINARY_DIR}/generated/embedded_shaders.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-shaders.cmake
    DEPENDS ${shader_inputs} cmake/embed-shaders.cmake
    VERBATIM)
target_sources(ProjectCane PRIVATE "${CMAKE_BINARY_DIR}/generated/embedded_shaders.h")

foreach(header
    platform/platform.h
    Sly1-Release/sw.h Sly1-Release/save.h Sly1-Release/freeze.h
    Sly1-Release/act.h Sly1-Release/wr.h Sly1-Release/shadow.h
    Sly1-Release/fader.h Sly1-Release/splicemap.h Sly1-Release/gc.h)
    target_precompile_headers(ProjectCane PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/${header}>")
endforeach()
set_source_files_properties(Sly1-Release/alo.cpp PROPERTIES
    COMPILE_DEFINITIONS ALO_IMPLEMENTATION SKIP_PRECOMPILE_HEADERS ON)

if(WIN32)
    if(NOT CANE_FFMPEG_ROOT)
        set(CANE_FFMPEG_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/ffmpeg/ffmpeg-master-latest-win64-lgpl-shared")
    endif()
    target_include_directories(ProjectCane PRIVATE "${CANE_FFMPEG_ROOT}/include")
    target_link_directories(ProjectCane PRIVATE "${CANE_FFMPEG_ROOT}/lib")
    target_link_libraries(ProjectCane PRIVATE
        opengl32 winmm xaudio2 avformat avcodec avutil swscale)
else()
    if(ANDROID)
        # FFmpeg is cross-built per ABI (see android/README.md); point
        # CANE_FFMPEG_ROOT at a prefix containing include/ and lib/<abi>/.
        if(NOT CANE_FFMPEG_ROOT)
            message(FATAL_ERROR "Set CANE_FFMPEG_ROOT to the Android FFmpeg prefix")
        endif()
        add_library(cane_ffmpeg INTERFACE)
        target_include_directories(cane_ffmpeg INTERFACE "${CANE_FFMPEG_ROOT}/include")
        target_link_directories(cane_ffmpeg INTERFACE "${CANE_FFMPEG_ROOT}/lib/${ANDROID_ABI}")
        target_link_libraries(cane_ffmpeg INTERFACE avformat avcodec swscale avutil z log)
        add_library(PkgConfig::FFMPEG ALIAS cane_ffmpeg)
    else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale)
    endif()

    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(faudio
        URL https://codeload.github.com/FNA-XNA/FAudio/tar.gz/refs/tags/25.02
        URL_HASH SHA256=eab667d231e5036b0e5c3c6f89fb4ea792284493d46161bc056a88a6dadc2683)
    # FAudio's install(EXPORT) requires SDL2 to be in an export set; when SDL2
    # comes from add_subdirectory (Android) it is not, which fails generation.
    # This superbuild never installs FAudio, so skip its install rules.
    set(_cane_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
    set(CMAKE_SKIP_INSTALL_RULES ON)
    FetchContent_MakeAvailable(faudio)
    set(CMAKE_SKIP_INSTALL_RULES "${_cane_skip_install_rules}")
    if(TARGET FAudio-shared)
        set(cane_audio_target FAudio-shared)
    else()
        set(cane_audio_target FAudio-static)
    endif()

    target_link_libraries(ProjectCane PRIVATE PkgConfig::FFMPEG ${cane_audio_target})
endif()

if(MSVC AND NOT CANE_GLES)
    set_property(TARGET ProjectCane cane_ui glfw PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT ANDROID)
    install(TARGETS ProjectCane Sly1BrxRebuilder Sly1PalExtractor RUNTIME DESTINATION bin)
endif()
