/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the engine (engine.h). Click-free throughout: every gain glides a sample at a time, a voice
 * taken for another fades out over 3 ms, an effect's type changes through a short dip, the punch-ins cross over in
 * 4 ms, filters' settings move in small steps every few samples. */
#include "engine.h"

const char *const PU_NAME[NUM_PU] = { "", "LOW-PASS", "RATCHET", "REPEAT 1/8", "REPEAT 1/16", "REPEAT 1/32", "GLITCH",
    "PITCH DOWN", "PITCH UP", "TAPE STOP", "REVERSE", "FREEZE", "SWELL", "CRUSH", "KILL KICK", "REPEAT 1/12", "HIGH-PASS" };

/* the pool: the engine and the screen's canvas (29 KB) must fit the FM-1's 336 KB, with room for fm1-base's own */
typedef char engine_fits_the_pool[sizeof(Engine) <= 285u * 1024u ? 1 : -1];

#define LOOK 12                          /* ticks dealt ahead: room for a hit nudged early */
#define BAR (TPB * 4)

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float from16(int16_t v) { return (float)v * (1.0f / 16384.0f); }
static inline int16_t to16(float x) { x = clampf(x, -1.99f, 1.99f); return (int16_t)(x * 16384.0f); }
static inline float glide_k(float sec) { return 1.0f - fm_expf(-1.0f / (sec * SR)); }

static uint32_t sig_of(const Groove *g, int p)
{
    const PartP *q = &g->p[p];
    uint32_t h = g->seed * 2654435761u;
    h ^= (uint32_t)q->salt * 0x85EBCA6Bu; h ^= (uint32_t)q->gen << 3 ^ (uint32_t)q->len << 7 ^ (uint32_t)q->rate << 13;
    h ^= (uint32_t)q->muts * 0xC2B2AE35u;
    if (q->gen == G_MAP) h ^= (uint32_t)g->mapx << 17 ^ (uint32_t)g->mapy << 24;
    return h | 1u;
}

/* the DJ filter (TONE): 12 o'clock open, left a low-pass coming down, right a high-pass going up */
static void tone_set(Part *P, float v)
{
    float lp = v < 0.5f ? 20000.0f * fm_exp2f(-(0.5f - v) * 2.0f * 7.2f) : 20000.0f;
    float hp = v > 0.5f ? 20.0f * fm_exp2f((v - 0.5f) * 2.0f * 8.6f) : 20.0f;
    for (int c = 0; c < 2; c++) { svf_set_res(&P->lp[c], lp, 0.8f); svf_set(&P->hp[c], hp, 0.75f); }
}

void engine_init(Engine *e, Groove *g)
{
    memset(e, 0, sizeof *e);
    e->g = g;
    rng_seed(&e->rng, g->seed ^ 0xA5A5u);
    reverb_init(&e->rv, 1.0f); reverb_set(&e->rv, 1.2f, 6000.0f);
    for (int p = 0; p < NPARTS; p++) {
        Part *P = &e->P[p];
        pattern_make(&P->pat, g, p); P->sig = sig_of(g, p);
        P->present = P->present_t = 1.0f; P->gain = 0;
        P->tone_now = (float)g->p[p].tone * 0.01f; tone_set(P, P->tone_now);
        {   /* each part's floor: the low end left to the kick (the snare from 100 Hz, the perc from 50, the hats from 300) */
            static const float FLOOR[NPARTS] = { 0, 100.0f, 50.0f, 300.0f };
            for (int c = 0; c < 2; c++) svf_set(&P->floor[c], FLOOR[p] > 0 ? FLOOR[p] : 20.0f, 0.707f);
        }
        P->fw.len = 23.0f + 7.0f * (float)p; P->fe.len = 31.0f + 5.0f * (float)p; P->fb.len = 19.0f + 11.0f * (float)p;
    }
    e->bpm = (float)(g->tempo + 40);
    e->rate = e->bpm * ((float)TPB / 60.0f) / SR;
    e->dt_a = e->dt_b = 2000.0f; e->dxf = 1.0f; e->dly_dip = 1.0f; e->room_dip = 1.0f;
    e->room_t_now = g->room_t; e->dly_t_now = g->delay_t;
    e->cmp_g = 1.0f; e->kill_g = 1.0f; e->gov = 1.0f;
    e->ai_ground = e->ai_light = -1;
    e->midi_ch = 9;
    for (int c = 0; c < 2; c++) { svf_set(&e->plp[c], 18000.0f, 0.7f); svf_set(&e->php[c], 20.0f, 0.7f); op_set(&e->dlp[c], 6500.0f); }
    e->lp_c = e->hp_c = 0;
    e->dg_rate = e->dg_rate_t = 1.0f;
    e->smash = g->comp_t == CT_SMASH ? 1.0f : 0.0f;
}

float engine_part_tps(const Engine *e, int part) { return (float)RATE_TICKS[e->P[part].pat.rate % NUM_RATES]; }

void engine_all_off(Engine *e)
{
    for (int p = 0; p < NPARTS; p++) { for (int v = 0; v < NV; v++) hit_choke(&e->P[p].v[v]); e->P[p].nq = 0; e->P[p].nat = 0; }
    if (e->midi) for (int p = 0; p < NPARTS; p++) if (e->off_left[p]) { e->midi(0x08u | (0x80u | e->midi_ch) << 8 | (uint32_t)e->off_note[p] << 16); e->off_left[p] = 0; }
}

void engine_overload(Engine *e) { e->shed++; }

void engine_hear(Engine *e, int ch, int note, int vel)
{
    if (vel <= 0) return;
    e->ai_act += 1.0f; e->ai_notes++;
    if (ch == 0) { e->ai_ground = note; e->ai_ground_new = 1; }   /* GROUND */
    if (ch == 2) { e->ai_light = note; e->ai_light_new = 1; }     /* LIGHT */
}

void engine_play(Engine *e, int part, float vel, int alt)
{
    uint32_t w = e->kq_w;
    if (w - e->kq_r >= 16u) return;
    e->kq[w & 15u] = (uint32_t)(part & 3) | (uint32_t)clampi((int)(vel * 127.0f), 1, 127) << 8 | (uint32_t)(alt ? 1 : 0) << 16;
    e->kq_w = w + 1u;
}

/* ------------------------------------------------------------------ dealing a step */
static void push_ev(Part *P, uint32_t t0, float off, const HitP *hp, float pan, int alt)
{
    if (P->nq >= EVQ) return;
    float o = fm_floorf(off);
    Ev *v = &P->q[P->nq++];
    v->tick = (int32_t)(t0 + (uint32_t)(int32_t)o); v->sub = off - o; v->hp = *hp; v->pan = pan; v->alt = (uint8_t)alt;
}

static int cond_ok(int cd, uint32_t cyc, int fill)
{
    switch (cd) {
    case CD_1_2: return cyc % 2u == 0;
    case CD_2_2: return cyc % 2u == 1;
    case CD_1_4: return cyc % 4u == 0;
    case CD_3_4: return cyc % 4u == 2;
    case CD_FILL: return fill;
    case CD_NOTFILL: return !fill;
    default: return cyc % 8u == 0;
    }
}

static float part_note(const Engine *e, int p, float pitch)
{
    const Groove *g = e->g;
    if (p != P_PERC) return 60.0f + (float)g->p[p].pitch + pitch;
    const Scale *sc = &SCALES[g->scale % NUM_SCALES];
    int deg = (int)pitch, n = sc->n, o = deg >= 0 ? deg / n : -((-deg + n - 1) / n), k = deg - o * n;
    int root = g->follow && e->ai_ground >= 0 ? e->ai_ground % 12 : g->key;   /* (in After Images' key, as its GROUND has it) */
    return 36.0f + (float)root + (float)(sc->st[k] + 12 * o) + (float)g->p[p].pitch;   /* (low: C2 to B2, just above the kick) */
}

static void make_hit(Engine *e, int p, float vel, float pitch, int alt, HitP *hp)
{
    const Groove *g = e->g; const PartP *q = &g->p[p]; Part *P = &e->P[p];
    /* MOOD: hard, staccato and bright at 0; soft, long, full and darker at 100. Between, a variety: each part dealt its
     * own place (most spread at the middle, none at the ends), each hit a little either side */
    float mood = (float)g->mood * 0.01f, spread = 1.0f - fm_fabsf(2.0f * mood - 1.0f);
    uint32_t hsh = (g->seed ^ (uint32_t)q->salt * 2654435761u) * 2246822519u;
    float own = (float)(hsh >> 8) * (1.0f / 16777216.0f) - 0.5f;   /* (the part's place, -0.5..0.5: fixed for the groove) */
    float c = clamp01(mood + own * spread * 1.1f + rng_bi(&e->rng) * 0.08f * spread);
    hp->engine = q->engine;
    hp->vel = clamp01(vel);
    hp->weight = clamp01((float)q->weight * 0.01f + (c - 0.45f) * 0.3f + P->fw_v);
    hp->entropy = clamp01((float)q->entropy * 0.01f + P->fe_v);
    hp->bright = clamp01(0.88f - 0.7f * c + P->fb_v);
    hp->decay = fm_exp2f(((float)q->decay - 50.0f) * 0.04f) * fm_exp2f(lerpf(-1.2f, p == P_KICK ? 0.8f : 1.3f, c));   /* (x0.44 .. x2.5; the kick x1.7) */
    hp->soft = clamp01((c - 0.55f) / 0.45f);
    hp->bright = clamp01(hp->bright + (hp->vel - 0.7f) * 0.25f);   /* (harder: brighter, and a little heavier) */
    hp->weight = clamp01(hp->weight + (hp->vel - 0.7f) * 0.12f);
    hp->note = part_note(e, p, pitch);
    if (alt) {
        if (p == P_HATS) { hp->decay *= 4.0f; hp->weight = clamp01(hp->weight + 0.1f); }   /* the open hat */
        else if (p == P_SNARE) { hp->decay *= 0.35f; hp->entropy = clamp01(hp->entropy + 0.2f); }
        else if (p == P_PERC) { hp->note += 12.0f; }
        else hp->decay *= 0.6f;
    }
    uint8_t rb = e->rnd_bits[p];
    if (rb) {                                    /* a black key held: this hit's colours thrown, held till the next */
        float d = e->rnd_depth[p];
        if (rb & 1u) P->tone_rnd = rng_bi(&e->rng) * 0.45f * d;
        if (rb & 2u) hp->weight = clamp01(hp->weight + rng_bi(&e->rng) * 0.6f * d);
        if (rb & 4u) hp->entropy = clamp01(hp->entropy + rng_bi(&e->rng) * 0.7f * d);
    } else P->tone_rnd = 0;
    hp->seed = rng_u32(&e->rng); hp->gseed = g->seed ^ (uint32_t)q->salt;
}

static float hit_pan(Engine *e, int p)
{
    const Groove *g = e->g;
    if (p == P_KICK) return 0.5f;
    float pan = (float)g->p[p].pan * 0.01f;
    if (g->pan_t == PT_SCATTER) pan += rng_bi(&e->rng) * 0.5f * (float)g->pan * 0.01f * (float)g->p[p].s_pan * 0.01f;   /* (each hit thrown somewhere) */
    return clamp01(pan);
}

/* a fill: a bouncing ball (the gaps shrinking) on the snare, or a run down the perc, or the hats rolling */
static void fill_roll(Engine *e, uint32_t t0)
{
    int kind = rng_int(&e->rng, 3);
    float off = 0, gap = 20.0f, v = 0.75f;
    for (int k = 0; k < 14 && off < (float)TPB; k++) {
        int p = kind == 0 ? P_SNARE : kind == 1 ? P_PERC : P_HATS;
        HitP hp; make_hit(e, p, v, kind == 1 ? (float)(6 - k) : 0.0f, kind == 0 && (k & 1), &hp);
        push_ev(&e->P[p], t0, off, &hp, hit_pan(e, p), 0);
        off += gap; gap *= 0.8f; v *= kind == 2 ? 1.03f : 0.93f;
    }
    e->fill_until = t0 + TPB;
}

static void deal_step(Engine *e, int p, uint32_t s)
{
    Part *P = &e->P[p]; Groove *g = e->g; Pattern *pt = &P->pat;
    int len = pt->len; uint32_t tps = RATE_TICKS[pt->rate % NUM_RATES], t0 = s * tps;
    uint32_t bar = t0 / BAR; int in16 = (int)((t0 % BAR) / 24u), on16 = t0 % 24u == 0;
    float E = (float)g->evolve * 0.01f, A = (float)g->activity * 0.01f, L = (float)g->link * 0.01f, W = (float)g->warp * 0.01f, C = (float)g->chaos * 0.01f;
    int fill = bar % 4u == 3u;
    /* EVOLVE past 85: now and then a beat played from another beat of the pattern (reality) */
    uint32_t spb = TPB / tps; if (!spb) spb = 1;
    if (t0 % TPB < tps) {
        int beat = (int)(t0 / TPB);
        if (beat != P->remap_beat) { P->remap_beat = beat; P->remap = E > 0.85f && rng_chance(&e->rng, (E - 0.85f) * 3.3f) ? (int)spb * (1 + rng_int(&e->rng, 3)) : 0; }
    }
    int ls = (int)((s + (uint32_t)P->remap) % (uint32_t)len);
    uint32_t cyc = s / (uint32_t)len;
    const Step *st = &pt->s[ls];
    float act = A * 0.9f + ((float)g->p[p].density - 50.0f) * 0.006f + L * 0.3f * clamp01((e->ai_act - 4.0f) / 16.0f);   /* After Images busier: busier */
    float thr = 255.0f * (1.0f - clampf(act, 0.02f, 1.0f));
    int plays = st->lvl > 0 && (float)st->lvl >= thr;
    if (p == P_KICK && ls == 0 && E < 0.9f && st->lvl >= 250) plays = 1;   /* (the one: always, till EVOLVE is right up) */
    float vel = (float)st->vel / 127.0f, pitch = (float)st->pitch, off = 0;
    int alt = st->alt, ratch = 1;
    int comp = st->ct && g->evolve >= st->cu ? st->ct : C_NONE;
    switch (comp) {
    case C_COND: { int ok = cond_ok(st->ca, cyc, fill); if (plays) plays = ok; else if (ok && st->lvl > 20) { plays = 1; vel = fm_maxf(vel * 0.8f, 0.35f); } break; }
    case C_PROB: if (plays) plays = rng_f(&e->rng) * 100.0f < (float)st->ca; break;
    case C_GHOST: if (!plays) { plays = 1; vel = (float)st->ca / 127.0f; } break;
    case C_RATCHET: ratch = st->ca; break;
    case C_NUDGE: off += (float)((int)st->ca - 128); break;
    case C_ALT: alt = !alt; break;
    case C_PITCH: pitch += (float)((int)st->ca - 128); break;
    case C_ACCENT: vel += (float)st->ca / 127.0f; break;
    default: break;
    }
    /* LINK: the parts, and After Images, answering each other */
    if (p == P_KICK && e->ai_ground_new && L > 0) {
        e->ai_ground_new = 0;
        if (!plays && (float)st->lvl > thr * 0.35f && on16 && rng_chance(&e->rng, L)) { plays = 1; vel = 0.7f; }
        else if (plays) vel += 0.25f * L;
    }
    if (p == P_HATS && L > 0 && plays)
        for (int k = 0; k < 8; k++) if (e->kick_ticks[k] == t0 && t0) { if (L > 0.7f && rng_chance(&e->rng, L * 0.6f)) plays = 0; else vel *= 1.0f - 0.5f * L; break; }
    if (p == P_PERC && e->ai_light_new && L > 0 && on16) {
        e->ai_light_new = 0;
        if (rng_chance(&e->rng, L * 0.7f) && e->ai_light >= 0) {   /* LIGHT's melody answered, an eighth later, an octave up */
            HitP hp; make_hit(e, P_PERC, 0.35f, 0, 0, &hp);
            hp.note = (float)(e->ai_light % 12 + 48) + (float)g->p[P_PERC].pitch;
            push_ev(P, t0, 48.0f, &hp, hit_pan(e, p), 0);
        }
    }
    /* CHAOS: rolls, gaps and hits pushed off the grid, at random */
    if (plays && p != P_KICK && rng_chance(&e->rng, C * 0.25f)) ratch = ratch > 1 ? ratch : 2 + rng_int(&e->rng, 3);
    if (plays && !(p == P_KICK && ls == 0) && rng_chance(&e->rng, C * 0.12f)) plays = 0;
    if (plays && p != P_KICK && rng_chance(&e->rng, C * 0.2f)) off += rng_chance(&e->rng, 0.5f) ? 12.0f : -12.0f;
    /* SWING (the groove's): the off sixteenths late. WARP, the humaniser: every hit's timing loosened and its
     * velocity varied, a player's lean (the snare a touch late, the hats a touch early); the kick held tighter */
    float sw = (float)g->swing * 0.0018f;
    if (t0 % 48u == 24u) off += sw * 48.0f; else if (t0 % 24u == 12u) off += sw * 24.0f;
    float hum = (rng_bi(&e->rng) + rng_bi(&e->rng)) * 0.5f;      /* (two dice: near the beat more often than far) */
    off += hum * W * (p == P_KICK ? 2.5f : 6.0f);
    if (p == P_SNARE) off += W * 2.5f;
    if (p == P_HATS) off -= W * 1.5f;
    vel *= 1.0f + rng_bi(&e->rng) * W * (p == P_KICK ? 0.15f : 0.35f);
    /* the velocity's shape: the bar's weight (the one strongest, then three, then two and four, the eighths, the
     * sixteenths softest; the kick leaning on it most), an accent every few sixteenths drifting across the bar, and a
     * slow wave over a few bars. All dealt by REC */
    {
        float B = (float)g->breathe * 0.01f;
        int pos = (int)((t0 % BAR) / 24u);
        float mw = t0 % 24u ? 0.45f : pos == 0 ? 1.0f : pos == 8 ? 0.9f : pos % 4 == 0 ? 0.8f : pos % 2 == 0 ? 0.66f : 0.55f;
        vel *= lerpf(1.0f, mw, B * (p == P_KICK ? 0.7f : 0.45f)) * (1.0f + 0.25f * B);
        if (g->accent) vel *= (t0 / 24u) % g->accent == 0 ? 1.0f + 0.2f * B : 1.0f - 0.08f * B;
        vel *= 1.0f + 0.14f * B * sine_c((float)(t0 % (BAR * 3u)) / (float)(BAR * 3u));   /* (three bars: against the fours) */
    }
    if (off < -(float)LOOK) off = -(float)LOOK;
    /* a fill: the last beat of every 8 bars (every 4, EVOLVE well up) */
    if (p == P_SNARE && on16 && in16 == 12 && g->evolve >= 30 && (bar % 8u == 7u || (E > 0.6f && fill)) && rng_chance(&e->rng, 0.3f + 0.5f * E)) {
        fill_roll(e, t0);
        return;
    }
    if ((p == P_KICK || p == P_SNARE) && t0 < e->fill_until && t0 + TPB > e->fill_until && in16 > 12) plays = 0;
    if (!plays) return;
    if (p == P_KICK) { e->kick_ticks[e->kick_n++ & 7] = t0; }
    HitP hp; make_hit(e, p, vel, pitch, alt, &hp);
    float pan = hit_pan(e, p);
    if (ratch > 4) ratch = 4;
    for (int k = 0; k < ratch; k++) {
        HitP h2 = hp; h2.vel = clamp01(hp.vel * fm_powf(0.82f, (float)k));
        if (k) h2.seed = rng_u32(&e->rng);
        push_ev(P, t0, off + (float)tps * (float)k / (float)ratch, &h2, pan, alt);
    }
}

/* ------------------------------------------------------------------ a block: control */
static void bar_turned(Engine *e, uint32_t bar)
{
    Groove *g = e->g;
    float E = (float)g->evolve * 0.01f, D = (float)g->drift * 0.01f;
    /* EVOLVE past 70: the patterns rewrite themselves, a step at a time (SAVE keeps how far) */
    if (E > 0.7f && rng_chance(&e->rng, (E - 0.7f) / 0.3f * 0.6f)) {   /* (at full: a rewrite most bars) */
        int p = rng_chance(&e->rng, 0.3f) ? P_KICK + rng_int(&e->rng, 2) : P_PERC + rng_int(&e->rng, 2);
        PartP *q = &g->p[p];
        if (q->muts < 999u) { pattern_mutate(&e->P[p].pat, g, p, q->muts); q->muts++; e->P[p].sig = sig_of(g, p); e->mutated++; }
    }
    /* DRIFT: parts fading out and back, a few bars at a time (never fewer than two playing) */
    static const float OUT[NPARTS] = { 0.25f, 0.3f, 0.5f, 0.45f };
    int in = 0;
    for (int p = 0; p < NPARTS; p++) {
        Part *P = &e->P[p];
        if (D <= 0) { P->present_t = 1.0f; P->drift_bars = 0; continue; }
        if (--P->drift_bars <= 0) {
            P->drift_bars = 2 + rng_int(&e->rng, 7);
            P->present_t = rng_chance(&e->rng, D * OUT[p]) ? 0.0f : 1.0f;
        }
        in += P->present_t > 0.5f;
    }
    for (int p = 0; in < 2 && p < NPARTS; p++) if (e->P[p].present_t < 0.5f) { e->P[p].present_t = 1.0f; in++; }
    (void)bar;
}

static void midi_hit(Engine *e, int p, const Ev *v)
{
    if (!e->midi || !e->midi_on) return;
    static const uint8_t NOTE[NPARTS][2] = { { 36, 35 }, { 38, 40 }, { 0, 0 }, { 42, 46 } };
    int note = p == P_PERC ? clampi((int)(v->hp.note + 0.5f), 0, 127) : NOTE[p][v->alt ? 1 : 0];
    if (e->off_left[p]) e->midi(0x08u | (0x80u | e->midi_ch) << 8 | (uint32_t)e->off_note[p] << 16);
    e->midi(0x09u | (0x90u | e->midi_ch) << 8 | (uint32_t)note << 16 | (uint32_t)clampi((int)(v->hp.vel * 127.0f), 1, 127) << 24);
    e->off_note[p] = (uint8_t)note; e->off_left[p] = 12;   /* (its note-off ~70 ms on) */
}

void engine_block_begin(Engine *e, int nsub)
{
    Groove *g = e->g;
    int n = nsub * BLK; if (n > BLKN) n = BLKN;
    e->nblk = n;
    float bpm = e->ext_bpm > 20.0f ? e->ext_bpm : (float)(g->tempo + 40);
    e->bpm = bpm;
    float r = bpm * ((float)TPB / 60.0f) / SR;
    if (e->beat_adj != 0.0f) {                   /* following a clock: our beat walked onto the leader's, a little a block */
        float take = clampf(e->beat_adj * 0.08f, -0.05f, 0.05f);
        e->beat_adj -= take; r += take * (float)TPB / (float)n;
        if (r < 0.0005f) r = 0.0005f;
    }
    e->rate = r;
    if (e->shed != e->shed_seen) { e->shed_seen = e->shed; e->gov = fm_maxf(0.3f, e->gov * 0.8f); }
    if (e->sync_req) {
        e->sync_req = 0; e->tick = 0; e->frac = 0; e->clk_tick = 0; e->last_bar = 0; e->fill_until = 0;
        for (int p = 0; p < NPARTS; p++) { e->P[p].step = 0; e->P[p].nq = 0; e->P[p].remap = 0; }
    }
    for (int p = 0; p < NPARTS; p++) {           /* a pattern's settings changed (a knob, REC, a saved groove): written anew */
        Part *P = &e->P[p];
        uint32_t sg = sig_of(g, p);
        if (sg != P->sig) {
            uint32_t old_tps = RATE_TICKS[P->pat.rate % NUM_RATES];
            pattern_make(&P->pat, g, p); P->sig = sg;
            uint32_t tps = RATE_TICKS[P->pat.rate % NUM_RATES];
            if (tps != old_tps) P->step = (e->tick + LOOK + tps - 1u) / tps;   /* (a new rate: stepping on from here) */
        }
        P->nat = 0;
    }
    /* the keys' hits: now */
    while (e->kq_r != e->kq_w) {
        uint32_t k = e->kq[e->kq_r & 15u]; e->kq_r++;
        int p = (int)(k & 3u); HitP hp; make_hit(e, p, (float)(k >> 8 & 127u) / 127.0f, 0, (int)(k >> 16 & 1u), &hp);
        push_ev(&e->P[p], e->tick, e->frac, &hp, hit_pan(e, p), (int)(k >> 16 & 1u));
    }
    /* FLOW: the parts' colours wandering, slowly */
    float dt = (float)n / SR, F = (float)g->flow * 0.01f;
    for (int p = 0; p < NPARTS; p++) {
        Part *P = &e->P[p];
        P->fw_v = drift_step(&P->fw, &e->rng, dt) * F * 0.25f;
        P->fe_v = drift_step(&P->fe, &e->rng, dt) * F * 0.3f;
        P->fb_v = drift_step(&P->fb, &e->rng, dt) * F * 0.2f;
    }
    e->ai_act *= fm_expf(-dt / 1.5f);
    for (int p = 0; p < NPARTS; p++) if (e->off_left[p] && --e->off_left[p] == 0 && e->midi) e->midi(0x08u | (0x80u | e->midi_ch) << 8 | (uint32_t)e->off_note[p] << 16);
    float span = r * (float)n, endf = e->frac + span;
    uint32_t end = e->tick + (uint32_t)endf;      /* (the block's last tick, whole) */
    if (e->playing) {
        /* the bars turning */
        while (e->last_bar * BAR + BAR <= end) { e->last_bar++; bar_turned(e, e->last_bar); }
        /* the steps: each part dealt one ahead of time */
        for (int p = 0; p < NPARTS; p++) {
            Part *P = &e->P[p]; uint32_t tps = RATE_TICKS[P->pat.rate % NUM_RATES];
            int guard = 0;
            while ((int32_t)(P->step * tps - (uint32_t)LOOK - end) <= 0 && guard++ < 16) deal_step(e, p, P->step++);
        }
        /* MIDI clock out: 24 a beat (every 4 ticks) */
        if (e->midi && e->midi_clock) {
            int guard = 0;
            while ((int32_t)(e->clk_tick - end) <= 0 && guard++ < 32) { e->midi(0x0Fu | 0xF8u << 8); e->clk_tick += 4u; }
        }
    }
    /* the hits due this block: each at its sample */
    for (int p = 0; p < NPARTS; p++) {
        Part *P = &e->P[p];
        for (int i = 1; i < P->nq; i++) {        /* in time order (a nudge or a roll can put one before another) */
            Ev v = P->q[i]; int j = i;
            while (j > 0 && ((int32_t)(P->q[j - 1].tick - v.tick) > 0 || (P->q[j - 1].tick == v.tick && P->q[j - 1].sub > v.sub))) { P->q[j] = P->q[j - 1]; j--; }
            P->q[j] = v;
        }
        int k = 0;
        for (; k < P->nq; k++) {
            float at = ((float)(int32_t)(P->q[k].tick - e->tick) + P->q[k].sub - e->frac) / r;
            if (at >= (float)n) break;
            P->at[k] = (int16_t)clampi((int)at, 0, n - 1);
            midi_hit(e, p, &P->q[k]);
            UiHit u; u.part = (uint8_t)p; u.vel = (uint8_t)clampi((int)(P->q[k].hp.vel * 127.0f), 1, 127); u.alt = P->q[k].alt;
            u.dur = (uint8_t)clampi((int)(P->q[k].hp.decay * (p == P_HATS ? 1.0f : p == P_PERC ? 4.0f : 2.5f)), 1, 30);
            e->uiq[e->uiq_head % UI_Q] = u; e->uiq_head++;
            e->events[p]++;
        }
        P->nat = (uint8_t)k;
    }
    e->tick += (uint32_t)endf; e->frac = endf - fm_floorf(endf);
    e->beat = (float)(e->tick % ((uint32_t)TPB * 4096u)) / (float)TPB + e->frac / (float)TPB;
}

/* ------------------------------------------------------------------ a block: a part's voices */
static void start_voice(Engine *e, Part *P, int p, const Ev *v)
{
    int sounding = 0, free_v = -1, quiet = -1; float ql = 1e9f;
    int cap = e->gov < 0.6f ? 2 : e->gov < 0.85f ? 3 : 4;
    for (int i = 0; i < NV; i++) {
        Hit *h = &P->v[i];
        if (!h->on) { if (free_v < 0) free_v = i; continue; }
        if (p == P_KICK) hit_choke(h);                                /* a kick ends the last: no mud */
        if (p == P_HATS && !v->alt && P->valt[i]) hit_choke(h);       /* a closed hat chokes the open one */
        if (h->killk > 0) continue;
        sounding++;
        float l = hit_level(h); if (l < ql) { ql = l; quiet = i; }
    }
    if (sounding >= cap && quiet >= 0) hit_choke(&P->v[quiet]);
    if (free_v < 0) {                            /* all busy, some fading: the faintest of those (all but out) */
        float k = 2.0f;
        for (int i = 0; i < NV; i++) if (P->v[i].killk > 0 && P->v[i].kill < k) { k = P->v[i].kill; free_v = i; }
        if (free_v < 0) return;
    }
    hit_start(&P->v[free_v], &v->hp);
    P->vl[free_v] = fm_cosf(v->pan * PI_F * 0.5f) * 1.41f; P->vr[free_v] = fm_sinf(v->pan * PI_F * 0.5f) * 1.41f;
    P->valt[free_v] = v->alt;
    (void)e;
}

void engine_block_part(Engine *e, int p)
{
    Part *P = &e->P[p]; const PartP *q = &e->g->p[p];
    int n = e->nblk;
    uint32_t c0 = e->clock ? e->clock() : 0;
    static float tmp_s[NPARTS][BLKN];            /* (one a part: two cores may render two parts at once) */
    float *tmp = tmp_s[p];
    memset(P->bus, 0, sizeof P->bus);
    int ev = 0, at = 0;
    while (at < n) {
        int until = ev < P->nat ? P->at[ev] : n;
        if (until < at) until = at;
        if (until > at) {
            for (int i = 0; i < NV; i++) {
                Hit *h = &P->v[i]; if (!h->on) continue;
                memset(tmp + at, 0, sizeof(float) * (unsigned)(until - at));
                hit_render(h, tmp + at, until - at);
                float gl = P->vl[i], gr = P->vr[i];
                for (int j = at; j < until; j++) { P->bus[0][j] += tmp[j] * gl; P->bus[1][j] += tmp[j] * gr; }
            }
            at = until;
        }
        while (ev < P->nat && P->at[ev] <= at) start_voice(e, P, p, &P->q[ev++]);
        if (ev >= P->nat && at >= n) break;
    }
    while (ev < P->nat) start_voice(e, P, p, &P->q[ev++]);   /* (none left over; but never dropped) */
    if (P->nat) {                                /* (the ones played, out: no memmove on the FM-1's library) */
        for (int i = P->nat; i < P->nq; i++) P->q[i - P->nat] = P->q[i];
        P->nq -= P->nat; P->nat = 0;
    }
    /* TONE (the DJ filter), the level, DRIFT's presence: every sample glided */
    float tone_t = clamp01((float)q->tone * 0.01f + P->tone_rnd), lv = (float)q->level * 0.01f, gain_t = lv * fm_sqrtf(lv) * 1.15f;
    float kt = glide_k(P->tone_rnd != 0 ? 0.006f : 0.03f), kg = glide_k(0.02f), kp = glide_k(60.0f / fm_maxf(e->bpm, 30.0f));
    float pk = 0;
    for (int i = 0; i < n; i++) {
        if ((P->tone_cnt++ & 7) == 0 && fm_fabsf(tone_t - P->tone_now) > 0.0005f) { P->tone_now += (tone_t - P->tone_now) * fm_minf(1.0f, kt * 8.0f); tone_set(P, P->tone_now); }
        P->gain += (gain_t - P->gain) * kg; P->present += (P->present_t - P->present) * kp;
        float g = P->gain * P->present;
        int use_lp = P->tone_now < 0.505f, use_hp = P->tone_now > 0.495f;   /* (one side at a time; the other held ready) */
        for (int c = 0; c < 2; c++) {
            float x = P->bus[c][i];
            if (use_lp) x = svf_lp(&P->lp[c], x); else { P->lp[c].ic1 = 0; P->lp[c].ic2 = x; }   /* (its state: the sound itself) */
            if (use_hp) x = svf_hp(&P->hp[c], x); else { P->hp[c].ic1 = 0; P->hp[c].ic2 = 0; }
            if (p != P_KICK) x = svf_hp(&P->floor[c], x);
            P->bus[c][i] = x * g;
        }
        float a = fm_fabsf(P->bus[0][i]) + fm_fabsf(P->bus[1][i]); if (a > pk) pk = a;
    }
    P->meter += (fm_minf(1.0f, pk) - P->meter) * (pk > P->meter ? 0.6f : 0.08f);
    e->meter[p] = P->meter;
    if (e->clock) e->cost[p] = (float)(e->clock() - c0);
}

/* ------------------------------------------------------------------ a block: the bus */
static float dly_samples(const Engine *e, int idx)
{
    float s = (float)DTIME_TICKS[idx % NUM_DTIMES] / e->rate;
    while (s > (float)(DLY_BUF - 64)) s *= 0.5f;   /* (longer than its line: half as long) */
    return s < 4.0f ? 4.0f : s;
}

static inline float dl_read(const int16_t *b, int w, float d)
{
    float rp = (float)w - d; while (rp < 0) rp += (float)DLY_BUF;
    int i0 = (int)rp; float fr = rp - (float)i0; if (i0 >= DLY_BUF) i0 -= DLY_BUF;
    int i1 = i0 + 1; if (i1 >= DLY_BUF) i1 = 0;
    return from16(b[i0]) + (from16(b[i1]) - from16(b[i0])) * fr;
}

static float grain_rate(Engine *e)            /* a grain's speed: mostly as it was, some an octave down or up */
{
    float x = rng_f(&e->rng);
    return x < 0.6f ? 1.0f : x < 0.75f ? 0.5f : x < 0.9f ? 2.0f : 1.03f;
}

static void punch_engage(Engine *e, int pu)
{
    e->pu = pu; e->rpos = 0; e->ps0 = e->pw; e->rnext = 0; e->rat_left = 2;
    float beat = 60.0f / e->bpm * SR, cap = (float)(PUNCH_BUF - 256);
    /* div: of a bar (1/8 an eighth). RATCHET starts on an eighth and doubles, REVERSE plays an eighth backwards */
    float div = pu == PU_REP8 || pu == PU_RATCHET || pu == PU_REVERSE || pu == PU_GLITCH ? 0.125f : pu == PU_REP16 ? 0.0625f
              : pu == PU_REP32 ? 0.03125f : pu == PU_REP12 ? 1.0f / 12.0f : 0.25f;
    e->rlen = fm_minf(beat * div * 4.0f, cap);
    if (pu == PU_STOP) e->rate_ts = 1.0f;
    if (pu == PU_SWELL) {                        /* the next beat (or eighth) it lands on, within its memory */
        float to = ((float)TPB - (float)(e->tick % TPB) - e->frac) / e->rate;
        float half = (float)(PUNCH_BUF / 2 - 64);
        while (to > half) to -= beat * 0.5f;
        if (to < beat * 0.125f) to += beat * 0.5f;
        if (to > half) to = half;
        e->swell_len = e->swell_left = (int)to;
    }
    /* the repeats, RATCHET, GLITCH and REVERSE are held to the grid: the slice starts on the last line of its own grid
     * (an eighth, a sixteenth...) before the key, so it loops in time with the groove however late it was pressed. The
     * memory already holds the slice up to now: it plays on as it is (nothing jumps) to the slice's end, then loops (one
     * unbroken stretch: no seam, no click). REVERSE plays the eighth before that line backwards, in step with the grid */
    int slice = pu == PU_RATCHET || pu == PU_REP8 || pu == PU_REP16 || pu == PU_REP32 || pu == PU_REP12 || pu == PU_GLITCH || pu == PU_REVERSE;
    float ph = 0;                                /* samples since the slice's grid line */
    if (slice && e->playing && e->rlen < cap) {
        uint32_t gt = (uint32_t)((float)TPB * 4.0f * div + 0.5f);   /* (its grid in ticks: 48 an eighth) */
        float t = (float)(e->tick % gt) + e->frac - e->rate * (float)e->nblk;   /* (where this block began) */
        while (t < 0) t += (float)gt;
        ph = t / e->rate;
        if (ph >= e->rlen - 64.0f) ph = 0;       /* (all but on the next line: start there) */
    }
    if (pu == PU_REVERSE) {
        int clean = (float)e->rec_since >= ph + e->rlen + 8.0f;
        e->pu_fwd = !clean; e->pu_cap = 0;
        if (clean) { e->ps0 = e->pw - (int)ph; if (e->ps0 < 0) e->ps0 += PUNCH_BUF; e->rpos = ph; }
    } else {
        e->pu_fwd = slice; e->pu_cap = 0;
        if (slice && (float)e->rec_since >= ph + 8.0f) { e->ps0 = e->pw - (int)ph; if (e->ps0 < 0) e->ps0 += PUNCH_BUF; e->pu_cap = (int)ph; }
    }
    e->pu_frozen = slice && !e->pu_fwd;
}

void engine_block_mix(Engine *e, float *outL, float *outR)
{
    Groove *g = e->g; int n = e->nblk;
    float kb = glide_k(0.05f);
    float t_room = (float)g->room * 0.01f, t_dly = (float)g->delay * 0.01f, t_comp = (float)g->comp * 0.01f, t_pan = (float)g->pan * 0.01f;
    float sc_att = glide_k(0.002f), sc_rel = glide_k(0.12f), kk = glide_k(0.005f);
    float duck[NPARTS], sroom[NPARTS], sdly[NPARTS], span[NPARTS];
    for (int p = 0; p < NPARTS; p++) {
        const PartP *q = &g->p[p];
        float d = (float)q->s_duck * 0.01f; duck[p] = p == P_KICK ? 0.0f : d * d * 10.0f;
        sroom[p] = p == P_KICK ? 0.0f : (float)q->s_room * 0.01f;
        sdly[p] = p == P_KICK ? 0.0f : (float)q->s_delay * 0.01f;
        span[p] = p == P_KICK ? 0.0f : (float)q->s_pan * 0.01f;
    }
    /* the room's type and size; its type changed: a dip, the switch, back */
    float fz = e->frz;
    if (g->room_t != e->room_t_now) { e->room_dip -= (float)n / (0.02f * SR); if (e->room_dip <= 0) { e->room_dip = 0; e->room_t_now = g->room_t; e->gate = 0; e->rchunk = 0; e->room_w = 0; } }
    else if (e->room_dip < 1.0f) e->room_dip = fm_minf(1.0f, e->room_dip + (float)n / (0.02f * SR));
    int rt = (int)e->room_t_now;
    float rt60 = rt == RT_GATED ? 1.6f : rt == RT_REVERSE ? 1.2f + 1.5f * t_room : rt == RT_FREEZE ? 25.0f : 0.4f + 2.4f * t_room;
    reverb_set(&e->rv, lerpf(rt60, 40.0f, fz), lerpf(rt == RT_FREEZE ? 9000.0f : 6500.0f, 12000.0f, fz));
    float beat = 60.0f / e->bpm * SR;
    int pre = rt == RT_GATED ? clampi((int)(beat / 8.0f), 1, ROOM_BUF - 1) : 0;   /* (a 1/32's pre-delay) */
    float hold = beat * 0.25f;                    /* (the gate: open a sixteenth past the last hit... ) */
    int chunk = clampi((int)(beat * 0.25f), 256, ROOM_BUF / 2);   /* (the reversed room: a sixteenth's chunks) */
    if (g->delay_t != e->dly_t_now) { e->dly_dip -= (float)n / (0.02f * SR); if (e->dly_dip <= 0) { e->dly_dip = 0; e->dly_t_now = g->delay_t; } }
    else if (e->dly_dip < 1.0f) e->dly_dip = fm_minf(1.0f, e->dly_dip + (float)n / (0.02f * SR));
    float dfb = fm_minf(0.92f, (float)g->dfb * 0.0092f), want = dly_samples(e, g->dtime);
    float sm_t = g->comp_t == CT_SMASH ? 1.0f : 0.0f;
    float bps = e->bpm / 60.0f;
    /* pass 1: the parts into the dry mix and the sends, the kick ducking the rest */
    for (int i = 0; i < n; i++) {
        float kL = e->P[P_KICK].bus[0][i] * e->kill_g, kR = e->P[P_KICK].bus[1][i] * e->kill_g;
        e->kill_g += ((e->pu == PU_KILL && e->pu_mix > 0.5f ? 0.0f : 1.0f) - e->kill_g) * kk;
        float k = fm_fabsf(kL) + fm_fabsf(kR);
        e->sc += (k - e->sc) * (k > e->sc ? sc_att : sc_rel);
        e->lev_pan += (t_pan - e->lev_pan) * kb;
        e->pan_ph += bps / (8.0f * SR); if (e->pan_ph >= 1.0f) e->pan_ph -= 1.0f;     /* SWEEP: two bars across */
        e->pan_spin += bps * 2.0f / SR; if (e->pan_spin >= 1.0f) e->pan_spin -= 1.0f;   /* SPIN: an eighth round */
        float L = kL, R = kR, rl = 0, rr = 0, dsum = 0;
        for (int p = P_SNARE; p < NPARTS; p++) {
            float x0 = e->P[p].bus[0][i], x1 = e->P[p].bus[1][i];
            float dk = 1.0f / (1.0f + duck[p] * e->sc);
            x0 *= dk; x1 *= dk;
            float pm = 0;                         /* PAN: each part moved across the field */
            if (g->pan_t == PT_SWEEP) pm = sine_c(e->pan_ph + (float)p * 0.31f) * 0.9f;
            else if (g->pan_t == PT_SPIN) pm = sine_c(e->pan_spin + (float)p * 0.25f) * 0.9f;
            float a = e->lev_pan * span[p];
            e->pan_now[p] += (pm * a - e->pan_now[p]) * 0.002f;
            if (fm_fabsf(e->pan_now[p]) > 0.0005f) {
                float m = (x0 + x1) * 0.5f, th = (0.5f + 0.5f * clampf(e->pan_now[p], -1.0f, 1.0f)) * 0.25f, w = fm_fabsf(e->pan_now[p]);   /* (th in turns) */
                x0 = x0 * (1.0f - w) + m * w * sine_c(th + 0.25f) * 1.41f; x1 = x1 * (1.0f - w) + m * w * sine_c(th) * 1.41f;
            }
            L += x0; R += x1;
            rl += x0 * sroom[p]; rr += x1 * sroom[p]; dsum += (x0 + x1) * 0.5f * sdly[p];
        }
        {   /* SWELL: the last moments backwards, swelling into the room, landing on the beat; the kit dipped under it */
            float dip_t = 0;
            if (e->pu == PU_SWELL && e->swell_left > 0) {
                int ks = e->swell_len - e->swell_left;
                int rd = e->ps0 - 1 - ks; while (rd < 0) rd += PUNCH_BUF;
                float t = e->swell_len ? (float)ks / (float)e->swell_len : 1.0f;
                float gsw = t * t * e->pu_mix * (e->swell_left < 132 ? (float)e->swell_left / 132.0f : 1.0f);   /* (its last 3 ms faded) */
                float r0 = from16(e->pb[0][rd % PUNCH_BUF]) * gsw, r1 = from16(e->pb[1][rd % PUNCH_BUF]) * gsw;
                L += r0 * 1.3f; R += r1 * 1.3f; rl += r0 * 1.5f; rr += r1 * 1.5f;
                dip_t = 0.5f * t * e->pu_mix;
                if (--e->swell_left <= 0) e->pu_rel = 1;
            }
            e->sw_dip += (dip_t - e->sw_dip) * sc_att * 0.7f;   /* (the dip glided, in and out: ~3 ms) */
            if (e->sw_dip > 0.0001f) { float k2 = 1.0f - e->sw_dip; L = kL + (L - kL) * k2; R = kR + (R - kR) * k2; }
        }
        outL[i] = L; outR[i] = R;
        /* the room's input: through the gated room's pre-delay, held a moment at FREEZE (a punch-in) */
        float feed = 1.0f - fz * (1.0f - e->frz_feed);
        if (pre) {
            float m = (rl + rr) * 0.5f;
            e->room_buf[e->room_w] = to16(m);
            int rd = e->room_w - pre; if (rd < 0) rd += ROOM_BUF;
            float d = from16(e->room_buf[rd]); rl = rr = d;
            if (++e->room_w >= ROOM_BUF) e->room_w = 0;
            float lv = fm_fabsf(m);                /* the gate: open while hits come, closing a sixteenth after the last */
            if (lv > 0.01f) e->gate_hold = hold;
            if (e->gate_hold > 0) { e->gate_hold -= 1.0f; e->gate += (1.0f - e->gate) * 0.02f; }
            else e->gate += (0.0f - e->gate) * 0.004f;
        }
        e->gate_s[i] = e->gate;                  /* (each sample's: the room after it gated sample by sample, no step a block) */
        e->rv_in[0][i] = rl * feed * (rt == RT_FREEZE ? 0.35f : 1.0f); e->rv_in[1][i] = rr * feed * (rt == RT_FREEZE ? 0.35f : 1.0f);
        /* the delay: two taps crossfaded (a new time, GLITCH's jumps), ping-pong; GRANULAR: grains read from it, jittered,
         * some an octave off, dirty in its loop */
        uint32_t st16 = (e->tick + (uint32_t)((float)i * e->rate)) / 24u;
        if (st16 != e->dl_step) {
            e->dl_step = st16;
            if (g->glitch && rng_f(&e->rng) * 100.0f < (float)g->glitch && e->dxf >= 1.0f) {
                e->dt_a = e->dt_b; e->dt_b = rng_chance(&e->rng, 0.3f) ? want : dly_samples(e, 3 + rng_int(&e->rng, 5)); e->dxf = 0;
            }
        }
        if (!g->glitch && fm_fabsf(want - e->dt_b) > 1.0f && e->dxf >= 1.0f) { e->dt_a = e->dt_b; e->dt_b = want; e->dxf = 0; }
        if (e->dxf < 1.0f) e->dxf = fm_minf(1.0f, e->dxf + 1.0f / (0.004f * SR));
        float o[2];
        if (e->dly_t_now == DT_GRAIN) {
            float gl = clampf(e->dt_b * 0.5f, 600.0f, 4000.0f);   /* a grain: half the time, 14 to 90 ms */
            e->dg_ph += 1.0f / gl;
            if (e->dg_ph >= 1.0f) { e->dg_ph -= 1.0f; e->dg_rate = grain_rate(e); }
            float phb = e->dg_ph + 0.5f; if (phb >= 1.0f) phb -= 1.0f;
            if (phb < 1.0f / gl) e->dg_rate_t = grain_rate(e);   /* (the second head's new grain) */
            float wa = sine_c(e->dg_ph * 0.5f), wb = sine_c(phb * 0.5f); wa *= wa; wb *= wb;   /* (sin^2: the two sum to one) */
            float da = clampf(e->dt_b + (1.0f - e->dg_rate) * e->dg_ph * gl, 8.0f, (float)(DLY_BUF - 16));
            float db = clampf(e->dt_b + (1.0f - e->dg_rate_t) * phb * gl, 8.0f, (float)(DLY_BUF - 16));
            for (int c = 0; c < 2; c++) o[c] = dl_read(e->dl[c], e->dw, da) * wa + dl_read(e->dl[c], e->dw, db) * wb;
        } else if (e->dxf >= 1.0f)
            for (int c = 0; c < 2; c++) o[c] = dl_read(e->dl[c], e->dw, e->dt_b);
        else
            for (int c = 0; c < 2; c++) { float a = dl_read(e->dl[c], e->dw, e->dt_a); o[c] = a + (dl_read(e->dl[c], e->dw, e->dt_b) - a) * e->dxf; }
        float dirt = e->dly_t_now == DT_GRAIN ? 1.0f : 0.0f;
        float w0 = op_lp(&e->dlp[0], dsum + o[1] * dfb), w1 = op_lp(&e->dlp[1], o[0] * dfb);
        if (dirt > 0) { w0 = soft_clip(w0 * 2.2f) * 0.6f; w1 = soft_clip(w1 * 2.2f) * 0.6f; }
        e->dl[0][e->dw] = to16(soft_clip(w0)); e->dl[1][e->dw] = to16(soft_clip(w1));
        if (++e->dw >= DLY_BUF) e->dw = 0;
        e->lev_dly += (t_dly - e->lev_dly) * kb;
        float dg = e->lev_dly * e->dly_dip * 1.1f;
        outL[i] += o[0] * dg; outR[i] += o[1] * dg;
    }
    /* the room */
    reverb_run(&e->rv, e->rv_in[0], e->rv_in[1], e->rv_out[0], e->rv_out[1], n);
    for (int i = 0; i < n; i++) {
        float wl = e->rv_out[0][i], wr = e->rv_out[1][i];
        if (rt == RT_GATED) { wl *= e->gate_s[i] * 1.4f; wr *= e->gate_s[i] * 1.4f; }
        if (rt == RT_REVERSE) {                   /* the room's sound in chunks, each played backwards as the next is kept */
            int half = e->rchunk & 1, pos = e->room_w;
            e->room_buf[half * (ROOM_BUF / 2) + pos] = to16((wl + wr) * 0.5f);
            int rd = (1 - half) * (ROOM_BUF / 2) + (chunk - 1 - pos);
            float win = pos < 220 ? (float)pos / 220.0f : pos > chunk - 220 ? (float)(chunk - pos) / 220.0f : 1.0f;
            float rv = from16(e->room_buf[rd]) * win * 1.6f;
            wl = rv + wl * 0.15f; wr = rv + wr * 0.15f;
            if (++e->room_w >= chunk) { e->room_w = 0; e->rchunk++; }
        }
        e->lev_room += (t_room - e->lev_room) * kb;
        float rg = (e->lev_room * 0.9f + fz * 0.5f) * e->room_dip;
        outL[i] += wl * rg; outR[i] += wr * rg;
    }
    /* COMP: the whole kit, kick and all, glued (CLEAN) or squashed into tape (TAPE SMASH); the kick its sidechain,
     * through each part's DUCK above */
    float c_att = glide_k(0.003f), c_rel = glide_k(0.15f), c_g = glide_k(0.004f);
    for (int i = 0; i < n; i++) {
        e->lev_comp += (t_comp - e->lev_comp) * kb;
        e->smash += (sm_t - e->smash) * kb;
        float a = e->lev_comp, sm = e->smash;
        float x0 = outL[i], x1 = outR[i];
        float l = fm_fabsf(x0) + fm_fabsf(x1);
        e->cmp_lvl += (l - e->cmp_lvl) * (l > e->cmp_lvl ? c_att : c_rel);
        float thr = lerpf(0.9f - 0.55f * a, 0.5f - 0.42f * a, sm), ratio_e = lerpf(0.25f + 0.45f * a, 0.5f + 0.4f * a, sm);
        float over = e->cmp_lvl > thr ? e->cmp_lvl / thr : 1.0f;
        float gt = over > 1.0005f ? fm_powf(over, -ratio_e) : 1.0f;   /* (under the threshold: no curve to work out) */
        e->cmp_g += (gt - e->cmp_g) * c_g;
        float drive = lerpf(1.0f + 0.6f * a, (1.0f + 5.0f * a * a) * (1.0f + 1.5f * a), sm) * e->cmp_g;
        float og = lerpf(1.15f / (1.0f + 0.25f * a), 0.85f, sm);   /* (TAPE SMASH: never past 0.85, however hard it's pushed) */
        float y0 = soft_clip(x0 * drive) * og, y1 = soft_clip(x1 * drive) * og;
        float mx = clamp01(a * 4.0f);
        outL[i] = x0 + (y0 - x0) * mx; outR[i] = x1 + (y1 - x1) * mx;
    }
    /* the punch-ins */
    int want_pu = e->pu_want;
    if (e->pu && (want_pu != e->pu) && !e->pu_rel && e->pu != PU_SWELL) e->pu_rel = 1;
    if (!e->pu && want_pu && want_pu != PU_LPF && want_pu != PU_HPF) punch_engage(e, want_pu);   /* (the filters: their own, below) */
    float lpw = e->pu_lpw, hpw = e->pu_hpw;
    for (int i = 0; i < n; i++) {
        float y[2] = { outL[i], outR[i] };
        if (!e->pu_frozen) { e->pb[0][e->pw] = to16(y[0]); e->pb[1][e->pw] = to16(y[1]); }
        float pu_[2] = { 0, 0 }, pd_[2] = { 0, 0 };
        if (e->pu == PU_PUP || e->pu == PU_PDOWN || want_pu == PU_PUP || want_pu == PU_PDOWN)
            for (int c = 0; c < 2; c++) shifter_run(&e->shf[c], &y[c], &pu_[c], &pd_[c], 1);
        else for (int c = 0; c < 2; c++) {        /* (only fed, so it's ready the moment a pitch key is pressed) */
            Shifter *s = &e->shf[c]; s->buf[s->pos] = to16(y[c] * 0.999f); s->pos = (s->pos + 1) & (SHIM_LINE - 1);
        }
        int pu = e->pu;
        float tgt = pu && !e->pu_rel ? 1.0f : 0.0f;
        { float st = 1.0f / (0.004f * SR); e->pu_mix += clampf(tgt - e->pu_mix, -st, st); }
        if (pu && e->pu_rel && e->pu_mix < 0.0005f) { e->pu = pu = 0; e->pu_rel = 0; e->pu_frozen = 0; e->pu_mix = 0; e->frz = 0; }
        if (pu == PU_FREEZE) {                    /* FREEZE: the room fed a moment, then held, the groove going on over it */
            e->frz_feed = e->rpos < beat * 0.125f ? 1.0f : 0.0f;
            e->frz += ((e->pu_rel ? 0.0f : 1.0f) - e->frz) * 0.0004f;
            e->rpos += 1.0f;
        } else if (e->frz > 0) { e->frz += (0.0f - e->frz) * 0.0004f; if (e->frz < 0.001f) e->frz = 0; }
        if (pu && pu != PU_FREEZE && pu != PU_KILL) {
            float fx[2] = { y[0], y[1] };
            if (pu == PU_PDOWN || pu == PU_PUP) { fx[0] = (pu == PU_PUP ? pu_[0] : pd_[0]) * 1.2f; fx[1] = (pu == PU_PUP ? pu_[1] : pd_[1]) * 1.2f; }
            else if (pu == PU_CRUSH) {
                e->crush_h += 6000.0f / SR; if (e->crush_h >= 1.0f) { e->crush_h -= 1.0f; e->crush_v[0] = (float)(int)(y[0] * 24.0f) / 24.0f; e->crush_v[1] = (float)(int)(y[1] * 24.0f) / 24.0f; }
                fx[0] = e->crush_v[0]; fx[1] = e->crush_v[1];
            } else if (pu == PU_STOP) {           /* the tape slowing to a stop over half a beat */
                e->rate_ts += (0.0f - e->rate_ts) * (1.0f / (0.5f * beat)) * 2.0f;
                e->rpos += 1.0f - e->rate_ts; if (e->rpos > (float)(PUNCH_BUF - 8)) e->rpos = (float)(PUNCH_BUF - 8);
                float rp = (float)e->pw - e->rpos; while (rp < 0) rp += (float)PUNCH_BUF;
                int i0 = (int)rp % PUNCH_BUF, i1 = (i0 + 1) % PUNCH_BUF; float fr = rp - fm_floorf(rp), gn = 0.15f + 0.85f * e->rate_ts;
                for (int c = 0; c < 2; c++) fx[c] = (from16(e->pb[c][i0]) + (from16(e->pb[c][i1]) - from16(e->pb[c][i0])) * fr) * gn;
            } else if (pu == PU_SWELL) {          /* (done above, before the room) */
                fx[0] = y[0]; fx[1] = y[1];
            } else if (e->pu_fwd && e->pu_cap < (int)e->rlen) {   /* capturing: the first pass as it is (its last 1 ms faded
                                                                  * out, as the loop's first fades in: no step between them) */
                float left = e->rlen - (float)e->pu_cap, fo = left < 44.0f ? left / 44.0f : 1.0f;
                fx[0] = y[0] * fo; fx[1] = y[1] * fo;
                if (++e->pu_cap >= (int)e->rlen) { e->pu_frozen = 1; e->rpos = 0; e->pu_cap = (int)e->rlen; }
            } else {                              /* the repeats, GLITCH, REVERSE: the slice looped */
                float len = e->rlen, ph = e->rpos;
                float base = e->pu_fwd ? (float)e->ps0 : (float)e->ps0 - len;   /* (the slice's first sample) */
                float rd = pu == PU_REVERSE ? base + len - 1.0f - ph : base + ph;
                while (rd < 0) rd += (float)PUNCH_BUF;
                float fade = ph < 44.0f ? ph / 44.0f : ph > len - 44.0f ? (len - ph) / 44.0f : 1.0f;
                int i0 = (int)rd % PUNCH_BUF;
                for (int c = 0; c < 2; c++) fx[c] = from16(e->pb[c][i0]) * clamp01(fade);
                e->rpos += 1.0f;
                if (e->rpos >= len) {
                    e->rpos -= len;                /* (the fraction kept: hours of repeats stay on the grid) */
                    if (pu == PU_RATCHET && --e->rat_left <= 0 && e->rlen > beat * 0.0625f * 1.5f) {
                        e->rlen *= 0.5f; e->rat_left = 4;   /* RATCHET: eighths, sixteenths for a beat, 32nds for half, then 64ths */
                        if (e->rpos >= e->rlen) e->rpos = 0;
                    }
                    if (pu == PU_GLITCH) {        /* GLITCH: a new length each pass (within what it kept) */
                        static const float GL[5] = { 0.125f, 0.0625f, 0.03125f, 1.0f / 12.0f, 1.0f / 24.0f };
                        e->rpos = 0; e->rlen = fm_minf(beat * GL[rng_int(&e->rng, 5)] * 4.0f, (float)e->pu_cap);
                        if (e->rlen < 64.0f) e->rlen = 64.0f;
                    }
                }
            }
            y[0] += (fx[0] - y[0]) * e->pu_mix; y[1] += (fx[1] - y[1]) * e->pu_mix;
        }
        /* the filter keys: the bottom white key a high-pass from about 500 Hz, the top one a low-pass from about
         * 1.5 kHz, further as they're pressed harder; each faded in and out over 25 ms, the filters always running
         * (so nothing starts cold: no click) */
        {
            float st = 1.0f / (0.025f * SR);
            e->lp_c += clampf((lpw > 0 ? 1.0f : 0.0f) - e->lp_c, -st, st);
            e->hp_c += clampf((hpw > 0 ? 1.0f : 0.0f) - e->hp_c, -st, st);
            if (lpw > 0) e->lp_d += (clamp01(lpw - 1.0f) - e->lp_d) * 0.002f;
            if (hpw > 0) e->hp_d += (clamp01(hpw - 1.0f) - e->hp_d) * 0.002f;
            if ((e->pf_cnt++ & 7) == 0) {
                float fl = 1500.0f * fm_exp2f(-2.0f * e->lp_d), fh = 500.0f * fm_exp2f(2.0f * e->hp_d);
                for (int c = 0; c < 2; c++) { svf_set_res(&e->plp[c], fl, 0.9f); svf_set_res(&e->php[c], fh, 0.9f); }
            }
            for (int c = 0; c < 2; c++) {         /* (in series: each filter run once a sample) */
                float l = svf_lp(&e->plp[c], y[c]);
                y[c] += (l - y[c]) * e->lp_c;
                float hh = svf_hp(&e->php[c], y[c]);
                y[c] += (hh - y[c]) * e->hp_c;
            }
        }
        if (!e->pu_frozen || e->pu_rel) { /* (the memory recording again once let go) */ }
        if (!e->pu_frozen) { e->pw = (e->pw + 1) % PUNCH_BUF; if (e->rec_since < PUNCH_BUF) e->rec_since++; }
        else e->rec_since = 0;
        outL[i] = y[0]; outR[i] = y[1];
    }
    /* a new key while one plays: let go of that first (4 ms), then the new one comes in */
    (void)kb;
}
