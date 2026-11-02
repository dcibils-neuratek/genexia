/* gamelib.c - the one entry point of an N64 game library, see N64Env in r4300_recomp.h.
 * Built with the game's recompiled code and -DGAME_REGISTER=its register function. */
#include "r4300_recomp.h"
N64Env genv;
void GAME_REGISTER(void);
void genexia_game(const N64Env *e) {
    genv = *e;
    GAME_REGISTER();
}
