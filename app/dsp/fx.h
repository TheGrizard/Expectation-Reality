/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the effects. A reverb (a feedback delay network, after The Outside Inside's), a tempo-locked echo
 * that wobbles like tape, and a grain cloud that can freeze and sing an octave up into the reverb (SHIMMER). Their
 * lines are 16-bit, to fit the FM-1's RAM. */
#ifndef AI_FX_H
#define AI_FX_H

#include "dsp.h"

#define RV_LINES 8
#define RV_MAX 3200                      /* (its longest line 3119 at size 1: Expectation Reality never asks for more) */
#define RV_AP 512
typedef struct {
    int16_t buf[RV_LINES][RV_MAX]; int len[RV_LINES], pos[RV_LINES];
    float g[RV_LINES]; OnePole damp[RV_LINES];
    float ap[4][2][RV_AP]; int aplen[4], appos[4];
    float rt60, damp_hz;
} Reverb;
void reverb_init(Reverb *rv, float size);
void reverb_set(Reverb *rv, float rt60, float damp_hz);   /* (cheap: no clearing) */
void reverb_run(Reverb *rv, const float *inL, const float *inR, float *outL, float *outR, int n);

#define ECHO_MAX 32768                   /* 0.74 s */
#define SMEAR_AP 4
typedef struct {
    int16_t buf[ECHO_MAX]; int pos;
    float time_s, fb, wow_ph, wow;       /* the delay, its feedback, the tape's wobble */
    OnePole lp, hp;
    float tl, tr;                        /* the two taps, glided */
    float rev;                           /* 0..1: how far it has turned into the reverse smear (ECHO past 12 o'clock) */
    float rph;                           /* the reverse heads' phase */
    float ap[SMEAR_AP][2][1024]; int aplen[SMEAR_AP], appos[SMEAR_AP];   /* the smear: all-passes on the reversed sound */
} Echo;
void echo_init(Echo *e);
void echo_run(Echo *e, const float *inL, const float *inR, float *outL, float *outR, int n);

#define ENS_LINE 2048                    /* the send's chorus: 46 ms of line */
typedef struct { int16_t buf[ENS_LINE]; int pos; float ps, pf; float base, ds, df, fb; } Ens;   /* (base .. fb: glided) */
void ens_init(Ens *c, uint32_t seed);
/* in: mono; out: stereo, the wet alone. amt: the shared CHORUS, 0..1 */
void ens_run(Ens *c, float amt, const float *in, float *outL, float *outR, int n);

/* DIRT: tape, pushed. Up to a third, warm saturation (asymmetric, as tape's: even harmonics); to two thirds, fuzz, the
 * top end going as saturated tape loses it; past that, breakup: hiss that rises with the sound, the fuzz sputtering as
 * notes die away, and dropouts (the level dipping and dulling a moment, as worn tape lifting off the head). No digital
 * crushing. On the dry sound only: the reverb's tails stay clean */
typedef struct { Rng rng; OnePole tone[2], hiss; float env, drop, drop_t, drop_left; } Dirt;
void dirt_init(Dirt *d, uint32_t seed);
void dirt_run(Dirt *d, float character, float mix, float *L, float *R, int n);   /* character: fold .. crush .. noise; mix: how much */

/* SHIMMER (REVERB past 12 o'clock): the reverb's own sound pitched an octave up and an octave down (two heads each over a
 * short line, crossfaded) and fed back into it, so the shimmer climbs and sinks as it rings */
#define SHIM_LINE 4096
typedef struct { int16_t buf[SHIM_LINE]; int pos; float ph[2]; } Shifter;
void shifter_run(Shifter *s, const float *in, float *up, float *down, int n);

/* CHORUS (WARBLE's), the Juno's way: one short line read at two points swung by a slow triangle, the sides opposite */
typedef struct { float l1, l2, r1, r2, s1, ph, ph2; } Wear;
void wear_run(Wear *w, float amount, float *L, float *R, int n);   /* WARBLE's WEAR, on the whole mix */

#endif
