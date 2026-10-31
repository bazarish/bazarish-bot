# Bazarish project (c) 2026
cmake_minimum_required(VERSION 3.20)
project(bazarish-bot CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Both submodules build against this tree's copy of common, so it is compiled once.
set(BAZARISH_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common" CACHE PATH
    "Path to the bazarish common library repository")
set(BAZARISH_SERVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bazarish-server" CACHE PATH
    "Path to the bazarish server repository")

add_subdirectory(${BAZARISH_COMMON_DIR} bazarish-common)
add_subdirectory(${BAZARISH_SERVER_DIR}/server-core bazarish-server-core)
add_subdirectory(${BAZARISH_SERVER_DIR}/service-node bazarish-service-node)
add_subdirectory(${BAZARISH_SERVER_DIR}/facade-http bazarish-facade-http)

find_package(Threads REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)

add_library(bazarish_bot_lib STATIC
    src/Account.cpp
    src/Backup.cpp
    src/Config.cpp
    src/HttpFace.cpp
    src/OperatorAuth.cpp
    src/Runtime.cpp
    src/Database.cpp
    src/FileRules.cpp
    src/GroupCopy.cpp
    src/GroupEngine.cpp
    src/GroupStore.cpp
    src/NameRules.cpp
    src/PictureRules.cpp
    src/MenuRules.cpp
    src/Transcript.cpp
    src/Transfers.cpp
    src/ServerSide.cpp
    src/Settings.cpp
    src/SystemInfo.cpp
    src/Zip.cpp
)
bazarish_stamp_version(bazarish_bot_lib)
target_include_directories(bazarish_bot_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(bazarish_bot_lib PRIVATE ${SQLCIPHER_INCLUDE_DIR})
target_link_libraries(bazarish_bot_lib PUBLIC
    Bazarish::Client bazarish_server_lib bazarish_service_lib bazarish_facade_lib
    ICU::uc ICU::i18n Threads::Threads)
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
target_compile_definitions(TestConfig PRIVATE BAZARISH_BOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
bazarish_bot_add_test(TestOperatorAuth)
bazarish_bot_add_test(TestSettings)
bazarish_bot_add_test(TestTranscript)
bazarish_bot_add_test(TestGroupEngine)
bazarish_bot_add_test(TestNameRules)
bazarish_bot_add_test(TestPictureRules)
bazarish_bot_add_test(TestFileRules)
bazarish_bot_add_test(TestTransfers)
bazarish_bot_add_test(TestMenuRules)
bazarish_bot_add_test(TestGroupCopy)
bazarish_bot_add_test(TestBackup)
bazarish_bot_add_test(TestServerSide)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME WebTranslations
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-web-i18n.py
        ${CMAKE_CURRENT_SOURCE_DIR}/web)
