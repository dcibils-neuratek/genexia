/*
 * combo.c - one-button combos: each character has two scripted 5-6 move
 * sequences (a "fire" and a "thunder" combo) built from the game's own
 * special-move inputs. While a combo runs it drives that player's pad, and
 * the fighter gets a flame or lightning aura (fx.c).
 *
 * Script language (one token per step, separated by spaces):
 *   D[+BTN...]:N   hold direction D for N frames, with buttons
 *                  D is numpad notation relative to facing:
 *                  7 8 9      4 = back, 6 = forward (toward the opponent),
 *                  4 5 6      2 = down, 8 = up, 5 = neutral
 *                  1 2 3
 *                  BTN: LP MP HP LK MK HK (6-button pad: X Y Z A B C)
 *   BTN[+BTN]:N    same as 5+BTN:N
 *   aNN            walk forward until the opponent is within NN pixels (max 2 s)
 *
 * RAM used (found by correlating RAM with the fighters' sprites, see
 * docs/ANALYSIS.md): player records at $FF8000 (P1) and $FF8280 (P2);
 * +$006 world X (word), +$042 health (word, 176 = full), +$1DB character.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "bus.h"
#include "combo.h"

typedef struct { s8 dir; u8 approach; u8 tunable; u16 btn; u16 frames; } Step;
typedef struct { const char *name; int fx; const char *script; } ComboDef;

/*
 * Killer combos: 8-9 fixed moves each, built only from normal punches, kicks,
 * jumps and the special-move inputs of this version (Genesis SF2' SCE):
 *   Ryu/Ken   Hadoken QCF+P, Shoryuken F,D,DF+P, Tatsumaki QCB+K
 *   E.Honda   Hundred Hand Slap tap P, Sumo Headbutt charge B,F+P
 *   Blanka    Electric Thunder tap P, Rolling Attack charge B,F+P
 *   Guile     Sonic Boom charge B,F+P, Flash Kick charge D,U+K
 *   Chun-Li   Lightning Kick tap K, Kikoken half-circle B..F+P
 *   Zangief   Spinning Clothesline PPP, Spinning Piledriver 360+P
 *   Dhalsim   Yoga Fire QCF+P, Yoga Flame half-circle B..F+P, Teleport F,D,DF+PPP/KKK
 *   Balrog    Dash Punch charge B,F+P, Dash Uppercut charge B,F+K
 *   Vega      Claw Roll charge B,F+P, Claw Dive charge D,U+K then P
 *   Sagat     Tiger Shot QCF+P, Tiger Uppercut F,D,DF+P
 *   M.Bison   Psycho Crusher charge B,F+P, Scissor Kick charge B,F+K
 * Gaps written 5:~N are tuned automatically (tools: SF2_COMBO_TUNE) against a
 * standing opponent so each next move connects; charges use crouch-back (1),
 * which charges back and down at once without walking away.
 * TAPP / TAPK expand to six rapid punch / kick presses.
 */
static const ComboDef combos[12][2] = {
    /* 0 Ryu: jump HK, cr.MK > Hadoken, cr.LK, cr.MP > Tatsumaki, Hadoken, st.HP, Shoryuken */ {
        { "Shinku Inferno", FX_FIRE,
          "a50 9:3 5:~36 HK:4 5:~20 2+MK:4 3:2 6+HP:4 5:~28 a40 2+LK:3 5:~56 2+MP:4 2:2 1:2 4+HK:4 5:~56 "
          "2:2 3:2 6+HP:4 5:~20 a36 HP:4 5:~8 6:2 2:2 3+HP:4 5:50" },
        /* cr.LK x2, cr.MP > Hadoken, st.MK, Tatsumaki, jump HP, cr.HP > Shoryuken */
        { "Thunder Storm", FX_THUNDER,
          "a36 2+LK:3 5:~36 2+LK:3 5:~14 2+MP:4 3:2 6+MP:4 5:~31 a40 MK:4 5:~60 2:2 1:2 4+MK:4 5:~30 a44 "
          "9:3 5:~16 HP:4 5:~12 2+HP:4 6:2 2:2 3+HP:4 5:50" } },
    /* 1 E.Honda: Headbutt, cr.MK, Hundred Hands, st.HP, cr.LK, Hundred Hands, Headbutt, jump HK */ {
        { "Sumo Inferno", FX_FIRE,
          "1:62 6+HP:4 5:~64 a36 2+MK:4 5:~32 TAPP 5:~20 a34 HP:4 5:~10 2+LK:3 5:~6 TAPP 5:~16 1:62 6+MP:4 "
          "5:~30 a40 9:3 5:~16 HK:4 5:40" },
        /* cr.LK, cr.MP, Hundred Hands, st.MK, Headbutt, jump HP, cr.MK, Hundred Hands */
        { "Thunder Palms", FX_THUNDER,
          "a36 2+LK:3 5:~48 2+MP:4 5:~6 TAPP 5:~48 MK:4 5:~52 1:62 6+HP:4 5:~0 a40 9:3 5:~28 HP:4 5:~36 "
          "2+MK:4 5:~6 TAPP 5:40" } },
    /* 2 Blanka: Roll, cr.MK, Electric, st.HP, Roll, jump HK, cr.LP, Electric */ {
        { "Jungle Blaze", FX_FIRE,
          "1:62 6+HP:4 5:~30 a34 2+MK:4 5:~36 TAPP 5:~16 HP:4 5:~48 1:62 6+MP:4 5:~30 a40 9:3 5:~8 HK:4 "
          "5:~12 2+LP:3 5:~6 TAPP 5:40" },
        /* cr.LK, Electric, cr.MP, Electric, Roll, st.HK, cr.MK, Electric */
        { "Electric Storm", FX_THUNDER,
          "a34 2+LK:3 5:~44 TAPP 5:~14 2+MP:4 5:~52 TAPP 5:~14 1:62 6+HP:4 5:~30 a40 HK:4 5:~10 2+MK:4 5:~6 "
          "TAPP 5:40" } },
    /* 3 Guile: Sonic Boom, Flash Kick, jump HK, cr.MK, Sonic Boom, cr.LP, st.HP, Flash Kick */ {
        { "Sonic Inferno", FX_FIRE,
          "1:62 6+HP:4 1:56 9+HK:4 5:~59 a44 9:3 5:~53 HK:4 5:~29 2+MK:4 5:~11 1:62 6+MP:4 5:~6 a40 2+LP:3 "
          "5:~40 HP:4 5:~8 1:62 9+HK:4 5:50" },
        /* cr.LK, cr.MP, Sonic Boom, Flash Kick, st.MK, cr.HK, Sonic Boom, Flash Kick */
        { "Flash Thunder", FX_THUNDER,
          "a40 2+LK:3 5:~48 2+MP:4 5:~52 1:62 6+MP:4 5:~5 1:56 9+MK:4 5:~40 a44 MK:4 5:~10 2+HK:4 5:~30 "
          "1:62 6+HP:4 5:~6 1:56 9+HK:4 5:50" } },
    /* 4 Ken: same strings as Ryu (his Tatsumaki hits more often) */ {
        { "Dragon Inferno", FX_FIRE,
          "a50 9:3 5:~64 HK:4 5:~64 2+MK:4 3:2 6+HP:4 5:~28 a40 2+LK:3 5:~12 2+MP:4 2:2 1:2 4+HK:4 5:~0 2:2 "
          "3:2 6+HP:4 5:~26 a36 HP:4 5:~8 6:2 2:2 3+HP:4 5:50" },
        { "Thunder Dragon", FX_THUNDER,
          "a36 2+LK:3 5:~35 2+LK:3 5:~11 2+MP:4 3:2 6+MP:4 5:~28 a40 MK:4 5:~45 2:2 1:2 4+MK:4 5:~30 a44 "
          "9:3 5:~16 HP:4 5:~12 2+HP:4 6:2 2:2 3+HP:4 5:50" } },
    /* 5 Chun-Li: Kikoken, cr.MK, Lightning Kick, st.HP, cr.LK, Lightning Kick, Kikoken, jump HK */ {
        { "Phoenix Legs", FX_FIRE,
          "4:2 1:2 2:2 3:2 6+HP:4 5:~48 a36 2+MK:4 5:~6 TAPK 5:~16 a36 HP:4 5:~28 2+LK:3 5:~9 TAPK 5:~22 "
          "4:2 1:2 2:2 3:2 6+MP:4 5:~26 a40 9:3 5:~48 HK:4 5:40" },
        /* cr.LK, Lightning Kick, cr.MP, Kikoken, st.MK, Lightning Kick, cr.HK, Kikoken */
        { "Thunder Kicks", FX_THUNDER,
          "a36 2+LK:3 5:~56 TAPK 5:~16 2+MP:4 5:~8 4:2 1:2 2:2 3:2 6+HP:4 5:~26 a40 MK:4 5:~41 TAPK 5:~16 "
          "2+HK:4 5:~30 4:2 1:2 2:2 3:2 6+LP:4 5:40" } },
    /* 6 Zangief: st.HP, cr.MK, Clothesline, cr.LK, st.MP, Clothesline, Piledriver, jump HK */ {
        { "Red Cyclone", FX_FIRE,
          "a24 HP:4 5:~48 2+MK:4 5:~8 LP+MP+HP:4 5:~52 a20 2+LK:3 5:~6 MP:4 5:~10 LP+MP+HP:4 5:~30 a16 6:2 "
          "3:2 2:2 1:2 4:2 7+HP:3 5:~40 a30 9:3 5:~16 HK:4 5:40" },
        /* cr.LK x2, cr.MP, Clothesline, st.HK, Piledriver, cr.HK, Clothesline */
        { "Siberian Thunder", FX_THUNDER,
          "a24 2+LK:3 5:~28 2+LK:3 5:~12 2+MP:4 5:~40 LP+MP+HP:4 5:~30 a20 HK:4 5:~60 6:2 3:2 2:2 1:2 4:2 "
          "7+HP:3 5:~40 a30 2+HK:4 5:~40 LP+MP+HP:4 5:40" } },
    /* 7 Dhalsim: Yoga Fire, Yoga Flame, Teleport, cr.MK, Yoga Fire, st.HP, Yoga Flame, slide */ {
        { "Yoga Inferno", FX_FIRE,
          "2:2 3:2 6+HP:4 5:~18 a50 4:2 1:2 2:2 3:2 6+HP:4 5:~34 6:2 2:2 3+LP+MP+HP:4 5:~60 a40 2+MK:4 "
          "5:~64 2:2 3:2 6+MP:4 5:~26 a46 HP:4 5:~36 4:2 1:2 2:2 3:2 6+MP:4 5:~30 2+HK:4 5:40" },
        /* cr.LK, Yoga Fire, st.MK, Yoga Flame, Teleport, Yoga Fire, slide, Yoga Fire */
        { "Yoga Thunder", FX_THUNDER,
          "a50 2+LK:3 5:~8 2:2 3:2 6+LP:4 5:~56 a46 MK:4 5:~20 4:2 1:2 2:2 3:2 6+HP:4 5:~44 6:2 2:2 "
          "3+LK+MK+HK:4 5:~53 2:2 3:2 6+HP:4 5:~26 a50 2+HK:4 5:~20 2:2 3:2 6+MP:4 5:40" } },
    /* 8 M.Bison: Psycho Crusher, cr.MK, Scissor Kick, st.HP, cr.LK, Psycho Crusher, jump HK, Scissor Kick */ {
        { "Psycho Inferno", FX_FIRE,
          "1:62 6+HP:4 5:~56 a40 2+MK:4 5:~28 1:62 6+HK:4 5:~60 a36 HP:4 5:~10 2+LK:3 5:~6 1:62 6+MP:4 "
          "5:~48 a40 9:3 5:~16 HK:4 5:~12 1:62 6+MK:4 5:40" },
        /* cr.LK, cr.MP, Scissor Kick, st.MK, Psycho Crusher, cr.HK, Scissor Kick, Psycho Crusher */
        { "Psycho Thunder", FX_THUNDER,
          "a36 2+LK:3 5:~28 2+MP:4 5:~52 1:62 6+MK:4 5:~44 a40 MK:4 5:~40 1:62 6+HP:4 5:~52 a40 2+HK:4 "
          "5:~24 1:62 6+HK:4 5:~26 1:62 6+LP:4 5:40" } },
    /* 9 Sagat: Tiger Shot x2, jump HK, cr.MK > Tiger Shot, st.HP, cr.LK, Tiger Uppercut */ {
        { "Tiger Blaze", FX_FIRE,
          "2:2 3:2 6+HP:4 5:~16 2:2 3:2 6+MP:4 5:~32 a40 9:3 5:~40 HK:4 5:~12 2+MK:4 3:2 6+HP:4 5:~16 a36 "
          "HP:4 5:~10 2+LK:3 5:~6 6:2 2:2 3+HP:4 5:50" },
        /* cr.LK x2, cr.MP > Tiger Shot, st.MK, cr.HK, Tiger Shot, st.HP, Tiger Uppercut */
        { "Tiger Thunder", FX_THUNDER,
          "a36 2+LK:3 5:~20 2+LK:3 5:~32 2+MP:4 3:2 6+HP:4 5:~26 a36 MK:4 5:~44 2+HK:4 5:~24 2:2 3:2 6+LP:4 "
          "5:~28 a36 HP:4 5:~8 6:2 2:2 3+HP:4 5:50" } },
    /* 10 Balrog: Dash Punch, cr.MP, Dash Uppercut, st.HP, cr.LP, Dash Punch, jump HP, Dash Uppercut */ {
        { "Buffalo Blaze", FX_FIRE,
          "1:62 6+HP:4 5:~42 a30 2+MP:4 5:~48 1:62 6+HK:4 5:~60 a30 HP:4 5:~32 2+LP:3 5:~6 1:62 6+MP:4 "
          "5:~26 a36 9:3 5:~56 HP:4 5:~12 1:62 6+HK:4 5:40" },
        /* cr.LP x2, cr.MP, Dash Punch, st.HP, Dash Uppercut, cr.HP, Dash Punch */
        { "Buffalo Thunder", FX_THUNDER,
          "a30 2+LP:3 5:~28 2+LP:3 5:~28 2+MP:4 5:~8 1:62 6+MP:4 5:~26 a30 HP:4 5:~60 1:62 6+MK:4 5:~26 a30 "
          "2+HP:4 5:~36 1:62 6+HP:4 5:40" } },
    /* 11 Vega: Claw Roll, cr.MK, Claw Dive, st.HP, cr.LK, Claw Roll, jump HK, cr.HK */ {
        { "Scarlet Flame", FX_FIRE,
          "1:62 6+HP:4 5:~4 a34 2+MK:4 5:~8 2:62 8+HK:4 5:~16 HP:4 5:~30 a34 HP:4 5:~52 2+LK:3 5:~6 1:62 "
          "6+MP:4 5:~52 a40 9:3 5:~16 HK:4 5:~12 2+HK:4 5:40" },
        /* cr.LK, cr.MP, Claw Roll, st.MK, Claw Dive, jump HK, cr.HK, Claw Roll */
        { "Claw Thunder", FX_THUNDER,
          "a34 2+LK:3 5:~56 2+MP:4 5:~24 1:62 6+HP:4 5:~30 a34 MK:4 5:~10 2:62 8+MK:4 5:~22 MP:4 5:~30 a40 "
          "9:3 5:~16 HK:4 5:~12 2+HK:4 5:~24 1:62 6+LP:4 5:40" } },
};

/* ------------------------------------------------------------ parsing */
static Step parsed[12][2][96];
static int nsteps[12][2];

static u16 btn_bits(const char *s, int n) {
    if (n == 2 && !strncmp(s, "LP", 2)) return PAD_X;
    if (n == 2 && !strncmp(s, "MP", 2)) return PAD_Y;
    if (n == 2 && !strncmp(s, "HP", 2)) return PAD_Z;
    if (n == 2 && !strncmp(s, "LK", 2)) return PAD_A;
    if (n == 2 && !strncmp(s, "MK", 2)) return PAD_B;
    if (n == 2 && !strncmp(s, "HK", 2)) return PAD_C;
    return 0;
}

static int parse(const char *src, Step *out) {
    int n = 0;
    const char *p = src;
    while (*p && n < 90) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (!strncmp(p, "TAPP", 4) || !strncmp(p, "TAPK", 4)) {
            /* six rapid presses of light/medium/heavy, released in between */
            u16 b3[3];
            if (p[3] == 'P') { b3[0] = PAD_X; b3[1] = PAD_Y; b3[2] = PAD_Z; }
            else             { b3[0] = PAD_A; b3[1] = PAD_B; b3[2] = PAD_C; }
            for (int i = 0; i < 6 && n < 88; i++) {
                out[n++] = (Step){ 5, 0, 0, b3[i % 3], 2 };
                out[n++] = (Step){ 5, 0, 0, 0, 2 };
            }
        } else {
            Step st = { 5, 0, 0, 0, 1 };
            if (*p == 'a') {
                st.approach = (u8)atoi(p + 1); st.frames = 120; st.dir = 6;
            } else {
                const char *q = p;
                if (*q >= '1' && *q <= '9') { st.dir = (s8)(*q - '0'); q++; if (*q == '+') q++; }
                while (*q && *q != ':' && *q != ' ') {
                    const char *e = q;
                    while (*e && *e != '+' && *e != ':' && *e != ' ') e++;
                    st.btn |= btn_bits(q, (int)(e - q));
                    q = *e == '+' ? e + 1 : e;
                }
                if (*q == ':') {
                    q++;
                    if (*q == '~') { st.tunable = 1; q++; }
                    st.frames = (u16)atoi(q);
                }
            }
            out[n++] = st;
        }
        while (*p && *p != ' ') p++;
    }
    return n;
}

static void ensure_parsed(void) {
    static int done;
    if (done) return;
    for (int c = 0; c < 12; c++)
        for (int k = 0; k < 2; k++) nsteps[c][k] = parse(combos[c][k].script, parsed[c][k]);
    done = 1;
}

/* ------------------------------------------------------------- engine */
typedef struct { int active, ch, which, step, left, total, elapsed; } Run;
static Run run[2];
int combo_fx_frames[2];       /* frames of aura left (fades out after the combo) */
int combo_fx_type[2];
int combo_flash[2];           /* start-of-combo flash frames */
int combo_hits[2];            /* hits landed by the current combo */
int combo_hit_pop[2];         /* frames since the last hit (counter pop animation) */
static int opp_hp_last[2];

/* Mana: a combo needs a full meter and empties it. The meter fills over
 * combo_mana_seconds of fighting (game time, so it follows the speed
 * setting), pauses while a combo runs and restarts empty every round. */
double combo_mana[2];               /* 0..1 */
int combo_mana_seconds = 10;
int combo_mana_enabled = 1;
int combo_denied[2];                /* frames left of the "NOT READY" flash */
int combo_pad_hint[2];              /* 1 = this player uses a gamepad: show pad buttons */
static int prev_hp[2] = { -1, -1 };

int combo_in_fight(void) {
    if (!game_is_sf2) return 0;
    int h1 = combo_player_hp(0), h2 = combo_player_hp(1);
    return vdp_native_w == 256 && h1 > 0 && h1 <= 176 && h2 > 0 && h2 <= 176;
}

static inline int rw(u32 a) { return (s16)(ram[a & 0xFFFF] << 8 | ram[(a + 1) & 0xFFFF]); }
int combo_player_x(int p) { return rw(0x8006 + (u32)p * 0x280); }
int combo_player_hp(int p) { return rw(0x8042 + (u32)p * 0x280); }
int combo_player_char(int p) { return ram[(0x81DB + (u32)p * 0x280) & 0xFFFF]; }

static int run_which[2];
const char *combo_current_name(int p) { return combo_name(p, run_which[p]); }
const char *combo_name(int p, int which) {
    if (!game_is_sf2) return "";
    int c = combo_player_char(p);
    return c < 12 ? combos[c][which & 1].name : "";
}

void combo_start(int p, int which) {
    ensure_parsed();
    int c = combo_player_char(p);
    /* only during a fight: 256-px fight screen mode and both health bars valid */
    if (c >= 12 || run[p].active || !combo_in_fight()) return;
    if (combo_mana_enabled) {
        if (combo_mana[p] < 1.0) { combo_denied[p] = 30; return; }
        combo_mana[p] = 0;
    }
    run[p] = (Run){ 1, c, which & 1, 0, parsed[c][which & 1][0].frames, 0, 0 };
    run_which[p] = which & 1;
    combo_fx_type[p] = combos[c][which & 1].fx;
    combo_flash[p] = 6;
    combo_hits[p] = 0;
    combo_hit_pop[p] = 99;
    opp_hp_last[p] = combo_player_hp(p ^ 1);
}

void combo_cancel(int p) { run[p].active = 0; }
int combo_running(int p) { return run[p].active; }

static u16 dir_bits(int d, int facing_right) {
    u16 b = 0;
    int h = (d - 1) % 3, v = (d - 1) / 3;        /* h: 0 back,1 none,2 fwd; v: 0 down,1 mid,2 up */
    if (v == 0) b |= PAD_DOWN;
    if (v == 2) b |= PAD_UP;
    if (h == 0) b |= facing_right ? PAD_LEFT : PAD_RIGHT;
    if (h == 2) b |= facing_right ? PAD_RIGHT : PAD_LEFT;
    return b;
}

/* called once per real frame with the player's live pad; returns the pad to use */
u16 combo_input(int p, u16 live) {
    if (!game_is_sf2) return live;
    /* mana: restart empty when a new round begins (both health bars refill) */
    int hp_me = combo_player_hp(p), hp_opp = combo_player_hp(p ^ 1);
    if (prev_hp[p] >= 0 && hp_me == 176 && hp_opp == 176 && (prev_hp[p] < 176 || opp_hp_last[p] < 176)) combo_mana[p] = 0;
    prev_hp[p] = hp_me;
    if (combo_in_fight() && !run[p].active && combo_mana[p] < 1.0) {
        combo_mana[p] += 1.0 / (combo_mana_seconds * 60.0);
        if (combo_mana[p] > 1.0) combo_mana[p] = 1.0;
    }
    if (combo_denied[p] > 0) combo_denied[p]--;
    if (!combo_fx_frames[p]) opp_hp_last[p] = hp_opp;
    if (combo_fx_frames[p] > 0) {
        int hp = combo_player_hp(p ^ 1);           /* count hits while the aura is up */
        if (hp < opp_hp_last[p]) { combo_hits[p]++; combo_hit_pop[p] = 0; }
        opp_hp_last[p] = hp;
        if (combo_hit_pop[p] < 99) combo_hit_pop[p]++;
    }
    if (combo_fx_frames[p] > 0) combo_fx_frames[p]--;
    if (combo_flash[p] > 0) combo_flash[p]--;
    Run *r = &run[p];
    if (!r->active) return live;
    /* the player can always break out with Start */
    if (live & PAD_START) { r->active = 0; return live; }
    combo_fx_frames[p] = 40;                       /* keep the aura up + 40 frames of fade */
    const Step *st = &parsed[r->ch][r->which][r->step];
    int me = combo_player_x(p), opp = combo_player_x(p ^ 1);
    int facing_right = opp >= me;
    u16 out = dir_bits(st->dir, facing_right) | st->btn;
    r->elapsed++;
    int next = 0;
    if (st->approach) {
        if (abs(opp - me) <= st->approach || --r->left <= 0) next = 1;
    } else if (--r->left <= 0) next = 1;
    if (next) {
        if (++r->step >= nsteps[r->ch][r->which]) r->active = 0;
        else r->left = parsed[r->ch][r->which][r->step].frames;
    }
    return out;
}

/* ------------------------------------------------------- tuner support */
int combo_tunables(int ch, int which, int *idx, int max) {
    ensure_parsed();
    int n = 0;
    for (int i = 0; i < nsteps[ch][which] && n < max; i++) if (parsed[ch][which][i].tunable) idx[n++] = i;
    return n;
}
int  combo_step_frames(int ch, int which, int i) { ensure_parsed(); return parsed[ch][which][i].frames; }
void combo_set_step_frames(int ch, int which, int i, int frames) { ensure_parsed(); parsed[ch][which][i].frames = (u16)frames; }
/* print the script with the current tuned gap values (for baking back in) */
void combo_print_tuned(int ch, int which, FILE *f) {
    ensure_parsed();
    const char *src = combos[ch][which].script;
    int step = 0;
    const char *p = src;
    fprintf(f, "\"");
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (!strncmp(p, "TAPP", 4) || !strncmp(p, "TAPK", 4)) { fprintf(f, "%.*s ", (int)(e - p), p); step += 12; }
        else {
            const char *t = memchr(p, '~', (size_t)(e - p));
            if (t) fprintf(f, "%.*s%d ", (int)(t + 1 - p), p, parsed[ch][which][step].frames);
            else fprintf(f, "%.*s ", (int)(e - p), p);
            step++;
        }
        p = e;
    }
    fprintf(f, "\"\n");
}
