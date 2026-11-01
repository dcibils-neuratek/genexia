# Small Mac OS 9 program that reports what OpenGL can do on the machine:
#   cmake -S tools/os9_glprobe -B build/os9/glprobe -DCMAKE_TOOLCHAIN_FILE=.../retroppc.toolchain.cmake
cmake_minimum_required(VERSION 3.15)
project(GLProbe C)
add_application(GLProbe glprobe.c)
target_compile_options(GLProbe PRIVATE -std=gnu11 -O2 -w -mcpu=750)
