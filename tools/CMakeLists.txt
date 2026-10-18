# Outils hors jeu : bibliothèque protocole/réseau, faux pair, tests unitaires. Aucune dépendance UE.
cmake_minimum_required(VERSION 3.22)
project(E33CoopTools CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../native)

add_library(e33proto STATIC
    ${NATIVE}/protocol/messages.cpp
    ${NATIVE}/net/link.cpp
    ${NATIVE}/net/udp.cpp
)
target_include_directories(e33proto PUBLIC ${NATIVE}/common ${NATIVE}/protocol ${NATIVE}/net)
target_link_libraries(e33proto PUBLIC ws2_32)

add_executable(fakepeer fakepeer/main.cpp)
target_link_libraries(fakepeer PRIVATE e33proto)

enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE e33proto)
add_test(NAME protocol COMMAND test_protocol)
