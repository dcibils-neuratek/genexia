#ifndef STATE_H
#define STATE_H
#include "genesis.h"
void state_use_section(const char *name);
size_t state_size(void);
void state_save(void *buf);
void state_load(const void *buf);
int  state_write_file(const char *path);
int  state_read_file(const char *path);
#endif
