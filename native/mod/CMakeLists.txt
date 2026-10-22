set(TARGET E33Coop)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    ../hooks/ue.cpp
    ../hooks/log.cpp
    ../hooks/hookbus.cpp
    ../hooks/game.cpp
    ../hooks/reflect.cpp
    ../testrunner/runner.cpp
    ../sync/coop.cpp
    ../sync/explore.cpp
    ../sync/builds.cpp
    ../sync/coopsave.cpp
    ../sync/intent.cpp
    ../sync/state.cpp
    ../sync/presence.cpp
    ../sync/update.cpp
    ../protocol/messages.cpp
    ../net/link.cpp
    ../net/udp.cpp
    ../net/steam.cpp
    ../ui/ui.cpp
    ../ui/overlay.cpp
    ../ui/options.cpp
    ../remote/remote.cpp
    ../remote/remote_proto.cpp
    ../ui/owners_ui.cpp
    ../ui/joinbutton.cpp
)

# Version du mod : fichier VERSION à la racine (une seule source pour le build et la release).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION E33COOP_VERSION LIMIT_COUNT 1)
string(STRIP "${E33COOP_VERSION}" E33COOP_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION)
target_compile_definitions(${TARGET} PRIVATE E33COOP_VERSION="${E33COOP_VERSION}")

target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_include_directories(${TARGET} PRIVATE ../common ../hooks ../testrunner ../sync ../protocol ../net ../ui ../remote)
if(MSVC)
    target_compile_options(${TARGET} PRIVATE /utf-8)
endif()
target_link_libraries(${TARGET} PUBLIC UE4SS ws2_32 shell32 bcrypt winhttp)
