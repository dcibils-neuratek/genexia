/*
 * n64.h - contract between the Nintendo 64 chips and the machine.
 *
 * The chips live in src/chips (r4300.c, rsp.c, rdp.c) and know nothing about
 * each other: they reach the machine through the functions declared here.
 * src/backends/n64/machine.c provides the memory map, MI/PI/SI/VI/AI/RI
 * registers, SP and DP registers, boot and the frame scheduler.
 *
 * All N64 emulated state is tagged N64_STATE. It lives in its own section so
 * the 8 MB of RDRAM does not inflate save states of other consoles.
 *
 * Memory is big-endian: n64_rdram[0] is the first byte of RDRAM as the N64
 * sees it (bytes are stored in N64 order; multi-byte values need byte swaps).
 */
#ifndef N64_H
#define N64_H
#include "genesis.h"              /* u8..s64, vdp_framebuffer */

#define N64_STATE __attribute__((section("__DATA,__n64state")))
#define N64_RDRAM_SIZE 0x800000   /* 8 MB (expansion pak present) */

extern u8 n64_rdram[N64_RDRAM_SIZE];
extern u8 n64_sp_mem[0x2000];     /* DMEM at 0x0000, IMEM at 0x1000 */

/* -------------------------------------------- physical memory (machine.c)
 * Big-endian values, any physical address: RDRAM, SP memory, registers, ROM,
 * PIF. The CPU should use its own fast path for RDRAM. */
u8   n64_read8(u32 pa);
u16  n64_read16(u32 pa);
u32  n64_read32(u32 pa);
u64  n64_read64(u32 pa);
void n64_write8(u32 pa, u8 v);
void n64_write16(u32 pa, u16 v);
void n64_write32(u32 pa, u32 v);
void n64_write64(u32 pa, u64 v);

/* --------------------------------------------------------- R4300i (r4300.c) */
void r4300_reset(u32 pc);          /* cold state, then PC (virtual) */
int  r4300_run(int cycles);        /* execute at least `cycles` CPU cycles, returns cycles used */
void r4300_set_irq(int asserted);  /* level of the MI interrupt line (Cause.IP2) */
extern u64 r4300_gpr[32];          /* boot code seeds these */
extern u32 r4300_cop0[32];         /* boot code seeds Status, Config etc */

/* ------------------------------------------------------------- RSP (rsp.c) */
void rsp_reset(void);
/* Run until halted or `cycles` used; returns cycles used. The machine owns the
 * SP_STATUS register and only calls this while the RSP is not halted. */
int  rsp_run(int cycles);
void rsp_set_pc(u32 pc);           /* SP_PC register, 12 bits */
u32  rsp_get_pc(void);
/* RSP COP0 registers 0-7 are the SP registers, 8-15 the DP command registers.
 * BREAK sets halt via n64_sp_break(), which also raises SP interrupt if enabled. */
u32  n64_rsp_cop0_read(int reg);
void n64_rsp_cop0_write(int reg, u32 v);
void n64_sp_break(void);
int  n64_sp_halted(void);          /* RSP must stop when this returns 1 (SSTEP not needed) */

/* ------------------------------------------------------------- RDP (rdp.c) */
void rdp_reset(void);
/* Process commands in [start, end). xbus=1 reads them from DMEM, else RDRAM.
 * A partial command at the end stays buffered for the next call. */
void rdp_run(u32 start, u32 end, int xbus);
void rdp_set_hires(int n);          /* draw scale, host side only; 1 = off */
void rdp_hires_resync(void);        /* S changed without commands: a save state loaded */
int  rdp_hires_fetch(u32 origin, int aa, u32 *out, int w, int h);
void n64_rdp_full_sync(void);      /* Sync Full command finished: raises DP interrupt */
#endif
