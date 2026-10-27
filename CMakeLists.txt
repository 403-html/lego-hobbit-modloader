cmake_minimum_required(VERSION 3.16)
project(HobbitModLoader VERSION 0.6.0 LANGUAGES C CXX)

# LEGOHobbit.exe (DX9) is a 32-bit process, so everything here must be x86.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Build for 32-bit x86: MinGW toolchain cmake/mingw-i686.cmake, "
        "or Visual Studio with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT DEFINED HML_LOADER_VERSION)
    set(HML_LOADER_VERSION "${PROJECT_VERSION}")  # override with -DHML_LOADER_VERSION=...
endif()

# ---- MinHook (BSD-2-Clause), pinned ------------------------------------------
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()
add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# ---- SDK (header only) -------------------------------------------------------
add_library(hml_sdk INTERFACE)
target_include_directories(hml_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Static runtime so no extra DLLs have to sit next to the game.
function(hml_static_runtime target)
    if(MINGW)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    elseif(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

# ---- Loader: dinput8.dll -------------------------------------------------------
add_library(hml_loader SHARED
    src/loader/main.cpp
    src/loader/proxy.cpp
    src/loader/log.cpp
    src/loader/game.cpp
    src/loader/rtti.cpp
    src/loader/pattern.cpp
    src/loader/hooks.cpp
    src/loader/mods.cpp
    src/loader/d3d9_hooks.cpp
    src/loader/input.cpp
    src/loader/ui.cpp
    src/loader/native_ui.cpp
    src/loader/gui_probe.cpp
    src/loader/pause_entry.cpp
    src/loader/menu.cpp
    src/loader/ui_api.cpp
    src/loader/dinput8.def)
set_target_properties(hml_loader PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
target_compile_definitions(hml_loader PRIVATE
    HML_LOADER_VERSION="${HML_LOADER_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(hml_loader PRIVATE hml_sdk minhook shell32)
hml_static_runtime(hml_loader)
if(MINGW)
    # .def lists undecorated names; let ld map them to the stdcall symbols quietly.
    target_link_options(hml_loader PRIVATE -Wl,--enable-stdcall-fixup)
    target_compile_options(hml_loader PRIVATE -Wall -Wextra -Wno-cast-function-type)
endif()
if(MSVC)
    set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- Mods ------------------------------------------------------------------------
# hml_add_mod(<name> sources...) builds mods/<name>.dll against the SDK;
# hml_install_mod(<name>) puts it into the install tree's mods\.
function(hml_add_mod name)
    add_library(${name} SHARED ${ARGN})
    set_target_properties(${name} PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods
                                             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mods)
    target_link_libraries(${name} PRIVATE hml_sdk)
    hml_static_runtime(${name})
    if(MINGW)  # strip symbols from release builds
        target_link_options(${name} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
    endif()
endfunction()
function(hml_install_mod name)
    install(TARGETS ${name} RUNTIME DESTINATION mods LIBRARY DESTINATION mods)
endfunction()

hml_add_mod(hello mods/hello/hello.c)
hml_install_mod(hello)

# Mods checked out next to the example (mods/<name>/ with its own
# CMakeLists.txt) build along with the loader. They are separate projects and
# not part of this repository.
file(GLOB HML_MOD_DIRS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/mods/*)
foreach(dir ${HML_MOD_DIRS})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()

option(HML_BUILD_TESTS "Build the test mods and the menu test host (development only)" OFF)
if(HML_BUILD_TESTS)
    hml_add_mod(selftest tests/selftest/selftest.cpp)
    hml_add_mod(menudemo tests/menudemo/menudemo.c)
    # A host exe that loads dinput8.dll like the game does and drives the menu.
    add_executable(menu_host tests/menu_host.cpp)
    target_link_libraries(menu_host PRIVATE hml_sdk d3d9 dinput8 dxguid)
    hml_static_runtime(menu_host)
endif()

# ---- Install: lays out a game-folder-shaped tree ---------------------------------
#   cmake --install build --prefix dist/game   then copy dist/game/* into the game folder
# (hml.ini is written by the loader on first start.)
install(TARGETS hml_loader RUNTIME DESTINATION . LIBRARY DESTINATION .)
if(MINGW)
    # Strip symbols from release builds (650 KB -> ~200 KB).
    target_link_options(hml_loader PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
