/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the drum engines (drums.h). Every fading value snaps to zero once it can't be heard: values
 * creeping towards zero forever (denormals) cost the processor many times more, a hidden spike. Every hit opens over
 * RAMP samples, and a voice taken for another fades out over 3 ms: no clicks. */
#include "drums.h"

const char *const ENGINE_NAME[NUM_ENGINES] = { "MEMBRANE", "BOX", "FM KICK", "WIRES", "GRAIN", "CRACK",
                                               "MATERIAL", "FM PERC", "STRING", "METAL", "DUST", "DIGITAL" };

/* each engine's level at a firm hit, mid settings: measured (tools/render calibrate), so the four parts sit together */
static const float GAIN[NUM_ENGINES] = { 0.42f, 2.7f, 0.7f, 0.96f, 0.85f, 2.3f, 0.62f, 0.45f, 1.12f, 0.88f, 0.65f, 1.15f };

#define RAMP 10                          /* samples of opening (0.23 ms): the hit's edge kept, its click not */
#define LIVE 1e-4f                       /* -80 dB: gone */

static inline float nz(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return (float)(int32_t)x * (1.0f / 2147483648.0f); }
static inline float nu(uint32_t *s) { return nz(s) * 0.5f + 0.5f; }
static inline float dk(float sec) { if (sec < 0.0005f) sec = 0.0005f; return fm_expf(-1.0f / (sec * SR)); }
static inline void fade(float *x, float k) { *x *= k; if (fm_fabsf(*x) < 1e-6f) *x = 0; }
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

void hit_start(Hit *h, const HitP *p)
{
    Rng r; rng_seed(&r, p->seed);
    float w = clamp01(p->weight), e = clamp01(p->entropy), b = clamp01(p->bright), d = clampf(p->decay, 0.1f, 8.0f);
    int eng = p->engine % NUM_ENGINES;
    memset(h, 0, sizeof *h - sizeof h->line);    /* (the line: its engine fills it) */
    h->on = 1; h->engine = (uint8_t)eng; h->vel = clamp01(p->vel) * GAIN[eng];
    h->w = w; h->e = e; h->b = b; h->kill = 1.0f; h->env = 1.0f; h->ns = p->seed | 1u;
    float pit = semis(p->note - 60.0f), hz = clampf(mtof(p->note), 30.0f, 6000.0f);
    switch (eng) {
    case D_MEMBRANE:
        h->f0 = (38.0f + 22.0f * b) * (1.25f - 0.45f * w) * pit;
        h->f = h->f0 * (2.2f + 4.0f * b + 2.0f * (1.0f - w));
        h->fk = dk(0.006f + 0.035f * w);
        h->ek = dk((0.07f + 0.6f * w * fm_sqrtf(w)) * d);
        h->df[1] = 1.593f * (1.0f + e * 0.25f * rng_bi(&r)); h->df[2] = 2.136f * (1.0f + e * 0.4f * rng_bi(&r));   /* (ratios) */
        h->a[1] = 0.35f - 0.2f * w; h->a[2] = 0.2f + 0.2f * e; h->ak[1] = h->ak[2] = dk(0.04f + 0.1f * w);
        h->idx = e * e * 2.5f; h->ik = dk(0.05f + 0.25f * e);
        h->nenv = 0.25f + 0.4f * b * (1.0f - w); h->nk = dk(0.0025f);
        svf_set(&h->f1, 2500.0f, 0.7f);
        if (e > 0.4f) { h->bend = (e - 0.4f) * 0.6f * rng_bi(&r); h->bk = dk(0.08f); }
        break;
    case D_BOX:
        h->f0 = (45.0f + 30.0f * b) * (1.2f - 0.4f * w) * pit;
        h->f = e < 0.6f ? h->f0 * (1.8f + 2.0f * b) : h->f0 * (0.55f + 0.2f * rng_f(&r));   /* (ENTROPY past 0.6: rising) */
        if (e >= 0.6f) h->f0 *= 1.0f + (e - 0.6f) * 1.5f;
        h->fk = dk(0.012f + 0.05f * w);
        h->ek = dk((0.12f + 0.5f * w) * d);
        h->nenv = 1.0f; h->nk = dk(0.003f + 0.012f * w);
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
        h->ek = dk((0.1f + 0.6f * w) * d);
        break;
    case D_WIRES: {
        h->f0 = (165.0f + 90.0f * b) * pit;
        h->df[0] = h->f0 / SR; h->df[1] = h->f0 * 1.59f / SR; h->a[0] = 0.55f; h->a[1] = 0.3f;
        h->ak[0] = h->ak[1] = dk(0.03f + 0.09f * w);
        h->nenv = 1.0f; h->nk = dk((0.06f + 0.25f * w) * d); h->env = 0; h->ek = 1.0f;
        float fc = 900.0f + 2200.0f * b + 600.0f * e * rng_bi(&r);
        h->clen = clampi((int)(SR / fc), 8, 159);
        h->fb = 0.15f + 0.82f * fm_powf(e, 1.2f);   /* the wires ringing into metal */
        memset(h->line.comb, 0, sizeof h->line.comb);
        svf_set(&h->f1, 350.0f, 0.7f); op_set(&h->o1, 3000.0f + 6000.0f * b);
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
            h->g_fc[i] = (2500.0f + 4000.0f * b) * (1.0f + e * (rng_f(&r) * 1.6f - 0.6f));
        }
        h->g_n = (uint8_t)n; h->g_cur = 255;
        h->df[0] = 190.0f * pit / SR; h->a[0] = 0.3f * w + 0.08f; h->ak[0] = dk(0.04f);
        h->nk = dk(0.02f + 0.1f * w * d); h->env = 0; h->ek = 1.0f;
        svf_set(&h->f2, 400.0f, 0.7f);
        break; }
    case D_CRACK: {
        static const float FR[3] = { 1700.0f, 2950.0f, 4800.0f };
        float q = 8.0f + 25.0f * e + 10.0f * w;
        svf_set(&h->f1, FR[0] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        svf_set(&h->f2, FR[1] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        svf_set(&h->f3, FR[2] * (0.7f + 0.6f * b) * (1.0f + e * 0.35f * rng_bi(&r)) * pit, q);
        h->nenv = 1.0f; h->nk = dk(0.001f);
        h->ek = dk((0.05f + 0.3f * w) * d);
        h->df[0] = 160.0f * pit / SR; h->a[0] = 0.4f * w; h->ak[0] = dk(0.05f);
        h->fb = q;
        break; }
    case D_MATERIAL: {
        static const float R[3][4] = { { 1.0f, 2.57f, 4.21f, 6.3f }, { 1.0f, 2.756f, 5.404f, 8.933f }, { 1.0f, 2.32f, 4.25f, 6.63f } };
        static const float T[3][4] = { { 0.15f, 0.05f, 0.025f, 0.012f }, { 1.2f, 0.8f, 0.45f, 0.25f }, { 1.5f, 1.0f, 0.6f, 0.35f } };
        static const float A[4] = { 0.7f, 0.45f, 0.3f, 0.2f };
        int s0 = e < 0.5f ? 0 : 1; float m = e < 0.5f ? e * 2.0f : (e - 0.5f) * 2.0f;   /* wood -> metal -> glass */
        for (int i = 0; i < 4; i++) {
            float rr = lerpf(R[s0][i], R[s0 + 1][i], m), tt = lerpf(T[s0][i], T[s0 + 1][i], m);
            float f = hz * rr; if (f > SR * 0.42f) continue;
            h->df[i] = f / SR; h->a[i] = A[i] * (i ? 0.5f + 1.0f * b : 1.0f); h->ak[i] = dk(tt * (0.25f + 1.6f * w) * d);
        }
        if (e > 0.66f) {                          /* glass: a twin beating against the first, the strike bending up */
            h->df[4] = hz * 1.004f / SR; h->a[4] = 0.3f; h->ak[4] = h->ak[0];
            h->bend = -(e - 0.66f) * 0.15f; h->bk = dk(0.04f);
        }
        h->nenv = 0.5f; h->nk = dk(0.002f); svf_set(&h->f1, fm_minf(hz * 3.0f, 9000.0f), 1.2f);
        h->env = 0; h->ek = 1.0f;
        break; }
    case D_FMPERC: {
        h->f0 = hz; float sw = 0.3f + 3.0f * e;
        h->f = hz * (1.0f + sw);
        h->fk = dk(0.004f + 0.02f * (1.0f - e));
        h->df[1] = e < 0.3f ? 2.0f : 1.41f + e * 2.3f * rng_f(&r);
        h->idx = 1.0f + 3.0f * w + 1.5f * b; h->ik = dk(0.03f + 0.15f * w);
        h->fb = e * 0.2f;
        h->ek = dk((0.04f + 0.35f * w) * d);
        h->hk = e > 0.6f ? 1.0f - (e - 0.6f) * 2.0f : 1.0f; if (h->hk < 0.12f) h->hk = 0.12f;   /* (decimated past 0.6) */
        break; }
    case D_STRING: {
        float f = hz; while (f < SR / (float)(KS_LEN - 4)) f *= 2.0f;
        h->ks_len = SR / f; h->ks_rate = 1.0f + 0.06f * e;   /* slack: the line lengthening */
        float T = (0.15f + 2.5f * w) * d;
        h->ks_damp = 0.25f + 0.7f * b;            /* the loop's low-pass: bright, or dull and woody */
        h->fb = fm_expf(-1.0f / (T * f));         /* the loop's gain a pass */
        h->ks_apc = e * 0.6f;                     /* dispersion: the overtones stretched, metallic */
        int len = (int)h->ks_len + 2; OnePole lp; lp.z = 0; op_set(&lp, 1500.0f + 9000.0f * b);
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
        h->ek = dk((0.018f + 0.6f * fm_powf(w, 2.2f)) * d);
        svf_set(&h->f1, 7000.0f + 4000.0f * b, 1.2f); svf_set(&h->f2, 5000.0f + 2500.0f * b, 0.7f);
        break; }
    case D_DUST:
        h->hk = 0.02f + 0.25f * w;               /* grains a sample */
        h->ek = dk((0.03f + 0.25f * w) * d);
        h->nk = 1.0f / ((0.001f + 0.03f * e) * SR);   /* the smear: a shaker's soft front */
        svf_set(&h->f1, 4000.0f + 5000.0f * b, 0.7f + 8.0f * e); svf_set(&h->f2, 2500.0f, 0.7f);
        break;
    case D_DIGITAL:
        h->hk = (3000.0f + 12000.0f * b) / SR;    /* the sample rate */
        h->fb = 8.0f - 6.0f * e;                  /* the bits */
        h->ek = dk((0.02f + 0.4f * w * w) * d);
        for (int i = 0; i < 6; i++) h->a[i] = nz(&h->ns);   /* a short loop that sings, past halfway */
        svf_set(&h->f2, 3000.0f, 0.7f);
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
            s = (s + 2.0f * x) * 0.7f;
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
            h->ks_len *= 1.0f + (h->ks_rate - 1.0f) * 0.00002f;   /* (slack: sagging, slowly) */
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
        case D_DIGITAL: {
            float j = 1.0f + h->e * 0.8f * nz(&h->ns);
            h->hold += h->hk * j;
            if (h->hold >= 1.0f) {
                h->hold -= 1.0f;
                float v = h->e > 0.5f ? h->a[(h->n / 7u) % 6u] : nz(&h->ns);   /* (past halfway, a loop of six: it sings) */
                float qn = fm_exp2f(h->fb - 1.0f);
                h->hv = (float)(int)(v * qn) / qn;
            }
            s = svf_hp(&h->f2, h->hv) * h->env * 0.6f; fade(&h->env, h->ek);
            break; }
        }
        if (h->n < RAMP) { float t = (float)(h->n + 1) * (1.0f / RAMP); s *= t * t * (3.0f - 2.0f * t); }   /* (an S-curve: no corner where it ends) */
        if (h->killk > 0) { h->kill -= h->killk; if (h->kill <= 0) { h->on = 0; return 0; } s *= h->kill; }
        out[i] += s * h->vel;
        h->n++;
    }
    /* finished: everything in it has died away */
    float lvl = h->env + h->nenv;
    switch (h->engine) {
    case D_WIRES: lvl = h->a[0] + h->a[1] + h->nenv + (h->n < 4000u ? 1.0f : 0.0f) + fm_fabsf(h->line.comb[h->cpos]) * 4.0f; break;
    case D_GRAIN: lvl = h->env + h->nenv + h->a[0] + (h->hk == 0 ? 1.0f : 0.0f); break;
    case D_CRACK: lvl = h->env + h->a[0]; break;
    case D_MATERIAL: lvl = h->a[0] + h->a[1] + h->a[2] + h->a[3] + h->a[4] + h->nenv; break;
    case D_STRING: lvl = h->hold + (h->n < 2000u ? 1.0f : 0.0f); break;
    default: break;
    }
    if (lvl < LIVE) h->on = 0;
    return h->on;
}
