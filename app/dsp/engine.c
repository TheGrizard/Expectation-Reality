/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the engine (engine.h). Click-free throughout: every gain glides a sample at a time, a voice
 * taken for another fades out over 3 ms, an effect's type changes through a short dip, the punch-ins cross over in
 * 4 ms, filters' settings move in small steps every few samples. */
#include "engine.h"

const char *const PU_NAME[NUM_PU] = { "", "LOW-PASS", "FILL", "REPEAT 1/8", "REPEAT 1/16", "REPEAT 1/32", "GLITCH",
    "PITCH DOWN", "PITCH UP", "TAPE MASH", "REVERSE", "FREEZE", "SWELL", "CRUSH", "KILL KICK", "REPEAT 1/12", "HIGH-PASS" };

/* the pool: the engine and the screen's canvas (29 KB) must fit the FM-1's 336 KB, with room for fm1-base's own */
typedef char engine_fits_the_pool[sizeof(Engine) <= 285u * 1024u ? 1 : -1];

#include "balance.h"
int engine_balance_raw;                  /* (the simulator's measuring: 1 no balance, 2 the parts' gains alone) */
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
    for (int c = 0; c < 2; c++) { svf_set(&e->plp[c], 18000.0f, 0.7f); svf_set(&e->php[c], 20.0f, 0.7f); op_set(&e->dlp[c], 6500.0f * 2.0f);   /* (run at half the rate) */ op_set(&e->plp1[c], 18000.0f); op_set(&e->frz_hp[c], 200.0f); }
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
    /* MOOD: heavy, long, full, soft and darker at 0; bright, short, hard and staccato at 100. Between, a variety: each
     * part dealt its own place (most spread at the middle, none at the ends), each hit a little either side. (c: 0 the
     * short side, 1 the long) */
    float mood = 1.0f - (float)g->mood * 0.01f, spread = 1.0f - fm_fabsf(2.0f * mood - 1.0f);
    uint32_t hsh = (g->seed ^ (uint32_t)q->salt * 2654435761u) * 2246822519u;
    float own = (float)(hsh >> 8) * (1.0f / 16777216.0f) - 0.5f;   /* (the part's place, -0.5..0.5: fixed for the groove) */
    float c = clamp01(mood + own * spread * 1.1f + rng_bi(&e->rng) * 0.08f * spread);
    float craw = c;
    if (p == P_KICK) c = 0.5f + (c - 0.5f) * 0.35f;   /* (the kick: MOOD moves its tone and weight a third as far: the anchor) */
    hp->engine = q->engine;
    hp->vel = clamp01(vel);
    hp->weight = clamp01((float)q->weight * 0.01f + (c - 0.45f) * 0.3f + P->fw_v);
    hp->entropy = clamp01((float)q->entropy * 0.01f + P->fe_v);
    hp->bright = clamp01(0.88f - 0.7f * c + P->fb_v);
    hp->decay = fm_exp2f(((float)q->decay - 50.0f) * 0.04f) * (p == P_KICK ? fm_exp2f(lerpf(-0.15f, 0.5f, craw)) : fm_exp2f(lerpf(-1.2f, 1.3f, c)));   /* (x0.44 .. x2.5;
                                                    * the kick only x0.9 .. x1.4: never cut short at MOOD's bright end) */
    hp->soft = clamp01((c - 0.55f) / 0.45f);
    hp->bright = clamp01(hp->bright + (hp->vel - 0.7f) * 0.25f);   /* (harder: brighter, and a little heavier) */
    hp->weight = clamp01(hp->weight + (hp->vel - 0.7f) * 0.12f);
    hp->note = part_note(e, p, pitch);
    {   /* an engine on another part's lane plays in that part's register: a snare's or a hat's engine on the kick dropped to
         * a kick's depth, a kick's on the hats lifted. CEN: where each part's engines sit, and each part's lane (MIDI);
         * the perc's engines take their note as it is, the others theirs as an offset from 60 */
        static const float CEN[NPARTS] = { 29.0f, 54.0f, 42.0f, 90.0f };
        int home = engine_part(q->engine); hp->shift = 0;
        if (home != p) {
            float x = home == P_PERC ? CEN[p] - 60.0f : CEN[p] - CEN[home];
            if (p == P_PERC) hp->note += 60.0f - CEN[P_PERC];     /* (the perc's tune kept, around the engine's own pitch) */
            x = clampf(x, -36.0f, 36.0f); hp->note += x; hp->shift = x;
        }
    }
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
    float pan = p == P_KICK ? 0.5f : (float)g->p[p].pan * 0.01f;   /* (the kick in the middle; scattered only if sent to PAN) */
    if (g->pan_t == PT_SCATTER) pan += rng_bi(&e->rng) * 0.5f * (float)g->pan * 0.01f * (float)g->p[p].s_pan * 0.01f;   /* (each hit thrown somewhere) */
    return clamp01(pan);
}

/* a fill: a simple change of the groove, not a roll. Every part plays a slight variation of its own pattern (a hit
 * moved next door, two steps traded: the snare most, the perc and hats a little, the kick barely), from t0 to t1.
 * Each fill its own, the variation dealt from the pattern as it is now */
static void fill_start(Engine *e, uint32_t t0, uint32_t t1)
{
    static const int MUT[NPARTS] = { 1, 3, 2, 2 };
    e->fill_n++;
    /* a fill is a contrast: ACTIVITY low, it adds (the perc and hats busier); high, it takes away (back to their core
     * hits, a breakdown); in between, either, at random */
    e->fill_add = rng_chance(&e->rng, clamp01(0.5f + (0.5f - (float)e->g->activity * 0.01f) * 1.6f));
    for (int p = 0; p < NPARTS; p++) {
        e->fpat[p] = e->P[p].pat;
        for (int k = 0; k < MUT[p]; k++) pattern_mutate(&e->fpat[p], e->g, p, 262144u + e->fill_n * 8u + (uint32_t)k);
    }
    e->fill_from = t0; e->fill_to = t1;
}

/* EVOLVE: 0 the loop exactly as dealt; up to 50 weirder and weirder, but every decision the same every time round
 * (each part's variations a loop of four passes of its own pattern, the fills one of sixteen bars, dealt from the
 * groove's seed), so it repeats exactly; past 50 free-flowing, a quarter of them left to chance just past it, gently,
 * all of them at 100, the patterns rewriting themselves as it goes */
/* the kick's and snare's EVOLVE: untouched from 0 to 25 and from 50 to 75; 25 to 50 their fixed variations coming in,
 * 75 to 100 their free flow (the perc and hats evolving all the way) */
static float evo_ks(float E) { return E < 0.25f ? 0.0f : E < 0.5f ? (E - 0.25f) * 2.0f : E < 0.75f ? 0.5f : 0.5f + (E - 0.75f) * 2.0f; }
static float evo_free(float E) { return E > 0.505f ? 0.25f + 0.75f * clamp01((E - 0.5f) * 2.0f) : 0.0f; }
static uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static uint32_t evo_key(int p, int ls, uint32_t cyc, uint32_t salt)
{
    return (uint32_t)p * 0x01000193u ^ (uint32_t)ls * 0x2545F491u ^ (cyc & 3u) * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
}
static int evo_chance(Engine *e, uint32_t key, float prob, float fr)
{
    if (fr > 0 && rng_f(&e->rng) < fr) return rng_f(&e->rng) < prob;
    return (float)(mix32(key ^ e->g->seed * 0x9E3779B1u) >> 8) * (1.0f / 16777216.0f) < prob;
}

static void deal_step(Engine *e, int p, uint32_t s)
{
    Part *P = &e->P[p]; Groove *g = e->g; Pattern *pt = &P->pat;
    int len = pt->len; uint32_t tps = RATE_TICKS[pt->rate % NUM_RATES], t0 = s * tps;
    uint32_t bar = t0 / BAR; int on16 = t0 % 24u == 0, infill = 0;
    {   /* the fills by themselves: the last beat of every 8 bars (every 4, EVOLVE well up), now and then, decided as the bar
         * begins; then, in a fill, the groove's variation in place of the pattern */
        float Ef = (float)g->evolve * 0.01f;
        if (bar != e->afill_bar) {
            e->afill_bar = bar;
            if (g->evolve >= 30 && (bar % 8u == 7u || (Ef > 0.6f && bar % 4u == 3u))
                && evo_chance(e, evo_key(15, (int)(bar & 15u), 0, 3), 0.3f + 0.5f * Ef, evo_free(Ef))
                && !(e->fill_to > bar * BAR)) fill_start(e, bar * BAR + 3u * TPB, (bar + 1u) * BAR);
        }
        infill = t0 >= e->fill_from && t0 < e->fill_to;
        if (infill && e->fpat[p].len == len) pt = &e->fpat[p];
    }
    float E = (float)g->evolve * 0.01f; if (p <= P_SNARE) E = evo_ks(E);
    float A = (float)g->activity * 0.01f, L = (float)g->link * 0.01f, W = (float)g->warp * 0.01f, C = (float)g->chaos * 0.01f;
    int fill = bar % 4u == 3u;
    /* EVOLVE past 50: now and then a beat played from another beat of the pattern (reality), more as it rises */
    uint32_t spb = TPB / tps; if (!spb) spb = 1;
    if (t0 % TPB < tps) {
        int beat = (int)(t0 / TPB);
        if (beat != P->remap_beat) { P->remap_beat = beat; P->remap = p != P_KICK && E > 0.505f && rng_chance(&e->rng, (E - 0.5f) * 0.9f) ? (int)spb * (1 + rng_int(&e->rng, 3)) : 0; }
    }
    int ls = (int)((s + (uint32_t)P->remap) % (uint32_t)len);
    uint32_t cyc = s / (uint32_t)len;
    float fr = evo_free(E);                       /* (EVOLVE past 50: how much is left to chance) */
    {   /* EVOLVE: neighbouring sixteenths trading places now and then (a hit moved), more as it rises: from 10, a loop of
         * four passes at 50, free past it. Never the kick's: it holds the groove */
        float vp = p == P_KICK ? 0.0f : 0.3f * clamp01((E - 0.1f) / 0.4f) + 0.15f * clamp01((E - 0.5f) * 2.0f);
        int ls2 = ls ^ 1;
        if (vp > 0 && ls2 < len && evo_chance(e, evo_key(p, ls >> 1, cyc, 1), vp, fr)) ls = ls2;
    }
    const Step *st = &pt->s[ls];
    /* ACTIVITY: at 0 stripped back (each part's very strongest hits: the kick's, the snare's backbeat, a perc or hat or
     * two), more coming in as it rises, to a full but never frantic groove at 100 (what 40 was in v0.2); After Images
     * busier: busier */
    float act = 0.14f + A * 0.315f + ((float)g->p[p].density - 50.0f) * 0.006f + L * 0.3f * clamp01((e->ai_act - 4.0f) / 16.0f);
    float thr = 255.0f * (1.0f - clampf(act, 0.12f, 1.0f));
    /* what plays: by rank within the part's own pattern, so every generator thins alike. Its core always (its strongest
     * hits, about two a bar, the perc one), then the next strongest coming in as ACTIVITY rises: at 100 about a third
     * of the rest of the pattern's hits (DENSITY a little more or less, After Images busier more) */
    int plays = 0;
    if (st->lvl > 0) {
        static const float CORE[NPARTS] = { 2.0f, 2.0f, 1.0f, 2.0f }, TCORE[NPARTS] = { 4.0f, 2.0f, 1.0f, 2.0f };   /* (TECHNO: the kick's
                                                  * core its four on the floor) */
        int techno = g->genre == GENRE_TECHNO;
        float spb16 = (float)BAR / (float)tps;   /* (its steps a bar) */
        int core_n = (int)((float)len * (techno ? TCORE : CORE)[p] / spb16 + 0.5f); if (core_n < 1) core_n = 1;
        int above = 0, hits = 0;
        for (int k = 0; k < len; k++) { if (pt->s[k].lvl > st->lvl) above++; if (pt->s[k].lvl >= 60) hits++; }
        static const float FRAC[NPARTS] = { 0.25f, 0.12f, 0.6f, 0.68f }, TFRAC[NPARTS] = { 0.6f, 0.45f, 0.6f, 0.68f };   /* (at 100: what 40
                                                  * was in v0.2, measured; TECHNO: the kick's pounding extras and the
                                                  * snare's ghosts in reach) */
        float frac = clampf(A * (techno ? TFRAC : FRAC)[p] + ((float)g->p[p].density - 50.0f) * 0.006f + L * 0.2f * clamp01((e->ai_act - 4.0f) / 16.0f), 0.0f, 1.0f);
        int allowed = core_n + (int)((float)(hits > core_n ? hits - core_n : 0) * frac + 0.5f);
        if (infill && (p == P_PERC || p == P_HATS)) {   /* (a fill: the perc and hats busier, more of their own pattern's
                                                         * steps, on the sixteenths, never rolled; or, ACTIVITY up, back
                                                         * to their core hits) */
            static const float MORE[NPARTS] = { 0, 0, 4.0f, 5.0f };   /* (a bar's worth more) */
            if (e->fill_add) allowed += (int)((float)len * MORE[p] / spb16 + 0.5f) + (hits > allowed ? (hits - allowed) / 2 : 0);
            else allowed = core_n < allowed ? core_n : allowed;
        }
        plays = above < allowed;
    }
    if (!on16 && A < 0.35f && rng_f(&e->rng) > A / 0.35f) plays = 0;   /* (low ACTIVITY: the 32nds and off-grid steps thinned) */
    if (p == P_KICK && ls == 0 && E < 0.9f && st->lvl >= 250) plays = 1;   /* (the one: always, till EVOLVE is right up) */
    int floor_k = p == P_KICK && g->genre == GENRE_TECHNO && st->lvl >= 250 && !infill;   /* (TECHNO: the four on the floor) */
    if (floor_k) plays = 1;
    float vel = (float)st->vel / 127.0f, pitch = (float)st->pitch, off = 0;
    int alt = st->alt, ratch = 1;
    int comp = st->ct && (int)(E * 100.0f + 0.5f) >= st->cu ? st->ct : C_NONE;
    if (p == P_KICK && (comp == C_NUDGE || comp == C_RATCHET || comp == C_PROB)) comp = C_NONE;   /* (the kick: the anchor. From EVOLVE
                                                    * only accents, ghosts and conditions: never pushed, rolled or dropped) */
    switch (comp) {
    case C_COND: { int ok = cond_ok(st->ca, cyc, fill); if (plays) plays = ok; else if (ok && st->lvl > 20) { plays = 1; vel = fm_maxf(vel * 0.8f, 0.35f); } break; }
    case C_PROB: if (plays) plays = evo_chance(e, evo_key(p, ls, cyc, 4), (float)st->ca * 0.01f, fr); break;
    case C_GHOST: if (!plays) { plays = 1; vel = (float)st->ca / 127.0f; } break;
    case C_RATCHET: ratch = st->ca; break;
    case C_NUDGE: off += (float)((int)st->ca - 128); break;
    case C_ALT: alt = !alt; break;
    case C_PITCH: pitch += (float)((int)st->ca - 128); break;
    case C_ACCENT: vel += (float)st->ca / 127.0f; break;
    default: break;
    }
    if (floor_k) plays = 1;                       /* (no condition takes a floor kick away) */
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
    if (p == P_KICK && infill) {                 /* a fill: the kick on the bar's downbeat alone (whatever its pattern has
                                                  * there), the rest of it left to the perc and hats */
        plays = t0 % BAR == 0;
        if (plays && st->lvl == 0) vel = fm_maxf((float)pt->s[0].vel / 127.0f, 0.75f);
    }
    /* CHAOS: rolls, gaps and hits pushed off the grid, at random */
    if (plays && p != P_KICK && rng_chance(&e->rng, C * 0.25f)) ratch = ratch > 1 ? ratch : 2 + rng_int(&e->rng, 3);
    if (plays && p != P_KICK && rng_chance(&e->rng, C * 0.12f)) plays = 0;   /* (the kick: none of it; it holds the groove) */
    if (plays && p != P_KICK && rng_chance(&e->rng, C * 0.2f)) off += rng_chance(&e->rng, 0.5f) ? 12.0f : -12.0f;
    /* SWING (the groove's): the off sixteenths late. WARP, the humaniser: every hit's timing loosened and its
     * velocity varied, a player's lean (the snare a touch late, the hats a touch early); the kick held tighter */
    float sw = (float)g->swing * 0.0018f;
    if (t0 % 48u == 24u) off += sw * 48.0f; else if (t0 % 24u == 12u) off += sw * 24.0f;
    float hum = (rng_bi(&e->rng) + rng_bi(&e->rng)) * 0.5f;      /* (two dice: near the beat more often than far) */
    off += hum * W * (p == P_KICK ? 1.0f : 6.0f);
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
    if (!plays) return;
    if (p == P_KICK) {
        /* the kick held firm, whatever ACTIVITY, LINK or a ghost adds: a few more hits a bar as ACTIVITY rises (2 to 4
         * in 16 sixteenths, beside the ones written strong), and no two on neighbouring sixteenths unless the pattern wrote both strong */
        if (bar != e->kick_bar) { e->kick_bar = bar; e->kick_in_bar = 0; }
        int cap = (g->genre == GENRE_TECHNO ? 4 : 2) + (int)(A * 2.0f + 0.5f), strong = st->lvl >= 200;
        if (!strong && (e->kick_in_bar >= cap || (e->kick_n && t0 - e->kick_last <= 24u && t0 != e->kick_last))) return;
        e->kick_in_bar++; e->kick_last = t0;
        e->kick_ticks[e->kick_n++ & 7] = t0;
    }
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
    /* EVOLVE past 50: the patterns rewrite themselves, a step at a time (SAVE keeps how far): free-flowing, never
     * coming back; just past 50 one every eight bars or so, at full every bar */
    if (E > 0.505f && rng_chance(&e->rng, 0.12f + 0.88f * clamp01((E - 0.5f) * 2.0f))) {
        int p = rng_chance(&e->rng, 0.3f) && evo_ks(E) > 0.505f ? P_SNARE : P_PERC + rng_int(&e->rng, 2);   /* (never the kick: it holds
                                                  * the groove; the snare only past 75) */
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
        e->sync_req = 0; e->tick = 0; e->frac = 0; e->clk_tick = 0; e->last_bar = 0; e->fill_from = e->fill_to = 0; e->afill_bar = 0xFFFFFFFFu;
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
    if (e->playing && e->fill_want) {           /* the FILL key: the groove's variation from the next beat, for a bar; held,
                                                  * for as long as it's held */
        uint32_t nb = (e->tick / TPB + 1u) * TPB;
        if (nb != e->mfill_last && nb <= end + (uint32_t)LOOK + 1u) {
            e->mfill_last = nb;
            if (e->fill_to > nb && e->fill_from <= nb) { if (e->fill_to < nb + TPB) e->fill_to = nb + TPB; }   /* (still held: on) */
            else fill_start(e, nb, nb + BAR);
        }
    }
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
    static const int CAP[NPARTS] = { 2, 3, 3, 3 };   /* (at most this many ringing at once: the oldest faintest let go) */
    int cap = e->gov < 0.6f ? 1 + (p != P_KICK) : e->gov < 0.85f ? 2 + (p == P_SNARE) : CAP[p];
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
    {   /* a silent part (no voice, its filters died away): nothing to filter, its glides simply arrive */
        int any = 0; for (int i = 0; i < NV; i++) any |= P->v[i].on;
        float st = 0; for (int c = 0; c < 2; c++) st += fm_fabsf(P->lp[c].ic1) + fm_fabsf(P->lp[c].ic2) + fm_fabsf(P->hp[c].ic1) + fm_fabsf(P->hp[c].ic2) + fm_fabsf(P->floor[c].ic1) + fm_fabsf(P->floor[c].ic2);
        if (!any && st < 1e-6f) {
            for (int c = 0; c < 2; c++) { P->lp[c].ic1 = P->lp[c].ic2 = P->hp[c].ic1 = P->hp[c].ic2 = P->floor[c].ic1 = P->floor[c].ic2 = 0; }
            P->gain = gain_t; P->present += (P->present_t - P->present) * fm_minf(1.0f, kp * (float)n);
            if (fm_fabsf(tone_t - P->tone_now) > 0.0005f) { P->tone_now = tone_t; tone_set(P, P->tone_now); }
            P->meter *= 0.92f; e->meter[p] = P->meter;
            if (e->clock) e->cost[p] = (float)(e->clock() - c0);
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        if ((P->tone_cnt++ & 7) == 0 && fm_fabsf(tone_t - P->tone_now) > 0.0005f) { P->tone_now += (tone_t - P->tone_now) * fm_minf(1.0f, kt * 8.0f); tone_set(P, P->tone_now); }
        P->gain += (gain_t - P->gain) * kg; P->present += (P->present_t - P->present) * kp;
        float g = P->gain * P->present;
        int use_lp = P->tone_now < 0.495f, use_hp = P->tone_now > 0.505f;   /* (one side at a time, none at 12 o'clock (open);
                                                    * the other held ready, its state the sound itself) */
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

/* ------------------------------------------------------------------ the reverb, beside the parts (the second core)
 * The last block's sends (a block late: 6 ms, part of the room's own pre-delay). Silent (nothing sent, its tail gone
 * below hearing) it rests: no work at all */
void engine_block_reverb(Engine *e)
{
    int n = e->nblk;
    uint32_t c0 = e->clock ? e->clock() : 0;
    if (!e->rv_fed && e->rv_quiet > 8) {
        if (e->rv_quiet == 9) { memset(e->rv_out, 0, sizeof e->rv_out); e->rv_last[0] = e->rv_last[1] = 0; e->rv_quiet++; }
        e->rv_cost = 0;
        return;
    }
    {   /* at half the rate: each pair of samples in averaged, each one out followed by the halfway point to the next
         * (half a sample late, unheard in a room) */
        int h = n >> 1;
        for (int k = 0; k < h; k++) {
            e->rv_h[0][k] = 0.5f * (e->rv_in[0][2 * k] + e->rv_in[0][2 * k + 1]);
            e->rv_h[1][k] = 0.5f * (e->rv_in[1][2 * k] + e->rv_in[1][2 * k + 1]);
        }
        reverb_run(&e->rv, e->rv_h[0], e->rv_h[1], e->rv_h[2], e->rv_h[3], h);
        for (int c = 0; c < 2; c++) {
            float prev = e->rv_last[c]; const float *y = e->rv_h[2 + c]; float *o = e->rv_out[c];
            for (int k = 0; k < h; k++) { o[2 * k] = 0.5f * (prev + y[k]); o[2 * k + 1] = y[k]; prev = y[k]; }
            e->rv_last[c] = prev;
        }
    }
    float mx = 0; for (int i = 0; i < n; i++) mx = fm_maxf(mx, fm_fabsf(e->rv_out[0][i]) + fm_fabsf(e->rv_out[1][i]));
    if (e->rv_fed || mx > 2e-5f) e->rv_quiet = 0; else if (e->rv_quiet < 9) e->rv_quiet++;
    if (e->clock) e->rv_cost = (float)(e->clock() - c0);
}

/* ------------------------------------------------------------------ a block: the bus */
static float dly_samples(const Engine *e, int idx)
{
    float s = (float)DTIME_TICKS[idx % NUM_DTIMES] / e->rate;
    while (s > (float)(DLY_BUF - 64)) s *= 0.5f;   /* (longer than its line: half as long) */
    return s < 4.0f ? 4.0f : s;
}

static inline float dl_read(const int16_t *b, int w, float d)   /* (d in the sample rate's samples; the line at half) */
{
    float rp = (float)w - d * 0.5f; while (rp < 0) rp += (float)DLY_H;
    int i0 = (int)rp; float fr = rp - (float)i0; if (i0 >= DLY_H) i0 -= DLY_H;
    int i1 = i0 + 1; if (i1 >= DLY_H) i1 = 0;
    return from16(b[i0]) + (from16(b[i1]) - from16(b[i0])) * fr;
}

static float grain_rate(Engine *e)            /* a grain's speed: mostly as it was, some an octave down or up */
{
    float x = rng_f(&e->rng);
    return x < 0.6f ? 1.0f : x < 0.75f ? 0.5f : x < 0.9f ? 2.0f : 1.03f;
}

/* the punch-ins' memory: the loudest point of a stretch of it (every fourth sample: enough to find a hit) */
static float pb_peak(const Engine *e, int start, int len)
{
    float m = 0;
    for (int k = 0; k < len; k += 4) {
        int i = (start + k) % PUNCH_BUF; if (i < 0) i += PUNCH_BUF;
        float v = fm_fabsf(from16(e->pb[0][i])) + fm_fabsf(from16(e->pb[1][i]));
        if (v > m) m = v;
    }
    return m;
}
/* a loop's slice with nothing in it (pressed between the hits: a sixteenth of a staccato kit can be silence): the
 * nearest one before it on the same grid with a hit, whole slices back so it stays in time. first: the slice's first
 * sample; returns the one to play */
static int slice_with_a_hit(const Engine *e, int first, int len)
{
    if (len < 64) return first;
    float here = pb_peak(e, first, len), best = here; int at = first;
    for (int j = 1; ; j++) {
        int st = first - j * len, back = e->pw - st; while (back < 0) back += PUNCH_BUF;
        if (back + 256 > PUNCH_BUF || back < len) break;   /* (older than the memory keeps) */
        float pk = pb_peak(e, st, len);
        if (pk > best * 1.5f) { best = pk; at = st; }
    }
    return here < 0.3f * best ? at : first;     /* (it had a hit of its own: kept) */
}
/* nothing in it: its loudest point under a quarter of the groove's recent loudest (a hit's faint tail is still nothing) */
static int slice_quiet(const Engine *e, int start, int len) { return pb_peak(e, start, len) < fm_maxf(0.25f * e->pk_run, 0.01f); }

/* the live chopper's sample: act 0 the groove as it plays, 1 the last len before the step repeated, 2 the last len
 * backwards; s0 the step's first sample in the memory, ph how far into the step. Each pass faded at its ends (1 ms) */
enum { CH_LIVE, CH_REP, CH_REV };
static inline float ch_sample(const Engine *e, int act, int s0, int len, float ph, int c, float live)
{
    if (act == CH_LIVE || len < 64) return live;
    int k = (int)ph % len, i = act == CH_REP ? s0 - len + k : s0 - 1 - k;
    while (i < 0) i += PUNCH_BUF;
    float w = fm_minf(1.0f, fm_minf((float)k, (float)(len - k)) / 44.0f);
    return from16(e->pb[c][i % PUNCH_BUF]) * w;
}

static void punch_engage(Engine *e, int pu, int at)   /* (at: the sample of this block it starts on) */
{
    e->pu = pu; e->rpos = 0; e->ps0 = e->pw; e->rnext = 0;
    float beat = 60.0f / e->bpm * SR, cap = (float)(PUNCH_BUF - 256);
    /* div: of a bar (1/8 an eighth). REVERSE plays an eighth backwards */
    float div = pu == PU_REP8 || pu == PU_REVERSE || pu == PU_GLITCH ? 0.125f : pu == PU_REP16 ? 0.0625f
              : pu == PU_REP32 ? 0.03125f : pu == PU_REP12 ? 1.0f / 12.0f : 0.25f;
    e->rlen = fm_minf(beat * div * 4.0f, cap);
    if (pu == PU_MASH) { e->m_d = 0.012f * SR + 64.0f; e->m_x = e->m_tgt = 0; e->m_g = e->m_gt = 1.0f; e->m_cnt = (int)(0.05f * SR); e->m_lp[0] = e->m_lp[1] = 0; }
    if (pu == PU_SWELL) {                        /* the next beat (or eighth) it lands on, within its memory */
        float to = ((float)TPB - (float)(e->tick % TPB) - e->frac) / e->rate;   /* (engaged just early enough for it: see
                                                  * the quantising below) */
        float half = (float)(PUNCH_BUF / 2 - 64);
        if (to < 64.0f) to += beat;
        if (to > half) to = half;
        e->swell_len = e->swell_left = (int)to;
        /* what it swells into: the strongest hit in the memory (the nearer the better), reached backwards just as the
         * beat lands, so there is always something there to swell, however staccato the kit */
        int len = (int)to, best = len; float bv = -1.0f;
        for (int d = len; d <= PUNCH_BUF - len - 64; d += 16) {
            int rd = e->pw - d; while (rd < 0) rd += PUNCH_BUF;
            float v = (fm_fabsf(from16(e->pb[0][rd])) + fm_fabsf(from16(e->pb[1][rd]))) * (1.0f - 0.35f * (float)d / (float)PUNCH_BUF);
            if (v > bv) { bv = v; best = d; }
        }
        e->sw_off = best - len;
    }
    if (pu == PU_FREEZE) {                       /* FREEZE catches the whole kit's last half beat from the memory, whatever
                                                  * is sent to the room and wherever it's pressed */
        e->frz_len = e->frz_left = (int)fm_minf(beat * 0.5f, (float)(PUNCH_BUF - 512));
        e->frz_got = 0; e->frz_live = 0; e->frz_hit = 0; e->frz_bar = e->tick / BAR; e->frz_ref = 0;
    }
    /* the repeats, GLITCH and REVERSE are held to the grid: the slice starts on the last line of its own grid
     * (an eighth, a sixteenth...) before the key, so it loops in time with the groove however late it was pressed. The
     * memory already holds the slice up to now: it plays on as it is (nothing jumps) to the slice's end, then loops (one
     * unbroken stretch: no seam, no click). REVERSE plays the eighth before that line backwards, in step with the grid */
    int slice = pu == PU_REP8 || pu == PU_REP16 || pu == PU_REP32 || pu == PU_REP12;
    if (pu == PU_GLITCH || pu == PU_REVERSE) {   /* the live ones: on everything as it plays, the beat going on through them */
        float st = pu == PU_GLITCH ? beat * 0.25f : beat * 0.5f;   /* (REVERSE: an eighth, or less where the memory is short) */
        while (st * 2.0f > (float)(PUNCH_BUF - 512)) st *= 0.5f;
        e->ch_step = st; e->ch_ph = 0; e->ch_s0 = e->pw; e->ch_len = (int)st; e->ch_act = 0; e->ch_xf = 0;
        e->ch_pact = 0; e->ch_ps0 = e->pw; e->ch_plen = (int)st; e->ch_pph = 0;
        e->pu_fwd = 0; e->pu_cap = 0; e->pu_frozen = 0; e->pu_quiet = 0; e->pu_tries = 0;
        return;
    }
    float ph = 0;                                /* samples since the slice's grid line */
    if (slice && e->playing && e->rlen < cap) {
        uint32_t gt = (uint32_t)((float)TPB * 4.0f * div + 0.5f);   /* (its grid in ticks: 48 an eighth) */
        float t = (float)(e->tick % gt) + e->frac - e->rate * (float)(e->nblk - at);   /* (where it starts) */
        while (t < 0) t += (float)gt;
        ph = t / e->rate;
        if (ph >= e->rlen - 64.0f) ph = 0;       /* (all but on the next line: start there) */
    }
    if (pu == PU_REVERSE) {
        int clean = (float)e->rec_since >= ph + e->rlen + 8.0f;
        e->pu_fwd = !clean; e->pu_cap = 0;
        if (clean) {
            e->ps0 = e->pw - (int)ph; if (e->ps0 < 0) e->ps0 += PUNCH_BUF; e->rpos = ph;
            int len = (int)e->rlen, st = slice_with_a_hit(e, e->ps0 - len, len);   /* (an eighth with a hit in it) */
            e->ps0 = (st + len) % PUNCH_BUF; if (e->ps0 < 0) e->ps0 += PUNCH_BUF;
            if (slice_quiet(e, st, len)) { e->pu_fwd = 1; e->pu_cap = 0; e->ps0 = e->pw; e->rpos = 0; }   /* (none
                                                  * in the memory: it catches the next one as it plays) */
        }
    } else {
        e->pu_fwd = slice; e->pu_cap = 0;
        if (slice && (float)e->rec_since >= ph + 8.0f) { e->ps0 = e->pw - (int)ph; if (e->ps0 < 0) e->ps0 += PUNCH_BUF; e->pu_cap = (int)ph; }
    }
    e->pu_frozen = slice && !e->pu_fwd;
    e->pu_quiet = 0; e->pu_tries = 0;
}

void engine_block_mix(Engine *e, float *outL, float *outR)
{
    Groove *g = e->g; int n = e->nblk;
    float kb = glide_k(0.05f);
    float fxon = e->clean ? 0.0f : 1.0f;      /* (OUTPUT CLEAN: the four bus effects glide out, as their amounts do) */
    /* the reverb's and delay's returns at a set level (each part's sends say how much goes in); REVERB SIZE its length;
     * COMP AMOUNT and PAN AMOUNT as they were */
    float size = (float)g->room * 0.01f;
    float t_room = fxon, t_dly = 0.8f * fxon, t_comp = (float)g->comp * 0.01f * fxon, t_pan = (float)g->pan * 0.01f * fxon;
    float sc_att = glide_k(0.002f), sc_rel = glide_k(0.12f), kk = glide_k(0.005f);
    float scmp[NPARTS], sroom[NPARTS], sdly[NPARTS], span[NPARTS], pgt[NPARTS];
    {   /* each part's gain from the balance table (MOOD 0, 25, 50, 75, 100; dB), glided per sample below */
        float m = (float)g->mood * 0.04f; int j = (int)m; if (j > 3) j = 3; m -= (float)j;
        for (int p = 0; p < NPARTS; p++) pgt[p] = engine_balance_raw == 1 ? 1.0f : fm_exp2f((BAL[p][j] + (BAL[p][j + 1] - BAL[p][j]) * m) * 0.16609f);
    }
    for (int p = 0; p < NPARTS; p++) {
        const PartP *q = &g->p[p];
        scmp[p] = (float)q->s_comp * 0.01f;
        sroom[p] = (float)q->s_room * 0.01f;      /* (the kick's too: dry unless you send it) */
        sdly[p] = (float)q->s_delay * 0.01f;
        span[p] = (float)q->s_pan * 0.01f;
    }
    /* the room's type and size; its type changed: a dip, the switch, back */
    float fz = e->frz;
    if (g->room_t != e->room_t_now) { e->room_dip -= (float)n / (0.02f * SR); if (e->room_dip <= 0) { e->room_dip = 0; e->room_t_now = g->room_t; e->gate = 0; e->rchunk = 0; e->room_w = 0; } }
    else if (e->room_dip < 1.0f) e->room_dip = fm_minf(1.0f, e->room_dip + (float)n / (0.02f * SR));
    int rt = (int)e->room_t_now;
    float rt60 = rt == RT_GATED ? 0.8f + 1.6f * size : rt == RT_REVERSE ? 0.8f + 2.0f * size : rt == RT_FREEZE ? 25.0f : 0.3f + 3.2f * size;
    if (e->pu == PU_FREEZE && !e->pu_rel && e->frz_left == 0 && e->frz_live == 0 && e->tick / BAR != e->frz_bar) {
        e->frz_bar = e->tick / BAR;               /* FREEZE held: each new bar, its downbeat caught (the next hit, and a
                                                  * quarter beat after it), the old wash let go meanwhile */
        e->frz_live = (int)(120.0f / e->bpm * SR); e->frz_hit = 0; e->frz_ref = 1.0f;   /* (two beats at most) */
    }
    if (e->frz_ref > 0 && e->frz_live == 0 && e->frz_lv <= 0) e->frz_ref = fm_maxf(0.0f, e->frz_ref - (float)n / (0.15f * SR));
    if (e->frz_go > 0 && e->pu == PU_FREEZE && !e->pu_rel && !e->pu_relq) e->frz_go = 0;   /* (pressed again) */
    reverb_set(&e->rv, lerpf(lerpf(rt60, lerpf(40.0f, 0.6f, e->frz_ref), fz), 0.25f, e->frz_go),
               lerpf(rt == RT_FREEZE ? 9000.0f : 6500.0f, 12000.0f, fz));
    if (e->frz_go > 0) e->frz_go = fm_maxf(0.0f, e->frz_go - (float)n / (0.4f * SR));   /* (the room's own tail back over
                                                  * 0.4 s, by when the wash has gone) */
    float beat = 60.0f / e->bpm * SR;
    int pre = rt == RT_GATED ? clampi((int)(beat / 8.0f), 1, ROOM_BUF - 1) : 0;   /* (a 1/32's pre-delay) */
    float hold = beat * 0.25f;                    /* (the gate: open a sixteenth past the last hit... ) */
    int chunk = clampi((int)(beat * 0.25f), 256, ROOM_BUF / 2);   /* (the reversed room: a sixteenth's chunks) */
    if (g->delay_t != e->dly_t_now) { e->dly_dip -= (float)n / (0.02f * SR); if (e->dly_dip <= 0) { e->dly_dip = 0; e->dly_t_now = g->delay_t; } }
    else if (e->dly_dip < 1.0f) e->dly_dip = fm_minf(1.0f, e->dly_dip + (float)n / (0.02f * SR));
    float dfb = fm_minf(0.92f, (float)g->dfb * 0.0092f), want = dly_samples(e, g->dtime);
    float sm_t = g->comp_t == CT_SMASH ? 1.0f : 0.0f;
    float bps = e->bpm / 60.0f;
    float c_att = glide_k(0.003f), c_rel = glide_k(0.15f), c_g = glide_k(0.004f);
    int fed = 0, comp_on = 0, dl_in = 0, dl_rest = e->dl_quiet > 8; float dl_mx = 0;
    for (int p = 0; p < NPARTS; p++) if (sdly[p] > 0) dl_rest = 0;   /* (sent to: awake at once) */
    for (int p = 0; p < NPARTS; p++) if (scmp[p] > 0) comp_on = 1;
    if (!comp_on && e->lev_comp < 0.0005f && t_comp * (1.0f - e->clean_g) < 0.0005f) e->lev_comp = 0;
    /* pass 1: the parts into the dry mix and the sends, the kick ducking the rest */
    for (int i = 0; i < n; i++) {
        float kL = e->P[P_KICK].bus[0][i] * e->kill_g, kR = e->P[P_KICK].bus[1][i] * e->kill_g;
        e->kill_g += ((e->pu == PU_KILL && e->pu_mix > 0.5f ? 0.0f : 1.0f) - e->kill_g) * kk;
        float k = fm_fabsf(kL) + fm_fabsf(kR);
        e->sc += (k - e->sc) * (k > e->sc ? sc_att : sc_rel);
        /* the reverb's and delay's returns ducked lightly under the kick (at most ~4.5 dB, in at once, back over ~120 ms:
         * the tails breathe round it, never pump) */
        e->duck_s[i] = 1.0f - 0.4f * fm_minf(1.0f, e->sc * 1.1f);
        e->lev_pan += (t_pan - e->lev_pan) * kb;
        e->clean_g += ((float)e->clean - e->clean_g) * kb;   /* (DUCK out with them, glided) */
        e->pan_ph += bps / (8.0f * SR); if (e->pan_ph >= 1.0f) e->pan_ph -= 1.0f;     /* SWEEP: two bars across */
        e->pan_spin += bps * 2.0f / SR; if (e->pan_spin >= 1.0f) e->pan_spin -= 1.0f;   /* SPIN: an eighth round */
        float L = 0, R = 0, rl = 0, rr = 0, dsum = 0, kx0 = kL, kx1 = kR, cl = 0, cr = 0;
        for (int p = P_KICK; p < NPARTS; p++) {
            if (e->pg[p] <= 0) e->pg[p] = pgt[p];
            e->pg[p] += (pgt[p] - e->pg[p]) * kb;
            float x0 = (p == P_KICK ? kL : e->P[p].bus[0][i]) * e->pg[p], x1 = (p == P_KICK ? kR : e->P[p].bus[1][i]) * e->pg[p];
            float pm = 0, a = e->lev_pan * span[p];   /* PAN: each part moved across the field (not sent: nothing to do) */
            if (a > 0 || e->pan_now[p] != 0) {
                if (g->pan_t == PT_SWEEP) pm = sine_c(e->pan_ph + (float)p * 0.31f) * 0.9f;
                else if (g->pan_t == PT_SPIN) pm = sine_c(e->pan_spin + (float)p * 0.25f) * 0.9f;
                e->pan_now[p] += (pm * a - e->pan_now[p]) * 0.002f;
                if (a <= 0 && fm_fabsf(e->pan_now[p]) < 1e-4f) e->pan_now[p] = 0;
            }
            if (fm_fabsf(e->pan_now[p]) > 0.0005f) {
                float m = (x0 + x1) * 0.5f, th = (0.5f + 0.5f * clampf(e->pan_now[p], -1.0f, 1.0f)) * 0.25f, w = fm_fabsf(e->pan_now[p]);   /* (th in turns) */
                x0 = x0 * (1.0f - w) + m * w * sine_c(th + 0.25f) * 1.41f; x1 = x1 * (1.0f - w) + m * w * sine_c(th) * 1.41f;
            }
            L += x0; R += x1;
            if (p == P_KICK) { kx0 = x0; kx1 = x1; }
            cl += x0 * scmp[p]; cr += x1 * scmp[p];
            rl += x0 * sroom[p]; rr += x1 * sroom[p]; dsum += (x0 + x1) * 0.5f * sdly[p];
        }
        {   /* SWELL: the last moments backwards, swelling into the room, landing on the beat; the kit dipped under it */
            float dip_t = 0;
            if (e->pu == PU_SWELL && e->swell_left > 0) {
                int ks = e->swell_len - e->swell_left;
                int rd = e->ps0 - 1 - e->sw_off - ks; while (rd < 0) rd += PUNCH_BUF;
                float t = e->swell_len ? (float)ks / (float)e->swell_len : 1.0f;
                float gsw = t * t * e->pu_mix * (e->swell_left < 132 ? (float)e->swell_left / 132.0f : 1.0f);   /* (its last 3 ms faded) */
                float r0 = from16(e->pb[0][rd % PUNCH_BUF]) * gsw, r1 = from16(e->pb[1][rd % PUNCH_BUF]) * gsw;
                L += r0 * 1.3f; R += r1 * 1.3f; rl += r0 * 1.5f; rr += r1 * 1.5f;
                dip_t = 0.5f * t * e->pu_mix;
                if (--e->swell_left <= 0) e->pu_rel = 1;
            }
            e->sw_dip += (dip_t - e->sw_dip) * sc_att * 0.7f;   /* (the dip glided, in and out: ~3 ms) */
            if (e->sw_dip > 0.0001f) { float k2 = 1.0f - e->sw_dip; L = kx0 + (L - kx0) * k2; R = kx1 + (R - kx1) * k2; }
        }
        if (comp_on || e->lev_comp > 0) {   /* COMP: what each part sends through it (COMP on SENDS), glued (CLEAN) or squashed
             * into tape (TAPE SMASH); the kick hard-wired to it: always heard by it, so whatever is sent pumps under the
             * kick. The rest passes by. (Nothing sent: nothing to do) */
            e->lev_comp += (t_comp * (1.0f - e->clean_g) - e->lev_comp) * kb;
            e->smash += (sm_t - e->smash) * kb;
            float a = e->lev_comp, sm = e->smash;
            float l = fm_fabsf(cl) + fm_fabsf(cr) + k;
            e->cmp_lvl += (l - e->cmp_lvl) * (l > e->cmp_lvl ? c_att : c_rel);
            if ((i & 3) == 0) {                       /* (its gain worked out every fourth sample, glided between) */
                float thr = lerpf(0.9f - 0.55f * a, 0.5f - 0.42f * a, sm), ratio_e = lerpf(0.25f + 0.45f * a, 0.5f + 0.4f * a, sm);
                float over = e->cmp_lvl > thr ? e->cmp_lvl / thr : 1.0f;
                e->cmp_gt = over > 1.0005f ? fm_powf(over, -ratio_e) : 1.0f;   /* (under the threshold: no curve to work out) */
            }
            e->cmp_g += (e->cmp_gt - e->cmp_g) * c_g;
            float drive = lerpf(1.0f + 0.6f * a, (1.0f + 5.0f * a * a) * (1.0f + 1.5f * a), sm) * e->cmp_g;
            float og = lerpf(1.15f / (1.0f + 0.25f * a), 0.85f, sm);   /* (TAPE SMASH: never past 0.85, however hard it's pushed) */
            float mx = clamp01(a * 4.0f);
            if (comp_on) { L += (soft_clip(cl * drive) * og - cl) * mx; R += (soft_clip(cr * drive) * og - cr) * mx; }
            if (!comp_on && e->lev_comp < 0.0005f) e->lev_comp = 0;
        }
        outL[i] = L; outR[i] = R;
        /* the room's input: through the gated room's pre-delay, held a moment at FREEZE (a punch-in) */
        float feed = 1.0f - fz * (1.0f - e->frz_feed);
        float fcl = 0, fcr = 0;                   /* (what FREEZE catches, into the room below) */
        /* FREEZE's catch: the whole kit (the kick too), its last half beat played into the room from the memory, its
         * ends faded over 3 ms */
        if (e->frz_left > 0) {
            if (e->pu != PU_FREEZE) e->frz_left = 0;
            else {
                int ks = e->frz_len - e->frz_left, rd = e->ps0 - e->frz_len + ks; while (rd < 0) rd += PUNCH_BUF;
                float w = fm_minf(1.0f, fm_minf((float)ks, (float)e->frz_left) / 132.0f) * 0.8f;
                float ml = from16(e->pb[0][rd % PUNCH_BUF]) * w, mr = from16(e->pb[1][rd % PUNCH_BUF]) * w;
                fcl += ml; fcr += mr; e->frz_got = fm_maxf(e->frz_got, fm_fabsf(ml) + fm_fabsf(mr));
                if (--e->frz_left == 0 && e->frz_got < fm_maxf(0.2f * e->pk_run, 0.01f)) e->frz_live = (int)(beat * 2.0f);   /* (nothing in it: pressed
                                                  * between the hits. It listens on, up to two beats, till it catches one) */
            }
        }
        if (e->frz_live > 0 && e->pu != PU_FREEZE) e->frz_live = 0;
        {   /* FREEZE listening on: the kit as it plays into the room, from the next hit to a quarter beat after it */
            float tgt = e->frz_live > 0 ? 1.0f : 0.0f;
            e->frz_lv += clampf(tgt - e->frz_lv, -1.0f / 132.0f, 1.0f / 132.0f);
            if (e->frz_lv > 0) { fcl += L * 0.8f * e->frz_lv; fcr += R * 0.8f * e->frz_lv; }
            if (e->frz_live > 0) {
                if (!e->frz_hit && fm_fabsf(L) + fm_fabsf(R) > 0.1f) { e->frz_hit = 1; e->frz_live = (int)(beat * 0.25f); }
                e->frz_live--;
            }
        }
        if (fcl != 0 || fcr != 0 || e->frz_hp[0].z != 0) {   /* (its lows taken out below ~200 Hz, so the kick's click
                                                  * goes in but not its boom) */
            rl += fcl - op_lp(&e->frz_hp[0], fcl); rr += fcr - op_lp(&e->frz_hp[1], fcr);
            if (fcl == 0 && fcr == 0 && fm_fabsf(e->frz_hp[0].z) + fm_fabsf(e->frz_hp[1].z) < 1e-6f) e->frz_hp[0].z = e->frz_hp[1].z = 0;
        }
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
        if (rl != 0.0f || rr != 0.0f) fed = 1;
        if (!dl_rest) {   /* (the delay resting: nothing sent, its echoes died away) */
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
            if (!(i & 1)) { e->dl_hin = dsum; o[0] = e->dl_ho[0]; o[1] = e->dl_ho[1]; }   /* (at half the rate: each second
                                                  * sample the pair in averaged, the line read and written; out, each
                                                  * new sample and the halfway point to it) */
            else {
            float din = 0.5f * (e->dl_hin + dsum), h[2];
            if (e->dly_t_now == DT_GRAIN) {
                float gl = clampf(e->dt_b * 0.5f, 600.0f, 4000.0f);   /* a grain: half the time, 14 to 90 ms */
                e->dg_ph += 2.0f / gl;                /* (two samples a step) */
                if (e->dg_ph >= 1.0f) { e->dg_ph -= 1.0f; e->dg_rate = grain_rate(e); }
                float phb = e->dg_ph + 0.5f; if (phb >= 1.0f) phb -= 1.0f;
                if (phb < 2.0f / gl) e->dg_rate_t = grain_rate(e);   /* (the second head's new grain) */
                float wa = sine_c(e->dg_ph * 0.5f), wb = sine_c(phb * 0.5f); wa *= wa; wb *= wb;   /* (sin^2: the two sum to one) */
                float da = clampf(e->dt_b + (1.0f - e->dg_rate) * e->dg_ph * gl, 8.0f, (float)(DLY_BUF - 16));
                float db = clampf(e->dt_b + (1.0f - e->dg_rate_t) * phb * gl, 8.0f, (float)(DLY_BUF - 16));
                for (int c = 0; c < 2; c++) h[c] = dl_read(e->dl[c], e->dw, da) * wa + dl_read(e->dl[c], e->dw, db) * wb;
            } else if (e->dxf >= 1.0f)
                for (int c = 0; c < 2; c++) h[c] = dl_read(e->dl[c], e->dw, e->dt_b);
            else
                for (int c = 0; c < 2; c++) { float a = dl_read(e->dl[c], e->dw, e->dt_a); h[c] = a + (dl_read(e->dl[c], e->dw, e->dt_b) - a) * e->dxf; }
            float dirt = e->dly_t_now == DT_GRAIN ? 1.0f : 0.0f;
            float w0 = op_lp(&e->dlp[0], din + h[1] * dfb), w1 = op_lp(&e->dlp[1], h[0] * dfb);
            if (dirt > 0) { w0 = soft_clip(w0 * 2.2f) * 0.6f; w1 = soft_clip(w1 * 2.2f) * 0.6f; }
            e->dl[0][e->dw] = to16(soft_clip(w0)); e->dl[1][e->dw] = to16(soft_clip(w1));
            if (++e->dw >= DLY_H) e->dw = 0;
            for (int c = 0; c < 2; c++) { o[c] = 0.5f * (e->dl_ho[c] + h[c]); e->dl_ho[c] = h[c]; }
            }
            e->lev_dly += (t_dly - e->lev_dly) * kb;
            float dg = e->lev_dly * e->dly_dip * 1.1f * e->duck_s[i];
            outL[i] += o[0] * dg; outR[i] += o[1] * dg;
        dl_mx = fm_maxf(dl_mx, fm_fabsf(o[0]) + fm_fabsf(o[1])); if (dsum != 0.0f) dl_in = 1;
        }
    }
    e->rv_fed = fed;
    if (!dl_rest) { if (dl_in || dl_mx > 2e-5f) e->dl_quiet = 0; else if (e->dl_quiet < 9) e->dl_quiet++; }
    /* the room: its sound from engine_block_reverb (the last block's sends, run beside the parts) */
    for (int i = 0; i < n && !(e->rv_quiet > 8 && rt != RT_REVERSE && e->lev_room > 0.999f * t_room && t_room > 0); i++) {
        float wl = e->rv_out[0][i], wr = e->rv_out[1][i];
        float fzi = e->frz_prev + (fz - e->frz_prev) * (float)(i + 1) / (float)n;   /* (FREEZE's level glided across the block) */
        if (rt == RT_GATED) { float gk = lerpf(e->gate_s[i] * 1.4f, 1.0f, fzi); wl *= gk; wr *= gk; }   /* (FREEZE holds the gate
                                                  * open: nothing goes in while it holds, and the gate would shut it) */
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
        float rg = (e->lev_room * 0.9f + fzi * 0.5f) * e->duck_s[i] * e->room_dip;   /* (FREEZE's held wash too: the kick
                                                  * cuts through it) */
        outL[i] += wl * rg; outR[i] += wr * rg;
    }
    e->frz_prev = fz;
    /* the balance, as After Images': the mix's lows (under 150 Hz) and highs (over 3 kHz) weighed over seconds, the mix
     * tilted round 800 Hz towards the grooves' usual top-to-bottom (up to 5 dB, slowly; held while stopped), then a
     * leveller: every groove, every MOOD, at about the same loudness (+-6 dB at most, over seconds, never pumping) */
    if (!engine_balance_raw) {   /* (1: no balance at all; 2: the parts' gains alone) */
        const float alo = 1.0f - 0.9789f, ahi = 1.0f - 0.6526f, at = 1.0f - 0.8923f;   /* (one-poles: 150 Hz, 3 kHz, 800 Hz) */
        float slo = 0, shi = 0, ss = 0;
        for (int i = 0; i < n; i++) {
            float m = 0.5f * (outL[i] + outR[i]);
            e->tl_lo += alo * (m - e->tl_lo); e->tl_hi += ahi * (m - e->tl_hi);
            float h = m - e->tl_hi; slo += e->tl_lo * e->tl_lo; shi += h * h;
        }
        float bs = (float)n / SR;
        if (e->playing) {
            e->tl_elo += (slo / (float)n - e->tl_elo) * (bs / 5.0f); e->tl_ehi += (shi / (float)n - e->tl_ehi) * (bs / 5.0f);
            if (e->tl_elo > 1e-9f && e->tl_ehi > 1e-9f) {
                float now_db = 10.0f * 0.30103f * fm_log2f(e->tl_ehi / e->tl_elo);
                float tw = clampf(0.6f * (BAL_TILT - now_db), -5.0f, 5.0f);
                e->tl_db += (tw - e->tl_db) * (bs / 4.0f);
            }
        }
        float g1 = fm_exp2f(-e->tl_db * (3.3219f / 40.0f)), gg = e->tl_g > 0 ? e->tl_g : g1, gi = 1.0f / gg;
        float dg = (g1 - gg) / (float)n, dgi = (1.0f / g1 - gi) / (float)n;   /* (glided sample by sample: no step a block) */
        for (int i = 0; i < n; i++) {
            gg += dg; gi += dgi;
            e->tl_s[0] += at * (outL[i] - e->tl_s[0]); e->tl_s[1] += at * (outR[i] - e->tl_s[1]);
            outL[i] = e->tl_s[0] * gg + (outL[i] - e->tl_s[0]) * gi; outR[i] = e->tl_s[1] * gg + (outR[i] - e->tl_s[1]) * gi;
            ss += outL[i] * outL[i] + outR[i] * outR[i];
        }
        e->tl_g = g1;
        e->lev_ss += (ss / (2.0f * (float)n) - e->lev_ss) * (bs / 4.0f);
        float lg0 = e->lev_g > 0 ? e->lev_g : 1.0f, lg1 = lg0;
        if (e->lev_ss > 1e-6f && e->playing) {   /* (stopped: held, so fading tails aren't lifted) */
            float lw = clampf(BAL_RMS * fm_rsqrtf(e->lev_ss), 0.5f, 2.0f);
            lg1 = lg0 + (lw - lg0) * (bs / 3.0f);
        }
        float dl = (lg1 - lg0) / (float)n, lg = lg0;
        for (int i = 0; i < n; i++) { lg += dl; outL[i] *= lg; outR[i] *= lg; }
        e->lev_g = lg1;
    }
    /* the punch-ins */
    /* the punch-ins quantised (playing): one starts on the next beat (pressed up to a sixteenth late: at once), ends on
     * the sixteenth after it's let go; a tap let go before its beat still plays, a sixteenth's worth. SWELL keeps its
     * own timing (it lands on the beat); the filters their own, below. Stopped: at once */
    int want_pu = e->pu_want;
    if (want_pu == PU_LPF || want_pu == PU_HPF) want_pu = 0;
    int qz = e->playing;
    float a_ = e->rate * (float)n - e->frac;
    uint32_t kq = a_ > 0 ? (uint32_t)a_ + 1u : 0u;
    uint32_t bs = e->tick - kq; float bf = (float)kq - a_;   /* (where this block began: bs ticks and bf of one) */
    if (want_pu && want_pu != e->pu && want_pu != e->pu_pend) {
        uint32_t pos = bs % TPB;
        e->pu_pend = want_pu; e->pu_tap = 0;
        e->pu_at = !qz || pos < 24u ? bs : bs - pos + TPB;
        if (want_pu == PU_SWELL) {               /* SWELL: begun just early enough to land on the next beat (as long as its
                                                  * memory lets it swell; less if pressed late), and held, into every beat */
            float sw = fm_minf((float)(PUNCH_BUF / 2 - 64), 60.0f / e->bpm * SR * 0.75f) * e->rate;   /* (in ticks) */
            uint32_t nb = (bs / TPB + 1u) * TPB, st = nb - (uint32_t)sw;
            if (!qz) e->pu_at = bs;
            else if ((int32_t)(st - bs) >= 0) e->pu_at = st;
            else e->pu_at = (float)(nb - bs) * 1.0f >= sw * 0.4f ? bs : nb + TPB - (uint32_t)sw;   /* (too late for this beat: the next) */
        }
    }
    if (e->pu_pend && want_pu != e->pu_pend) e->pu_tap = 1;   /* (let go, or another, before it began) */
    if (e->pu && want_pu == e->pu) e->pu_relq = 0;           /* (pressed again before it ended: on) */
    else if (e->pu && !e->pu_rel && !e->pu_relq && e->pu != PU_SWELL) {
        e->pu_relq = 1; e->pu_rel_at = bs;       /* (let go: ends at once, faded over 4 ms. Only its start waits for the
                                                  * beat) */
        if (e->pu == PU_FREEZE) e->frz_go = 1.0f;   /* (FREEZE's wash let go within a quarter of a second) */
    }
    float lpw = e->pu_lpw, hpw = e->pu_hpw;
    for (int i = 0; i < n; i++) {
        if (e->pu_pend || e->pu_relq) {           /* (their moments, to the sample) */
            float ti = bf + e->rate * (float)i;
            if (e->pu_relq && !e->pu_rel && (float)(int32_t)(bs - e->pu_rel_at) + ti >= 0) { e->pu_rel = 1; e->pu_relq = 0; }
            if (e->pu_pend && !e->pu && (float)(int32_t)(bs - e->pu_at) + ti >= 0) {
                punch_engage(e, e->pu_pend, i);
                if (e->pu_tap && e->pu != PU_SWELL) { e->pu_relq = 1; e->pu_rel_at = (qz ? e->pu_at : bs) + 24u; }
                e->pu_pend = 0; e->pu_tap = 0;
            }
        }
        float y[2] = { outL[i], outR[i] };
        if (!e->pu_frozen) { e->pb[0][e->pw] = to16(y[0]); e->pb[1][e->pw] = to16(y[1]); }
        { float a = fm_fabsf(y[0]) + fm_fabsf(y[1]); if (a > e->pk_run) e->pk_run = a; }
        float pu_[2] = { 0, 0 }, pd_[2] = { 0, 0 };
        if (e->pu == PU_PUP || e->pu == PU_PDOWN || want_pu == PU_PUP || want_pu == PU_PDOWN)
            for (int c = 0; c < 2; c++) shifter_run(&e->shf[c], &y[c], &pu_[c], &pd_[c], 1);
        else for (int c = 0; c < 2; c++) {        /* (only fed, so it's ready the moment a pitch key is pressed) */
            Shifter *s = &e->shf[c]; s->buf[s->pos] = to16(y[c] * 0.999f); s->pos = (s->pos + 1) & (SHIM_LINE - 1);
        }
        int pu = e->pu;
        float tgt = pu && !e->pu_rel ? 1.0f : 0.0f;
        { float st = 1.0f / (0.004f * SR); e->pu_mix += clampf(tgt - e->pu_mix, -st, st); }
        if (pu && e->pu_rel && e->pu_mix < 0.0005f) { e->pu = pu = 0; e->pu_rel = 0; e->pu_frozen = 0; e->pu_mix = 0; }   /* (FREEZE's hold fades on, below) */
        if (pu == PU_FREEZE) {                    /* FREEZE: the room fed a moment, then held, the groove going on over it */
            e->frz_feed = e->rpos < (float)e->frz_len || e->frz_live > 0 || e->frz_lv > 0 ? 1.0f : 0.0f;
            e->frz += ((e->pu_rel ? 0.0f : 1.0f) - e->frz) * (e->pu_rel ? 0.0012f : 0.0004f);
            e->rpos += 1.0f;
        } else if (e->frz > 0) { e->frz += (0.0f - e->frz) * 0.0012f; if (e->frz < 1e-4f) e->frz = 0; }
        if (pu && pu != PU_FREEZE && pu != PU_KILL) {
            float fx[2] = { y[0], y[1] };
            if (pu == PU_PDOWN || pu == PU_PUP) { fx[0] = (pu == PU_PUP ? pu_[0] : pd_[0]) * 1.2f; fx[1] = (pu == PU_PUP ? pu_[1] : pd_[1]) * 1.2f; }
            else if (pu == PU_CRUSH) {
                e->crush_h += 6000.0f / SR; if (e->crush_h >= 1.0f) { e->crush_h -= 1.0f; e->crush_v[0] = (float)(int)(y[0] * 24.0f) / 24.0f; e->crush_v[1] = (float)(int)(y[1] * 24.0f) / 24.0f; }
                fx[0] = e->crush_v[0]; fx[1] = e->crush_v[1];
            } else if (pu == PU_MASH) {           /* TAPE MASH: a chewed tape, held: the sound read back late off the memory,
                                                   * the delay wobbling (wow, flutter) and now and then catching (a sag in
                                                   * pitch), saturated, dulled, with dropouts; pressed harder, mashed more.
                                                   * Every change glides: the read only ever slides, so it never clicks */
                float dep = 0.55f + 0.45f * clamp01(e->pu_press);
                if (--e->m_cnt <= 0) {
                    e->m_cnt = (int)((0.06f + 0.3f * rng_f(&e->rng)) * SR);
                    e->m_tgt = rng_chance(&e->rng, 0.4f) ? 0.4f + 0.6f * rng_f(&e->rng) : 0.0f;
                    e->m_gt = rng_chance(&e->rng, 0.18f * dep) ? 0.12f : 1.0f;
                }
                e->m_wow += (0.6f + 0.5f * e->m_x) / SR; if (e->m_wow >= 1.0f) e->m_wow -= 1.0f;
                e->m_flt += 11.0f / SR; if (e->m_flt >= 1.0f) e->m_flt -= 1.0f;
                e->m_x += (e->m_tgt - e->m_x) * 0.0007f;
                float want_d = (0.014f + dep * (0.006f * sine_c(e->m_wow) + 0.0009f * sine_c(e->m_flt) + 0.022f * e->m_x)) * SR + 64.0f;
                e->m_d += (want_d - e->m_d) * 0.003f;
                e->m_g += (e->m_gt - e->m_g) * 0.004f;
                float rp = (float)e->pw - e->m_d; while (rp < 0) rp += (float)PUNCH_BUF;
                int i0 = (int)rp % PUNCH_BUF, i1 = (i0 + 1) % PUNCH_BUF; float fr = rp - fm_floorf(rp);
                float lk = 0.25f + 0.3f * (1.0f - dep);   /* (dull: ~2.5 to 5 kHz) */
                for (int c = 0; c < 2; c++) {
                    float v = from16(e->pb[c][i0]) + (from16(e->pb[c][i1]) - from16(e->pb[c][i0])) * fr;
                    v = soft_clip(v * (1.6f + 1.6f * dep)) * 0.75f;
                    e->m_lp[c] += (v - e->m_lp[c]) * lk;
                    fx[c] = e->m_lp[c] * e->m_g;
                }
            } else if (pu == PU_SWELL) {          /* (done above, before the room) */
                fx[0] = y[0]; fx[1] = y[1];
            } else if (pu == PU_GLITCH || pu == PU_REVERSE) {   /* the live ones, on the grid from the beat they started on:
                                                  * REVERSE each eighth (or sixteenth) played backwards as the next comes
                                                  * in; GLITCH each sixteenth dealt anew: the groove through, its last
                                                  * sixteenth, 32nd, 64th or triplet repeated, or backwards. Never a
                                                  * repeat of nothing. Each change crossfaded over 1 ms */
                if (e->ch_ph >= e->ch_step) {
                    e->ch_pact = e->ch_act; e->ch_ps0 = e->ch_s0; e->ch_plen = e->ch_len; e->ch_pph = e->ch_ph;
                    e->ch_ph -= e->ch_step; e->ch_s0 = e->pw; e->ch_xf = 44;
                    if (pu == PU_REVERSE) { e->ch_act = CH_REV; e->ch_len = (int)e->ch_step; }
                    else {
                        float r = rng_f(&e->rng); int st = (int)e->ch_step;
                        e->ch_act = r < 0.35f ? CH_LIVE : r < 0.75f ? CH_REP : CH_REV;
                        e->ch_len = e->ch_act == CH_REV ? st : r < 0.45f ? st : r < 0.57f ? st / 2 : r < 0.67f ? st / 4 : st * 2 / 3;
                        if (e->ch_act != CH_LIVE && slice_quiet(e, e->ch_s0 - e->ch_len, e->ch_len)) e->ch_act = CH_LIVE;
                    }
                }
                for (int c = 0; c < 2; c++) {
                    float v = ch_sample(e, e->ch_act, e->ch_s0, e->ch_len, e->ch_ph, c, y[c]);
                    if (e->ch_xf > 0) { float k = (float)e->ch_xf / 44.0f; v += (ch_sample(e, e->ch_pact, e->ch_ps0, e->ch_plen, e->ch_pph, c, y[c]) - v) * k; }
                    fx[c] = v;
                }
                e->ch_ph += 1.0f;
                if (e->ch_xf > 0) { e->ch_xf--; e->ch_pph += 1.0f; }
            } else if (e->pu_fwd && e->pu_cap < (int)e->rlen) {   /* capturing: the first pass as it is (its last 1 ms faded
                                                                  * out, as the loop's first fades in: no step between them) */
                int len = (int)e->rlen;
                if (e->pu_cap == len - 48) {      /* (nearly caught: a hit in it? If not, one in the memory before it,
                                                   * whole slices back? If not again, it listens on to the next slice
                                                   * on the grid, the groove playing meanwhile: up to two beats) */
                    int st = slice_with_a_hit(e, e->ps0, len);
                    e->pu_quiet = st == e->ps0 && slice_quiet(e, e->ps0, len - 48) && (float)(e->pu_tries + 1) * (float)len < beat * 2.0f;
                }
                float left = e->rlen - (float)e->pu_cap, fo = left < 44.0f && !e->pu_quiet ? left / 44.0f : 1.0f;
                fx[0] = y[0] * fo; fx[1] = y[1] * fo;
                if (++e->pu_cap >= len) {
                    if (e->pu_quiet) {            /* (the next slice, from here: on the same grid) */
                        e->pu_quiet = 0; e->pu_tries++; e->pu_cap = 0; e->ps0 = (e->pw + 1) % PUNCH_BUF;
                    } else {
                        e->pu_frozen = 1; e->rpos = 0; e->pu_cap = len;
                        e->ps0 = slice_with_a_hit(e, e->ps0, len);   /* (caught between the hits: the nearest slice with
                                                  * one, whole slices back) */
                    }
                }
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
                }
            }
            y[0] += (fx[0] - y[0]) * e->pu_mix; y[1] += (fx[1] - y[1]) * e->pu_mix;
        }
        /* the filter keys: the bottom white key a high-pass from about 350 Hz, the top one a gently sloped low-pass from
         * about 1.5 kHz, further as they're pressed harder; each faded in and out over 25 ms, the filters always running
         * (so nothing starts cold: no click) */
        {
            float st = 1.0f / (0.025f * SR);
            e->lp_c += clampf((lpw > 0 ? 1.0f : 0.0f) - e->lp_c, -st, st);
            e->hp_c += clampf((hpw > 0 ? 1.0f : 0.0f) - e->hp_c, -st, st);
            if (lpw > 0) e->lp_d += (clamp01(lpw - 1.0f) - e->lp_d) * 0.002f;
            if (hpw > 0) e->hp_d += (clamp01(hpw - 1.0f) - e->hp_d) * 0.002f;
            int lon = e->lp_c > 0 || lpw > 0, hon = e->hp_c > 0 || hpw > 0;   /* (each runs only while its key is down or
                                                  * fading: started from rest, faded in over 25 ms from nothing, it never
                                                  * clicks) */
            if (lon || hon) {
                if ((e->pf_cnt++ & 7) == 0) {
                    float fl = 1500.0f * fm_exp2f(-2.0f * e->lp_d), fh = 350.0f * fm_exp2f(2.0f * e->hp_d);
                    for (int c = 0; c < 2; c++) { svf_set_res(&e->plp[c], fl, 0.65f); op_set(&e->plp1[c], fl); svf_set_res(&e->php[c], fh, 0.9f); }
                }
                for (int c = 0; c < 2; c++) {     /* (in series: each filter run once a sample) */
                    if (lon) {
                        float l = svf_lp(&e->plp[c], y[c]) * 0.7f + op_lp(&e->plp1[c], y[c]) * 0.3f;   /* (the low-pass's slope a
                                                  * gentle one: about 9 dB an octave, no bump at its corner) */
                        y[c] += (l - y[c]) * e->lp_c;
                    }
                    if (hon) { float hh = svf_hp(&e->php[c], y[c]); y[c] += (hh - y[c]) * e->hp_c; }
                }
            } else e->pf_cnt = 0;                 /* (from rest: its settings worked out at the first sample) */
            if (!lon && e->plp[0].ic1 != 0) for (int c = 0; c < 2; c++) { e->plp[c].ic1 = e->plp[c].ic2 = 0; e->plp1[c].z = 0; }
            if (!hon && e->php[0].ic1 != 0) for (int c = 0; c < 2; c++) e->php[c].ic1 = e->php[c].ic2 = 0;
        }
        if (!e->pu_frozen || e->pu_rel) { /* (the memory recording again once let go) */ }
        if (!e->pu_frozen) { e->pw = (e->pw + 1) % PUNCH_BUF; if (e->rec_since < PUNCH_BUF) e->rec_since++; }
        else e->rec_since = 0;
        outL[i] = y[0]; outR[i] = y[1];
    }
    e->pk_run *= 0.995f;                         /* (the groove's recent loudest, let go over a couple of seconds) */
    /* a new key while one plays: let go of that first (4 ms), then the new one comes in */
    (void)kb;
}
