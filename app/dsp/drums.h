/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the drum engines. Three to each part, each a few operations a sample, every one with the same
 * three colours as After Images' engines: WEIGHT (light and short to heavy, long and driven), ENTROPY (clean to its own
 * kind of strange) and, from MOOD, BRIGHT (dark to bright). TONE, After Images' DJ filter, is the part's, after its
 * voices. A voice is mono; the part pans it. Plain C, no hardware calls: the FM-1, the simulator and tools/render. */
#ifndef ER_DRUMS_H
#define ER_DRUMS_H

#include "dsp.h"

enum {
    /* KICK */
    D_MEMBRANE,  /* a struck skin: a falling sine and two of a drumhead's off-pitch modes; ENTROPY bends them into a growl */
    D_BOX,       /* a thump in a box: a burst of noise ringing two tuned resonances that fall; ENTROPY rings them and turns the fall upwards */
    D_FMKICK,    /* two operators, the pitch falling; ENTROPY takes the ratio off the harmonics and feeds it back on itself */
    /* SNARE */
    D_WIRES,     /* a body of two modes and the wires: noise through a short comb; ENTROPY rings the comb into metal */
    D_GRAIN,     /* noise in grains, a cluster of tiny bursts; ENTROPY scatters them into a stutter, each grain its own pitch */
    D_CRACK,     /* a crack: a click ringing three resonances; ENTROPY pulls them apart and lengthens them, glassy */
    /* PERC (tuned) */
    D_MATERIAL,  /* a struck bar: four modes, wood to metal to glass as ENTROPY rises, the glass beating and bending */
    D_FMPERC,    /* FM blips: the pitch swept, the index falling; ENTROPY: inharmonic, fed back, decimated */
    D_STRING,    /* a struck string (Karplus-Strong); ENTROPY lets it go slack, sagging, and stretches its overtones */
    /* HATS */
    D_METAL,     /* six square waves at metal's ratios, ENTROPY bending them towards the groove's own and ring-modulating */
    D_DUST,      /* crackle: noise in sparse grains through a band; ENTROPY pitches the grains and smears them, a shaker */
    D_DIGITAL,   /* digital: noise held at a low sample rate and few bits; ENTROPY: the rate jitters, the bits fall, a loop sings */
    NUM_ENGINES
};
extern const char *const ENGINE_NAME[NUM_ENGINES];
#define ENGINES_A_PART 3
static inline int engine_part(int e) { return e / ENGINES_A_PART; }   /* KICK, SNARE, PERC, HATS */

typedef struct {
    uint8_t engine;
    float note;                          /* MIDI pitch (the tuned ones; the others take it as an offset from 60) */
    float vel;                           /* 0..1 */
    float weight, entropy, bright;       /* 0..1 each */
    float decay;                         /* the length, times (MIX's DECAY, a step's open hat) */
    uint32_t seed;                       /* the hit's own (ENTROPY's randomness), and the groove's (METAL's ratios) */
    uint32_t gseed;
} HitP;

#define KS_LEN 512                       /* STRING's longest: 86 Hz (lower notes an octave up) */
typedef struct {
    uint8_t on, engine;
    uint32_t n;                          /* samples since the hit */
    float vel, w, e, b;
    float ph[6], df[6], a[6], ak[6];     /* oscillators: phase, phase step, level, decay a sample */
    float f, f0, fk;                     /* the pitch, gliding to f0 by fk a sample */
    float env, ek, nenv, nk;             /* main and noise envelopes */
    float idx, ik, fb;                   /* FM's index, its decay; feedback */
    float kill, killk;                   /* 1, or fading out over 3 ms (choked or stolen): never cut */
    float hold, hk, hv;                  /* sample-and-hold: its phase, rate, value */
    float bend, bk;                      /* a pitch bend fading (ENTROPY) */
    Svf f1, f2, f3; OnePole o1, o2;
    uint32_t ns;                         /* the noise */
    /* GRAIN: its grains, laid out at the hit */
    uint16_t g_at[10], g_len[10]; float g_amp[10]; float g_fc[10]; uint8_t g_n, g_cur;
    /* WIRES: the comb; STRING: the line */
    int cpos, clen;
    float ks_len, ks_rate, ks_lp, ks_damp, ks_ap, ks_apc;
    union { float comb[160]; int16_t ks[KS_LEN]; } line;   /* (last: hit_start clears all before it; the engines fill it) */
} Hit;

void drums_init(void);
void hit_start(Hit *h, const HitP *p);
void hit_choke(Hit *h);                  /* fade out over 3 ms */
/* add n samples into out (mono); returns 1 while it sounds */
int hit_render(Hit *h, float *out, int n);
float hit_level(const Hit *h);           /* roughly how loud it is now (stealing the quietest) */

#endif
