cmake_minimum_required(VERSION 3.21)

# Geode v5 requires C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64"
)
endif()

project(AutoDecorator VERSION 1.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define the GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Work around a duplicate declaration in some Geode SDK Windows headers.
# Clang rejects `bool init();` and `virtual bool init();` when both appear
# in CCFileUtilsWin32 in the same class.
if (WIN32)
    set(_geode_win32_header
        "$ENV{GEODE_SDK}/loader/include/Geode/cocos/platform/win32/CCFileUtilsWin32.h"
    )
    if (EXISTS "${_geode_win32_header}")
        file(READ "${_geode_win32_header}" _geode_win32_header_content)
        string(FIND "${_geode_win32_header_content}"
            "    virtual bool init();" _duplicate_init_pos)
        if (NOT _duplicate_init_pos EQUAL -1)
            string(REPLACE
                "    virtual bool init();"
                ""
                _geode_win32_header_content
                "${_geode_win32_header_content}"
            )
            file(WRITE "${_geode_win32_header}" "${_geode_win32_header_content}")
            message(STATUS "Applied Geode Win32 duplicate-init compatibility fix")
        endif()
    endif()
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Links Geode, packages mod.json/resources into AutoDecorator.geode
setup_geode_mod(${PROJECT_NAME})
