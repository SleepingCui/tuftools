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
else()
    # macOS ships libcurl in the SDK; Linux needs libcurl4-openssl-dev (or equivalent).
    find_package(CURL REQUIRED)
    target_link_libraries(tuftools PRIVATE CURL::libcurl)
endif()

if(MINGW)
    # Keep the executable free of libwinpthread-1.dll / libstdc++-6.dll.
    target_link_options(tuftools PRIVATE -static)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    # std::filesystem lives in a separate library before GCC 9.
    target_link_libraries(tuftools PRIVATE stdc++fs)
endif()

if(MSVC)
    # Source files contain UTF-8 literals.
    target_compile_options(tuftools PRIVATE /utf-8)
endif()

# --------------------------------------------------------------- version ----
# Single source of truth for the version banner. CI passes the release tag it
# just created (and the time that went into that tag); a local build derives an
# id from git. Both land in a generated header, so the program never guesses.
set(TUF_VERSION "" CACHE STRING "")
set(TUF_BUILD_TIME "" CACHE STRING "")

if(NOT TUF_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --exact-match --tags HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE has_tag
            OUTPUT_VARIABLE exact_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE short_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(has_tag EQUAL 0)
            # Built from a tagged commit: show exactly the published version.
            set(TUF_VERSION "${exact_tag}")
        elseif(short_sha)
            set(TUF_VERSION "dev-${short_sha}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(TUF_VERSION AND dirty)
            string(APPEND TUF_VERSION "-dirty")
        endif()
    endif()
endif()

if(NOT TUF_VERSION)
    set(TUF_VERSION "unknown")
endif()
if(NOT TUF_BUILD_TIME)
    string(TIMESTAMP TUF_BUILD_TIME "%Y-%m-%d %H:%M:%S")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/version.hpp" @ONLY)
target_include_directories(tuftools PRIVATE "${CMAKE_BINARY_DIR}/generated")
