/* rom_embed.S - the cartridge image built into the executable (make copies
 * the ROM to build/rom.bin; assembled with -I build). */
    .section __TEXT,__const
    .globl _sf2_rom_data
    .globl _sf2_rom_data_end
    .p2align 4
_sf2_rom_data:
    .incbin "rom.bin"
_sf2_rom_data_end:
