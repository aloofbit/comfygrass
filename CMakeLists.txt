# 32-bit only: the 1.12 client is x86, so the DLL loaded into it must be too.
#   cmake -B build -A Win32 && cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
# CMP0091 must be NEW before project() for CMAKE_MSVC_RUNTIME_LIBRARY to take effect.
cmake_policy(SET CMP0091 NEW)

# Static CRT: the DLL is dropped into a game folder that may have no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(comfygrass CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(comfygrass SHARED
    src/comfygrass.cpp
    src/config.cpp
)

# comfygrass.dll, loaded by VanillaFixes from dlls.txt. It exports nothing: it attaches by patching
# DXVK's device vtable from DllMain, so there is no .def and nothing for the client to import.
#
# The DLL lands in the project root rather than build/Release, so it sits next to comfygrass.ini and the
# two files a player has to copy are always together. The empty $<0:> generator expression is what stops
# MSVC appending its own per-config subfolder.
set_target_properties(comfygrass PROPERTIES
    OUTPUT_NAME "comfygrass"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}$<0:>")

if(MSVC)
    target_compile_options(comfygrass PRIVATE /W3 /permissive- /GS-)
endif()
