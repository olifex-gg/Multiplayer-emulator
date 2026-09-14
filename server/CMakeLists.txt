cmake_minimum_required(VERSION 3.16)
project(acnet_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ENET_DIR "${REPO_ROOT}/pc/lib/enet")

add_library(enet STATIC
    ${ENET_DIR}/callbacks.c
    ${ENET_DIR}/compress.c
    ${ENET_DIR}/host.c
    ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c
    ${ENET_DIR}/peer.c
    ${ENET_DIR}/protocol.c
    ${ENET_DIR}/unix.c
    ${ENET_DIR}/win32.c
)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
target_compile_options(enet PRIVATE -w)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
    target_compile_definitions(enet PRIVATE
        HAS_SOCKLEN_T=1 HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1
        HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1)
endif()

set(ACNET_WARN -Wall -Wextra -Wno-unused-parameter)

add_executable(acnet_server src/main.c src/town.c)
target_include_directories(acnet_server PRIVATE ${REPO_ROOT}/net src)
target_compile_options(acnet_server PRIVATE ${ACNET_WARN})
target_link_libraries(acnet_server enet)

add_executable(acnet_cli tools/acnet_cli.c src/town.c)
target_include_directories(acnet_cli PRIVATE ${REPO_ROOT}/net src)
target_compile_options(acnet_cli PRIVATE ${ACNET_WARN})
target_link_libraries(acnet_cli enet)

install(TARGETS acnet_server acnet_cli RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e.sh
            $<TARGET_FILE:acnet_server> $<TARGET_FILE:acnet_cli>)
