# Bazarish project (c) 2026
cmake_minimum_required(VERSION 3.20)
project(bazarish-client-daemon CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The client core and the crypto come from common; the mailbox and the
# federation this daemon serves to itself come from the server repository. Both
# are submodules, and both are told to use this tree's copy of common so the
# library is compiled once.
set(BAZARISH_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common" CACHE PATH
    "Path to the bazarish common library repository")
set(BAZARISH_SERVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bazarish-server" CACHE PATH
    "Path to the bazarish server repository")

add_subdirectory(${BAZARISH_COMMON_DIR} bazarish-common)
add_subdirectory(${BAZARISH_SERVER_DIR}/server-core bazarish-server-core)

find_package(Threads REQUIRED)

add_library(bazarish_daemon_lib STATIC
    src/Config.cpp
)
target_include_directories(bazarish_daemon_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bazarish_daemon_lib PUBLIC
    Bazarish::Client bazarish_server_lib Threads::Threads)
target_compile_options(bazarish_daemon_lib PRIVATE -Wall -Wextra -Werror)

add_executable(bazarish-daemon src/Main.cpp)
target_link_libraries(bazarish-daemon PRIVATE bazarish_daemon_lib)
target_compile_options(bazarish-daemon PRIVATE -Wall -Wextra -Werror)

enable_testing()

function(bazarish_daemon_add_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE bazarish_daemon_lib)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bazarish_daemon_add_test(TestConfig)
