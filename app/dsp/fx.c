/* SPDX-License-Identifier: MPL-2.0 */
/* After Images: the effects (fx.h). */
#include "fx.h"

/* ------------------------------------------------------------------ reverb */
void reverb_init(Reverb *rv, float size)
{
    static const int base[RV_LINES] = { 1031, 1327, 1523, 1801, 2063, 2357, 2731, 3119 };
    static const int apb[4] = { 142, 107, 379, 277 };
    memset(rv, 0, sizeof *rv);
    for (int i = 0; i < RV_LINES; i++) {
        int n = (int)((float)base[i] * size); if (n >= RV_MAX) n = RV_MAX - 1; if (n < 64) n = 64;
        rv->len[i] = n;
    }
    for (int i = 0; i < 4; i++) { int n = (int)((float)apb[i] * size); if (n >= RV_AP) n = RV_AP - 1; rv->aplen[i] = n; }
    reverb_set(rv, 6.0f, 5000.0f);
}

void reverb_set(Reverb *rv, float rt60, float damp_hz)
{
    if (fm_fabsf(rt60 - rv->rt60) < 0.05f && fm_fabsf(damp_hz - rv->damp_hz) < 20.0f) return;
    rv->rt60 = rt60; rv->damp_hz = damp_hz;
    for (int i = 0; i < RV_LINES; i++) {
        rv->g[i] = fm_powf(10.0f, -3.0f * (float)rv->len[i] / (rt60 * SR));
        op_set(&rv->damp[i], damp_hz);
    }
}

static inline int16_t to16(float x) { x *= 16384.0f; if (x > 32767.0f) x = 32767.0f; if (x < -32768.0f) x = -32768.0f; return (int16_t)x; }

void reverb_run(Reverb *rv, const float *inL, const float *inR, float *outL, float *outR, int n)
{
    for (int i = 0; i < n; i++) {
        float in[2] = { inL[i], inR[i] };
        for (int a = 0; a < 4; a++) {                    /* diffusion */
            int p = rv->appos[a];
            for (int c = 0; c < 2; c++) {
                float d = rv->ap[a][c][p], y = -0.6f * in[c] + d;
                rv->ap[a][c][p] = in[c] + 0.6f * y;
                in[c] = y;
            }
            rv->appos[a] = p + 1 >= rv->aplen[a] ? 0 : p + 1;
        }
        float o[RV_LINES], h[RV_LINES];
        for (int k = 0; k < RV_LINES; k++) { o[k] = op_lp(&rv->damp[k], (float)rv->buf[k][rv->pos[k]] * (1.0f / 16384.0f)) * rv->g[k]; h[k] = o[k]; }
        for (int len = 1; len < RV_LINES; len <<= 1)     /* Hadamard */
            for (int j = 0; j < RV_LINES; j += len << 1)
                for (int k = j; k < j + len; k++) { float a = h[k], b = h[k + len]; h[k] = a + b; h[k + len] = a - b; }
        for (int k = 0; k < RV_LINES; k++) {
            float x = h[k] * 0.35355339f + ((k & 1) ? in[1] : in[0]) * ((k & 2) ? -0.5f : 0.5f);
            rv->buf[k][rv->pos[k]] = to16(x);
            rv->pos[k] = rv->pos[k] + 1 >= rv->len[k] ? 0 : rv->pos[k] + 1;
        }
        outL[i] = o[0] - o[2] + o[4] - o[6];
        outR[i] = o[1] - o[3] + o[5] - o[7];
    }
}

/* ------------------------------------------------------------------ echo: one line, two taps (the right 3/2 the left's) */
void echo_init(Echo *e)
{
    memset(e, 0, sizeof *e);
    op_set(&e->lp, 3500.0f); op_set(&e->hp, 180.0f);
    e->time_s = 0.4f; e->fb = 0.4f; e->tl = e->tr = 0.4f * SR;
    static const int AP[SMEAR_AP] = { 347, 563, 797, 1009 };
    for (int a = 0; a < SMEAR_AP; a++) e->aplen[a] = AP[a];
}

static inline float tap(const Echo *e, float d)
{
    float p = (float)e->pos - d;
    while (p < 0) p += (float)ECHO_MAX;
    int i = (int)p; float f = p - (float)i; int j = i + 1 >= ECHO_MAX ? 0 : i + 1;
    return ((float)e->buf[i] + ((float)e->buf[j] - (float)e->buf[i]) * f) * (1.0f / 16384.0f);
}

/* the reverse heads: each reads backwards through the last window while the line moves on, so its delay grows from 0
 * to twice the window; two of them half a window apart, each faded in and out (Hann), sum to a steady reversed sound */
static inline float rev_head(const Echo *e, float ph, float win)
{
    float d = 2.0f * win * ph + 1.0f;
    return tap(e, d) * (0.5f - 0.5f * sine_c(ph + 0.25f));
}

void echo_run(Echo *e, const float *inL, const float *inR, float *outL, float *outR, int n)
{
    if (e->rev > 0.001f) {                               /* ECHO past 12: the echo turns into a reversed, smeared wash */
        float win = clampf(e->time_s * SR * 1.5f, 3000.0f, (float)ECHO_MAX * 0.45f), rv = e->rev;
        float dl = clampf(e->time_s * SR, 200.0f, (float)ECHO_MAX / 1.6f), dr = dl * 1.5f;
        for (int i = 0; i < n; i++) {
            e->tl += (dl - e->tl) * 0.0005f; e->tr += (dr - e->tr) * 0.0005f;
            float fl = tap(e, e->tl), fr = tap(e, e->tr);              /* the forward echo, fading out */
            e->rph += 1.0f / win; if (e->rph >= 1.0f) e->rph -= 1.0f;
            float p2 = e->rph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
            float r = rev_head(e, e->rph, win) + rev_head(e, p2, win);
            float sm[2] = { r, r };                                    /* the smear: all-passes, the sides apart */
            for (int a = 0; a < SMEAR_AP; a++) {
                int p = e->appos[a];
                for (int c = 0; c < 2; c++) {
                    float g = 0.45f + 0.25f * rv, dd = e->ap[a][c][(p + c * 37) % e->aplen[a]];
                    float y = -g * sm[c] + dd;
                    e->ap[a][c][p] = sm[c] + g * y;
                    sm[c] = sm[c] * (1.0f - rv) + y * rv;
                }
                e->appos[a] = p + 1 >= e->aplen[a] ? 0 : p + 1;
            }
            float x = 0.5f * (inL[i] + inR[i]) + e->fb * ((1.0f - rv) * (0.6f * fl + 0.4f * fr) + rv * 0.7f * r);
            x = op_hp(&e->hp, op_lp(&e->lp, x));
            e->buf[e->pos] = to16(soft_clip(x));
            e->pos = e->pos + 1 >= ECHO_MAX ? 0 : e->pos + 1;
            outL[i] = fl * (1.0f - rv) + sm[0] * rv * 1.2f;
            outR[i] = fr * (1.0f - rv) + sm[1] * rv * 1.2f;
        }
        return;
    }
    float dl = clampf(e->time_s * SR, 200.0f, (float)ECHO_MAX / 1.6f), dr = dl * 1.5f;
    e->wow_ph += (float)n / SR * 0.37f; if (e->wow_ph >= 1.0f) e->wow_ph -= 1.0f;
    float w = e->wow * 40.0f * fm_sin_turns(e->wow_ph);
    for (int i = 0; i < n; i++) {
        e->tl += (dl + w - e->tl) * 0.0005f;              /* the taps glide: a change of time pitches the echoes */
        e->tr += (dr + w - e->tr) * 0.0005f;
        float l = tap(e, e->tl), r = tap(e, e->tr);
        float x = 0.5f * (inL[i] + inR[i]) + e->fb * (0.6f * l + 0.4f * r);
        x = op_hp(&e->hp, op_lp(&e->lp, x));
        e->buf[e->pos] = to16(soft_clip(x));
        e->pos = e->pos + 1 >= ECHO_MAX ? 0 : e->pos + 1;
        outL[i] = l; outR[i] = r;
    }
}

/* ------------------------------------------------------------------ chorus (the send's): an ensemble
 * Three voices reading one short line, each swung by a slow and a fast sine a third of a turn apart (the string
 * machines' ensemble), left, middle and right. Up to 12 o'clock a chorus, gentle to lush; past it slower, deeper and
 * wider, the voices fed back a little: a thick ensemble. Its line moves smoothly, never jumps: no clicks */
void ens_init(Ens *c, uint32_t seed)
{
    memset(c, 0, sizeof *c);
    c->ps = (float)(seed & 255u) / 256.0f; c->pf = (float)(seed >> 8 & 255u) / 256.0f;
}

static float ens_tap(const Ens *c, float d)
{
    int di = (int)d; float f = d - (float)di;
    int a = (c->pos - di) & (ENS_LINE - 1), b = (a - 1) & (ENS_LINE - 1);
    return ((float)c->buf[a] + ((float)c->buf[b] - (float)c->buf[a]) * f) * (1.0f / 16384.0f);
}

void ens_run(Ens *c, float amt, const float *in, float *outL, float *outR, int n)
{
    float lush = clamp01(amt * 2.0f - 1.0f);                  /* past 12 o'clock: the ensemble */
    float rs = (0.55f - 0.3f * lush) / SR, rf = (5.2f + 0.8f * amt) / SR;
    float base = (8.0f + 6.0f * lush) * (SR / 1000.0f);        /* ms, in samples */
    float ds = (1.2f + 1.6f * clamp01(amt * 2.0f) + 2.5f * lush) * (SR / 1000.0f), df = (0.12f + 0.25f * lush) * (SR / 1000.0f);
    float fb = 0.25f * lush;
    if (c->base <= 0) { c->base = base; c->ds = ds; c->df = df; c->fb = fb; }
    /* (the knob turned, or DRIFT moving it: the lines' lengths glide there over ~50 ms, sample by sample; a jump in a
     * delay's length is a click) */
    const float gl = 1.0f / (0.05f * SR);
    for (int i = 0; i < n; i++) {
        c->base += (base - c->base) * gl; c->ds += (ds - c->ds) * gl; c->df += (df - c->df) * gl; c->fb += (fb - c->fb) * gl;
        c->ps += rs; if (c->ps >= 1.0f) c->ps -= 1.0f;
        c->pf += rf; if (c->pf >= 1.0f) c->pf -= 1.0f;
        float t[3];
        for (int k = 0; k < 3; k++) {
            float th = (float)k * (1.0f / 3.0f);
            t[k] = ens_tap(c, c->base + c->ds * (0.5f + 0.5f * sine_c(c->ps + th)) + c->df * (0.5f + 0.5f * sine_c(c->pf + th)));
        }
        c->buf[c->pos] = to16(soft_clip(in[i] + c->fb * t[1]));
        c->pos = (c->pos + 1) & (ENS_LINE - 1);
        outL[i] = 0.75f * t[0] + 0.45f * t[1];
        outR[i] = 0.75f * t[2] + 0.45f * t[1];
    }
}

/* ------------------------------------------------------------------ dirt */
void dirt_init(Dirt *d, uint32_t seed)
{
    memset(d, 0, sizeof *d);
    rng_seed(&d->rng, seed ^ 0xD127u);
    op_set(&d->tone[0], 12000.0f); op_set(&d->tone[1], 12000.0f); op_set(&d->hiss, 7000.0f);
    d->drop = d->drop_t = 1.0f;
}

static inline float tape_sat(float x)            /* tanh, near enough (and flat past +-3) */
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void dirt_run(Dirt *d, float a, float mix, float *L, float *R, int n)
{
    if (mix < 0.005f) return;
    a = clamp01(a + 0.45f * mix);                        /* (a send drives harder into the tape: with DIRT's own at 0,
                                                          * a send at full is warm tipping into fuzz) */
    a = 0.05f + 0.95f * a;                               /* (its gentlest still a little warmth) */
    float w = clamp01(a * 3.0f), f = clamp01(a * 3.0f - 1.0f), z = clamp01(a * 3.0f - 2.0f);   /* warm, fuzz, breakup */
    float drive = 1.0f + 3.0f * w + 14.0f * f + 10.0f * z;
    float bias = 0.12f + 0.3f * f, sb = tape_sat(bias);  /* (the asymmetry: tape's even harmonics) */
    float norm = fm_minf(1.0f, fm_maxf(1.0f / drive, 0.09f) * 1.1f);   /* (about the same loudness, a little denser) */
    if (z > 0 && (d->drop_left -= (float)n / SR) <= 0) {   /* the dropouts: now and then a dip, 40 to 400 ms */
        d->drop_t = rng_chance(&d->rng, 0.15f + 0.45f * z) ? 0.15f + 0.4f * rng_f(&d->rng) : 1.0f;
        d->drop_left = 0.04f + 0.36f * rng_f(&d->rng);
    }
    if (z <= 0) d->drop_t = 1.0f;
    float cutoff = (12000.0f - 5500.0f * f - 3500.0f * z) * (0.3f + 0.7f * d->drop);   /* (a dropout dull too) */
    op_set(&d->tone[0], cutoff); op_set(&d->tone[1], cutoff);
    for (int i = 0; i < n; i++) {
        float x[2] = { L[i], R[i] };
        d->env += (fm_fabsf(x[0]) + fm_fabsf(x[1]) - d->env) * 0.002f;
        d->drop += (d->drop_t - d->drop) * 0.0025f;      /* (~9 ms: a dip, never a click) */
        float hs = 0, choke = 1.0f;
        if (z > 0) {
            hs = op_lp(&d->hiss, rng_bi(&d->rng)) * z * (0.004f + 0.3f * d->env);   /* hiss, rising with the sound */
            choke = 1.0f - 0.7f * z * (1.0f - clamp01(d->env * 25.0f));            /* the fuzz sputtering as it dies */
        }
        for (int ch = 0; ch < 2; ch++) {
            float y = (tape_sat(x[ch] * drive + bias) - sb) * norm;
            if (f > 0 || z > 0) y = op_lp(&d->tone[ch], y);
            y = (y * choke + hs) * d->drop;
            y = x[ch] + (y - x[ch]) * mix;
            if (ch) R[i] = y; else L[i] = y;
        }
    }
}

/* ------------------------------------------------------------------ the shimmer's pitch shifter */
void shifter_run(Shifter *s, const float *in, float *up, float *down, int n)
{
    const float W = 2048.0f;                             /* each head's sweep: 46 ms */
    for (int i = 0; i < n; i++) {
        s->buf[s->pos] = to16(in[i]);
        float o[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {                    /* k 0: up (rate 2), 1: down (rate 0.5) */
            s->ph[k] += (k ? 0.5f : -1.0f) / W;          /* the delay shrinks (up) or grows (down) */
            if (s->ph[k] >= 1.0f) s->ph[k] -= 1.0f;
            if (s->ph[k] < 0.0f) s->ph[k] += 1.0f;
            for (int h = 0; h < 2; h++) {
                float ph = s->ph[k] + 0.5f * (float)h; if (ph >= 1.0f) ph -= 1.0f;
                float p = (float)s->pos - (8.0f + ph * W); while (p < 0) p += (float)SHIM_LINE;
                int a = (int)p; float f = p - (float)a; int b = (a + 1) & (SHIM_LINE - 1);
                float x = ((float)s->buf[a] + ((float)s->buf[b] - (float)s->buf[a]) * f) * (1.0f / 16384.0f);
                o[k] += x * (0.5f - 0.5f * sine_c(ph + 0.25f));
            }
        }
        up[i] = o[0]; down[i] = o[1];
        s->pos = (s->pos + 1) & (SHIM_LINE - 1);
    }
}

/* ------------------------------------------------------------------ WEAR: the whole mix as on an old, worn tape
 * The top dulling (two poles, 20 kHz down to about 2 kHz by full), wandering a little as a worn tape's azimuth does,
 * and the stereo narrowing, its sides darker still */
void wear_run(Wear *w, float amt, float *L, float *R, int n)
{
    if (amt < 0.005f) { w->l1 = w->l2 = w->r1 = w->r2 = w->s1 = 0; return; }
    w->ph += (float)n * (0.11f / SR); if (w->ph >= 1.0f) w->ph -= 1.0f;
    w->ph2 += (float)n * (0.37f / SR); if (w->ph2 >= 1.0f) w->ph2 -= 1.0f;
    float wob = 1.0f + amt * (0.18f * sine_c(w->ph) + 0.07f * sine_c(w->ph2));   /* (the azimuth wandering) */
    float fc = 20000.0f * fm_exp2f(-3.2f * amt) * wob;          /* (20 kHz, 2.2 kHz by full) */
    float a = 1.0f - fm_expf(-2.0f * PI_F * fm_minf(fc, 16000.0f) / SR);
    float as = 1.0f - fm_expf(-2.0f * PI_F * fm_minf(fc * 0.5f, 12000.0f) / SR), width = 1.0f - 0.7f * amt;
    for (int i = 0; i < n; i++) {
        w->l1 += a * (L[i] - w->l1); w->l2 += a * (w->l1 - w->l2);
        w->r1 += a * (R[i] - w->r1); w->r2 += a * (w->r1 - w->r2);
        float m = 0.5f * (w->l2 + w->r2), sd = 0.5f * (w->l2 - w->r2);
        w->s1 += as * (sd - w->s1);
        L[i] = m + w->s1 * width; R[i] = m - w->s1 * width;
    }
}
