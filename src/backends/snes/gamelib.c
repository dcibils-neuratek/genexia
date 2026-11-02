/* gamelib.c - the one entry point of a game library, see SnesEnv in machine.h.
 * Built with the game's recompiled code and -DGAME_REGISTER=its register function. */
#include "machine.h"
SnesEnv genv;
void GAME_REGISTER(void);
void genexia_game(const SnesEnv *e) {
    genv = *e;
    GAME_REGISTER();
}
