/* rom_embed.c - access to the ROM image linked in by rom_embed.S */
#include <stddef.h>
extern const unsigned char sf2_rom_data[], sf2_rom_data_end[];
size_t sf2_embedded_rom(const unsigned char **p) { *p = sf2_rom_data; return (size_t)(sf2_rom_data_end - sf2_rom_data); }
