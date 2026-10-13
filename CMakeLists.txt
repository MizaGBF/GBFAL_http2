cmake_minimum_required(VERSION 3.15)
project(http2 LANGUAGES C)

# Set C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CURL_ROOT "" CACHE PATH "Path to the root of libcurl installation")
set(Python_ROOT_DIR "" CACHE PATH "Path to the root of Python installation")

if(CURL_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${CURL_ROOT}")
endif()

find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(CURL REQUIRED)

set(SRC_FILES src/main.c)

Python_add_library(http2 MODULE WITH_SOABI ${SRC_FILES})
target_link_libraries(http2 PRIVATE
    CURL::libcurl
    Python::Python
)

if(NOT MSVC)
  target_compile_options(http2 PRIVATE "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=")
endif()

# Static runtime option
option(BUILD_STATIC_RUNTIME "Link the C runtime statically" OFF)
if(BUILD_STATIC_RUNTIME)
  if(MSVC)
    set_property(TARGET http2 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    target_link_options(http2 PRIVATE "-static")
  endif()
endif()

# Compiler warnings
target_compile_options(http2 PRIVATE
    # Base warnings (all configs)
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# Specific linker options
target_link_options(http2 PRIVATE
    $<$<AND:$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>,$<CONFIG:Release>>:-s>
)