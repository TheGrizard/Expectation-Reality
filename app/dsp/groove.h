/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the groove. What the knobs set and SAVE keeps (Groove), and the patterns REC writes from it:
 * each part a fixed loop of steps on its own length (polymeters), each step carrying at most one component (a
 * ratchet, a nudge, a condition, a probability, a ghost, an open hat, a pitch, an accent) that EVOLVE unlocks at a
 * point of its own. With EVOLVE at 0 the groove loops exactly; turned up, its components come in one by one, then
 * (past 70) the pattern starts to rewrite itself, a step at a time, and SAVE keeps where it has got to. */
#ifndef ER_GROOVE_H
#define ER_GROOVE_H

#include "drums.h"

enum { P_KICK, P_SNARE, P_PERC, P_HATS, NPARTS };
extern const char *const PART_NAME[NPARTS];

enum { G_MAP, G_EUCLID, G_TURING, G_BROKEN, NUM_GENS };   /* SEQ's TYPE */
extern const char *const GEN_NAME[NUM_GENS];
enum { R_16, R_8, R_32, R_16T, R_8T, NUM_RATES };          /* SEQ's RATE */
extern const char *const RATE_NAME[NUM_RATES];
extern const uint8_t RATE_TICKS[NUM_RATES];                 /* ticks a step (96 a beat) */
#define TPB 96                                              /* ticks a beat */
#define MAXLEN 32
#define MINLEN 3

/* the scales the tuned percussion plays in (SCALE's) */
#define NUM_SCALES 8
typedef struct { const char *name; uint8_t n; int8_t st[7]; } Scale;
extern const Scale SCALES[NUM_SCALES];

/* ---------------- the settings: what the knobs set, what SAVE keeps */
typedef struct {
    uint8_t engine, tone, weight, entropy;        /* EDIT: SOUND */
    uint8_t gen, len, density, rate;              /* SEQ: GROOVE (gen, rate indexes; len in steps) */
    uint8_t level, decay; int8_t pitch; uint8_t pan;   /* ENV: MIX (decay 0..100, 50 as dealt; pitch semitones; pan 0..100) */
    uint8_t s_room, s_delay, s_pan, s_duck;       /* FX: SENDS */
    uint16_t salt;                                /* the part's own share of the seed (a part dealt anew alone) */
    uint16_t muts;                                /* how many times EVOLVE has rewritten its pattern (SAVE keeps it) */
} PartP;

typedef struct {
    uint32_t seed;
    uint8_t mood, activity, evolve, drift;        /* HOME held: the macros */
    uint8_t link, warp, flow, chaos;              /* LFO: MOTION */
    uint8_t key, scale, follow, tempo;            /* GLO again: SCALE (tempo: BPM - 40; follow: the key from After Images' notes) */
    uint8_t room, comp, delay, pan;               /* FX again: the bus's amounts */
    uint8_t room_t, comp_t, delay_t, pan_t;       /* FX a third time: their types */
    uint8_t dtime, dfb, glitch, mapx;             /* FX a fourth time: DELAY's TIME (index), FEEDBACK, GLITCH; the groove map's place */
    uint8_t mapy, swing, accent, breathe;         /* swing: SWING (GLO again), 0..100: straight .. 68 %; accent: the accent
                                                   * cycle (0 none, else its length in sixteenths, 3..7); breathe: the
                                                   * velocity's slow wave and the bar's weight, 0..100: all dealt by REC */
    PartP p[NPARTS];
} Groove;

enum { RT_ROOM, RT_GATED, RT_REVERSE, RT_FREEZE, NUM_RT };
enum { CT_CLEAN, CT_SMASH, NUM_CT };
enum { DT_CLEAN, DT_GRAIN, NUM_DT };
enum { PT_SWEEP, PT_SPIN, PT_SCATTER, NUM_PT };
extern const char *const RT_NAME[NUM_RT], *const CT_NAME[NUM_CT], *const DT_NAME[NUM_DT], *const PT_NAME[NUM_PT];
#define NUM_DTIMES 8
extern const char *const DTIME_NAME[NUM_DTIMES];
extern const uint8_t DTIME_TICKS[NUM_DTIMES];

void groove_default(Groove *g);
int groove_ok(const Groove *g);
/* REC: a new groove from a seed; the macros (HOME's and MOTION's) are kept, they're yours. all: the key and tempo too */
void groove_roll(Groove *g, uint32_t seed, int all);
void groove_roll_part(Groove *g, int part, uint32_t seed);   /* SEL held + REC: that part anew */
void groove_part_engine(Groove *g, int part);                /* its engine changed: its colours dealt to suit */
void groove_name(uint32_t seed, char *out);                  /* two words: STATIC LATTICE */

/* ---------------- the patterns */
enum { C_NONE, C_RATCHET, C_NUDGE, C_COND, C_PROB, C_GHOST, C_ALT, C_PITCH, C_ACCENT, NUM_COMP };
enum { CD_1_2, CD_2_2, CD_1_4, CD_3_4, CD_FILL, CD_NOTFILL, CD_FIRST, NUM_CD };
typedef struct {
    uint8_t lvl;                                  /* how much it belongs: plays when ACTIVITY's threshold is below it */
    uint8_t vel;                                  /* 1..127 */
    int8_t pitch;                                 /* PERC: a scale degree; the others a few semitones */
    uint8_t alt;                                  /* the other sound: HATS' open hat, SNARE's short, PERC's other */
    uint8_t ct, cu, ca;                           /* its component: what, the EVOLVE it unlocks at (1..100), its value */
} Step;
typedef struct { Step s[MAXLEN]; uint8_t len, rate; uint16_t reg; } Pattern;

void pattern_make(Pattern *pt, const Groove *g, int part);   /* from the groove (its mutations replayed) */
void pattern_mutate(Pattern *pt, const Groove *g, int part, uint32_t i);   /* the i-th rewrite (the same every time) */

#endif
