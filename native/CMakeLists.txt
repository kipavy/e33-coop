cmake_minimum_required(VERSION 3.22)
project(E33CoopNative)

# RE-UE4SS épinglé sur le même commit que l'UE4SS.dll livré (ABI C++ non stable).
add_subdirectory(third_party/RE-UE4SS)
add_subdirectory(mod)
