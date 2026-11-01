/*
 * w65816.h - WDC 65C816 CPU core. Console-independent: the machine provides
 * the three bus callbacks below and owns all timing, so the same core serves
 * the SNES and any other 65816 system.
 */
#ifndef W65816_H
#define W65816_H
#include "genesis.h"

typedef struct {
    u16 a, x, y, s, d, pc;
    u8  pb, db, p;          /* p: N V M X D I Z C */
    u8  e;                  /* emulation mode */
    u8  stopped, waiting;   /* STP, WAI */
    u8  nmi_pending, irq_line;
} W65816;
extern W65816 w65;

/* provided by the machine */
u8   w65_bus_read(u32 addr);
void w65_bus_write(u32 addr, u8 v);
void w65_bus_idle(void);        /* one internal CPU cycle */

void w65_reset(void);
void w65_step(void);            /* one instruction, or one pending interrupt */
void w65_exec(void);            /* one instruction at PC, no interrupt checks */
#endif
