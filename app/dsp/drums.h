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
    /* added later, at the end (so the others keep their numbers) */
    D_808,       /* KICK: a soft, round kick: a sine, a gentle fall, a softened front; ENTROPY: dust, a deeper fall, warmth */
    D_CLAP,      /* SNARE: a synthesised clap: bursts of noise a few ms apart and a tail; ENTROPY loosens and roughens them */
    D_VOX,       /* HATS: almost a voice: breath and a buzz through three vowel formants, each hit its own pitch and vowel,
                  * very short; ENTROPY: wider pitch jumps, the vowel sliding */
    D_CLICK,     /* HATS: impulse clicks through a resonance: a dry tick, then (ENTROPY) a ping whose tone moves hit to hit,
                  * then a little burst of clicks */
    D_FOLEY,     /* PERC: found sounds: a drop, a knock, a snap, keys; ENTROPY moves from one to the next, each hit a little
                  * different; WEIGHT: a bigger object */
    D_BENT,      /* SNARE (and anywhere): a circuit-bent toy, two oscillators wired into each other, pitch jumps, a broken
                  * sample rate; WEIGHT and ENTROPY rewire it: every setting its own wild sound */
    /* the twenty more (v0.13), five a part. Their bodies take ENTROPY's whole range (the older ones a little over half:
     * theirs is mostly the front) */
    D_PING,      /* KICK: a resonant filter struck (the analogue kick's bridged-T), the pitch falling; ENTROPY drives it and
                  * rings a clanging second resonance; WEIGHT: longer, lower */
    D_FOLD,      /* KICK: a falling sine through a wavefolder; ENTROPY folds it further and lopsided, the folds opening and
                  * closing; WEIGHT: deeper, longer */
    D_TAPE,      /* KICK: a kick on worn tape: saturated, the highs rolled off; ENTROPY: wow, flutter, dropouts, hiss */
    D_THUD,      /* KICK: a felt thump, its air and its skin; ENTROPY: felt to cardboard (a boxy ring, a slap) */
    D_RUMBLE,    /* KICK: a distorted kick and its rumble (two low tones beating, darkened); ENTROPY: more drive and rumble;
                  * WEIGHT: a longer rumble */
    D_RIM,       /* SNARE: a rim click: two short resonances; ENTROPY: wood to metal (ringing, a third mode); WEIGHT: body */
    D_SNAP,      /* SNARE: a finger snap, a pop in a band of noise; ENTROPY: flammed, two or three snaps; WEIGHT: a tail */
    D_BRUSH,     /* SNARE: a brush swished: noise swelling through a sweeping band; ENTROPY: bristles and grit, a faster
                  * sweep; WEIGHT: a longer stroke */
    D_SYNTH,     /* SNARE: the drum machine's: a tuned body and a noise snap; ENTROPY: more snap, then crushed;
                  * WEIGHT: a bigger body */
    D_SPRING,    /* SNARE: a snare through a spring: a burst boinging round a dispersive loop; ENTROPY: more boing and
                  * chirp; WEIGHT: longer, lower */
    D_BLOCK,     /* PERC: a wood block, a knock in a hollow; ENTROPY: towards a slit drum, its two tongues a fourth or
                  * fifth apart, each hit either; WEIGHT: longer, lower */
    D_TABLA,     /* PERC: a membrane's harmonic modes (a tabla's); ENTROPY: damped to ringing (the rim's tones);
                  * WEIGHT: the bass drum, a quick dip of pitch */
    D_HAND,      /* PERC: a hand drum: open, muted, then (ENTROPY) a slap; WEIGHT: bongo to tumba */
    D_TINE,      /* PERC: a plucked tine (a kalimba's); ENTROPY: a buzzing rattle and a beating detune; WEIGHT: longer */
    D_BOTTLE,    /* PERC: a struck bottle, glass partials over its note; ENTROPY: water in it (a bubbling wobble);
                  * WEIGHT: a bigger bottle, breath in it */
    D_SHAKER,    /* HATS: a shaker: noise swelling, its beads rattling; ENTROPY: more beads, grittier, wider; WEIGHT:
                  * a longer shake */
    D_FMHAT,     /* HATS: FM: a high carrier, an inharmonic modulator; ENTROPY: further off and fed back; WEIGHT: open */
    D_AIR,       /* HATS: a breath; ENTROPY: tightening into a whistle that wavers; WEIGHT: longer, fuller */
    D_CHIP,      /* HATS: an 8-bit console's noise channel: hiss, then (ENTROPY) its short loop's metallic tone, stepping
                  * down, the level in sixteen steps; WEIGHT: longer, lower */
    D_FOIL,      /* HATS: tin foil crinkled: tiny clicks through two high resonances, thinning; ENTROPY: denser, ringing
                  * (crinkle to tinkle); WEIGHT: a longer crinkle */
    NUM_ENGINES
};
extern const char *const ENGINE_NAME[NUM_ENGINES];
extern const uint8_t ENGINE_PART[NUM_ENGINES];   /* each engine's part: KICK, SNARE, PERC, HATS */
static inline int engine_part(int e) { return ENGINE_PART[e % NUM_ENGINES]; }

typedef struct {
    uint8_t engine;
    float note;                          /* MIDI pitch (the tuned ones; the others take it as an offset from 60) */
    float vel;                           /* 0..1 */
    float weight, entropy, bright;       /* 0..1 each */
    float decay;                         /* the length, times (MIX's DECAY, a step's open hat) */
    float shift;                         /* semitones it was moved to play on another part's lane (0 on its own) */
    float soft;                          /* MOOD's soft side: 0 a hard front, 1 a soft one (the attack eased, the exciter lower) */
    uint32_t seed;                       /* the hit's own (ENTROPY's randomness), and the groove's (METAL's ratios) */
    uint32_t gseed;
} HitP;

#define KS_LEN 512                       /* STRING's longest: 86 Hz (lower notes an octave up) */
typedef struct {
    uint8_t on, engine;
    uint32_t n;                          /* samples since the hit */
    float vel, w, e, b, vr;              /* (vr: the velocity it was played at) */
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
    /* the front: every engine's attack and its character through the rest of the hit, from ENTROPY (what kind) and WEIGHT
     * (how wide and long it spreads), stretched as the hit gets longer: a softened start, a click that leaves the body
     * ringing, a noise snap that leaves a rasp, a zap that keeps diving, a ring-modulated edge, a crushed stutter */
    struct { Svf f, f2; float click, nzv, nzk, zf, zf0, zf1, zk, za, zak, zph, rm, rmf, rmk, rph, rdf, cr, crf, crk, crr, cph, cv,
             gph, gdf, gs, fol, tn, tz, tres, zt, xg; uint16_t atk, len, zper, zcnt; uint8_t ring, tail; } fr;   /* (xg: the tail fading out) */
    union { float comb[160]; int16_t ks[KS_LEN]; } line;   /* (last: hit_start clears all before it; the engines fill it) */
} Hit;

void drums_init(void);
void hit_start(Hit *h, const HitP *p);
void hit_choke(Hit *h);                  /* fade out over 3 ms */
/* add n samples into out (mono); returns 1 while it sounds */
int hit_render(Hit *h, float *out, int n);
float hit_level(const Hit *h);           /* roughly how loud it is now (stealing the quietest) */

#endif
