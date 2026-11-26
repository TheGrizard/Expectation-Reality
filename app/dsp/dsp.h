/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: small DSP helpers. Header-only, no allocation, no hardware calls (the same code runs on the FM-1, in
 * the host simulator and in tools/render). After The Outside Inside's dsp.h. */
#ifndef AI_DSP_H
#define AI_DSP_H

#include <stdint.h>
/* memset and memcpy: the C library's on a computer; on the FM-1 there is no string.h, and felucca-bsp's libc.c has them */
#if defined(__has_include) && __has_include(<string.h>)
#include <string.h>
#else
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
#endif
#include "fastmath.h"   /* fm1-base: the FM-1 links no libm; the same maths on the desktop */

#define SR 44100.0f
#define BLK 32                           /* samples a control step: 0.73 ms */
#define BLK_S ((float)BLK / SR)
#define TWO_PI_F 6.28318530718f
#define PI_F 3.14159265359f

/* ---------- random: xorshift32 (deterministic: a piece is a seed) ---------- */
typedef struct { uint32_t s; } Rng;
static inline void rng_seed(Rng *r, uint32_t seed)
{
    seed ^= seed >> 16; seed *= 0x7FEB352Du; seed ^= seed >> 15; seed *= 0x846CA68Bu; seed ^= seed >> 16;
    r->s = seed ? seed : 0x9E3779B9u;
}
static inline uint32_t rng_u32(Rng *r) { uint32_t x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return r->s = x; }
static inline float rng_f(Rng *r) { return (float)(rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
static inline float rng_bi(Rng *r) { return rng_f(r) * 2.0f - 1.0f; }
static inline float rng_range(Rng *r, float a, float b) { return a + (b - a) * rng_f(r); }
static inline int rng_int(Rng *r, int n) { return n > 0 ? (int)(rng_u32(r) % (uint32_t)n) : 0; }
static inline int rng_chance(Rng *r, float p) { return rng_f(r) < p; }

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float mtof(float m) { return 440.0f * fm_exp2f((m - 69.0f) * (1.0f / 12.0f)); }

/* ---------- sine table ---------- */
#define SINE_BITS 11
#define SINE_SIZE (1 << SINE_BITS)
extern float g_sine[SINE_SIZE + 1];      /* filled by dsp_init() */
void dsp_init(void);
static inline float sine_c(float ph)     /* phase in cycles, any value within +-2048 */
{
    /* one conversion to 12.20 fixed point; the wrap is the integer's own (two's complement), no floor */
    uint32_t q = (uint32_t)(int32_t)(ph * 1048576.0f), i = (q >> (20 - SINE_BITS)) & (SINE_SIZE - 1);
    float f = (float)(q & ((1u << (20 - SINE_BITS)) - 1u)) * (1.0f / (float)(1u << (20 - SINE_BITS)));
    return g_sine[i] + (g_sine[i + 1] - g_sine[i]) * f;
}

/* Denormals: a whisper, alternating, keeps every recursive state in the normal range */
static float dn_v = 1e-18f;
static inline float dn(void) { dn_v = -dn_v; return dn_v; }

/* ---------- filters ---------- */
typedef struct { float ic1, ic2, a1, a2, a3, k; } Svf;
static inline void svf_set(Svf *f, float fc, float q)
{
    if (fc > SR * 0.45f) fc = SR * 0.45f;
    if (fc < 15.0f) fc = 15.0f;
    if (q < 0.3f) q = 0.3f;
    float g = fm_tanf(PI_F * fc / SR);
    f->k = 1.0f / q;
    f->a1 = 1.0f / (1.0f + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}
/* a synth filter's resonance, tamed as the cutoff climbs: full under ~1 kHz, half by 2 kHz, a fifth by 4 kHz (a
 * resonant peak sweeping through 2-5 kHz is what's harsh on monitors) */
static inline void svf_set_res(Svf *f, float fc, float q)
{
    if (q > 0.7f) { float r = fc * (1.0f / 2000.0f); q = 0.7f + (q - 0.7f) / (1.0f + r * r); }
    svf_set(f, fc, q);
}
static inline void svf_run(Svf *f, float v0, float *lp, float *bp, float *hp)
{
    float v3 = v0 + dn() - f->ic2;
    float v1 = f->a1 * f->ic1 + f->a2 * v3;
    float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = v0 - f->k * v1 - v2;
}
static inline float svf_lp(Svf *f, float x) { float l; svf_run(f, x, &l, 0, 0); return l; }
static inline float svf_bp(Svf *f, float x) { float b; svf_run(f, x, 0, &b, 0); return b; }
static inline float svf_hp(Svf *f, float x) { float h; svf_run(f, x, 0, 0, &h); return h; }

typedef struct { float z, a; } OnePole;
static inline void op_set(OnePole *o, float fc) { if (fc > SR * 0.49f) fc = SR * 0.49f; o->a = 1.0f - fm_expf(-TWO_PI_F * fc / SR); }
static inline float op_lp(OnePole *o, float x) { o->z += o->a * (x + dn() - o->z); return o->z; }
static inline float op_hp(OnePole *o, float x) { return x - op_lp(o, x); }

/* ---------- shapers ---------- */
static inline float soft_clip(float x) { return x / (1.0f + fm_fabsf(x)); }   /* cheap and smooth */
static inline float fold(float x)        /* a sine wavefolder: past +-1 the wave folds back */
{
    return fm_sin_turns(x * 0.25f);
}

/* polyBLEP: the saw's and the square's steps rounded off, so they don't alias into whistles */
static inline float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* ---------- slow drift: a smoothed random walk (seconds to minutes) ---------- */
typedef struct { float v, target, left, len; } Drift;
static inline float drift_step(Drift *d, Rng *r, float dt)
{
    if ((d->left -= dt) <= 0.0f) { d->target = rng_bi(r); d->left = d->len * rng_range(r, 0.5f, 1.5f); }
    d->v += (d->target - d->v) * clamp01(dt * 2.0f / d->len);
    return d->v;
}

#endif
