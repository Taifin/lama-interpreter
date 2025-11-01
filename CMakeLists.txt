cmake_minimum_required(VERSION 3.5)
project(lama_interpreter)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS -O3)

add_executable(lama_interpreter main.cpp
        runtime/runtime.c
        bytecode/bytefile.cpp
        runtime/gc.c
)
