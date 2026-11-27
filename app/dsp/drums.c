/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the drum engines (drums.h). Every fading value snaps to zero once it can't be heard: values
 * creeping towards zero forever (denormals) cost the processor many times more, a hidden spike. Every hit opens over
 * RAMP samples, and a voice taken for another fades out over 3 ms: no clicks. */
#include "drums.h"

const char *const ENGINE_NAME[NUM_ENGINES] = { "MEMBRANE", "BOX", "FM KICK", "WIRES", "GRAIN", "CRACK",
                                               "MATERIAL", "FM PERC", "STRING", "METAL", "DUST", "DIGITAL", "808", "CLAP", "VOX", "CLICK", "FOLEY", "BENT",
                                               "PING", "FOLD", "TAPE", "THUD", "RUMBLE", "RIM", "SNAP", "BRUSH", "SYNTH", "SPRING",
                                               "BLOCK", "TABLA", "HAND", "TINE", "BOTTLE", "SHAKER", "FM HAT", "AIR", "CHIP", "FOIL" };
const uint8_t ENGINE_PART[NUM_ENGINES] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 0, 1, 3, 3, 2, 1,
                                           0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3 };

/* each engine's level at a firm hit, mid settings: measured (tools/render calibrate), so the four parts sit together */
static const float GAIN[NUM_ENGINES] = { 0.42f, 2.7f, 0.7f, 0.96f, 0.85f, 2.3f, 0.93f, 0.45f, 1.12f, 0.88f, 0.65f, 1.15f, 0.6f, 1.6f, 2.2f, 1.4f, 1.1f, 0.97f,
                                         1.25f, 0.59f, 0.40f, 1.07f, 0.41f, 6.99f, 1.40f, 0.74f, 0.51f, 0.53f, 0.42f, 0.42f, 0.48f, 0.38f, 0.41f, 1.00f, 0.34f, 1.12f, 0.64f, 0.55f };   /* (the twenty newer: render calibrate,
                                                  * each to its part's median; BRUSH and SHAKER, which swell, by their peak) */

#include "drums_level.h"                 /* LEVEL: each engine's loudness across WEIGHT and ENTROPY, measured (render levels) */
static float level_corr(int eng, float w, float e)
{
    float x = clamp01(w) * 4.0f, y = clamp01(e) * 16.0f; int i = (int)x, j = (int)y; if (i > 3) i = 3; if (j > 15) j = 15;
    x -= (float)i; y -= (float)j;
    const float (*t)[17] = LEVEL[eng];
    return lerpf(lerpf(t[i][j], t[i][j + 1], y), lerpf(t[i + 1][j], t[i + 1][j + 1], y), x);
}
int drums_level_raw;
static const float WOOD_G = 13.0f, FOOT_G = 0.8f, DRAG_G = 4.0f;   /* FOLEY's four sounds evened out (measured: the box's level) */                     /* (render levels: measured without the correction) */

#define RAMP 10                          /* samples of opening (0.23 ms): the hit's edge kept, its click not */
#define LIVE 1e-4f                       /* -80 dB: gone */

static inline float nz(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return (float)(int32_t)x * (1.0f / 2147483648.0f); }
static inline float nu(uint32_t *s) { return nz(s) * 0.5f + 0.5f; }
static inline float dk(float sec) { if (sec < 0.0005f) sec = 0.0005f; return fm_expf(-1.0f / (sec * SR)); }
static inline void fade(float *x, float k) { *x *= k; }   /* (a decay: snapped to zero once it's far gone at the end of each
                                              * block, hit_render's last step, not checked every sample) */
static inline void snap6(float *x) { if (fm_fabsf(*x) < 1e-6f) *x = 0; }
static inline void fade_z(float *x, float k) { *x *= k; if (fm_fabsf(*x) < 1e-6f) *x = 0; }   /* (one whose zero stops
                                              * something at once: the front's noise and zap) */
static inline void glide(Hit *h) { float d = (h->f - h->f0) * h->fk; h->f = fm_fabsf(d) < 0.01f ? h->f0 : h->f0 + d; }
static inline float semis(float s) { return fm_exp2f(s * (1.0f / 12.0f)); }
static inline float bpn(Svf *f, float x) { float b; svf_run(f, x, 0, &b, 0); return b * f->k; }   /* a band-pass, unity at its peak */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int16_t to16(float x) { x = clampf(x, -1.99f, 1.99f); return (int16_t)(x * 16384.0f); }

void drums_init(void) {}

void hit_choke(Hit *h) { if (h->on && h->killk == 0) h->killk = 1.0f / (0.003f * SR); }

float hit_level(const Hit *h)
{
    if (!h->on) return 0;
    float l = h->env;
    if (h->engine == D_MATERIAL) l = h->a[0] + h->a[1] + h->a[2] + h->a[3];
    if (h->engine == D_STRING) l = h->hold;
    if (h->engine == D_WIRES || h->engine == D_CRACK || h->engine == D_GRAIN) l = h->env + h->nenv + h->a[0];
    return l * h->vel * h->kill;
}

/* ------------------------------------------------------------------ the front (every engine's attack)
 * ENTROPY walks through the kinds of attack, each fading into the next: at 0 a softened start (no edge at all), then a
 * click, a noise snap, a zap (a sine diving from high), the body ring-modulated for its first moments (a metal edge),
 * and at the top the body crushed and stuttering. WEIGHT spreads the exciter: longer, lower and wider (fuller). */
static const float FRONT_HZ[4] = { 1600.0f, 2600.0f, 2200.0f, 6000.0f };   /* (each part's exciter, at its centre) */
static inline float bump(float x, float c, float wd) { float t = 1.0f - fm_fabsf(x - c) / wd; if (t <= 0) return 0; return t * t * (3.0f - 2.0f * t); }

static void front_start(Hit *h, const HitP *p, float E, float w, float b, int eng)
{
    Rng r; rng_seed(&r, p->seed ^ 0x9E3779B9u);
    float dl = clampf(p->decay, 0.4f, 4.0f);              /* (a longer hit: its front stretched with it) */
    float spread = (0.6f + 1.3f * w) * fm_powf(dl, 0.7f); /* (WEIGHT: how long and wide the exciter) */
    float fc = FRONT_HZ[ENGINE_PART[eng]] * semis(p->shift * 0.8f) * (0.6f + 0.8f * b) * (1.25f - 0.45f * w) * (0.75f + 0.5f * rng_f(&r));
    float soft = clamp01(p->soft), lv = 0.9f / GAIN[eng];   /* (the exciter as loud whatever the engine's own level) */
    float ak = eng == D_CLICK ? 0.15f : 1.0f;             /* (a click softened, not swallowed) */
    h->fr.atk = (uint16_t)(((E < 0.15f ? (1.0f - E / 0.15f) * (1.0f - E / 0.15f) : 0.0f) + soft * soft * 0.8f) * (0.002f + 0.005f * w) * ak * SR);
    float cc = bump(E, 0.3f, 0.17f), cn = bump(E, 0.47f, 0.17f), cz = bump(E, 0.64f, 0.17f), cr = bump(E, 0.8f, 0.17f);
    float cx = clamp01((E - 0.8f) / 0.17f);
    float hard = 1.0f - 0.75f * soft;                   /* (MOOD's soft side: the exciter lower) */
    cc *= hard; cn *= hard; cz *= hard; cr *= hard; cx *= hard;
    /* the attack */
    h->fr.click = cc * 3.0f * lv; h->fr.ring = cc + cn > 0;
    h->fr.nzv = cn * 1.1f * lv; h->fr.nzk = dk((0.0015f + 0.003f * rng_f(&r)) * spread);
    svf_set(&h->fr.f, clampf(fc, 300.0f, 12000.0f), lerpf(3.0f + 6.0f * rng_f(&r), 0.9f, cn / (cc + cn + 1e-6f)) / fm_sqrtf(0.6f + 1.3f * w));
    h->fr.zf1 = h->fr.zf = clampf(fc * (1.6f + 1.2f * rng_f(&r)), 800.0f, 11000.0f); h->fr.zf0 = 90.0f + 250.0f * rng_f(&r);
    h->fr.zk = dk((0.001f + 0.0025f * rng_f(&r)) * spread); h->fr.za = cz * 0.75f * lv; h->fr.zak = dk((0.003f + 0.004f * rng_f(&r)) * spread);
    h->fr.rm = cr * 0.95f; h->fr.rmk = dk((0.005f + 0.01f * rng_f(&r)) * spread); h->fr.rdf = (350.0f + 2600.0f * rng_f(&r)) * (0.6f + 0.8f * b) / SR;
    h->fr.cr = cx; h->fr.crk = dk((0.01f + 0.02f * rng_f(&r)) * spread); h->fr.crr = (1200.0f + 4500.0f * rng_f(&r)) / SR;
    h->fr.gdf = 1.0f / ((0.0012f + 0.003f * rng_f(&r)) * SR); h->fr.gs = 1.0f;
    /* and its character through the rest of the hit, following the body as it dies away (so a long hit keeps it) */
    h->fr.tres = cc * 0.55f;                              /* (the body ringing in the click's band) */
    h->fr.tn = cn * 1.3f; svf_set(&h->fr.f2, clampf(fc * 0.8f, 300.0f, 10000.0f), 1.3f);   /* (a rasp riding the body) */
    h->fr.tz = cz * 0.9f;                                 /* (the zap diving again and again) */
    h->fr.zper = (uint16_t)((0.025f + 0.05f * rng_f(&r)) * fm_sqrtf(dl) * SR); h->fr.zcnt = h->fr.zper;
    h->fr.rmf = cr * 0.55f;                               /* (the ring-modulation stays, lighter) */
    h->fr.crf = cx * 0.6f;                                /* (and the crush) */
    h->fr.tail = h->fr.tres > 0 || h->fr.tn > 0 || h->fr.tz > 0 || h->fr.rmf > 0 || h->fr.crf > 0;
    float len = (0.012f + 0.045f * spread);
    h->fr.len = (uint16_t)fm_minf(fm_maxf(len * SR, (float)h->fr.atk) + 64.0f, 60000.0f);
}

#ifndef FR_TAIL_END
#define FR_TAIL_END 0.005f                         /* (-46 dB below a full hit) */
#endif
static inline float front_run(Hit *h, float s)
{
    if (h->fr.tn > 0 || h->fr.tz > 0) { float a = fm_fabsf(s); h->fr.fol += (a - h->fr.fol) * (a > h->fr.fol ? 0.02f : 0.0015f); }   /* (the
                                                  * body's level, followed: only for the tails that ride it) */
    if (h->fr.atk && h->n < h->fr.atk) { float u = (float)h->n / (float)h->fr.atk; s *= u * u * (3.0f - 2.0f * u); }   /* (softened) */
    if (h->fr.rm > 0) {                                   /* a ring-modulated edge: the body times a fast sine, settling lighter */
        h->fr.rph += h->fr.rdf; if (h->fr.rph >= 1.0f) h->fr.rph -= 1.0f;
        s *= 1.0f - h->fr.rm + h->fr.rm * sine_c(h->fr.rph) * 1.6f; h->fr.rm = h->fr.rmf + (h->fr.rm - h->fr.rmf) * h->fr.rmk; if (h->fr.rm - h->fr.rmf < 1e-5f) h->fr.rm = h->fr.rmf;
    }
    if (h->fr.cr > 0) {                                   /* crushed, stuttering, settling lighter */
        h->fr.cph += h->fr.crr; if (h->fr.cph >= 1.0f) { h->fr.cph -= 1.0f; h->fr.cv = (float)(int)(s * 5.0f) * 0.2f; }
        h->fr.gph += h->fr.gdf; if (h->fr.gph >= 1.0f) h->fr.gph -= 1.0f;
        h->fr.gs += ((h->fr.gph < 0.6f ? 1.0f : 0.0f) - h->fr.gs) * 0.25f;   /* (the stutter's edges eased) */
        s = lerpf(s, h->fr.cv * h->fr.gs * 1.3f, h->fr.cr); h->fr.cr = h->fr.crf + (h->fr.cr - h->fr.crf) * h->fr.crk; if (h->fr.cr - h->fr.crf < 1e-5f) h->fr.cr = h->fr.crf;
    }
    float x = 0, ex = 0;                                  /* the exciters, added */
    if (h->n == RAMP + 1) x += h->fr.click;
    if (h->fr.nzv > 0) { x += nz(&h->ns) * h->fr.nzv; fade_z(&h->fr.nzv, h->fr.nzk); }
    if (h->fr.ring) ex = bpn(&h->fr.f, x + s * h->fr.tres);   /* (the click and snap ringing out in their band, the body with them) */
    if (h->fr.tn > 0) ex += bpn(&h->fr.f2, nz(&h->ns)) * h->fr.fol * h->fr.tn;
    if (h->fr.tz > 0 && --h->fr.zcnt == 0) {              /* the zap again, as loud as the body is now */
        h->fr.zcnt = h->fr.zper; h->fr.zf = h->fr.zf1; h->fr.zt = h->fr.fol * h->fr.tz;   /* (risen to, not jumped to) */
    }
    if (h->fr.zt > 0) {                                   /* (each zap again eased in over half a millisecond: no click) */
        h->fr.za += (h->fr.zt - h->fr.za) * 0.1f;
        if (h->fr.za >= h->fr.zt * 0.98f) h->fr.zt = 0;
    }
    if (h->fr.za > 0) {                                   /* the zap: a sine diving from high to low, fast */
        h->fr.zf = h->fr.zf0 + (h->fr.zf - h->fr.zf0) * h->fr.zk;
        h->fr.zph += h->fr.zf / SR; if (h->fr.zph >= 1.0f) h->fr.zph -= 1.0f;
        ex += sine_c(h->fr.zph) * h->fr.za; if (h->fr.zt == 0) fade_z(&h->fr.za, h->fr.zak);
    }
    return s + ex;
}

void hit_start(Hit *h, const HitP *p)
{
    Rng r; rng_seed(&r, p->seed);
    float w = clamp01(p->weight), e = clamp01(p->entropy), b = clamp01(p->bright), d = clampf(p->decay, 0.1f, 8.0f);
    int eng = p->engine % NUM_ENGINES;
    float ef = e;                            /* ENTROPY: mostly the hit's front (below); the body takes less of it, except
                                              * FOLEY's choice of sound and BENT's wiring, which are the body */
    if (eng != D_FOLEY && eng != D_BENT && eng < D_PING) e *= 0.55f;   /* (the twenty newer: their bodies the whole range) */
    memset(h, 0, sizeof *h - sizeof h->line);    /* (the line: its engine fills it) */
    h->on = 1; h->engine = (uint8_t)eng; h->vel = clamp01(p->vel) * GAIN[eng] * (drums_level_raw ? 1.0f : level_corr(eng, w, ef)) * (p->shift < 0 ? fm_minf(semis(-p->shift * 0.3f), 3.0f) : 1.0f);   /* (WEIGHT and ENTROPY change the sound, not how loud) */
    h->w = w; h->e = e; h->b = b; h->vr = clamp01(p->vel);
    front_start(h, p, ef, w, b, eng); h->kill = 1.0f; h->env = 1.0f; h->ns = p->seed | 1u;
    float pit = semis(p->note - 60.0f), hz = clampf(mtof(p->note), 30.0f, 6000.0f);
    switch (eng) {
    case D_MEMBRANE:
        h->f0 = (38.0f + 22.0f * b) * (1.25f - 0.45f * w) * pit;
        h->f = h->f0 * (2.2f + 4.0f * b + 2.0f * (1.0f - w));
        h->fk = dk(0.006f + 0.035f * w);
        h->ek = dk((0.04f + 0.21f * w * fm_sqrtf(w)) * d);
        h->df[1] = 1.593f * (1.0f + e * 0.25f * rng_bi(&r)); h->df[2] = 2.136f * (1.0f + e * 0.4f * rng_bi(&r));   /* (ratios) */
        h->a[1] = 0.35f - 0.2f * w; h->a[2] = 0.2f + 0.2f * e; h->ak[1] = h->ak[2] = dk(0.04f + 0.1f * w);
        h->idx = e * e * 2.5f; h->ik = dk(0.05f + 0.25f * e);
        h->nenv = 0.25f + 0.4f * b * (1.0f - w); h->nk = dk(0.0025f);
        svf_set(&h->f1, 2500.0f, 0.7f);
        if (e > 0.4f) { h->bend = (e - 0.4f) * 0.6f * rng_bi(&r); h->bk = dk(0.025f); }
        break;
    case D_BOX:
        h->f0 = (45.0f + 30.0f * b) * (1.2f - 0.4f * w) * pit;
        {   /* the fall's start: from above (a fall), turning gradually, past halfway, to below (a rise) */
            float u = clamp01((e - 0.45f) / 0.3f); u = u * u * (3.0f - 2.0f * u);
            h->f = h->f0 * lerpf(1.8f + 2.0f * b, 0.55f + 0.2f * rng_f(&r), u);
        }
        if (e >= 0.6f) h->f0 *= 1.0f + (e - 0.6f) * 1.5f;
        h->fk = dk(0.012f + 0.05f * w);
        h->ek = dk((0.05f + 0.17f * w) * d);
        h->nenv = 1.0f; h->nk = dk(0.003f + 0.01f * w);
        h->fb = 5.0f + 22.0f * e + 6.0f * w;      /* (the resonances' Q) */
        h->hk = 0; svf_set(&h->f1, h->f, h->fb); svf_set(&h->f2, h->f * 2.31f, h->fb * 0.7f);
        break;
    case D_FMKICK:
        h->f0 = (40.0f + 20.0f * b) * (1.2f - 0.35f * w) * pit;
        h->f = h->f0 * (3.0f + 3.0f * b);
        h->fk = dk(0.01f + 0.03f * w);
        h->df[1] = e < 0.15f ? 0.5f : 0.5f + e * (1.0f + 1.3f * rng_f(&r));   /* (the modulator's ratio) */
        h->idx = 1.2f + 2.5f * w + 1.5f * b; h->ik = dk(0.03f + 0.08f * w);
        h->fb = e * e * 0.2f;                    /* (feedback in turns: past ~0.25 it is white noise, not a growl) */
        h->ek = dk((0.045f + 0.19f * w) * d);
        break;
    case D_WIRES: {
        h->f0 = (165.0f + 90.0f * b) * pit;
        h->df[0] = h->f0 / SR; h->df[1] = h->f0 * 1.59f / SR; h->a[0] = 0.55f; h->a[1] = 0.3f;
        h->ak[0] = h->ak[1] = dk(0.02f + 0.05f * w);
        h->nenv = 1.0f; h->nk = dk((0.025f + 0.08f * w) * d); h->env = 0; h->ek = 1.0f;
        float fc = 900.0f + 2200.0f * b + 600.0f * e * rng_bi(&r);
        h->clen = clampi((int)(SR / fc), 8, 159);
        h->fb = 0.15f + 0.82f * fm_powf(e, 1.2f);   /* the wires ringing into metal */
        memset(h->line.comb, 0, sizeof h->line.comb);
        svf_set(&h->f1, clampf(350.0f * pit, 25.0f, 350.0f), 0.7f); op_set(&h->o1, clampf((3000.0f + 6000.0f * b) * pit, 300.0f, 9000.0f));
        break; }
    case D_GRAIN: {
        int n = 3 + (int)(5.0f * w + 2.0f * e); if (n > 10) n = 10;
        float win = (0.008f + 0.05f * w + 0.08f * e) * SR, t = 0;
        for (int i = 0; i < n; i++) {
            if (e < 0.4f) t = (float)i * win / (float)n + rng_f(&r) * win * 0.1f;
            else t += win / (float)n * (0.2f + 2.0f * e * rng_f(&r));   /* scattered: gaps opening up, a stutter */
            h->g_at[i] = (uint16_t)clampf(t, 0.0f, 60000.0f);
            h->g_len[i] = (uint16_t)((0.002f + 0.006f * w) * SR * (0.5f + rng_f(&r)));
            h->g_amp[i] = e < 0.5f ? fm_powf(0.82f, (float)i) : 0.3f + 0.7f * rng_f(&r);
            h->g_fc[i] = clampf((2500.0f + 4000.0f * b) * (1.0f + e * (rng_f(&r) * 1.6f - 0.6f)) * pit, 120.0f, 15000.0f);
        }
        h->g_n = (uint8_t)n; h->g_cur = 255;
        h->df[0] = 190.0f * pit / SR; h->a[0] = 0.3f * w + 0.08f; h->ak[0] = dk(0.04f);
        h->nk = dk((0.008f + 0.032f * w) * d); h->env = 0; h->ek = 1.0f;
        svf_set(&h->f2, clampf(400.0f * pit, 25.0f, 400.0f), 0.7f);
        break; }
    case D_CRACK: {
        static const float FR[3] = { 1700.0f, 2950.0f, 4800.0f };
        float q = 5.0f + 12.0f * e + 4.0f * w;   /* (lower: a crack, not a ring) */
        svf_set(&h->f1, FR[0] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        svf_set(&h->f2, FR[1] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        svf_set(&h->f3, FR[2] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        h->nenv = 1.0f; h->nk = dk(0.001f);
        h->ek = dk((0.02f + 0.07f * w) * d);
        h->df[0] = 160.0f * pit / SR; h->a[0] = 0.4f * w; h->ak[0] = dk(0.05f);
        h->fb = q;
        break; }
    case D_MATERIAL: {
        /* wood, clay (dense and inharmonic, damped: no metal bar's ring, the steel drum it made), a short glassy tick */
        static const float R[3][4] = { { 1.0f, 2.57f, 4.21f, 6.3f }, { 1.0f, 1.73f, 2.91f, 3.62f }, { 1.0f, 2.32f, 4.25f, 6.63f } };
        static const float T[3][4] = { { 0.09f, 0.035f, 0.018f, 0.01f }, { 0.07f, 0.04f, 0.025f, 0.015f }, { 0.16f, 0.09f, 0.05f, 0.03f } };
        static const float A[4] = { 0.7f, 0.45f, 0.3f, 0.2f };
        int s0 = e < 0.5f ? 0 : 1; float m = e < 0.5f ? e * 2.0f : (e - 0.5f) * 2.0f;   /* wood -> metal -> glass */
        for (int i = 0; i < 4; i++) {
            float rr = lerpf(R[s0][i], R[s0 + 1][i], m), tt = lerpf(T[s0][i], T[s0 + 1][i], m);
            float f = hz * rr; if (f > SR * 0.42f) continue;
            h->df[i] = f / SR; h->a[i] = A[i] * (i ? 0.5f + 1.0f * b : 1.0f); h->ak[i] = dk(tt * (0.35f + 0.9f * w) * d);
        }
        if (e > 0.66f) { h->bend = -(e - 0.66f) * 0.15f; h->bk = dk(0.005f); }   /* (glass: the strike bending up) */
        h->nenv = 0.9f; h->nk = dk(0.003f + 0.004f * w);   /* (the strike forward, the ringing back) */ svf_set(&h->f1, fm_minf(hz * 3.0f, 9000.0f), 1.2f);
        h->env = 0; h->ek = 1.0f;
        break; }
    case D_FMPERC: {
        h->f0 = hz; float sw = 0.25f + 1.8f * e;
        h->f = hz * (1.0f + sw);
        h->fk = dk(0.0012f + 0.004f * (1.0f - e));   /* (a snap of a fall, not a sweep) */
        h->df[1] = e < 0.3f ? 2.0f : 1.41f + e * 2.3f * rng_f(&r);
        h->idx = 1.0f + 3.0f * w + 1.5f * b; h->ik = dk(0.03f + 0.15f * w);
        h->fb = e * 0.2f;
        h->ek = dk((0.02f + 0.09f * w) * d);
        h->hk = e > 0.6f ? 1.0f - (e - 0.6f) * 2.0f : 1.0f; if (h->hk < 0.12f) h->hk = 0.12f;   /* (decimated past 0.6) */
        break; }
    case D_STRING: {
        float f = hz, dull = 1.0f; while (f < SR / (float)(KS_LEN - 4)) { f *= 2.0f; if (p->shift < 0) dull *= 0.5f; }   /* (too low for its line: an octave up; on a lower part's lane made duller to sit low) */
        h->f = SR / f; h->ks_len = h->f / (1.0f + 0.12f * e);   /* slack: struck sharp, settling onto its note in ~6 ms */
        h->fk = 1.0f - dk(0.006f);
        float T = (0.035f + 0.18f * w) * d;
        h->ks_damp = (0.25f + 0.7f * b) * dull;   /* the loop's low-pass: bright, or dull and woody */
        h->fb = fm_expf(-1.0f / (T * f));         /* the loop's gain a pass */
        h->ks_apc = e * 0.6f;                     /* dispersion: the overtones stretched, metallic */
        int len = (int)h->ks_len + 2; OnePole lp; lp.z = 0; op_set(&lp, (1500.0f + 9000.0f * b) * dull);
        float pick = 0.12f + 0.3f * rng_f(&r);     /* where it's struck */
        for (int i = 0; i < KS_LEN; i++) h->line.ks[i] = 0;
        for (int i = 0; i < len && i < KS_LEN; i++) h->line.ks[i] = to16(op_lp(&lp, nz(&h->ns)) * 0.9f);
        int off = (int)((float)len * pick); if (off < 1) off = 1;
        for (int i = len - 1; i >= off && i < KS_LEN; i--) h->line.ks[i] = to16(((float)h->line.ks[i] - (float)h->line.ks[i - off] * 0.6f) * (1.0f / 16384.0f));
        h->cpos = len % KS_LEN; h->hold = 1.0f;
        break; }
    case D_METAL: {
        static const float R808[6] = { 2.0f, 3.0f, 4.16f, 5.43f, 6.79f, 8.21f };
        Rng g; rng_seed(&g, p->gseed ^ 0x6A09E667u);   /* the groove's own ratios */
        float base = (130.0f + 120.0f * b) * pit;
        for (int i = 0; i < 6; i++) {
            float own = 1.7f + 7.8f * rng_f(&g);
            h->df[i] = base * lerpf(R808[i], own, e) / SR; h->ph[i] = rng_f(&r);
        }
        h->ek = dk((0.01f + 0.19f * fm_powf(w, 2.2f)) * d);
        svf_set(&h->f1, clampf((7000.0f + 4000.0f * b) * pit, 150.0f, 15000.0f), 1.2f); svf_set(&h->f2, clampf((5000.0f + 2500.0f * b) * pit, 30.0f, 12000.0f), 0.7f);
        break; }
    case D_DUST:
        h->hk = 0.02f + 0.25f * w;               /* grains a sample */
        h->ek = dk((0.015f + 0.07f * w) * d);
        h->nk = 1.0f / ((0.001f + 0.02f * e) * SR);   /* the smear: a shaker's soft front */
        svf_set(&h->f1, clampf((4000.0f + 5000.0f * b) * pit, 120.0f, 15000.0f), 0.7f + 8.0f * e); svf_set(&h->f2, clampf(2500.0f * pit, 30.0f, 2500.0f), 0.7f);
        break;
    case D_808:
        h->f0 = (42.0f + 16.0f * b) * (1.15f - 0.3f * w) * pit;
        h->f = h->f0 * (1.35f + 0.9f * e);        /* (a gentle fall; ENTROPY deepens it) */
        h->fk = dk(0.012f + 0.02f * w);
        h->ek = dk((0.07f + 0.3f * w) * d);
        h->nenv = 0.12f + 0.15f * b; h->nk = dk(0.0015f);
        op_set(&h->o1, 1800.0f + 2500.0f * b);    /* (the click softened) */
        op_set(&h->o2, 4000.0f);
        break;
    case D_CLAP: {
        int nb = 3 + (int)(w * 1.5f + e);          /* the bursts: 3 to 5 */
        float t = 0;
        for (int i = 0; i < nb && i < 10; i++) {
            h->g_at[i] = (uint16_t)(t * SR);
            h->g_len[i] = (uint16_t)((0.004f + 0.003f * rng_f(&r) * e) * SR);
            h->g_amp[i] = 0.75f + 0.25f * rng_f(&r);
            t += (0.008f + 0.004f * w) * (1.0f + e * 1.2f * rng_bi(&r) * 0.5f);   /* (ENTROPY: the spacing loosened) */
        }
        h->g_n = (uint8_t)(nb < 10 ? nb : 10);
        h->hk = t;                                /* (where the tail starts, in seconds) */
        h->nk = dk((0.035f + 0.12f * w) * d);
        svf_set(&h->f1, (900.0f + 700.0f * b) * pit, 1.6f + 1.5f * e);
        svf_set(&h->f2, clampf((2600.0f + 1800.0f * b) * pit, 200.0f, 9000.0f), 1.2f);
        svf_set(&h->f3, clampf(350.0f * pit, 25.0f, 350.0f), 0.7f);
        h->env = 0; h->ek = 1.0f;
        break; }
    case D_VOX: {
        static const float VF[5][3] = { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };   /* A E I O U */
        int v0 = rng_int(&r, 5), v1 = rng_int(&r, 5);
        float up = (1.5f + 0.8f * b) * fm_sqrtf(pit);   /* (the vowels moved up: a hat's register; down with it on a lower lane) */
        for (int k = 0; k < 3; k++) { h->a[k] = VF[v0][k] * up; h->ak[k] = VF[v1][k] * up; }   /* (from, to: ENTROPY slides it) */
        float jump = e * 7.0f * rng_bi(&r);        /* (each hit its own pitch) */
        h->f0 = clampf(mtof(p->note + 12.0f + jump), 35.0f, 2500.0f);
        h->df[0] = h->f0 / SR;
        h->hk = 0.25f + 0.5f * w;                  /* (how voiced: the buzz against the breath) */
        h->ek = dk((0.008f + 0.045f * w) * d);
        h->bend = e > 0.3f ? (e - 0.3f) * 1.4f : 0.0f;   /* (how far the vowel slides) */
        h->bk = dk(0.012f);
        for (int k = 0; k < 3; k++) svf_set(k == 0 ? &h->f1 : k == 1 ? &h->f2 : &h->f3, h->a[k], 6.0f);
        break; }
    case D_CLICK: {
        float fc = (2000.0f + 6000.0f * b) * pit * (1.0f + e * 0.8f * rng_bi(&r));   /* (ENTROPY: its tone moving hit to hit) */
        svf_set(&h->f1, clampf(fc, 400.0f, 15000.0f), 0.8f + 14.0f * e * e);        /* (a dry tick, then a ping) */
        svf_set(&h->f2, clampf(1500.0f * pit, 25.0f, 1500.0f), 0.7f);
        int nc = 1 + (e > 0.6f ? 1 + rng_int(&r, 2) : 0);   /* (past 0.6: a burst of two or three) */
        for (int i = 0; i < nc; i++) { h->g_at[i] = (uint16_t)(RAMP + 2 + (i ? (0.001f + 0.005f * rng_f(&r)) * SR * (float)i : 0));   /* (after the opening ramp) */ h->g_amp[i] = i ? 0.4f + 0.5f * rng_f(&r) : 1.0f; }
        h->g_n = (uint8_t)nc;
        h->ek = dk((0.0006f + 0.03f * w * w) * d);
        break; }
    case D_FOLEY: {
        /* four found sounds, low and dull: a box banged, a plank of wood, a footstep, something dragged. ENTROPY moves
         * from one to the next (between two, each hit either: never quite the same); WEIGHT: bigger, heavier, longer */
        float u = e * 3.0f; int ob = (int)u; if (ob > 3) ob = 3; if (ob < 3 && rng_f(&r) < u - (float)ob) ob++;
        h->g_cur = (uint8_t)ob;
        float sz = 1.0f + 0.8f * w;              /* (bigger: lower) */
        float vary = 1.0f + 0.1f * rng_bi(&r);   /* (each hit its own) */
        float lo = clampf(hz * vary / sz, 45.0f, 260.0f);
        switch (ob) {
        case 0:   /* a box: a hollow bonk, the thud of its air and two of its panel's resonances */
            svf_set(&h->f1, clampf(lo * 1.6f, 70.0f, 400.0f), 4.0f);
            svf_set(&h->f2, clampf(lo * 3.9f, 160.0f, 1100.0f), 3.0f);
            svf_set(&h->f3, clampf((550.0f + 400.0f * b) * vary, 300.0f, 1400.0f), 1.4f);
            op_set(&h->o1, 1200.0f + 1200.0f * b);
            h->nenv = 1.0f; h->nk = dk(0.002f + 0.004f * w);
            h->df[0] = clampf(lo * 1.6f, 70.0f, 400.0f) / SR; h->a[0] = 0.6f; h->ak[0] = dk(0.02f + 0.05f * w);
            h->env = 1.0f; h->ek = dk((0.04f + 0.09f * w) * d);
            break;
        case 1:   /* a plank: wood struck, three of its knocks, dull */
            svf_set(&h->f1, clampf(lo * 2.3f, 90.0f, 600.0f), 7.0f);
            svf_set(&h->f2, clampf(lo * 5.7f, 220.0f, 1500.0f), 5.0f);
            svf_set(&h->f3, clampf(lo * 8.2f, 350.0f, 1800.0f), 3.5f);
            op_set(&h->o1, 1100.0f + 1400.0f * b);
            h->nenv = 1.0f; h->nk = dk(0.0015f + 0.002f * w);
            h->env = 1.0f; h->ek = dk((0.035f + 0.07f * w) * d);
            break;
        case 2:   /* a footstep: the heel's thump, then the sole's scuff of grit, a moment later */
            h->df[0] = clampf(lo * 0.9f, 45.0f, 120.0f) / SR; h->a[0] = 0.9f; h->ak[0] = dk(0.018f + 0.03f * w);
            svf_set(&h->f3, 260.0f + 120.0f * b, 0.7f); h->nenv = 1.0f; h->nk = dk(0.008f + 0.008f * w);
            h->g_at[0] = (uint16_t)((0.012f + 0.03f * rng_f(&r) * sz) * SR); h->g_len[0] = (uint16_t)((0.03f + 0.05f * w) * SR);
            h->g_amp[0] = 0.5f + 0.4f * rng_f(&r); h->ak[1] = dk(0.02f + 0.03f * w);
            h->hk = 0.04f + 0.08f * rng_f(&r);   /* (the grit: grains a sample) */
            svf_set(&h->f1, clampf((650.0f + 450.0f * b) * vary, 400.0f, 1600.0f), 1.2f); op_set(&h->o1, 1300.0f);
            h->env = 1.0f; h->ek = dk((0.06f + 0.08f * w) * d);
            break;
        default:  /* a drag: something scraped along, catching and slipping, a soft front */
            h->fb = clampf((260.0f + 380.0f * b) * vary / fm_sqrtf(sz), 160.0f, 900.0f);   /* (its centre) */
            svf_set(&h->f1, h->fb, 2.2f); svf_set(&h->f2, h->fb * 0.45f, 1.5f); op_set(&h->o1, 1500.0f);
            h->hk = (18.0f + 40.0f * rng_f(&r)) / SR;   /* (catching and slipping: 18-58 times a second) */
            h->env = 1.0f; h->ek = dk((0.05f + 0.16f * w) * d);
            break;
        }
        break; }
    case D_BENT: {
        /* a circuit-bent toy: two oscillators wired into each other, a pitch that jumps, the sound held at a broken
         * sample rate. WEIGHT and ENTROPY are the bends: every setting wires it differently (not a smooth sweep: a
         * new circuit every few steps), so short hits still differ in their first moments; past halfway on ENTROPY
         * the wiring shorts out further, chattering */
        float c1 = w * 2.71f + e * 3.37f, c2 = w * 4.13f - e * 1.91f + 7.0f, c3 = w * 1.17f + e * 5.03f + 0.3f;
        float h1 = c1 - fm_floorf(c1), h2 = c2 - fm_floorf(c2), h3 = c3 - fm_floorf(c3), h4 = (h1 + h2 * 0.618f) - fm_floorf(h1 + h2 * 0.618f);
        float jit = 1.0f + e * 0.25f * rng_bi(&r);  /* (each hit a little off: more as ENTROPY rises) */
        float fa = 90.0f * fm_exp2f(5.5f * h1) * jit * (0.7f + 0.6f * b) * pit;
        h->df[0] = fa / SR; h->df[1] = fa * (0.13f + 3.2f * h2) / SR;
        h->a[0] = 0.3f + 2.2f * e * h3;          /* (how hard B bends A) */
        h->a[1] = 0.2f + 1.5f * h4;              /* (and A bends B) */
        h->a[2] = 0.08f + 0.42f * h3;            /* (A's pulse width) */
        h->a[3] = e > 0.25f ? (e - 0.25f) * 20.0f : 0.0f;   /* (the pitch jumps: up to 15 semitones) */
        h->clen = (int)((0.0012f + 0.012f * h2 * h2) * SR); h->cpos = h->clen;
        h->hk = (1800.0f + 20000.0f * h4 * h4) / SR;   /* (the broken sample rate) */
        h->a[4] = e > 0.55f ? (25.0f + 220.0f * h1) / SR : 0.0f;   /* (shorting: the sound chattering on and off) */
        h->bend = 1.0f;
        h->f = 1.0f; h->f0 = 0.35f + 1.2f * h3; h->fk = dk(0.003f + 0.01f * h1);   /* (a quick drop or lift of the whole) */
        h->ek = dk((0.006f + 0.14f * w * w) * d);
        svf_set(&h->f2, 120.0f, 0.7f);
        break; }
    /* ---------------- the twenty more ---------------- */
    case D_PING: {                               /* a resonance struck: the impulse scaled to ring it at about 1 */
        h->f0 = (40.0f + 22.0f * b) * (1.2f - 0.4f * w) * pit;
        h->f = h->f0 * (1.8f + 1.4f * b + 0.8f * e);
        h->fk = dk(0.007f + 0.022f * w);
        h->fb = 6.0f + 18.0f * w * w + 5.0f * e;   /* (its Q: how long it rings) */
        h->a[1] = e > 0.35f ? (e - 0.35f) * 1.2f : 0.0f; h->df[1] = 1.47f + 1.2f * e * rng_f(&r);   /* (the clang) */
        h->idx = 1.0f + 5.0f * e * e;              /* (the drive) */
        h->ek = dk((0.07f + 0.28f * w) * d);
        svf_set(&h->f1, h->f, h->fb); svf_set(&h->f2, h->f * h->df[1], h->fb * 0.6f);
        break; }
    case D_FOLD:
        h->f0 = (42.0f + 18.0f * b) * (1.15f - 0.35f * w) * pit;
        h->f = h->f0 * (2.4f + 2.0f * b);
        h->fk = dk(0.009f + 0.03f * w);
        h->a[0] = 0.6f + 6.5f * e;               /* (how far it folds, closing as it dies) */
        h->ak[0] = dk(0.02f + 0.08f * w + 0.06f * e);
        h->hv = e * e * 0.9f * (rng_f(&r) < 0.5f ? 1.0f : -1.0f);   /* (lopsided) */
        h->ek = dk((0.05f + 0.2f * w) * d);
        break;
    case D_TAPE:
        h->f0 = (44.0f + 18.0f * b) * (1.15f - 0.3f * w) * pit;
        h->f = h->f0 * (2.0f + 1.5f * b);
        h->fk = dk(0.01f + 0.025f * w);
        h->a[1] = e * 0.05f; h->df[1] = (3.5f + 3.0f * rng_f(&r)) / SR; h->ph[1] = rng_f(&r);   /* (wow) */
        h->a[2] = e * e * 0.02f; h->df[2] = (16.0f + 14.0f * rng_f(&r)) / SR;                   /* (flutter) */
        h->idx = 1.3f + 3.0f * e + 1.5f * w;       /* (saturation) */
        h->a[3] = e > 0.5f ? (e - 0.5f) * 0.9f : 0.0f;   /* (dropouts) */
        op_set(&h->o1, (2500.0f + 5000.0f * b) * (1.0f - 0.5f * e)); op_set(&h->o2, 6000.0f);
        h->ek = dk((0.05f + 0.2f * w) * d);
        break;
    case D_THUD: {
        h->f0 = (48.0f + 20.0f * b) * (1.15f - 0.35f * w) * pit;
        h->f = h->f0 * 1.6f; h->fk = dk(0.006f + 0.012f * w);
        h->a[0] = 0.8f; h->ak[0] = dk(0.03f + 0.15f * w);
        h->nenv = 1.0f; h->nk = dk(0.006f + 0.014f * w);
        float lo = clampf((90.0f + 70.0f * b) * (1.2f - 0.4f * w) * pit, 30.0f, 400.0f);
        svf_set(&h->f1, lo, 0.9f + 3.0f * e);
        h->hv = 1.0f / fm_sqrtf(lo * 2.0f / SR) * 0.35f;   /* (the low-passed noise brought up to the body) */
        svf_set(&h->f2, clampf((320.0f + 420.0f * e) * pit, 60.0f, 2000.0f), 2.0f + 9.0f * e);   /* (cardboard's ring) */
        h->a[1] = e * 1.4f;
        h->ek = dk((0.05f + 0.18f * w) * d);
        break; }
    case D_RUMBLE:
        h->f0 = (40.0f + 16.0f * b) * (1.15f - 0.3f * w) * pit;
        h->f = h->f0 * (2.2f + 2.0f * b);
        h->fk = dk(0.008f + 0.02f * w);
        h->a[1] = 0.25f + 0.75f * e; h->ak[1] = dk((0.07f + 0.28f * w) * d);   /* (the rumble, after the kick) */
        h->ek = dk((0.05f + 0.15f * w) * d);
        h->idx = 2.0f + 9.0f * e;                  /* (the drive) */
        op_set(&h->o1, (500.0f + 900.0f * b) * (1.0f - 0.35f * e)); op_set(&h->o2, 60.0f + 40.0f * b);
        break;
    case D_RIM: {
        float fc = (1700.0f + 900.0f * b) * pit;
        svf_set(&h->f1, clampf(fc, 300.0f, 9000.0f), 6.0f + 30.0f * e);
        svf_set(&h->f2, clampf((420.0f + 160.0f * b) * pit, 60.0f, 2000.0f), 3.0f + 6.0f * w);
        svf_set(&h->f3, clampf(fc * (1.53f + 0.4f * rng_f(&r)), 300.0f, 12000.0f), 10.0f + 40.0f * e);
        h->a[0] = 1.0f; h->a[1] = 0.25f + 0.9f * w; h->a[2] = e > 0.4f ? (e - 0.4f) * 1.4f : 0.0f;
        h->nenv = 1.0f; h->nk = dk(0.0012f);
        h->ek = dk((0.02f + 0.05f * w + 0.08f * e * e) * d);
        break; }
    case D_SNAP: {
        int nb = 1 + (e > 0.3f) + (e > 0.7f);      /* (flammed: one, two or three snaps) */
        float t = 0;
        for (int i = 0; i < nb; i++) {
            h->g_at[i] = (uint16_t)(t * SR); h->g_len[i] = (uint16_t)((0.0025f + 0.002f * rng_f(&r)) * SR);
            h->g_amp[i] = i ? 0.45f + 0.35f * rng_f(&r) : 1.0f;
            t += 0.006f + 0.016f * e * rng_f(&r);
        }
        h->g_n = (uint8_t)nb;
        svf_set(&h->f1, clampf((1900.0f + 1100.0f * b) * pit * (1.0f + 0.1f * rng_bi(&r)), 300.0f, 9000.0f), 2.2f + 2.0f * e);
        h->df[0] = (1050.0f + 400.0f * b) * pit / SR; h->a[0] = 0.0f; h->ak[0] = dk(0.004f);   /* (the pop) */
        h->nk = dk((0.012f + 0.07f * w) * d); h->env = 0; h->ek = 1.0f;
        svf_set(&h->f2, clampf(2600.0f * pit, 200.0f, 8000.0f), 0.8f);
        break; }
    case D_BRUSH: {
        float s0 = (900.0f + 600.0f * b) * pit, s1 = s0 * (2.2f + 2.0f * e);
        if (rng_f(&r) < 0.4f) { float t = s0; s0 = s1; s1 = t; }   /* (a stroke either way) */
        h->f = s0; h->f0 = s1; h->fk = dk(0.012f + 0.03f * w - 0.008f * e);
        h->hk = (0.004f + 0.022f * w) * SR;        /* (the swell, in samples) */
        h->a[1] = e * e * 0.08f;                   /* (bristles: how often one catches) */
        h->ek = dk((0.035f + 0.11f * w) * d);
        svf_set(&h->f1, h->f, 1.3f + 1.5f * e); svf_set(&h->f2, clampf(500.0f * pit, 60.0f, 2000.0f), 0.7f);
        break; }
    case D_SYNTH:
        h->f0 = (175.0f + 70.0f * b) * (1.12f - 0.25f * w) * pit;
        h->f = h->f0 * 1.5f; h->fk = dk(0.004f + 0.006f * w);
        h->df[1] = 1.78f;                          /* (its second mode) */
        h->a[0] = 0.8f + 0.5f * w; h->a[1] = 0.45f; h->ak[0] = dk(0.03f + 0.09f * w); h->ak[1] = dk(0.02f + 0.05f * w);
        h->nenv = 0.45f + 0.9f * e; h->nk = dk((0.035f + 0.08f * w) * d);
        h->hk = e > 0.55f ? (16000.0f - 22000.0f * (e - 0.55f)) / SR : 0.0f;   /* (crushed past halfway) */
        svf_set(&h->f1, clampf((1800.0f + 1500.0f * b) * pit, 200.0f, 9000.0f), 0.7f);
        svf_set(&h->f2, clampf((4500.0f + 2500.0f * b) * pit, 400.0f, 12000.0f), 1.0f);
        h->ek = dk((0.05f + 0.1f * w) * d);
        break;
    case D_SPRING: {
        h->clen = clampi((int)((60.0f + 90.0f * w) / fm_sqrtf(pit)), 20, 158); h->cpos = 0;
        h->fb = 0.55f + 0.25f * w + 0.15f * e;     /* (the loop's feedback) */
        h->ks_apc = 0.25f + 0.6f * e;              /* (its dispersion: the chirp) */
        h->nenv = 1.0f; h->nk = dk(0.004f + 0.004f * w);
        h->f0 = (190.0f + 60.0f * b) * pit; h->df[0] = h->f0 / SR; h->a[0] = 0.7f; h->ak[0] = dk(0.025f + 0.04f * w);
        h->ek = dk((0.06f + 0.16f * w + 0.06f * e) * d);
        svf_set(&h->f1, clampf(3500.0f * pit, 300.0f, 9000.0f), 0.8f);
        for (int i = 0; i < 160; i++) h->line.comb[i] = 0;
        break; }
    case D_BLOCK: {
        float f = hz * (1.0f + 0.45f * (1.0f - w));
        int tongue = e > 0.45f && rng_f(&r) < 0.5f;   /* (a slit drum: the other tongue, a fourth or fifth up) */
        if (tongue) f *= rng_f(&r) < 0.5f ? 1.335f : 1.498f;
        float rat = lerpf(2.92f, 1.52f, clamp01(e * 1.3f));   /* (block to slit drum) */
        h->df[0] = f / SR; h->df[1] = f * rat / SR; h->df[2] = f * rat * 1.9f / SR;
        h->a[0] = 1.0f; h->a[1] = 0.55f - 0.25f * e; h->a[2] = 0.2f * (1.0f - e);
        h->ak[0] = dk((0.022f + 0.07f * w + 0.12f * e) * d); h->ak[1] = dk((0.015f + 0.035f * w + 0.06f * e) * d); h->ak[2] = dk(0.01f);
        h->bend = 0.03f + 0.03f * w; h->bk = dk(0.004f);
        h->nenv = 0.6f; h->nk = dk(0.004f + 0.004f * w);
        svf_set(&h->f1, clampf(f * 1.1f, 40.0f, 3000.0f), 4.0f);   /* (the hollow) */
        h->env = 1.0f; h->ek = dk((0.035f + 0.13f * w + 0.18f * e) * d);
        break; }
    case D_TABLA: {
        float f = hz * (1.25f - 0.55f * w);
        static const float HM[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
        for (int k = 0; k < 5; k++) {
            h->df[k] = f * HM[k] * (1.0f + 0.004f * rng_bi(&r)) / SR;
            float rim = k == 1 || k == 2 ? e : e * 0.5f;   /* (ENTROPY: the rim's tones ring) */
            h->a[k] = (k == 0 ? 1.0f : 0.5f / (float)k) * (0.4f + 1.2f * rim);
            h->ak[k] = dk((0.02f + 0.08f * e + 0.06f * w) * d / (1.0f + 0.3f * (float)k));
        }
        h->a[0] = 1.0f;
        h->bend = w * 0.18f; h->bk = dk(0.05f + 0.05f * w);   /* (the bass drum's dip) */
        h->nenv = 0.5f * (1.0f - e); h->nk = dk(0.003f);   /* (a damped slap at 0) */
        svf_set(&h->f1, clampf(f * 6.0f, 200.0f, 5000.0f), 1.0f);
        h->env = 1.0f; h->ek = dk((0.06f + 0.14f * w + 0.12f * e) * d);
        break; }
    case D_HAND: {
        float f = hz * (1.45f - 0.6f * w);
        float slap = clamp01((e - 0.35f) / 0.5f);
        h->df[0] = f / SR; h->df[1] = f * 1.51f / SR; h->df[2] = f * 1.99f / SR;
        h->a[0] = 1.0f - 0.6f * slap; h->a[1] = 0.35f + 0.3f * slap; h->a[2] = 0.15f + 0.4f * slap;
        float mute = e < 0.35f ? e / 0.35f : 1.0f - slap;   /* (open at 0, muted in the middle) */
        float L = (0.06f + 0.14f * w) * (1.0f - 0.65f * mute) * (1.0f - 0.5f * slap) * d;
        h->ak[0] = dk(L); h->ak[1] = dk(L * 0.6f); h->ak[2] = dk(L * 0.4f);
        h->bend = 0.06f; h->bk = dk(0.006f);
        h->nenv = 0.15f + 1.3f * slap; h->nk = dk(0.004f + 0.006f * slap);
        svf_set(&h->f1, clampf(f * (8.0f + 6.0f * slap), 300.0f, 7000.0f), 1.2f + 1.5f * slap);
        h->env = 1.0f; h->ek = dk(L * 2.0f + 0.02f);
        break; }
    case D_TINE: {
        float f = hz * (1.6f - 0.6f * w);
        h->df[0] = f / SR; h->df[1] = f * 5.4f / SR; h->df[2] = f * (1.0f + 0.006f + 0.02f * e) / SR;   /* (a beating twin) */
        h->a[0] = 1.0f; h->a[1] = 0.35f; h->a[2] = 0.3f * e;
        h->ak[0] = dk((0.055f + 0.14f * w) * d); h->ak[1] = dk(0.02f + 0.02f * w); h->ak[2] = dk((0.05f + 0.12f * w) * d);
        h->hv = e * e * 0.9f;                       /* (the rattle) */
        h->df[3] = (60.0f + 80.0f * rng_f(&r)) / SR;   /* (its buzz) */
        svf_set(&h->f1, clampf(f * 9.0f, 400.0f, 9000.0f), 2.0f);
        h->env = 1.0f; h->ek = dk((0.09f + 0.24f * w) * d);
        break; }
    case D_BOTTLE: {
        float f = hz * (2.2f - 0.9f * w);
        h->df[0] = f / SR; h->df[1] = f * 2.76f / SR; h->df[2] = f * 5.4f / SR;
        h->a[0] = 1.0f; h->a[1] = 0.45f; h->a[2] = 0.25f;
        h->ak[0] = dk((0.05f + 0.16f * w) * d); h->ak[1] = dk(0.04f + 0.08f * w); h->ak[2] = dk(0.02f + 0.03f * w);
        h->bend = e * 0.06f; h->bk = dk(0.06f + 0.1f * e);   /* (water: the wobble, dying) */
        h->df[4] = (9.0f + 14.0f * e) / SR;
        h->nenv = 0.1f + 0.5f * w; h->nk = dk((0.03f + 0.1f * w) * d);   /* (breath in a big one) */
        svf_set(&h->f1, clampf(f, 60.0f, 4000.0f), 6.0f);
        h->env = 1.0f; h->ek = dk((0.06f + 0.18f * w) * d);
        break; }
    case D_SHAKER:
        h->hk = (0.003f + 0.02f * w) * SR;         /* (the swell, in samples) */
        h->a[1] = 0.004f + 0.03f * e;              /* (the beads: how often one rattles) */
        h->a[2] = 0.3f + 1.2f * e;                 /* (how loud they rattle) */
        svf_set(&h->f1, clampf((6500.0f + 3500.0f * b) * pit * (1.0f - 0.3f * w), 1500.0f, 15000.0f), 1.6f - 1.0f * e);
        svf_set(&h->f2, clampf(3500.0f * pit, 400.0f, 8000.0f), 0.7f);
        h->ek = dk((0.03f + 0.15f * w) * d);
        break;
    case D_FMHAT: {
        float fc = (5200.0f + 3000.0f * b) * pit * (1.0f - 0.25f * w);
        h->df[0] = fc / SR; h->df[1] = fc * (1.41f + e * (1.0f + 1.8f * rng_f(&r))) / SR;
        h->idx = 2.0f + 4.0f * e; h->ik = dk(0.02f + 0.08f * w);
        h->fb = e * e * 0.35f;
        svf_set(&h->f2, clampf(4500.0f * pit * (1.0f - 0.2f * w), 800.0f, 12000.0f), 0.8f);
        h->ek = dk((0.015f + 0.2f * w * w) * d);
        break; }
    case D_AIR:
        h->hk = (0.002f + 0.012f * w) * SR;
        h->f0 = clampf((4200.0f + 3000.0f * b) * pit * (1.0f - 0.35f * w), 500.0f, 13000.0f);
        h->fb = 0.8f + 1.2f * w * 0.0f + 26.0f * e * e;   /* (breath to whistle) */
        h->a[1] = e * 0.03f; h->df[1] = (5.0f + 4.0f * rng_f(&r)) / SR;   /* (the whistle wavering) */
        svf_set(&h->f1, h->f0, h->fb); svf_set(&h->f2, clampf(1200.0f * pit, 100.0f, 4000.0f), 0.7f);
        h->ek = dk((0.025f + 0.12f * w) * d);
        break;
    case D_CHIP:
        h->hk = clampf((22000.0f - 16000.0f * w * w) * (0.6f + 0.6f * b) * pit, 300.0f, 44000.0f) / SR;   /* (the clock) */
        h->cpos = 1;                               /* (the shift register) */
        h->clen = e > 0.45f;                       /* (its short loop: a tone) */
        h->hk *= 1.0f - 0.5f * e;
        h->fk = e > 0.3f ? dk(0.01f + 0.03f * w) : 1.0f;   /* (stepping down) */
        h->ek = dk((0.02f + 0.14f * w) * d);
        svf_set(&h->f2, clampf(1500.0f * pit, 100.0f, 5000.0f), 0.7f);
        break;
    case D_FOIL:
        h->nenv = 1.0f; h->nk = dk((0.02f + 0.12f * w) * d);   /* (how thick the crinkles, thinning) */
        h->hk = 0.08f + 0.35f * e;                 /* (and how many) */
        svf_set(&h->f1, clampf((6200.0f + 2600.0f * b) * pit, 1200.0f, 14000.0f), 1.5f + 18.0f * e * e);
        svf_set(&h->f2, clampf((10500.0f + 2000.0f * b) * pit, 2000.0f, 16000.0f), 1.2f + 12.0f * e * e);
        svf_set(&h->f3, clampf(3000.0f * pit, 300.0f, 8000.0f), 0.7f);
        h->ek = dk((0.025f + 0.12f * w) * d);
        break;
    case D_DIGITAL:
        h->hk = clampf((3000.0f + 12000.0f * b) * pit, 150.0f, 20000.0f) / SR;    /* the sample rate (following the pitch) */
        h->fb = 8.0f - 6.0f * e;                  /* the bits */
        h->ek = dk((0.01f + 0.11f * w * w) * d);
        for (int i = 0; i < 6; i++) h->a[i] = nz(&h->ns);   /* a short loop that sings, past halfway */
        svf_set(&h->f2, clampf(3000.0f * pit, 30.0f, 3000.0f), 0.7f);
        break;
    }
}

int hit_render(Hit *h, float *out, int n)
{
    if (!h->on) return 0;
    for (int i = 0; i < n; i++) {
        float s = 0, x;
        switch (h->engine) {
        case D_MEMBRANE: {
            glide(h); fade(&h->bend, h->bk);
            float f = h->f * (1.0f + h->bend);
            h->ph[0] += f / SR; h->ph[1] += f * h->df[1] / SR; h->ph[2] += f * h->df[2] / SR;
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            if (h->ph[2] > 1024.0f) h->ph[2] -= 1024.0f;
            float m2 = sine_c(h->ph[2]) * h->a[2];
            s = sine_c(h->ph[0] + h->idx * 0.16f * m2) + h->a[1] * sine_c(h->ph[1]) + 0.5f * m2;
            fade(&h->a[1], h->ak[1]); fade(&h->a[2], h->ak[2]); fade(&h->idx, h->ik);
            s = soft_clip(s * (1.0f + 3.0f * h->w + 2.0f * h->e)) * (1.4f + 0.6f * h->w);
            x = svf_hp(&h->f1, nz(&h->ns)) * h->nenv; fade(&h->nenv, h->nk);
            s = (s + x) * h->env; fade(&h->env, h->ek);
            break; }
        case D_BOX: {
            glide(h);
            if (++h->hold >= 16.0f) { h->hold = 0; svf_set(&h->f1, h->f, h->fb); svf_set(&h->f2, h->f * 2.31f, h->fb * 0.7f); }   /* (the fall: every 16 samples) */
            x = nz(&h->ns) * h->nenv + (h->n == 0 ? 4.0f : 0.0f); fade(&h->nenv, h->nk);
            s = bpn(&h->f1, x) * 6.0f + bpn(&h->f2, x) * 3.0f;
            s = soft_clip(s * (1.0f + 2.0f * h->e + 2.0f * h->w)) * h->env; fade(&h->env, h->ek);
            break; }
        case D_FMKICK: {
            glide(h);
            h->ph[0] += h->f / SR; h->ph[1] += h->f * h->df[1] / SR;
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            float m = sine_c(h->ph[1] + h->fb * h->a[5]); h->a[5] = m;
            s = soft_clip(sine_c(h->ph[0] + h->idx * 0.16f * m) * (1.2f + 2.0f * h->w)) * 1.2f * h->env;
            fade(&h->idx, h->ik); fade(&h->env, h->ek);
            break; }
        case D_WIRES: {
            float t = h->a[0] * sine_c(h->ph[0] += h->df[0]) + h->a[1] * sine_c(h->ph[1] += h->df[1]);
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            fade(&h->a[0], h->ak[0]); fade(&h->a[1], h->ak[1]);
            x = svf_hp(&h->f1, nz(&h->ns)) * h->nenv; fade(&h->nenv, h->nk);
            int rp = h->cpos - h->clen; if (rp < 0) rp += 160;
            float y = x + h->fb * h->line.comb[rp];
            h->line.comb[h->cpos] = fm_fabsf(y) < 1e-6f ? 0.0f : soft_clip(y);
            if (++h->cpos >= 160) h->cpos = 0;
            s = soft_clip((t * (0.4f + 0.6f * h->w) + op_lp(&h->o1, y) * (1.1f - 0.4f * h->fb)) * 1.3f);
            break; }
        case D_GRAIN: {
            float g = 0; int cur = -1;
            for (int k = 0; k < h->g_n; k++) {
                uint32_t a = h->g_at[k], l = h->g_len[k];
                if (h->n < a || h->n >= a + l) continue;
                float u = (float)(h->n - a) / (float)l;
                g += h->g_amp[k] * (u < 0.3f ? u / 0.3f : (1.0f - u) / 0.7f); cur = k;
            }
            if (cur >= 0 && cur != h->g_cur) { h->g_cur = (uint8_t)cur; svf_set(&h->f1, h->g_fc[cur], 0.8f + 5.0f * h->e); }
            uint32_t last = h->g_at[h->g_n - 1] + h->g_len[h->g_n - 1];
            if (h->n >= last && h->nenv == 0 && h->hk == 0) { h->nenv = 0.25f; h->hk = 1.0f; }   /* (the tail, once) */
            float nzv = nz(&h->ns);
            x = bpn(&h->f1, nzv * g) * 2.2f + svf_hp(&h->f2, nzv) * h->nenv; fade(&h->nenv, h->nk);
            s = x + h->a[0] * sine_c(h->ph[0] += h->df[0]); fade(&h->a[0], h->ak[0]);
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            h->env = h->n < last ? 1.0f : 0.0f;
            break; }
        case D_CRACK: {
            x = nz(&h->ns) * h->nenv + (h->n == 0 ? 3.0f : 0.0f); fade(&h->nenv, h->nk);
            float q = h->fb * 0.18f;
            s = (bpn(&h->f1, x) + 0.8f * bpn(&h->f2, x) + 0.6f * bpn(&h->f3, x)) * q * h->env;
            s += h->a[0] * sine_c(h->ph[0] += h->df[0]); fade(&h->a[0], h->ak[0]);
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            fade(&h->env, h->ek);
            s = soft_clip(s);
            break; }
        case D_MATERIAL: {
            fade(&h->bend, h->bk); float bm = 1.0f + h->bend;
            for (int k = 0; k < 5; k++) if (h->a[k] > 0) {
                h->ph[k] += h->df[k] * bm; if (h->ph[k] > 1024.0f) h->ph[k] -= 1024.0f;
                s += h->a[k] * sine_c(h->ph[k]); fade(&h->a[k], h->ak[k]);
                if (h->a[k] < 2e-5f) h->a[k] = 0;
            }
            x = bpn(&h->f1, nz(&h->ns)) * h->nenv; fade(&h->nenv, h->nk);
            s = (s * 0.8f + 2.6f * x) * 0.7f;
            break; }
        case D_FMPERC: {
            glide(h);
            h->ph[0] += h->f / SR; h->ph[1] += h->f * h->df[1] / SR;
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            float m = sine_c(h->ph[1] + h->fb * h->a[4]); h->a[4] = m;
            float v = sine_c(h->ph[0] + h->idx * 0.15f * m);
            h->hold += h->hk; if (h->hold >= 1.0f) { h->hold -= 1.0f; h->hv = v; }
            s = h->hv * h->env; fade(&h->idx, h->ik); fade(&h->env, h->ek);
            break; }
        case D_STRING: {
            h->ks_len += (h->f - h->ks_len) * h->fk;   /* (slack: settling, quickly) */
            if (h->ks_len > (float)(KS_LEN - 3)) h->ks_len = (float)(KS_LEN - 3);
            float rp = (float)h->cpos - h->ks_len; while (rp < 0) rp += (float)KS_LEN;
            int i0 = (int)rp; float fr = rp - (float)i0; int i1 = (i0 + 1) & (KS_LEN - 1);
            float y = ((float)h->line.ks[i0] + ((float)h->line.ks[i1] - (float)h->line.ks[i0]) * fr) * (1.0f / 16384.0f);
            h->ks_lp += (y - h->ks_lp) * h->ks_damp;
            float ap = -h->ks_apc * h->ks_lp + h->ks_ap + h->ks_apc * h->hv;   /* (an all-pass in the loop: dispersion) */
            h->ks_ap = h->ks_lp; h->hv = ap; if (fm_fabsf(h->hv) < 1e-7f) h->hv = 0;
            h->line.ks[h->cpos] = to16(ap * h->fb);
            h->cpos = (h->cpos + 1) & (KS_LEN - 1);
            s = y;
            h->hold += (fm_fabsf(y) - h->hold) * 0.002f; if (h->n > 2000 && h->hold < LIVE) h->hold = 0;
            break; }
        case D_METAL: {
            float q[6];
            for (int k = 0; k < 6; k++) { h->ph[k] += h->df[k]; if (h->ph[k] >= 1.0f) h->ph[k] -= 1.0f; q[k] = h->ph[k] < 0.5f ? 1.0f : -1.0f; }
            float rm = h->e > 0.5f ? (h->e - 0.5f) * 2.0f : 0.0f;
            s = (q[0] + q[1] + q[2] + q[3] + q[4] + q[5]) * (1.0f - 0.5f * rm) + rm * (q[0] * q[3] + q[1] * q[4] + q[2] * q[5]) * 1.2f;
            s = svf_hp(&h->f2, bpn(&h->f1, s * 0.3f)) * 2.0f;
            s *= h->env; fade(&h->env, h->ek);
            break; }
        case D_DUST: {
            float imp = nu(&h->ns) < h->hk ? nz(&h->ns) * 3.0f : 0.0f;
            float att = h->n * h->nk < 1.0f ? (float)h->n * h->nk : 1.0f;
            s = svf_hp(&h->f2, bpn(&h->f1, imp)) * 1.5f * att * h->env; fade(&h->env, h->ek);
            break; }
        case D_808: {
            glide(h);
            h->ph[0] += h->f / SR; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            s = sine_c(h->ph[0]);
            s = s + h->e * 0.25f * (soft_clip(s * 2.5f) - s);                 /* (warmth, a little) */
            x = op_lp(&h->o1, nz(&h->ns)) * h->nenv; fade(&h->nenv, h->nk);
            s = (s + x * 1.5f + op_lp(&h->o2, nz(&h->ns)) * h->e * 0.02f) * h->env;   /* (and dust) */
            if (h->n < 66) { float t = (float)h->n / 66.0f; s *= t * t * (3.0f - 2.0f * t); }   /* (a soft front: 1.5 ms) */
            fade(&h->env, h->ek);
            break; }
        case D_CLAP: {
            float g = 0;
            for (int k = 0; k < h->g_n; k++) {
                uint32_t a = h->g_at[k], l = h->g_len[k];
                if (h->n < a || h->n >= a + l * 3u) continue;
                float u = (float)(h->n - a) / (float)l;
                g += h->g_amp[k] * fm_expf(-u * 1.6f);   /* (each burst: a slap, falling away) */
            }
            if (h->nenv == 0 && (float)h->n >= h->hk * SR) h->nenv = 0.55f;   /* (the tail, after the last burst) */
            float n0 = nz(&h->ns);
            x = bpn(&h->f1, n0) * (g + h->nenv) * 2.6f + bpn(&h->f2, n0) * g * 0.8f;
            fade(&h->nenv, h->nk);
            s = svf_hp(&h->f3, x);
            h->env = (float)h->n < h->hk * SR + 400.0f ? 1.0f : 0.0f;
            break; }
        case D_VOX: {
            h->ph[0] += h->df[0]; if (h->ph[0] >= 1.0f) h->ph[0] -= 1.0f;
            float buzz = h->ph[0] < 0.08f ? 1.0f : -0.087f;   /* (a pulse: the voice's buzz) */
            float ex = buzz * h->hk + nz(&h->ns) * (1.0f - h->hk);
            if (h->bend > 0 && (h->n & 15u) == 0) {    /* the vowel sliding, from one to the other */
                float m = clamp01((float)h->n / (0.02f * SR)) * clamp01(h->bend);
                svf_set(&h->f1, lerpf(h->a[0], h->ak[0], m), 6.0f); svf_set(&h->f2, lerpf(h->a[1], h->ak[1], m), 6.0f); svf_set(&h->f3, lerpf(h->a[2], h->ak[2], m), 6.0f);
            }
            s = (bpn(&h->f1, ex) * 0.7f + bpn(&h->f2, ex) + bpn(&h->f3, ex) * 0.8f) * h->env;
            fade(&h->env, h->ek);
            break; }
        case D_CLICK: {
            float imp = 0;
            for (int k = 0; k < h->g_n; k++) if (h->n == h->g_at[k]) imp += h->g_amp[k] * 6.0f;
            float y = bpn(&h->f1, imp);
            s = svf_hp(&h->f2, y + imp * 0.15f * (1.0f - h->e)) * (0.3f + 0.7f * h->env);   /* (the click itself, and its ring dying) */
            fade(&h->env, h->ek);
            if (h->n > 300u + RAMP && h->env < 0.02f) h->env = 0;
            break; }
        case D_FOLEY:
            switch (h->g_cur) {
            case 0: {
                x = nz(&h->ns) * h->nenv + (h->n == RAMP + 1 ? 1.5f : 0.0f); fade(&h->nenv, h->nk);
                float xl = op_lp(&h->o1, x);
                s = bpn(&h->f1, xl) * 3.0f + bpn(&h->f2, xl) * 1.5f + bpn(&h->f3, x) * 0.35f + h->a[0] * sine_c(h->ph[0] += h->df[0]);
                if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
                fade(&h->a[0], h->ak[0]);
                s = soft_clip(s * 1.2f) * h->env;
                break; }
            case 1: {
                x = nz(&h->ns) * h->nenv + (h->n == RAMP + 1 ? 0.8f : 0.0f); fade(&h->nenv, h->nk);
                float xl = op_lp(&h->o1, x);
                s = soft_clip((bpn(&h->f1, xl) * 2.4f + bpn(&h->f2, xl) * 1.4f + bpn(&h->f3, xl) * 0.5f) * 1.3f * WOOD_G) * h->env;   /* (as loud as the box; its bang rounded) */
                break; }
            case 2: {
                float th = h->a[0] * sine_c(h->ph[0] += h->df[0]) + svf_lp(&h->f3, nz(&h->ns)) * h->nenv * 2.5f;
                if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
                fade(&h->a[0], h->ak[0]); fade(&h->nenv, h->nk);
                if (h->n == h->g_at[0]) h->a[1] = h->g_amp[0];
                float grit = 0;
                if (h->a[1] > 0) { float imp = nu(&h->ns) < h->hk ? nz(&h->ns) * 2.0f : 0.0f; grit = op_lp(&h->o1, bpn(&h->f1, imp)) * h->a[1]; fade(&h->a[1], h->ak[1]); }
                s = soft_clip(th * 1.1f + grit * 1.6f) * FOOT_G * h->env;
                break; }
            default: {
                if ((h->n & 15u) == 0) {            /* (its centre wandering, a little) */
                    h->bend += nz(&h->ns) * 0.08f; h->bend = clampf(h->bend * 0.97f, -0.4f, 0.4f);
                    svf_set(&h->f1, h->fb * (1.0f + h->bend), 2.2f);
                }
                h->hold += h->hk * (0.6f + 0.8f * nu(&h->ns));
                if (h->hold >= 1.0f) { h->hold -= 1.0f; h->hv = 0.25f + 0.75f * nu(&h->ns); }   /* (it catches: a new grip) */
                h->a[2] += (h->hv - h->a[2]) * 0.01f;   /* (eased: no step) */
                float att = (float)h->n < 0.008f * SR ? (float)h->n / (0.008f * SR) : 1.0f;
                float n0 = nz(&h->ns);
                s = op_lp(&h->o1, bpn(&h->f1, n0) * 1.8f + bpn(&h->f2, n0) * 0.8f) * h->a[2] * att * h->env * DRAG_G;
                break; }
            }
            fade(&h->env, h->ek);
            h->idx += (fm_fabsf(s) - h->idx) * 0.003f;   /* (how loud it still is: it ends when it can't be heard) */
            break;
        case D_BENT: {
            glide(h);
            if (h->a[3] > 0 && --h->cpos <= 0) {   /* the pitch jumping, on its own clock */
                h->cpos = h->clen; h->bend = semis((float)(int)(h->a[3] * nz(&h->ns)));
            }
            float m = h->f * h->bend;
            float sb = h->ph[1] < 0.5f ? 4.0f * h->ph[1] - 1.0f : 3.0f - 4.0f * h->ph[1];   /* (B: a triangle) */
            float sa = h->ph[0] < h->a[2] ? 1.0f : -1.0f;                                    /* (A: a pulse) */
            h->ph[0] += h->df[0] * m * (1.0f + h->a[0] * sb); h->ph[0] -= fm_floorf(h->ph[0]);
            h->ph[1] += h->df[1] * m * (1.0f + h->a[1] * sa); h->ph[1] -= fm_floorf(h->ph[1]);
            float v = sa * 0.6f + sb * 0.5f * h->a[1] * 0.5f;
            h->hold += h->hk; if (h->hold >= 1.0f) { h->hold -= 1.0f; h->hv = (float)(int)(v * 6.0f) * (1.0f / 6.0f); }   /* (held, and few steps) */
            float g = 1.0f;
            if (h->a[4] > 0) { h->ph[5] += h->a[4]; if (h->ph[5] >= 1.0f) h->ph[5] -= 1.0f; g = h->ph[5] < 0.55f ? 1.0f : 0.0f; h->a[5] += (g - h->a[5]) * 0.08f; g = h->a[5]; }   /* (the chatter: edges eased, ~0.3 ms) */
            s = soft_clip(svf_hp(&h->f2, h->hv) * 1.6f) * g * h->env;   /* (clipped: as loud wherever the bends are) */
            fade(&h->env, h->ek);
            break; }
        /* ---------------- the twenty more ---------------- */
        case D_PING: {
            glide(h);
            if (++h->hold >= 8.0f) { h->hold = 0; svf_set(&h->f1, h->f, h->fb); if (h->a[1] > 0) svf_set(&h->f2, h->f * h->df[1], h->fb * 0.6f); }
            x = h->n == 0 ? SR / (6.2832f * h->f) * 0.5f : 0.0f;   /* (struck: once) */
            s = svf_bp(&h->f1, x);
            if (h->a[1] > 0) s += h->a[1] * svf_bp(&h->f2, x * 0.7f);
            s = soft_clip(s * h->idx) * (1.0f + 0.4f * h->idx) * 0.6f * h->env; fade(&h->env, h->ek);
            break; }
        case D_FOLD: {
            glide(h);
            h->ph[0] += h->f / SR; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            float g = 1.0f + h->a[0];
            s = fold(sine_c(h->ph[0]) * g + h->hv * h->a[0] * 0.3f) * h->env;
            fade(&h->a[0], h->ak[0]); fade(&h->env, h->ek);
            break; }
        case D_TAPE: {
            glide(h);
            h->ph[1] += h->df[1]; if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            h->ph[2] += h->df[2]; if (h->ph[2] > 1024.0f) h->ph[2] -= 1024.0f;
            float wob = h->a[1] * sine_c(h->ph[1]) + h->a[2] * sine_c(h->ph[2]);
            h->ph[0] += h->f * (1.0f + wob) / SR; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            s = op_lp(&h->o1, soft_clip(sine_c(h->ph[0]) * h->idx)) * (0.9f + 0.25f * h->idx);
            if (h->a[3] > 0) s *= 1.0f - h->a[3] * (0.5f + 0.5f * sine_c(h->ph[2] * 0.37f + 0.25f));   /* (dropouts) */
            s = (s + op_lp(&h->o2, nz(&h->ns)) * h->a[1] * 0.4f) * h->env; fade(&h->env, h->ek);
            break; }
        case D_THUD: {
            glide(h);
            h->ph[0] += h->f / SR; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            x = nz(&h->ns) * h->nenv; fade(&h->nenv, h->nk);
            s = sine_c(h->ph[0]) * h->a[0] + svf_lp(&h->f1, x) * h->hv;
            if (h->a[1] > 0) s += bpn(&h->f2, x) * h->a[1];
            s = soft_clip(s * 1.3f) * h->env; fade(&h->a[0], h->ak[0]); fade(&h->env, h->ek);
            break; }
        case D_RUMBLE: {
            glide(h);
            h->ph[0] += h->f / SR; h->ph[1] += h->f * 0.973f / SR;
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            float k = sine_c(h->ph[0]) * h->env, rm = (sine_c(h->ph[1]) + op_lp(&h->o2, nz(&h->ns)) * 6.0f) * h->a[1];
            s = op_lp(&h->o1, soft_clip((k + rm) * h->idx)) * 1.3f;
            fade(&h->env, h->ek); fade(&h->a[1], h->ak[1]);
            break; }
        case D_RIM: {
            x = (h->n == 0 ? 3.0f : 0.0f) + nz(&h->ns) * h->nenv; fade(&h->nenv, h->nk);
            s = bpn(&h->f1, x) * h->a[0] + bpn(&h->f2, x) * h->a[1];
            if (h->a[2] > 0) s += bpn(&h->f3, x) * h->a[2];
            s = soft_clip(s * 2.0f) * h->env; fade(&h->env, h->ek);
            break; }
        case D_SNAP: {
            float g = 0;
            for (int k = 0; k < h->g_n; k++) {
                uint32_t at = h->g_at[k], l = h->g_len[k];
                if (h->n >= at && h->n < at + l) { float u = (float)(h->n - at) / (float)l; g += h->g_amp[k] * (1.0f - u) * (1.0f - u); }
                if (h->n == at) h->a[0] = fm_maxf(h->a[0], h->g_amp[k]);
            }
            if (h->g_n && h->n == h->g_at[h->g_n - 1] + h->g_len[h->g_n - 1]) h->nenv = 0.35f;   /* (its tail) */
            x = nz(&h->ns);
            h->ph[0] += h->df[0]; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            s = bpn(&h->f1, x * (g + h->nenv)) * 2.2f + sine_c(h->ph[0]) * h->a[0] * 0.5f + svf_lp(&h->f2, x) * h->nenv * 0.3f;
            fade(&h->a[0], h->ak[0]); fade(&h->nenv, h->nk);
            if (h->g_n && h->n > (uint32_t)h->g_at[h->g_n - 1] + h->g_len[h->g_n - 1] && h->nenv == 0) h->env = 0;
            else h->env = 1.0f;
            break; }
        case D_BRUSH: {
            glide(h);
            if (++h->hold >= 16.0f) { h->hold = 0; svf_set(&h->f1, h->f, 1.3f); }
            float sw = (float)h->n < h->hk ? (float)h->n / h->hk : 1.0f;
            x = nz(&h->ns);
            if (h->a[1] > 0 && nu(&h->ns) < h->a[1]) h->hv = 1.0f;   /* (a bristle catches) */
            fade(&h->hv, 0.97f);
            s = (bpn(&h->f1, x) * (0.8f + h->hv * 2.0f) + svf_lp(&h->f2, x) * 0.15f) * sw * h->env;
            if ((float)h->n >= h->hk) fade(&h->env, h->ek);
            break; }
        case D_SYNTH: {
            glide(h);
            h->ph[0] += h->f / SR; h->ph[1] += h->f * h->df[1] / SR;
            if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            float body = sine_c(h->ph[0]) * h->a[0] + sine_c(h->ph[1]) * h->a[1];
            float nzz = nz(&h->ns);
            if (h->hk > 0) { h->hold += h->hk; if (h->hold >= 1.0f) { h->hold -= 1.0f; h->hv = nzz; } nzz = h->hv; }   /* (crushed) */
            x = (svf_hp(&h->f1, nzz) + bpn(&h->f2, nzz) * 0.6f) * h->nenv;
            s = soft_clip((body + x) * (1.2f + 0.8f * h->w)) * h->env;
            fade(&h->a[0], h->ak[0]); fade(&h->a[1], h->ak[1]); fade(&h->nenv, h->nk); fade(&h->env, h->ek);
            break; }
        case D_SPRING: {
            x = svf_hp(&h->f1, nz(&h->ns)) * h->nenv; fade(&h->nenv, h->nk);
            int rp = h->cpos - h->clen; if (rp < 0) rp += 160;
            float v = h->line.comb[rp];
            float y1 = -h->ks_apc * v + h->ks_ap; h->ks_ap = v + h->ks_apc * y1;        /* (two allpasses: dispersion) */
            float y2 = -h->ks_apc * y1 + h->ks_lp; h->ks_lp = y1 + h->ks_apc * y2;
            float y = x + h->fb * y2;
            h->line.comb[h->cpos] = fm_fabsf(y) < 1e-6f ? 0.0f : soft_clip(y);
            if (++h->cpos >= 160) h->cpos = 0;
            h->ph[0] += h->df[0]; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            s = (y * 0.8f + sine_c(h->ph[0]) * h->a[0]) * h->env;
            fade(&h->a[0], h->ak[0]); fade(&h->env, h->ek);
            if (fm_fabsf(h->ks_ap) < 1e-20f) h->ks_ap = 0;
            if (fm_fabsf(h->ks_lp) < 1e-20f) h->ks_lp = 0;
            break; }
        case D_BLOCK: case D_TABLA: case D_HAND: case D_TINE: case D_BOTTLE: {   /* the tuned ones: modes, a bend, noise */
            float bm = 1.0f;
            if (h->engine == D_BOTTLE) { h->ph[4] += h->df[4]; if (h->ph[4] > 1024.0f) h->ph[4] -= 1024.0f; bm += h->bend * sine_c(h->ph[4]); fade(&h->bend, h->bk); }
            else { bm += h->bend; fade(&h->bend, h->bk); }
            int nm = h->engine == D_TABLA ? 5 : 3;
            for (int k = 0; k < nm; k++) if (h->a[k] > 0) {
                h->ph[k] += h->df[k] * bm; if (h->ph[k] > 1024.0f) h->ph[k] -= 1024.0f;
                s += h->a[k] * sine_c(h->ph[k]); fade(&h->a[k], h->ak[k]);
                if (h->a[k] < 2e-5f) h->a[k] = 0;
            }
            if (h->engine == D_TINE && h->hv > 0) {   /* (the rattle: a buzz riding the tine) */
                h->ph[3] += h->df[3]; if (h->ph[3] > 1024.0f) h->ph[3] -= 1024.0f;
                s += bpn(&h->f1, nz(&h->ns)) * h->hv * h->a[0] * (h->ph[3] - fm_floorf(h->ph[3]) < 0.3f ? 1.0f : 0.0f);
            } else if (h->nenv > 0) { s += bpn(&h->f1, nz(&h->ns)) * h->nenv * 1.6f; fade(&h->nenv, h->nk); }
            s *= h->env; fade(&h->env, h->ek);
            break; }
        case D_SHAKER: {
            float sw = (float)h->n < h->hk ? (float)h->n / h->hk : 1.0f;
            if (nu(&h->ns) < h->a[1]) h->hv = h->a[2];   /* (a bead) */
            fade(&h->hv, 0.93f);
            x = nz(&h->ns);
            s = svf_hp(&h->f2, bpn(&h->f1, x)) * (0.6f + h->hv) * sw * h->env;
            if ((float)h->n >= h->hk) fade(&h->env, h->ek);
            break; }
        case D_FMHAT: {
            h->ph[1] += h->df[1]; if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
            float m = sine_c(h->ph[1] + h->fb * h->a[5]); h->a[5] = m;
            h->ph[0] += h->df[0]; if (h->ph[0] > 1024.0f) h->ph[0] -= 1024.0f;
            s = svf_hp(&h->f2, sine_c(h->ph[0] + h->idx * 0.16f * m)) * h->env;
            fade(&h->idx, h->ik); fade(&h->env, h->ek);
            break; }
        case D_AIR: {
            float sw = (float)h->n < h->hk ? (float)h->n / h->hk : 1.0f;
            if (h->a[1] > 0 && (h->n & 15u) == 0) {
                h->ph[1] += h->df[1] * 16.0f; if (h->ph[1] > 1024.0f) h->ph[1] -= 1024.0f;
                svf_set(&h->f1, h->f0 * (1.0f + h->a[1] * sine_c(h->ph[1])), h->fb);
            }
            x = nz(&h->ns);
            s = (bpn(&h->f1, x) + svf_hp(&h->f2, x) * 0.12f) * sw * h->env;
            if ((float)h->n >= h->hk) fade(&h->env, h->ek);
            break; }
        case D_CHIP: {
            h->hold += h->hk;
            if (h->hold >= 1.0f) {                 /* (the register clocked: the 8-bit console's noise) */
                h->hold -= 1.0f;
                uint32_t sr = (uint32_t)h->cpos, fbk = (sr ^ (sr >> (h->clen ? 6 : 1))) & 1u;
                h->cpos = (int)((sr >> 1) | (fbk << 14));
                h->hv = (h->cpos & 1) ? 1.0f : -1.0f;
            }
            if (h->fk < 1.0f) h->hk *= 1.0f - (1.0f - h->fk) * 0.25f;   /* (stepping down) */
            float lv = (float)(int)(h->env * 15.0f + 0.5f) * (1.0f / 15.0f);   /* (sixteen steps) */
            s = svf_hp(&h->f2, h->hv) * lv * 0.5f; fade(&h->env, h->ek);
            break; }
        case D_FOIL: {
            float imp = nu(&h->ns) < h->hk * h->nenv ? nz(&h->ns) * 2.5f : 0.0f;
            s = svf_hp(&h->f3, bpn(&h->f1, imp) + bpn(&h->f2, imp) * 0.7f) * 1.4f * h->env;
            fade(&h->nenv, h->nk); fade(&h->env, h->ek);
            break; }
        case D_DIGITAL: {
            float j = 1.0f + h->e * 0.8f * nz(&h->ns);
            h->hold += h->hk * j;
            if (h->hold >= 1.0f) {
                h->hold -= 1.0f;
                float v = h->e > 0.5f ? h->a[(h->n / 7u) % 6u] * 2.0f : nz(&h->ns);   /* (past halfway, a loop of six: it sings; as loud) */
                float qn = fm_exp2f(h->fb - 1.0f);
                h->hv = (float)(int)(v * qn) / qn;
            }
            s = svf_hp(&h->f2, h->hv) * h->env * 0.6f; fade(&h->env, h->ek);
            break; }
        }
        if (h->fr.tail || h->n < h->fr.len) {
            float y = front_run(h, s);
            if (h->fr.xg > 0) {                   /* (the tail let go, over 6 ms: the body goes on plain) */
                y = s + (y - s) * h->fr.xg; h->fr.xg -= 1.0f / 256.0f;
                if (h->fr.xg <= 0) { h->fr.xg = 0; h->fr.tail = 0; }
            }
            s = y;
        }
        if (h->n < RAMP) { float t = (float)(h->n + 1) * (1.0f / RAMP); s *= t * t * (3.0f - 2.0f * t); }   /* (an S-curve: no corner where it ends) */
        if (h->killk > 0) { h->kill -= h->killk; if (h->kill <= 0) { h->on = 0; return 0; } s *= h->kill; }
        out[i] += s * h->vel;
        h->n++;
    }
    /* the filters' memories snapped to zero once they near the denormals, far below hearing (dsp.h's tiny guard signal cancels out when a
     * sound runs an even number of filters, and the memories would creep into denormals: the hidden CPU spike) */
    {
        Svf *fs[5] = { &h->f1, &h->f2, &h->f3, &h->fr.f, &h->fr.f2 };
        for (int k = 0; k < 5; k++) { if (fm_fabsf(fs[k]->ic1) < 1e-30f) fs[k]->ic1 = 0; if (fm_fabsf(fs[k]->ic2) < 1e-30f) fs[k]->ic2 = 0; }
        if (fm_fabsf(h->o1.z) < 1e-30f) h->o1.z = 0;
        if (fm_fabsf(h->o2.z) < 1e-30f) h->o2.z = 0;
        if (h->fr.fol < 1e-30f) h->fr.fol = 0;
        /* the decays (fade) far gone: zero, so nothing creeps on into the denormals (in a block's 32 samples, nothing
         * gets from 1e-6 anywhere near them) */
        snap6(&h->env); snap6(&h->nenv); snap6(&h->idx); snap6(&h->bend); snap6(&h->hv); snap6(&h->fr.za); snap6(&h->fr.nzv);
        for (int k = 0; k < 6; k++) snap6(&h->a[k]);
    }
    /* finished: everything in it has died away */
    float lvl = h->env + h->nenv;
    switch (h->engine) {
    case D_WIRES: lvl = h->a[0] + h->a[1] + h->nenv + (h->n < 4000u ? 1.0f : 0.0f) + fm_fabsf(h->line.comb[h->cpos]) * 4.0f; break;
    case D_GRAIN: lvl = h->env + h->nenv + h->a[0] + (h->hk == 0 ? 1.0f : 0.0f); break;
    case D_CRACK: lvl = h->env + h->a[0]; break;
    case D_MATERIAL: lvl = h->a[0] + h->a[1] + h->a[2] + h->a[3] + h->a[4] + h->nenv; break;
    case D_STRING: lvl = h->hold + (h->n < 2000u ? 1.0f : 0.0f); break;
    case D_CLAP: lvl = h->env + h->nenv; break;
    case D_CLICK: lvl = h->env + (h->n < 400u ? 1.0f : 0.0f); break;
    case D_FOLEY: lvl = fm_minf(h->env, h->idx * 20.0f) + (h->n < 2500u ? 1.0f : 0.0f); break;
    case D_SNAP: lvl = h->nenv + h->a[0] + (h->env > 0 ? 0.001f : 0.0f) + (h->n < 4000u ? 1.0f : 0.0f); break;
    case D_SPRING: lvl = h->env * (fm_fabsf(h->ks_ap) + fm_fabsf(h->ks_lp) + 0.01f) + h->nenv + (h->n < 2000u ? 1.0f : 0.0f); break;
    case D_BLOCK: case D_TABLA: case D_HAND: case D_TINE: case D_BOTTLE: lvl = fm_minf(h->env, h->a[0] + h->a[1] + h->a[2] + h->a[3] + h->a[4] + h->nenv); break;
    case D_RUMBLE: lvl = h->env + h->a[1]; break;
    case D_BRUSH: case D_SHAKER: case D_AIR: lvl = h->env + ((float)h->n < h->hk + 100.0f ? 1.0f : 0.0f); break;
    default: break;
    }
    lvl *= h->vr;                                 /* (how loud it is against a full hit: a ghost note's tail ends sooner) */
    if (lvl < LIVE) h->on = 0;
    else if (lvl < 1e-3f && h->killk == 0) h->killk = 1.0f / (0.004f * SR);   /* (at -60 dB below a full hit, under the mix:
                                                    * faded out over 4 ms and done, rather than rendered on to silence) */
    if (h->fr.tail && h->fr.xg == 0 && h->n >= h->fr.len && lvl < FR_TAIL_END) h->fr.xg = 1.0f;   /* (the attack's character
                                                    * through the hit let go once the hit is far down: the rest of it plain) */
    return h->on;
}
