set(TARGET E33Coop)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    ../hooks/ue.cpp
    ../hooks/log.cpp
    ../hooks/hookbus.cpp
    ../hooks/game.cpp
    ../testrunner/runner.cpp
)

target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_include_directories(${TARGET} PRIVATE ../common ../hooks ../testrunner)
if(MSVC)
    target_compile_options(${TARGET} PRIVATE /utf-8)
endif()
target_link_libraries(${TARGET} PUBLIC UE4SS ws2_32 shell32)
