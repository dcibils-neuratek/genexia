/* rom_none.c - used when the build has no ROM file to embed: the game then
 * loads the cartridge from disk at start-up */
#include <stddef.h>
size_t sf2_embedded_rom(const unsigned char **p) { *p = NULL; return 0; }
