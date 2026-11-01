# Times Mario 64's game code alone on the iMac G4: RSP tasks finish at once and
# nothing is drawn. Reads sm64.z64 next to itself, writes n64bench.log.
cmake_minimum_required(VERSION 3.15)
project(N64Bench C)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB RECOMP ${ROOT}/recomps/n64/sm64c/recomp_*.c)
add_application(N64Bench n64bench.c n64bench.r ${ROOT}/src/chips/r4300.c ${ROOT}/src/backends/n64/machine.c ${RECOMP})
target_compile_definitions(N64Bench PRIVATE GENEXIA_OS9 N64_BENCH ROM_MAX=0x800000)
target_include_directories(N64Bench PRIVATE ${ROOT}/build ${ROOT}/src ${ROOT}/src/core ${ROOT}/src/chips ${ROOT}/src/backends/genesis ${ROOT}/src/backends/snes ${ROOT}/src/backends/n64)
target_compile_options(N64Bench PRIVATE -std=gnu11 -O2 -w -mminimal-toc -mcpu=750 -mtune=7450 -fno-strict-aliasing -ffp-contract=off)
target_link_libraries(N64Bench -lm)

# The same with pictures: graphics tasks go to OpenGL through src/chips/f3d.c.
add_application(N64GL n64bench.c n64bench.r ${ROOT}/src/chips/r4300.c ${ROOT}/src/backends/n64/machine.c ${ROOT}/src/chips/f3d.c ${RECOMP})
target_compile_definitions(N64GL PRIVATE GENEXIA_OS9 N64_BENCH N64_GL ROM_MAX=0x800000)
target_include_directories(N64GL PRIVATE ${ROOT}/build ${ROOT}/src ${ROOT}/src/core ${ROOT}/src/chips ${ROOT}/src/backends/genesis ${ROOT}/src/backends/snes ${ROOT}/src/backends/n64)
target_compile_options(N64GL PRIVATE -std=gnu11 -O2 -w -mminimal-toc -mcpu=750 -mtune=7450 -fno-strict-aliasing -ffp-contract=off)
target_link_libraries(N64GL -lm)
