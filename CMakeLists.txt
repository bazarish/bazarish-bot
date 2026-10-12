# Bazarish project (c) 2026
cmake_minimum_required(VERSION 3.20)
project(bazarish-bot CXX)

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
add_subdirectory(${BAZARISH_SERVER_DIR}/service-node bazarish-service-node)
add_subdirectory(${BAZARISH_SERVER_DIR}/facade-http bazarish-facade-http)

find_package(Threads REQUIRED)

add_library(bazarish_bot_lib STATIC
    src/Account.cpp
    src/Config.cpp
    src/HttpFace.cpp
    src/OperatorAuth.cpp
    src/Runtime.cpp
    src/Transcript.cpp
    src/ServerSide.cpp
    src/Settings.cpp
)
target_include_directories(bazarish_bot_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bazarish_bot_lib PUBLIC
    Bazarish::Client bazarish_server_lib bazarish_service_lib bazarish_facade_lib
    Threads::Threads)
target_compile_options(bazarish_bot_lib PRIVATE -Wall -Wextra -Werror)

add_executable(bazarish-bot src/Main.cpp)
target_link_libraries(bazarish-bot PRIVATE bazarish_bot_lib)
target_compile_options(bazarish-bot PRIVATE -Wall -Wextra -Werror)

enable_testing()

function(bazarish_bot_add_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE bazarish_bot_lib)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bazarish_bot_add_test(TestConfig)
bazarish_bot_add_test(TestOperatorAuth)
bazarish_bot_add_test(TestSettings)
bazarish_bot_add_test(TestTranscript)
