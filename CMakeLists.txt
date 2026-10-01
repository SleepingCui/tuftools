cmake_minimum_required(VERSION 3.15)
project(tuftools CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB TUFFTOOLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(tuftools ${TUFFTOOLS_SOURCES})
target_include_directories(tuftools PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

# Always emit build/tuftools.exe, also for multi-config generators (Visual Studio).
set_target_properties(tuftools PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)

if(NOT MSVC)
    target_compile_options(tuftools PRIVATE -Wall)
endif()

if(WIN32)
    target_link_libraries(tuftools PRIVATE winhttp)
endif()

if(MINGW)
    # Keep the executable free of libwinpthread-1.dll / libstdc++-6.dll.
    target_link_options(tuftools PRIVATE -static)
endif()

if(MSVC)
    # Source files contain UTF-8 literals.
    target_compile_options(tuftools PRIVATE /utf-8)
endif()
