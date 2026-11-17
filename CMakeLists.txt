cmake_minimum_required(VERSION 3.21)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:DEBUG>:Debug>DLL")

project(
    InteractiveMusicRadio
    VERSION 2.2.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /EHsc /permissive- /Zc:__cplusplus /W3 /MP")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /FS /GL")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:FULL /LTCG /OPT:REF /OPT:ICF")

list(APPEND CMAKE_PREFIX_PATH "D:/dev/deps")

set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_compile_definitions(SKYRIM_SUPPORT_AE=1)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

add_subdirectory("D:/dev/CommonLibSSE-NG" "${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE-NG")

set(IMR_CORE_SRC
    src/core.cpp
    src/platform.cpp
    src/sources.cpp
    src/session.cpp
    src/nowplaying.cpp
    src/lrc.cpp
    src/lyrics.cpp
    src/lyrics_net.cpp
    src/handshake.cpp
    src/ui.cpp
)

set(IMR_PLUGIN_SRC
    src/main.cpp
)

function(imr_add_build suffix zh output_name)
    add_library(InteractiveMusicRadioCore${suffix} STATIC ${IMR_CORE_SRC})
    # extern holds PrismaUI_API.h: the public modder header, copied verbatim
    # from the PrismaUI 1.5.1 release.  Nothing from PrismaUI is linked or built
    # here - the header only declares an interface that is resolved at runtime.
    target_include_directories(InteractiveMusicRadioCore${suffix}
        PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extern")
    target_compile_features(InteractiveMusicRadioCore${suffix} PUBLIC cxx_std_23)
    target_compile_definitions(InteractiveMusicRadioCore${suffix} PUBLIC IMR_UI_LANG_ZH=${zh})
    # ole32/oleaut32 : Core Audio COM (no UI Automation in 2.2)
    # winhttp        : lyric look-up (the only networked code)
    # shell32        : opening a music site in the system browser
    # windowsapp     : C++/WinRT activation for the media session (SMTC)
    target_link_libraries(InteractiveMusicRadioCore${suffix} PUBLIC
        ole32 oleaut32 winhttp shell32 windowsapp)

    add_library(InteractiveMusicRadioPlugin${suffix} SHARED ${IMR_PLUGIN_SRC})
    target_include_directories(InteractiveMusicRadioPlugin${suffix} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extern")
    target_link_libraries(InteractiveMusicRadioPlugin${suffix} PRIVATE InteractiveMusicRadioCore${suffix} CommonLibSSE::CommonLibSSE)
    set_target_properties(InteractiveMusicRadioPlugin${suffix} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        PDB_NAME "${output_name}"
        COMPILE_PDB_NAME "${output_name}_compile"
    )
endfunction()

# Chinese build -> InteractiveMusicRadio.dll
imr_add_build(""    1 "InteractiveMusicRadio")
# English build -> InteractiveMusicRadio_en.dll (rename to InteractiveMusicRadio.dll to use)
imr_add_build("_en" 0 "InteractiveMusicRadio_en")

# ---------------------------------------------------------------------- tests
add_executable(InteractiveMusicRadioTests tests/tests.cpp)
target_link_libraries(InteractiveMusicRadioTests PRIVATE InteractiveMusicRadioCore)

add_executable(InteractiveMusicRadioTests_en tests/tests.cpp)
target_link_libraries(InteractiveMusicRadioTests_en PRIVATE InteractiveMusicRadioCore_en)
