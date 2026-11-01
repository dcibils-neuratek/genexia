#ifndef COMBO_H
#define COMBO_H
#include "genesis.h"
enum { FX_NONE = 0, FX_FIRE = 1, FX_THUNDER = 2 };
void combo_start(int player, int which);      /* which: 0 = fire combo, 1 = thunder combo */
void combo_cancel(int player);
int  combo_running(int player);
u16  combo_input(int player, u16 live_pad);   /* once per real frame */
const char *combo_name(int player, int which);
int  combo_player_x(int p);
int  combo_player_hp(int p);
int  combo_player_char(int p);
extern int combo_fx_frames[2], combo_fx_type[2], combo_flash[2], combo_hits[2], combo_hit_pop[2];
const char *combo_current_name(int player);
extern double combo_mana[2];
extern int combo_mana_seconds, combo_mana_enabled, combo_denied[2], combo_pad_hint[2];
int combo_in_fight(void);
/* tuner support (headless) */
#include <stdio.h>
int  combo_tunables(int ch, int which, int *idx, int max);
int  combo_step_frames(int ch, int which, int i);
void combo_set_step_frames(int ch, int which, int i, int frames);
void combo_print_tuned(int ch, int which, FILE *f);
/* fx.c: draw auras onto the finished frame using the VDP's fighter mask */
void fx_apply(u32 *fb, int stride, int w, int h);
void fx_text(u32 *fb, int stride, int w, int h, const char *t, int x, int y, int sc, u32 c1, u32 c2);
int  fx_text_width(const char *t, int sc);
void fx_box(u32 *fb, int stride, int w, int h, int x0, int y0, int x1, int y1, u32 c, float a);
#endif
