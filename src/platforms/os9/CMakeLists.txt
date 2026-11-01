# Mac OS 9 build, compiled by Retro68:
#   make os9
cmake_minimum_required(VERSION 3.15)
project(GenexiaOS9 C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(GLOB RECOMP ${ROOT}/recomps/genesis/sf2/recomp_*.c ${ROOT}/recomps/genesis/umk3/recomp_*.c ${ROOT}/recomps/snes/smk/recomp_*.c)

add_application(Genexia
    os9_main.c
    genexia.r
    ${ROOT}/src/chips/m68k_core.c ${ROOT}/src/chips/m68k_interp.c ${ROOT}/src/chips/z80.c
    ${ROOT}/src/chips/ym2612.c ${ROOT}/src/chips/psg.c
    ${ROOT}/src/backends/genesis/bus.c ${ROOT}/src/backends/genesis/vdp.c ${ROOT}/src/backends/genesis/vdp_altivec.c
    ${ROOT}/src/backends/genesis/fx.c ${ROOT}/src/backends/genesis/combo.c ${ROOT}/src/core/enhance.c
    ${ROOT}/src/chips/w65816.c ${ROOT}/src/backends/snes/machine.c ${ROOT}/src/backends/snes/ppu.c
    ${ROOT}/src/backends/snes/apu.c ${ROOT}/src/backends/snes/dsp1.c
    ${RECOMP})

target_compile_definitions(Genexia PRIVATE GENEXIA_OS9)
target_include_directories(Genexia PRIVATE ${ROOT}/build ${ROOT}/src ${ROOT}/src/core ${ROOT}/src/chips ${ROOT}/src/backends/genesis ${ROOT}/src/backends/snes)
target_compile_options(Genexia PRIVATE -std=gnu11 -O2 -w -mminimal-toc -mcpu=750 -mtune=7450 -fno-strict-aliasing)
# the rest also runs on a G3. The compiler doesn't tell the assembler about the
# G4 on this target, so that needs saying.
set_source_files_properties(${ROOT}/src/backends/genesis/vdp_altivec.c PROPERTIES
    COMPILE_OPTIONS "-mcpu=7450;-maltivec;-mabi=altivec;-mvrsave;-Wa,-m7450")
set_source_files_properties(${ROOT}/src/backends/genesis/vdp.c ${ROOT}/src/chips/ym2612.c
    ${ROOT}/src/backends/snes/ppu.c ${ROOT}/src/backends/snes/apu.c PROPERTIES COMPILE_OPTIONS "-O3")
target_link_libraries(Genexia -lm)
