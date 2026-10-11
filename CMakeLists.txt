cmake_minimum_required(VERSION 3.20)

project(MyMerryMayFix LANGUAGES CXX)

add_library(dinput8 SHARED
    src/dinput8_proxy.cpp
    src/mod.cpp
    src/mod.h
    src/dinput8.def
)

target_compile_features(dinput8 PRIVATE cxx_std_20)
target_compile_definitions(dinput8 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(dinput8 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-cast-function-type
)

target_link_libraries(dinput8 PRIVATE
    d3d11
    dxgi
    gdi32
    user32
)

set_target_properties(dinput8 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dinput8"
)

if(MINGW)
    # Keep the drop-in DLL self-contained. In particular, do not require a
    # separate libwinpthread-1.dll beside the game executable.
    target_link_options(dinput8 PRIVATE -static -static-libgcc -static-libstdc++)
endif()
