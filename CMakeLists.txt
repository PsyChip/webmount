cmake_minimum_required(VERSION 3.24)
project(webmount VERSION 0.1.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "webmount requires MSVC targeting Windows x64")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
configure_file(resources/webmount.rc.in webmount.rc @ONLY)
add_executable(webmount
    src/main.cpp
    src/consoleView.cpp
    src/httpClient.cpp
    src/apacheIndex.cpp
    src/structuredIndex.cpp
    src/mountCache.cpp
    src/driveMount.cpp
    src/mountControl.cpp
    src/runtimeSetup.cpp
    src/startupMount.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/webmount.rc"
)
target_include_directories(webmount PRIVATE src)
target_include_directories(webmount SYSTEM PRIVATE vendor/winfsp/include)
target_compile_definitions(webmount PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(webmount PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(webmount PRIVATE winhttp shlwapi advapi32 shell32 ole32 uuid wintrust crypt32 runtimeobject xmllite)
target_link_options(webmount PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/webmount.manifest")
set_target_properties(webmount PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
