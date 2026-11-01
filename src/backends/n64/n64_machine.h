#ifndef N64_MACHINE_H
#define N64_MACHINE_H
#include "genesis.h"
int  n64_detect(void);
const char *n64_load(void);
void n64_reset(void);
void n64_run_frame(void);
void n64_debug_print(void);
int  n64_audio_take(s16 *out, int max);
extern int n64_audio_rate;
extern u8 n64_stick_x[2], n64_stick_y[2], n64_c_buttons[2];
#endif
