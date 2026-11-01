/*
 * dsp1.c - high-level DSP-1/DSP-1B math coprocessor.
 *
 * The chip is a uPD77C25 with an internal program no cartridge dump contains,
 * so its commands are reimplemented here. The arithmetic deliberately keeps
 * the firmware's 16-bit fixed-point shape (Q15 products with arithmetic shift,
 * coefficient/exponent pairs, Newton inverse) because game physics reads the
 * exact results.
 *
 * The chip's data ROM is rebuilt in dsp_rom[] from what its tables are known
 * to contain. See build_tables() for which entries are derived rather than
 * original; command 0x1F dumps this reconstruction, not the real ROM.
 *
 * Protocol: the SNES sees an 8-bit data register. A command byte is read in
 * 8-bit mode (DRC set), then parameters and results move as 16-bit little-endian
 * words (DRC clear, DRS tells which half is next). Reads and writes advance the
 * state machine alike, as the chip only sees RQM handshakes.
 */
#include <math.h>
#include "snes.h"

#define SR_RQM 0x80                    /* status register high byte */
#define SR_DRS 0x10
#define SR_DRC 0x04

enum { WAIT_COMMAND, READ_DATA, WRITE_DATA };

/* transport */
EMU_STATE static u8  sr, fsm, cmd;
EMU_STATE static u16 dr;
EMU_STATE static s16 data_count;
EMU_STATE static s16 in[7];
EMU_STATE static s16 out[1024];
/* attitude matrices A, B, C */
EMU_STATE static s16 matrix[3][3][3];
/* projection state set by Parameter */
EMU_STATE static s16 sin_aas, cos_aas, sin_azs, cos_azs;
EMU_STATE static s16 sin_azs_clip, cos_azs_clip;
EMU_STATE static s16 sec_azs_c1, sec_azs_e1, sec_azs_c2, sec_azs_e2;
EMU_STATE static s16 nx, ny, nz, gx, gy, gz, centre_x, centre_y;
EMU_STATE static s16 c_les, e_les, g_les, vplane_c, vplane_e, voffset;

/* Rebuilt once; contents never change, so they are not save-state data. */
static s16 dsp_rom[1024];
static s16 sin_tab[0xC0];              /* quarter-wave steps, entries 0..0xBF */
static s16 mul_tab[256];               /* low angle byte in Q15 radians */
static int tables_ready;

static void build_tables(void)
{
    const double pi = 3.14159265358979323846;

    if (tables_ready) return;
    tables_ready = 1;

    for (int i = 0; i < 0xC0; i++) {
        double v = floor(32768.0 * sin(2.0 * pi * i / 256.0));
        sin_tab[i] = (s16)(v > 32767.0 ? 32767.0 : v);
    }
    sin_tab[0x80] = 0;                 /* exact, despite rounding of sin(pi) */
    for (int i = 0; i < 256; i++)
        mul_tab[i] = (s16)floor(i * pi);

    /* $22-$40: powers of two used by the shift-by-multiply idiom */
    for (int i = 0; i < 15; i++) {
        dsp_rom[0x22 + i] = (s16)(1 << i);
        dsp_rom[0x32 + i] = (s16)(0x4000 >> i);
    }
    dsp_rom[0x31] = 0x7FFF;

    /* $65-$E4: first guess for Inverse, 1/(2c) over c in 0.5..1. Derived; the
     * ROM differs by a few units in places, which the two Newton steps absorb. */
    for (int i = 0; i < 128; i++) {
        s32 v = 0x20000000 / (0x4000 + i * 128);
        dsp_rom[0x65 + i] = (s16)(v > 0x7FFF ? 0x7FFF : v);
    }

    /* $E5-$115: sqrt nodes for Distance, sqrt(i/64). Derived. */
    for (int i = 0x10; i <= 0x40; i++) {
        double v = floor(sqrt(i / 64.0) * 32768.0 + 0.5);
        dsp_rom[0xD5 + i] = (s16)(v > 32767.0 ? 32767.0 : v);
    }

    /* $324-$328: series for the zenith angle past the clip limit, with
     * x = (pi/4)a: cos x = 1 - x^2/2 + x^4/24, tan x = x + x^3/3. Derived. */
    dsp_rom[0x324] = 0x0207;
    dsp_rom[0x325] = (s16)0xD886;
    dsp_rom[0x327] = 0x6488;
    dsp_rom[0x328] = 0x14AC;
}

/* ----------------------------------------------------------- fixed point */
static inline s16 mulq(s32 a, s32 b) { return (s16)(a * b >> 15); }

static s16 dsp_sin(s16 angle)
{
    if (angle < 0) {
        if (angle == -32768) return 0;
        return (s16)-dsp_sin((s16)-angle);
    }
    s32 hi = angle >> 8;
    s32 s = sin_tab[hi] + (mul_tab[angle & 0xFF] * sin_tab[0x40 + hi] >> 15);
    return (s16)(s > 32767 ? 32767 : s);
}

static s16 dsp_cos(s16 angle)
{
    if (angle < 0) {
        if (angle == -32768) return -32768;
        angle = (s16)-angle;
    }
    s32 hi = angle >> 8;
    s32 s = sin_tab[0x40 + hi] - (mul_tab[angle & 0xFF] * sin_tab[hi] >> 15);
    return (s16)(s < -32768 ? -32767 : s);
}

/* Shift m left until bit 14 differs from the sign; subtract the shift from *ex. */
static void normalize(s16 m, s16 *co, s16 *ex)
{
    int i = 0x4000, e = 0;
    if (m < 0) while ((m & i) && i) { i >>= 1; e++; }
    else       while (!(m & i) && i) { i >>= 1; e++; }
    *co = e > 0 ? (s16)(m * dsp_rom[0x21 + e] * 2) : m;
    *ex = (s16)(*ex - e);
}

/* The same for a 31-bit value; *ex receives the shift count. */
static void normalize_double(s32 product, s16 *co, s16 *ex)
{
    s16 n = (s16)(product & 0x7FFF);
    s16 m = (s16)(product >> 15);
    int i = 0x4000, e = 0;
    s16 c;

    if (m < 0) while ((m & i) && i) { i >>= 1; e++; }
    else       while (!(m & i) && i) { i >>= 1; e++; }

    if (e > 0) {
        c = (s16)(m * dsp_rom[0x21 + e] * 2);
        if (e < 15) {
            c = (s16)(c + (n * dsp_rom[0x40 - e] >> 15));
        } else {
            i = 0x4000;
            if (m < 0) while ((n & i) && i) { i >>= 1; e++; }
            else       while (!(n & i) && i) { i >>= 1; e++; }
            if (e > 15) c = (s16)(n * dsp_rom[0x12 + e] * 2);
            else        c = (s16)(c + n);
        }
    } else {
        c = m;
    }
    *co = c;
    *ex = (s16)e;
}

/* Back to a plain integer, saturating. */
static s16 truncate(s16 c, s16 e)
{
    if (e > 0) {
        if (c > 0) return 32767;
        if (c < 0) return -32767;
    } else if (e < 0) {
        if (e < -16) return 0;
        return (s16)(c * dsp_rom[0x31 + e] >> 15);
    }
    return c;
}

/* Divide by 2^e via the ROM table, including its quirk: e = 0 multiplies by
 * 0x7FFF, so it loses a unit instead of passing the value through. */
static s16 shift_r(s16 c, s16 e)
{
    return (s16)(c * dsp_rom[0x31 + e] >> 15);
}

static void inverse(s16 co, s16 ex, s16 *ico, s16 *iex)
{
    if (co == 0) {                     /* the chip's answer to division by zero */
        *ico = 0x7FFF;
        *iex = 0x002F;
        return;
    }

    s16 sign = 1;
    if (co < 0) {
        if (co < -32767) co = -32767;
        co = (s16)-co;
        sign = -1;
    }
    while (co < 0x4000) {
        co = (s16)(co << 1);
        ex--;
    }

    if (co == 0x4000) {
        if (sign == 1) {
            *ico = 0x7FFF;
        } else {
            *ico = -0x4000;
            ex--;
        }
    } else {
        s16 i = dsp_rom[((co - 0x4000) >> 7) + 0x65];
        /* two "estimated" Newton steps */
        i = (s16)((i + (-i * (co * i >> 15) >> 15)) * 2);
        i = (s16)((i + (-i * (co * i >> 15) >> 15)) * 2);
        *ico = (s16)(i * sign);
    }
    *iex = (s16)(1 - ex);
}

/* ---------------------------------------------------------------- commands */
static void attitude(s16 (*m)[3])
{
    s16 s = (s16)(in[0] >> 1);
    s16 sz = dsp_sin(in[1]), cz = dsp_cos(in[1]);
    s16 sy = dsp_sin(in[2]), cy = dsp_cos(in[2]);
    s16 sx = dsp_sin(in[3]), cx = dsp_cos(in[3]);
    s32 scz = s * cz >> 15, ssz = s * sz >> 15;
    s32 ssx = s * sx >> 15, scx = s * cx >> 15;

    m[0][0] = mulq(scz, cy);
    m[0][1] = (s16)-mulq(ssz, cy);
    m[0][2] = mulq(s, sy);
    m[1][0] = (s16)((ssz * cx >> 15) + (((scz * sx >> 15) * sy) >> 15));
    m[1][1] = (s16)((scz * cx >> 15) - (((ssz * sx >> 15) * sy) >> 15));
    m[1][2] = (s16)-mulq(ssx, cy);
    m[2][0] = (s16)((ssz * sx >> 15) - (((scz * cx >> 15) * sy) >> 15));
    m[2][1] = (s16)((scz * sx >> 15) + (((ssz * cx >> 15) * sy) >> 15));
    m[2][2] = mulq(scx, cy);
}

/* 0x03/13/23: F,L,U -> X,Y,Z (by the transpose) */
static void rotate_to_global(s16 (*m)[3])
{
    for (int c = 0; c < 3; c++)
        out[c] = (s16)((in[0] * m[0][c] >> 15) + (in[1] * m[1][c] >> 15)
                     + (in[2] * m[2][c] >> 15));
}

/* 0x0D/1D/2D: X,Y,Z -> F,L,U */
static void rotate_to_object(s16 (*m)[3])
{
    for (int r = 0; r < 3; r++)
        out[r] = (s16)((in[0] * m[r][0] >> 15) + (in[1] * m[r][1] >> 15)
                     + (in[2] * m[r][2] >> 15));
}

static void scalar(s16 (*m)[3])
{
    out[0] = (s16)((s32)((u32)(in[0] * m[0][0]) + (u32)(in[1] * m[0][1])
                        + (u32)(in[2] * m[0][2])) >> 15);
}

static void parameter(void)
{
    static const s16 max_azs_exp[16] = {
        0x38B4, 0x38B7, 0x38BA, 0x38BE, 0x38C0, 0x38C4, 0x38C7, 0x38CA,
        0x38CE, 0x38D0, 0x38D4, 0x38D7, 0x38DA, 0x38DD, 0x38E0, 0x38E4
    };
    s16 fx = in[0], fy = in[1], fz = in[2], lfe = in[3], les = in[4];
    s16 aas = in[5], azs = in[6];
    s16 azs_clip = azs;
    s16 c, e, csec, aux, max_azs, centre_z;
    s16 vof = 0;

    sin_aas = dsp_sin(aas);
    cos_aas = dsp_cos(aas);
    sin_azs = dsp_sin(azs);
    cos_azs = dsp_cos(azs);

    nx = mulq(sin_azs, -sin_aas);
    ny = mulq(sin_azs, cos_aas);
    nz = mulq(cos_azs, 0x7FFF);

    /* centre of projection, then the eye behind it */
    centre_x = (s16)(fx + mulq(lfe, nx));
    centre_y = (s16)(fy + mulq(lfe, ny));
    centre_z = (s16)(fz + mulq(lfe, nz));
    gx = (s16)(centre_x - mulq(les, nx));
    gy = (s16)(centre_y - mulq(les, ny));
    gz = (s16)(centre_z - mulq(les, nz));

    e_les = 0;
    normalize(les, &c_les, &e_les);
    g_les = les;

    e = 0;
    normalize(centre_z, &c, &e);
    vplane_c = c;
    vplane_e = e;

    /* the zenith angle is clipped according to the height */
    max_azs = max_azs_exp[-e];
    if (azs_clip < 0) {
        max_azs = (s16)-max_azs;
        if (azs_clip < max_azs + 1) azs_clip = (s16)(max_azs + 1);
    } else if (azs_clip > max_azs) {
        azs_clip = max_azs;
    }

    sin_azs_clip = dsp_sin(azs_clip);
    cos_azs_clip = dsp_cos(azs_clip);

    inverse(cos_azs_clip, 0, &sec_azs_c1, &sec_azs_e1);
    normalize(mulq(c, sec_azs_c1), &c, &e);
    e = (s16)(e + sec_azs_e1);

    c = mulq(truncate(c, e), sin_azs_clip);
    centre_x = (s16)(centre_x + mulq(c, sin_aas));
    centre_y = (s16)(centre_y - mulq(c, cos_aas));

    out[2] = centre_x;
    out[3] = centre_y;

    if (azs != azs_clip || azs == max_azs) {
        if (azs == -32768) azs = -32767;
        c = (s16)(azs - max_azs);
        if (c >= 0) c--;
        aux = (s16)~(c * 4);

        c = mulq(aux, dsp_rom[0x328]);
        c = (s16)(mulq(c, aux) + dsp_rom[0x327]);
        vof = (s16)(vof - mulq(mulq(c, aux), les));

        c = mulq(aux, aux);
        aux = (s16)(mulq(c, dsp_rom[0x324]) + dsp_rom[0x325]);
        cos_azs_clip = (s16)(cos_azs_clip + mulq(mulq(c, aux), cos_azs_clip));
    }

    voffset = mulq(les, cos_azs_clip);

    inverse(sin_azs_clip, 0, &csec, &e);
    normalize(voffset, &c, &e);
    normalize(mulq(c, csec), &c, &e);
    if (c == -32768) {
        c >>= 1;
        e++;
    }

    out[0] = vof;
    out[1] = truncate((s16)-c, e);

    inverse(cos_azs_clip, 0, &sec_azs_c2, &sec_azs_e2);
}

static void raster(void)
{
    s16 c, e, c1, e1;

    inverse((s16)(mulq(in[0], sin_azs) + voffset), 7, &c, &e);
    e = (s16)(e + vplane_e);

    c1 = mulq(c, vplane_c);
    e1 = (s16)(e + sec_azs_e2);

    normalize(c1, &c, &e);
    c = truncate(c, e);
    out[0] = mulq(c, cos_aas);         /* A */
    out[2] = mulq(c, sin_aas);         /* C */

    normalize(mulq(c1, sec_azs_c2), &c, &e1);
    c = truncate(c, e1);
    out[1] = mulq(c, -sin_aas);        /* B */
    out[3] = mulq(c, cos_aas);         /* D */
}

static void project(void)
{
    s16 e = 0, e2 = 0, e3 = 0, e4 = 0, e6 = 0, e7 = 0, ref;
    s16 px, py, pz, c2, c4, c6, c10, c19, c25;
    s32 aux4;

    normalize_double((s32)in[0] - gx, &px, &e4);
    normalize_double((s32)in[1] - gy, &py, &e);
    normalize_double((s32)in[2] - gz, &pz, &e3);
    /* headroom for the scalar products, then a common exponent */
    px >>= 1; e4--;
    py >>= 1; e--;
    pz >>= 1; e3--;

    ref = e < e3 ? e : e3;
    if (e4 < ref) ref = e4;

    px = shift_r(px, (s16)(e4 - ref));
    py = shift_r(py, (s16)(e - ref));
    pz = shift_r(pz, (s16)(e3 - ref));

    s16 c11 = (s16)-mulq(px, nx);
    s16 c8  = (s16)-mulq(py, ny);
    s16 c9  = (s16)-mulq(pz, nz);
    s16 c12 = (s16)(c11 + c8 + c9);    /* cannot overflow */

    aux4 = c12;
    ref = (s16)(16 - ref);
    if (ref >= 0) aux4 *= 1 << ref;
    else          aux4 >>= -ref;
    if (aux4 == -1) aux4 = 0;
    aux4 >>= 1;

    /* Les minus the depth of P along the screen normal */
    normalize_double((s32)(u16)g_les + aux4, &c10, &e2);
    e2 = (s16)(15 - e2);

    inverse(c10, 0, &c4, &e4);
    c2 = mulq(c4, c_les);              /* scale factor */

    s16 c16 = mulq(px, mulq(cos_aas, 0x7FFF));
    s16 c20 = mulq(py, mulq(sin_aas, 0x7FFF));
    normalize(mulq((s16)(c16 + c20), c2), &c19, &e7);
    out[0] = truncate(c19, (s16)(e_les - e2 + ref + e7));

    s16 c21 = mulq(px, mulq(cos_azs, -sin_aas));
    s16 c22 = mulq(py, mulq(cos_azs, cos_aas));
    s16 c23 = mulq(pz, mulq(-sin_azs, 0x7FFF));
    normalize(mulq((s16)(c21 + c22 + c23), c2), &c25, &e6);
    out[1] = truncate(c25, (s16)(e_les - e2 + ref + e6));

    normalize(c2, &c6, &e4);
    out[2] = truncate(c6, (s16)(e4 + e_les - e2 - 7));
}

static void target(void)
{
    s16 h = in[0], v = in[1];
    s16 c, e, c1, e1;

    inverse((s16)(mulq(v, sin_azs) + voffset), 8, &c, &e);
    e = (s16)(e + vplane_e);

    c1 = mulq(c, vplane_c);
    e1 = (s16)(e + sec_azs_e1);

    h = (s16)(h * 256);
    normalize(c1, &c, &e);
    c = mulq(truncate(c, e), h);
    out[0] = (s16)(centre_x + mulq(c, cos_aas));
    out[1] = (s16)(centre_y + mulq(c, sin_aas));

    v = (s16)(v * 256);
    normalize(mulq(c1, sec_azs_c1), &c, &e1);
    c = mulq(truncate(c, e1), v);
    out[0] = (s16)(out[0] + mulq(c, -sin_aas));
    out[1] = (s16)(out[1] + mulq(c, cos_aas));
}

static void gyrate(void)
{
    s16 zr = in[0], xr = in[1], yr = in[2], u = in[3], f = in[4], l = in[5];
    s16 csec, esec, ctan, csin, c, e;

    inverse(dsp_cos(xr), 0, &csec, &esec);

    normalize_double((s32)((u32)(u * dsp_cos(yr)) - (u32)(f * dsp_sin(yr))), &c, &e);
    e = (s16)(esec - e);
    normalize(mulq(c, csec), &c, &e);
    out[0] = (s16)(zr + truncate(c, e));

    out[1] = (s16)(xr + mulq(u, dsp_sin(yr)) + mulq(f, dsp_cos(yr)));

    normalize_double((s32)((u32)(u * dsp_cos(yr)) + (u32)(f * dsp_sin(yr))), &c, &e);
    e = (s16)(esec - e);
    normalize(dsp_sin(xr), &csin, &e);
    ctan = mulq(csec, csin);
    normalize((s16)-mulq(c, ctan), &c, &e);
    out[2] = (s16)(yr + truncate(c, e) + l);
}

static void rotate3d(void)
{
    s16 az = in[0], ay = in[1], ax = in[2];
    s16 x = in[3], y = in[4], z = in[5], t;

    t = (s16)(mulq(y, dsp_sin(az)) + mulq(x, dsp_cos(az)));
    y = (s16)(mulq(y, dsp_cos(az)) - mulq(x, dsp_sin(az)));
    x = t;

    t = (s16)(mulq(x, dsp_sin(ay)) + mulq(z, dsp_cos(ay)));
    x = (s16)(mulq(x, dsp_cos(ay)) - mulq(z, dsp_sin(ay)));
    z = t;

    t = (s16)(mulq(z, dsp_sin(ax)) + mulq(y, dsp_cos(ax)));
    z = (s16)(mulq(z, dsp_cos(ax)) - mulq(y, dsp_sin(ax)));
    y = t;

    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* Sum of squares; the chip's 32-bit accumulator wraps. */
static s32 sq3(void)
{
    return (s32)((u32)(in[0] * in[0]) + (u32)(in[1] * in[1]) + (u32)(in[2] * in[2]));
}

static void distance(void)
{
    s32 radius = sq3();
    s16 c, e;

    if (radius == 0) {
        out[0] = 0;
        return;
    }
    normalize_double(radius, &c, &e);
    if (e & 1) c = mulq(c, 0x4000);
    int pos = c >> 9;                  /* the wrapped negative case stays in ROM */
    s16 node1 = dsp_rom[0xD5 + pos], node2 = dsp_rom[0xD6 + pos];
    out[0] = (s16)((((node2 - node1) * (c & 0x1FF) >> 9) + node1) >> (e >> 1));
}

/* Reads/writes per command, in words. Tables are indexed by the low six bits;
 * the firmware decodes those, which is why the aliases exist. */
static const u8 cmd_reads[64] = {
    2,4,7,3,2,4,3,1, 3,3,1,3,3,3,2,1,   /* 00 */
    2,4,7,3,6,4,3,1, 4,3,1,3,6,3,2,1,   /* 10 */
    2,4,7,3,2,4,3,1, 3,3,1,3,3,3,2,1,   /* 20 */
    2,4,7,3,6,4,3,1, 4,3,1,3,6,3,2,1,   /* 30 */
};
static const u16 cmd_writes[64] = {
    1,0,4,3,2,0,3,1, 2,3,4,1,2,3,2,1,      /* 00 */
    2,0,4,3,3,0,3,1024, 1,3,4,1,3,3,2,1024,/* 10 */
    1,0,4,3,2,0,3,1, 1,3,4,1,2,3,2,1,      /* 20 */
    2,0,4,3,3,0,3,1024, 1,3,4,1,3,3,2,1024,/* 30 */
};

static void execute(void)
{
    s16 (*ma)[3] = matrix[0], (*mb)[3] = matrix[1], (*mc)[3] = matrix[2];

    switch (cmd) {
    case 0x00: out[0] = mulq(in[0], in[1]); break;
    case 0x20: out[0] = (s16)(mulq(in[0], in[1]) + 1); break;
    case 0x10: case 0x30: inverse(in[0], in[1], &out[0], &out[1]); break;
    case 0x01: case 0x05: case 0x31: case 0x35: attitude(ma); break;
    case 0x11: case 0x15: attitude(mb); break;
    case 0x21: case 0x25: attitude(mc); break;
    case 0x02: case 0x12: case 0x22: case 0x32: parameter(); break;
    case 0x03: case 0x33: rotate_to_global(ma); break;
    case 0x13: rotate_to_global(mb); break;
    case 0x23: rotate_to_global(mc); break;
    case 0x04: case 0x24:
        out[0] = mulq(in[1], dsp_sin(in[0]));
        out[1] = mulq(in[1], dsp_cos(in[0]));
        break;
    case 0x06: case 0x16: case 0x26: case 0x36: project(); break;
    case 0x07: case 0x0F: out[0] = 0; break;          /* memory test passes */
    case 0x27: case 0x2F: out[0] = 0x0100; break;     /* memory size / version */
    case 0x17: case 0x1F: case 0x37: case 0x3F:
        for (int i = 0; i < 1024; i++) out[i] = dsp_rom[i];
        break;
    case 0x08: {
        u32 size = (u32)sq3() << 1;
        out[0] = (s16)size;
        out[1] = (s16)(size >> 16);
        break;
    }
    case 0x18: case 0x38: {
        s32 d = (s32)((u32)sq3() - (u32)(in[3] * in[3]));
        out[0] = (s16)((d >> 15) + (cmd == 0x38));
        break;
    }
    case 0x28: distance(); break;
    case 0x09: case 0x0D: case 0x39: case 0x3D: rotate_to_object(ma); break;
    case 0x19: case 0x1D: rotate_to_object(mb); break;
    case 0x29: case 0x2D: rotate_to_object(mc); break;
    case 0x0A: case 0x1A: case 0x2A: case 0x3A: raster(); break;
    case 0x0B: case 0x3B: scalar(ma); break;
    case 0x1B: scalar(mb); break;
    case 0x2B: scalar(mc); break;
    case 0x0C: case 0x2C:
        out[0] = (s16)(mulq(in[2], dsp_sin(in[0])) + mulq(in[1], dsp_cos(in[0])));
        out[1] = (s16)(mulq(in[2], dsp_cos(in[0])) - mulq(in[1], dsp_sin(in[0])));
        break;
    case 0x1C: case 0x3C: rotate3d(); break;
    case 0x0E: case 0x1E: case 0x2E: case 0x3E: target(); break;
    case 0x14: case 0x34: gyrate(); break;
    default: break;
    }
}

/* ---------------------------------------------------------- state machine */
static void command_done(void)
{
    dr = 0x0080;
    fsm = WAIT_COMMAND;
    sr |= SR_DRC;
}

/* One RQM handshake has completed: the SNES read or wrote a byte. */
static void step(void)
{
    switch (fsm) {
    case WAIT_COMMAND: {
        u8 c = (u8)dr;
        if (!(c & 0xC0)) {
            cmd = c;
            data_count = 0;
            fsm = READ_DATA;
            sr &= (u8)~SR_DRC;
        }
        break;
    }
    case READ_DATA:
        sr ^= SR_DRS;
        if (!(sr & SR_DRS)) {
            in[data_count++] = (s16)dr;
            if (data_count >= cmd_reads[cmd]) {
                execute();
                if (cmd_writes[cmd]) {
                    data_count = 0;
                    dr = (u16)out[0];
                    fsm = WRITE_DATA;
                } else {
                    command_done();
                }
            }
        }
        break;
    case WRITE_DATA:
        sr ^= SR_DRS;
        if (!(sr & SR_DRS)) {
            data_count++;
            if (data_count < cmd_writes[cmd]) {
                dr = (u16)out[data_count];
            } else if ((cmd & 0x0F) == 0x0A && dr != 0x8000) {
                /* Raster keeps delivering lines until the block ends on
                 * $8000, which is what games write to leave the mode. */
                in[0]++;
                execute();
                data_count = 0;
                dr = (u16)out[0];
            } else {
                command_done();
            }
        }
        break;
    default:
        break;
    }
}

void snes_dsp1_reset(void)
{
    build_tables();
    sr = SR_RQM | SR_DRC;
    dr = 0x0080;
    fsm = WAIT_COMMAND;
    cmd = 0;
    data_count = 0;
}

u8 snes_dsp1_read(int is_status)
{
    if (is_status) return sr;
    u8 v = (sr & SR_DRS) ? (u8)(dr >> 8) : (u8)dr;
    step();
    return v;
}

void snes_dsp1_write(u8 v)
{
    if (sr & SR_DRS) dr = (u16)((dr & 0x00FF) | v << 8);
    else             dr = (u16)((dr & 0xFF00) | v);
    step();
}
