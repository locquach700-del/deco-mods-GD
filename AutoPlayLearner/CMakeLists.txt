cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(AutoPlayLearner VERSION 0.1.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK.")
endif()

# The SDK's Win32 compatibility header is also included by generated Android bindings.
set(_geode_win32_header
    "$ENV{GEODE_SDK}/loader/include/Geode/cocos/platform/win32/CCFileUtilsWin32.h"
)
if (EXISTS "${_geode_win32_header}")
    file(READ "${_geode_win32_header}" _geode_header)
    string(FIND "${_geode_header}" "    virtual bool init();" _duplicate_init_pos)
    if (NOT _duplicate_init_pos EQUAL -1)
        string(REPLACE "    virtual bool init();" "" _geode_header "${_geode_header}")
        file(WRITE "${_geode_win32_header}" "${_geode_header}")
        message(STATUS "Applied Geode duplicate-init compatibility fix")
    endif()
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
