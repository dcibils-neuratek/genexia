#ifndef N64_MACHINE_H
#define N64_MACHINE_H
#include "genesis.h"
int  n64_detect(void);
const char *n64_load(void);
extern void (*n64_register_game)(void);
extern int n64_force_interp;
void recomp_register_sm64(void);
void n64_state_loaded(void);
u32  n64_vi_origin(void);
void n64_set_wide(double aspect);  /* above 4:3 widens 3D; only the high-res image shows it */
void rdp_set_hires(int n, int wide);
int  rdp_hires_fetch(u32 origin, int aa, u32 *out, int w, int h);
void n64_reset(void);
void n64_run_frame(void);
void n64_debug_print(void);
int  n64_audio_take(s16 *out, int max);
extern int n64_audio_rate;
extern u8 n64_stick_x[2], n64_stick_y[2], n64_c_buttons[2];
#endif
