cmake_minimum_required(VERSION 3.5)
project(lama_interpreter)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS -O3)

include(FetchContent)
FetchContent_Declare(
        magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        GIT_TAG v0.9.7
)
FetchContent_MakeAvailable(magic_enum)

add_executable(lama_interpreter src/main.cpp
        runtime/runtime.c
        bytecode/bytefile.cpp
        runtime/gc.c
        src/common.h
        src/processor.h
)

#add_executable(lama_analyzer src/analyzer.cpp bytecode/bytefile.cpp runtime/runtime.c runtime/gc.c
#        src/processor.h)
#target_link_libraries(lama_analyzer PRIVATE magic_enum)
