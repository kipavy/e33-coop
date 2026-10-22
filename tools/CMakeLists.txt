# Outils hors jeu : bibliothèque protocole/réseau, faux pair, installateur, tests unitaires. Aucune dépendance UE.
cmake_minimum_required(VERSION 3.22)
project(E33CoopTools CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../native)

# Version du mod : une seule source, le fichier VERSION (vérifié contre le tag par .github/workflows/release.yml).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION E33COOP_VERSION LIMIT_COUNT 1)
string(STRIP "${E33COOP_VERSION}" E33COOP_VERSION)
string(REPLACE "." "," E33COOP_VERSION_COMMAS "${E33COOP_VERSION}")
add_compile_definitions(E33COOP_VERSION="${E33COOP_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION)

add_library(e33proto STATIC
    ${NATIVE}/protocol/messages.cpp
    ${NATIVE}/net/link.cpp
    ${NATIVE}/net/udp.cpp
    ${NATIVE}/net/steam.cpp
    ${NATIVE}/remote/remote_proto.cpp
)
target_include_directories(e33proto PUBLIC ${NATIVE}/common ${NATIVE}/protocol ${NATIVE}/net ${NATIVE}/remote)
target_link_libraries(e33proto PUBLIC ws2_32 shell32 bcrypt)

add_executable(fakepeer fakepeer/main.cpp)
target_link_libraries(fakepeer PRIVATE e33proto winmm)

# Installateur / mise à jour / désinstallation pour les joueurs (« Installer E33 Coop.exe »). Icône générée au build.
add_executable(mkicon installer/mkicon.cpp)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon.ico
    COMMAND mkicon ${CMAKE_CURRENT_BINARY_DIR}/icon.ico
    DEPENDS mkicon)
add_custom_target(installer_icon DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/icon.ico)
add_executable(installer WIN32 installer/main.cpp installer/installer.rc)
add_dependencies(installer installer_icon)
set_source_files_properties(installer/installer.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/icon.ico;${CMAKE_CURRENT_SOURCE_DIR}/installer/app.manifest")
target_include_directories(installer PRIVATE ${NATIVE}/common ${CMAKE_CURRENT_SOURCE_DIR}/installer ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(installer PRIVATE UNICODE _UNICODE E33COOP_VERSION_COMMAS=${E33COOP_VERSION_COMMAS})
set_source_files_properties(installer/installer.rc PROPERTIES COMPILE_DEFINITIONS
    "E33COOP_VERSION_STR=\"${E33COOP_VERSION}\";E33COOP_VERSION_COMMAS=${E33COOP_VERSION_COMMAS}")
target_link_libraries(installer PRIVATE winhttp bcrypt comctl32 shell32 ole32 advapi32 version)
set_target_properties(installer PROPERTIES OUTPUT_NAME "Installer E33 Coop" MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(MSVC)
    target_link_options(installer PRIVATE /MANIFEST:NO)   # manifeste fourni par installer.rc
endif()

enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE e33proto)
add_test(NAME protocol COMMAND test_protocol)
add_executable(test_remote tests/test_remote.cpp)
target_link_libraries(test_remote PRIVATE e33proto)
add_test(NAME remote COMMAND test_remote)
add_executable(test_release tests/test_release.cpp)
target_include_directories(test_release PRIVATE ${NATIVE}/common)
add_test(NAME release COMMAND test_release)
