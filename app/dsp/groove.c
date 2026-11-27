/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the groove (groove.h): REC's dealing, and the patterns it writes. */
#include "groove.h"

const char *const PART_NAME[NPARTS] = { "KICK", "SNARE", "PERC", "HATS" };
const char *const GEN_NAME[NUM_GENS] = { "MAP", "EUCLID", "TURING", "BROKEN" };
const char *const RATE_NAME[NUM_RATES] = { "1/16", "1/8", "1/32", "1/16T", "1/8T" };
const uint8_t RATE_TICKS[NUM_RATES] = { 24, 48, 12, 16, 32 };
const char *const RT_NAME[NUM_RT] = { "ROOM", "GATED", "REVERSE", "FREEZE" };
const char *const CT_NAME[NUM_CT] = { "CLEAN", "TAPE SMASH" };
const char *const DT_NAME[NUM_DT] = { "CLEAN", "GRANULAR" };
const char *const PT_NAME[NUM_PT] = { "SWEEP", "SPIN", "SCATTER" };
const char *const DTIME_NAME[NUM_DTIMES] = { "1/4", "3/16", "1/8", "1/8T", "1/16", "1/32", "1/64", "1/128" };
const uint8_t DTIME_TICKS[NUM_DTIMES] = { 96, 72, 48, 32, 24, 12, 6, 3 };

const Scale SCALES[NUM_SCALES] = {
    { "MINOR", 7, { 0, 2, 3, 5, 7, 8, 10 } },     { "DORIAN", 7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "PHRYGIAN", 7, { 0, 1, 3, 5, 7, 8, 10 } },  { "MAJOR", 7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "LYDIAN", 7, { 0, 2, 4, 6, 7, 9, 11 } },    { "MIN PENTA", 5, { 0, 3, 5, 7, 10, 0, 0 } },
    { "MAJ PENTA", 5, { 0, 2, 4, 7, 9, 0, 0 } },  { "WHOLE TONE", 6, { 0, 2, 4, 6, 8, 10, 0 } },
};

/* the groove map: four corners for kick, snare and hats, blended by the map's place (Grids' idea, grooves of our own):
 * HALF-TIME, 2-STEP, ELECTRO, BROKEN */
static const uint8_t MAP[4][3][16] = {
    { { 255,0,0,0, 0,0,40,0, 0,0,200,0, 0,0,60,0 },   { 0,0,0,0, 0,0,0,0, 255,0,0,30, 0,0,0,50 },  { 200,40,120,40, 200,40,120,60, 200,40,120,40, 200,60,140,90 } },
    { { 255,0,0,0, 0,0,0,90, 0,0,220,0, 0,0,0,0 },    { 0,0,0,30, 255,0,0,0, 0,0,0,40, 255,0,30,0 }, { 60,150,60,200, 60,150,60,200, 60,150,60,200, 60,170,80,230 } },
    { { 255,0,0,0, 0,0,180,0, 0,0,160,0, 0,90,0,0 },  { 0,0,0,0, 255,0,0,0, 0,0,0,0, 255,0,0,60 },  { 180,90,180,90, 180,90,180,120, 180,90,180,90, 180,120,200,140 } },
    { { 255,0,0,120, 0,0,0,0, 0,200,0,0, 0,0,140,0 }, { 0,0,0,0, 0,0,220,0, 0,0,0,0, 0,180,0,0 },  { 120,200,60,160, 220,40,180,90, 120,200,60,160, 100,230,40,200 } },
};
static float map_level(int row, int step, float x, float y)
{
    step &= 15;
    float a = MAP[0][row][step], b = MAP[1][row][step], c = MAP[2][row][step], d = MAP[3][row][step];
    float blend = lerpf(lerpf(a, b, x), lerpf(c, d, x), y);
    float near = MAP[(x >= 0.5f) + 2 * (y >= 0.5f)][row][step] * 0.9f;   /* (the nearest corner's own hits kept strong: a blend
                                                                           * alone smears every groove's backbeat away) */
    return fm_maxf(blend, near);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int pick(Rng *r, const uint8_t *w, int n)   /* a weighted choice */
{
    int t = 0; for (int i = 0; i < n; i++) t += w[i];
    int x = rng_int(r, t);
    for (int i = 0; i < n; i++) { if (x < w[i]) return i; x -= w[i]; }
    return 0;
}

/* ------------------------------------------------------------------ dealing */
static void deal_part(Groove *g, int p, Rng *r, int keep_seq)
{
    PartP *q = &g->p[p];
    q->salt = (uint16_t)rng_u32(r); q->muts = 0;
    {   /* its engine: the part's own, weighted (Burial and IDM first: the 808 and the clap often) */
        static const uint8_t EL[NPARTS][4] = { { D_808, D_MEMBRANE, D_BOX, D_FMKICK }, { D_CLAP, D_CRACK, D_WIRES, D_GRAIN },
                                               { D_MATERIAL, D_FMPERC, D_STRING, D_FOLEY }, { D_METAL, D_DUST, D_DIGITAL, D_VOX } };
        static const uint8_t EL5[NPARTS] = { D_FMKICK, D_BENT, D_BENT, D_CLICK };   /* (a fifth choice: BENT, the hats' clicks) */
        static const uint8_t EW[NPARTS][5] = { { 4, 3, 2, 2, 0 }, { 4, 3, 2, 2, 2 }, { 3, 3, 2, 3, 2 }, { 2, 3, 2, 3, 3 } };
        int k = pick(r, EW[p], 5);
        q->engine = k < 4 ? EL[p][k] : EL5[p];
    }
    groove_part_engine(g, p);
    if (!keep_seq) {
        static const uint8_t PLEN[] = { 5, 7, 9, 10, 11, 13 }, HLEN[] = { 6, 7, 9, 11, 12, 15, 16 };
        switch (p) {
        case P_KICK:  { static const uint8_t w[NUM_GENS] = { 6, 1, 0, 3 }; q->gen = (uint8_t)pick(r, w, NUM_GENS); q->len = rng_chance(r, 0.3f) ? 32 : 16; q->rate = R_16; break; }
        case P_SNARE: { static const uint8_t w[NUM_GENS] = { 7, 0, 0, 2 }; q->gen = (uint8_t)pick(r, w, NUM_GENS); q->len = rng_chance(r, 0.4f) ? 32 : 16; q->rate = R_16; break; }
        case P_PERC:  { static const uint8_t w[NUM_GENS] = { 1, 5, 2, 4 }; q->gen = (uint8_t)pick(r, w, NUM_GENS); q->len = PLEN[rng_int(r, 6)];
                        static const uint8_t rw[NUM_RATES] = { 7, 1, 1, 2, 1 }; q->rate = (uint8_t)pick(r, rw, NUM_RATES); break; }
        default:      { static const uint8_t w[NUM_GENS] = { 1, 1, 6, 1 }; q->gen = (uint8_t)pick(r, w, NUM_GENS); q->len = HLEN[rng_int(r, 7)];
                        static const uint8_t rw[NUM_RATES] = { 8, 0, 1, 2, 0 }; q->rate = (uint8_t)pick(r, rw, NUM_RATES); break; }   /* (sixteenths, a shuffle's triplets, now and then 1/32) */
        }
        static const uint8_t DEN[NPARTS][2] = { { 34, 52 }, { 30, 48 }, { 22, 40 }, { 34, 52 } };   /* (the perc sparse, the hats a little) */
        q->density = (uint8_t)(DEN[p][0] + rng_int(r, DEN[p][1] - DEN[p][0] + 1));
    }
    static const uint8_t LV[NPARTS] = { 85, 72, 62, 58 };
    q->level = LV[p]; q->decay = 50; q->pitch = 0;
    static const uint8_t PAN[NPARTS] = { 50, 52, 38, 64 };
    q->pan = (uint8_t)clampi(PAN[p] + (p >= P_PERC ? rng_int(r, 17) - 8 : 0), 0, 100);
    /* the sends: every part dealt dry (REVERB, DELAY, PAN and COMP are yours to add, the kick's too).
     * (the old amounts still drawn and dropped, so the rest of the groove deals as it did) */
    if (p == P_KICK) { q->s_room = q->s_delay = q->s_pan = q->s_comp = 0; }
    else {
        static const uint8_t RM[NPARTS][2] = { { 0, 0 }, { 35, 62 }, { 10, 35 }, { 5, 20 } }, DL[NPARTS][2] = { { 0, 0 }, { 0, 12 }, { 20, 60 }, { 10, 30 } };
        static const uint8_t PN[NPARTS][2] = { { 0, 0 }, { 0, 30 }, { 30, 70 }, { 20, 60 } }, DU[NPARTS][2] = { { 0, 0 }, { 20, 50 }, { 20, 60 }, { 40, 80 } };
        q->s_room = (uint8_t)(RM[p][0] + rng_int(r, RM[p][1] - RM[p][0] + 1));
        q->s_delay = (uint8_t)(DL[p][0] + rng_int(r, DL[p][1] - DL[p][0] + 1));
        q->s_pan = (uint8_t)(PN[p][0] + rng_int(r, PN[p][1] - PN[p][0] + 1));
        (void)rng_int(r, DU[p][1] - DU[p][0] + 1);
        q->s_room = q->s_delay = q->s_pan = q->s_comp = 0;
    }
}

/* an engine's colours as REC deals them: machine-like first (IDM), now and then further out */
void groove_part_engine(Groove *g, int p)
{
    PartP *q = &g->p[p];
    Rng r; rng_seed(&r, g->seed ^ q->salt * 2654435761u ^ q->engine * 40503u);
    q->weight = (uint8_t)(18 + rng_int(&r, 40));   /* (18-57: short, staccato, mostly) */
    q->entropy = (uint8_t)(rng_chance(&r, 0.25f) ? 55 + rng_int(&r, 36) : 10 + rng_int(&r, 46));
    q->tone = 50;
    if (p == P_HATS && rng_chance(&r, 0.4f)) q->tone = (uint8_t)(54 + rng_int(&r, 10));   /* (a hat thinned a little) */
    if (p == P_PERC && rng_chance(&r, 0.3f)) q->tone = (uint8_t)(36 + rng_int(&r, 12));   /* (perc darkened) */
}

static void deal_fx(Groove *g, Rng *r)
{
    static const uint8_t RTW[NUM_RT] = { 10, 5, 1, 1 }, PTW[NUM_PT] = { 4, 3, 3 }, TW[NUM_DTIMES] = { 1, 4, 3, 2, 3, 1, 0, 0 };
    g->room = (uint8_t)(30 + rng_int(r, 26)); g->comp = (uint8_t)(25 + rng_int(r, 31));
    g->delay = (uint8_t)(20 + rng_int(r, 26)); g->pan = (uint8_t)(30 + rng_int(r, 31));
    g->room_t = (uint8_t)pick(r, RTW, NUM_RT); g->comp_t = (uint8_t)(rng_chance(r, 0.3f) ? CT_SMASH : CT_CLEAN);
    g->delay_t = (uint8_t)(rng_chance(r, 0.35f) ? DT_GRAIN : DT_CLEAN); g->pan_t = (uint8_t)pick(r, PTW, NUM_PT);
    g->dtime = (uint8_t)pick(r, TW, NUM_DTIMES); g->dfb = (uint8_t)(25 + rng_int(r, 31)); g->glitch = (uint8_t)(rng_chance(r, 0.4f) ? rng_int(r, 35) : 0);
}

void groove_default(Groove *g)
{
    memset(g, 0, sizeof *g);
    g->mood = 50; g->activity = 50; g->evolve = 20; g->drift = 0;
    g->link = 40; g->warp = 20; g->flow = 25; g->chaos = 10;
    g->key = 9; g->scale = 0; g->follow = 1; g->tempo = 112 - 40;
    groove_roll(g, 0x1D3A5u, 0);
}

int groove_ok(const Groove *g)
{
    if (g->mood > 100 || g->activity > 100 || g->evolve > 100 || g->drift > 100 || g->link > 100 || g->warp > 100 || g->flow > 100 || g->chaos > 100) return 0;
    if (g->key > 11 || g->scale >= NUM_SCALES || g->follow > 1 || g->tempo > 160) return 0;
    if (g->room > 100 || g->comp > 100 || g->delay > 100 || g->pan > 100) return 0;
    if (g->room_t >= NUM_RT || g->comp_t >= NUM_CT || g->delay_t >= NUM_DT || g->pan_t >= NUM_PT) return 0;
    if (g->dtime >= NUM_DTIMES || g->dfb > 100 || g->glitch > 100 || g->mapx > 100 || g->mapy > 100 || g->swing > 100 || g->accent > 9 || g->breathe > 100) return 0;
    for (int p = 0; p < NPARTS; p++) {
        const PartP *q = &g->p[p];
        if (q->engine >= NUM_ENGINES || q->tone > 100 || q->weight > 100 || q->entropy > 100) return 0;
        if (q->gen >= NUM_GENS || q->len < MINLEN || q->len > MAXLEN || q->density > 100 || q->rate >= NUM_RATES) return 0;
        if (q->level > 100 || q->decay > 100 || q->pitch < -24 || q->pitch > 24 || q->pan > 100) return 0;
        if (q->s_room > 100 || q->s_delay > 100 || q->s_pan > 100 || q->s_comp > 100) return 0;
    }
    return 1;
}

void groove_roll(Groove *g, uint32_t seed, int all)
{
    Rng r; rng_seed(&r, seed);
    g->seed = seed;
    for (int p = 0; p < NPARTS; p++) deal_part(g, p, &r, 0);
    deal_fx(g, &r);
    /* Burial's 2-step under IDM's detail first: the map mostly near its 2-STEP corner, the swing a shuffle */
    if (rng_chance(&r, 0.6f)) { g->mapx = (uint8_t)(65 + rng_int(&r, 36)); g->mapy = (uint8_t)rng_int(&r, 36); }
    else { g->mapx = (uint8_t)rng_int(&r, 101); g->mapy = (uint8_t)rng_int(&r, 101); }
    g->swing = (uint8_t)(rng_chance(&r, 0.6f) ? 30 + rng_int(&r, 41) : rng_int(&r, 13));   /* SWING: a 2-step shuffle, or IDM's straight */
    { static const uint8_t AC[6] = { 0, 3, 5, 7, 5, 3 }; g->accent = AC[rng_int(&r, 6)]; }   /* the accents' cycle (Autechre's drifting groups) */
    g->breathe = (uint8_t)(30 + rng_int(&r, 61));                                          /* the bar's weight and the slow wave (BoC's breathing) */
    if (all) {
        g->key = (uint8_t)rng_int(&r, 12);
        static const uint8_t SW[NUM_SCALES] = { 6, 5, 3, 1, 1, 3, 1, 1 };
        g->scale = (uint8_t)pick(&r, SW, NUM_SCALES);
        g->tempo = (uint8_t)((rng_chance(&r, 0.7f) ? 122 + rng_int(&r, 19) : 84 + rng_int(&r, 15)) - 40);   /* 122 .. 140 BPM, or half-time 84 .. 98 */
    }
}

void groove_roll_part(Groove *g, int part, uint32_t seed)
{
    Rng r; rng_seed(&r, seed ^ (uint32_t)part * 0x9E3779B9u);
    deal_part(g, part, &r, 0);
}

void groove_name(uint32_t seed, char *out)
{
    static const char *const A[24] = { "STATIC", "COPPER", "HOLLOW", "BROKEN", "SILVER", "SODIUM", "PHANTOM", "RUSTED",
        "PARALLEL", "CHROME", "QUIET", "INVERTED", "MAGNETIC", "FOLDED", "LOW", "BRITTLE", "HEAVY", "VACANT", "SECOND",
        "DISTANT", "SPLIT", "TUNGSTEN", "AMBER", "COLD" };
    static const char *const B[24] = { "LATTICE", "SIGNAL", "ENGINE", "RELAY", "CIRCUIT", "MACHINE", "PULSE", "ORBIT",
        "SWITCH", "FURNACE", "DIODE", "GRID", "TRANSIT", "STATION", "PISTON", "FIELD", "VALVE", "CLOCK", "MIRROR",
        "PATTERN", "HINGE", "SPOOL", "RADIO", "HOUR" };
    Rng r; rng_seed(&r, seed ^ 0x5EED1E55u);
    const char *a = A[rng_int(&r, 24)], *b = B[rng_int(&r, 24)];
    int n = 0;
    while (*a) out[n++] = *a++;
    out[n++] = ' ';
    while (*b) out[n++] = *b++;
    out[n] = 0;
}

/* ------------------------------------------------------------------ the patterns */
static void deal_component(Step *s, int part, Rng *r, int step0)
{
    /* what, by part (its weights), unlocking where (cheap, steady ones early; chance late) */
    static const uint8_t W[NPARTS][NUM_COMP] = {
        /* none ratch nudge cond prob ghost alt pitch accent */
        { 0, 1, 5, 5, 3, 4, 0, 2, 3 },
        { 0, 3, 3, 2, 3, 2, 2, 1, 1 },
        { 0, 2, 2, 4, 4, 1, 3, 3, 1 },
        { 0, 5, 2, 2, 3, 2, 4, 1, 2 },
    };
    static const uint8_t UL[NUM_COMP][2] = { { 0, 0 }, { 15, 70 }, { 5, 45 }, { 10, 55 }, { 35, 95 }, { 20, 75 }, { 10, 60 }, { 15, 65 }, { 5, 40 } };
    int t = pick(r, W[part], NUM_COMP);
    if (t == C_NONE) return;
    if (step0 && part == P_KICK && (t == C_COND || t == C_PROB || t == C_NUDGE)) return;   /* (the one stays the one) */
    if (part <= P_SNARE && s->lvl >= 170 && (t == C_RATCHET || t == C_PROB || t == C_COND || t == C_GHOST))
        t = rng_chance(r, 0.5f) ? C_NUDGE : C_ACCENT;   /* (the kick's and snare's main hits: only the gentle ones; a
                                                          * backbeat rolled or dropped isn't a backbeat) */
    s->ct = (uint8_t)t;
    s->cu = (uint8_t)(UL[t][0] + rng_int(r, UL[t][1] - UL[t][0] + 1));
    switch (t) {
    case C_RATCHET: s->ca = (uint8_t)(2 + rng_int(r, 3)); break;
    case C_NUDGE: { int v = 1 + rng_int(r, 6); s->ca = (uint8_t)(128 + (rng_chance(r, 0.5f) ? v : -v)); break; }
    case C_COND: s->ca = (uint8_t)rng_int(r, NUM_CD); break;
    case C_PROB: s->ca = (uint8_t)(25 + rng_int(r, 51)); break;
    case C_GHOST: s->ca = (uint8_t)(14 + rng_int(r, 21)); break;
    case C_ALT: s->ca = 1; break;
    case C_PITCH: {
        static const int8_t PP[NPARTS][5] = { { -2, -3, -5, -7, 2 }, { -2, 1, 2, 3, -1 }, { 4, 7, 4, 7, 2 }, { 2, 3, 5, -2, 4 } };
        s->ca = (uint8_t)(128 + PP[part][rng_int(r, 5)]); break; }
    default: s->ca = (uint8_t)(20 + rng_int(r, 21)); break;   /* (accent) */
    }
}

static void turing_levels(Pattern *pt, int part)
{
    for (int i = 0; i < pt->len; i++) {
        int k = i & 15;
        uint32_t rot = ((uint32_t)pt->reg >> k | (uint32_t)pt->reg << (16 - k)) & 0xFFFFu;
        int v = (int)(rot & 0xFFu);
        Step *s = &pt->s[i];
        if (part == P_HATS) v = v * 3 / 4 + (i % 2 == 0 ? 70 : 0) + (i % 4 == 0 ? 30 : 0);   /* (a pulse under the hats' machine) */
        if (part == P_PERC) {                     /* (the perc sparse: on only where two of the register's bits agree, ~1 step in 4) */
            int on = (pt->reg >> k & 1u) && (pt->reg >> ((k + 5) & 15) & 1u);
            v = on ? 150 + v * 2 / 5 : v / 4;
        }
        s->lvl = (uint8_t)clampi(v, 0, 255);
        s->vel = (uint8_t)clampi(40 + (int)((rot >> 8) & 0x7Fu) * 87 / 127, 1, 127);
        s->alt = part == P_HATS && (rot & 0x1Cu) == 0x1Cu;     /* (an open hat, now and then) */
    }
}

void pattern_make(Pattern *pt, const Groove *g, int part)
{
    const PartP *q = &g->p[part];
    Rng r; rng_seed(&r, g->seed ^ (uint32_t)q->salt * 0x85EBCA6Bu ^ (uint32_t)q->gen * 0xC2B2AE35u ^ (uint32_t)q->len * 0x27D4EB2Fu);
    memset(pt, 0, sizeof *pt);
    int len = clampi(q->len, MINLEN, MAXLEN);
    pt->len = (uint8_t)len; pt->rate = q->rate;
    float mx = (float)g->mapx * 0.01f, my = (float)g->mapy * 0.01f;
    int row = part == P_KICK ? 0 : part == P_SNARE ? 1 : 2;
    switch (q->gen) {
    case G_MAP:
        for (int i = 0; i < len; i++) {
            float v = i < 16 || part == P_SNARE ? map_level(row, i + (part == P_PERC ? 3 : 0), mx, my)   /* (the snare's second bar: its backbeat kept) */
                             : map_level(row, i + (part == P_PERC ? 3 : 0), clamp01(mx + 0.25f * rng_bi(&r)), clamp01(my + 0.25f * rng_bi(&r)));   /* (the second bar: a neighbour) */
            if (part == P_PERC) v *= 0.8f;
            pt->s[i].lvl = (uint8_t)clampf(v + rng_bi(&r) * 12.0f, 0.0f, 255.0f);
            pt->s[i].vel = (uint8_t)clampi(70 + (int)(v * 57.0f / 255.0f), 1, 127);
        }
        if (part == P_KICK) pt->s[0].lvl = 255;
        break;
    case G_EUCLID: {
        float kf = part <= P_PERC ? 0.13f + 0.18f * rng_f(&r) : 0.25f + 0.35f * rng_f(&r);   /* (the kick, snare and perc: few hits a loop) */
        int k = clampi((int)((float)len * kf + 0.5f), 2, len - 1), rot = rng_int(&r, len);
        int k2 = clampi(k + 1 + rng_int(&r, len / 3 + 1), k + 1, len);
        for (int i = 0; i < len; i++) {
            int j = (i + rot) % len, on = (j * k) % len < k, on2 = (j * k2) % len < k2;
            Step *s = &pt->s[i];
            s->lvl = (uint8_t)(on ? 190 + rng_int(&r, 66) : on2 ? (part <= P_SNARE ? 60 + rng_int(&r, 61) : 90 + rng_int(&r, 81)) : rng_int(&r, 61));
            s->vel = (uint8_t)(on && (i == 0 || !((((i - 1 + rot) % len) * k) % len < k)) ? 110 : 75 + rng_int(&r, 25));
        }
        if (part == P_KICK) pt->s[0].lvl = 255;
        break; }
    case G_TURING:
        pt->reg = (uint16_t)(rng_u32(&r) | 1u);
        turing_levels(pt, part);
        if (part == P_KICK) pt->s[0].lvl = 255;
        break;
    default: {                                    /* BROKEN: groups of twos and threes (now and then a four, or a one) */
        int i = 0;
        static const uint8_t GW[4] = { 1, 9, 9, 3 };      /* sizes 1 .. 4 */
        int grp = 0;
        while (i < len) {
            int sz = 1 + pick(&r, GW, 4); if (i + sz > len) sz = len - i;
            int strong = part >= P_PERC || (grp & 1) == (part == P_SNARE);   /* (the kick and snare: every other group's start
                                                                               * strong, the rest in reserve for ACTIVITY) */
            grp++;
            for (int j = 0; j < sz; j++) {
                Step *s = &pt->s[i + j];
                if (j == 0 && strong) { s->lvl = (uint8_t)(205 + rng_int(&r, 51)); s->vel = (uint8_t)(90 + rng_int(&r, 38)); }
                else if (j == 0) { s->lvl = (uint8_t)(80 + rng_int(&r, 50)); s->vel = (uint8_t)(70 + rng_int(&r, 30)); }
                else if ((sz == 3 && j == 2) || (sz == 4 && j == 2)) { s->lvl = (uint8_t)(part <= P_SNARE ? 60 + rng_int(&r, 50) : 110 + rng_int(&r, 41)); s->vel = (uint8_t)(60 + rng_int(&r, 30)); }
                else { s->lvl = (uint8_t)rng_int(&r, 71); s->vel = (uint8_t)(50 + rng_int(&r, 30)); }
            }
            i += sz;
        }
        if (part == P_SNARE)                      /* (a snare's groups: its hits off the bar's one) */
            for (i = 0; i < len; i++) if (i % 8 == 0) pt->s[i].lvl = (uint8_t)(pt->s[i].lvl / 3);
        if (part == P_KICK) pt->s[0].lvl = 255;
        break; }
    }
    /* every pattern its core: at least so many strong steps (the strongest it has, raised), whatever wrote it */
    {
        int want = part == P_KICK ? (len + 7) / 8 : part == P_SNARE ? (len + 7) / 8 : part == P_PERC ? (len + 5) / 6 : (len + 2) / 3, have = 0;
        if (want < 2) want = 2;
        for (int i = 0; i < len; i++) have += pt->s[i].lvl >= 170;
        for (int k = have; k < want; k++) {
            int best = -1;
            for (int i = 0; i < len; i++) if (pt->s[i].lvl < 170 && pt->s[i].lvl > 0 && (best < 0 || pt->s[i].lvl > pt->s[best].lvl)) best = i;
            if (best < 0) for (int i = 0; i < len; i++) if (pt->s[i].lvl < 170) { best = (i * 5 + 3) % len; if (pt->s[best].lvl < 170) break; }
            if (best < 0) break;
            pt->s[best].lvl = (uint8_t)(175 + rng_int(&r, 60)); if (pt->s[best].vel < 80) pt->s[best].vel = 85;
        }
    }
    /* PERC's melody: a walk over the scale, the register or the walk deciding */
    /* PERC's notes: mostly one, now and then its fifth or its octave (a melody wandering the scale on a pitched
     * percussion reads as a steel band) */
    if (part == P_PERC) { static const int8_t PD[8] = { 0, 0, 0, 0, 0, 4, 7, 2 }; for (int i = 0; i < len; i++) pt->s[i].pitch = PD[rng_int(&r, 8)]; }
    if (part == P_HATS && q->gen != G_TURING) for (int i = 0; i < len; i++) pt->s[i].alt = rng_chance(&r, 0.08f) && pt->s[i].lvl > 100;
    for (int i = 0; i < len; i++) if (rng_chance(&r, 0.45f)) deal_component(&pt->s[i], part, &r, i == 0);
    uint32_t n = q->muts > 999u ? 999u : q->muts;
    for (uint32_t i = 0; i < n; i++) pattern_mutate(pt, g, part, i);
}

/* EVOLVE past 70: a rewrite, the same i-th every time (so SAVE need keep only how many) */
void pattern_mutate(Pattern *pt, const Groove *g, int part, uint32_t i)
{
    const PartP *q = &g->p[part];
    Rng r; rng_seed(&r, (uint32_t)q->salt * 0x9E3779B1u ^ i * 0x85EBCA77u ^ g->seed);
    int len = pt->len;
    if (q->gen == G_TURING) {                    /* the Turing machine: a bit flipped, the loop rewritten (its components kept) */
        pt->reg ^= (uint16_t)(1u << rng_int(&r, 16));
        if (!pt->reg) pt->reg = 1;
        turing_levels(pt, part);
        if (part == P_KICK) pt->s[0].lvl = 255;
        return;
    }
    int op = rng_int(&r, 3);
    if (op == 0) {                                /* a hit moves next door */
        for (int t = 0; t < 8; t++) {
            int a = rng_int(&r, len), b = (a + (rng_chance(&r, 0.5f) ? 1 : len - 1)) % len;
            if (part == P_KICK && (a == 0 || b == 0)) continue;
            if (pt->s[a].lvl >= 150 && pt->s[b].lvl < 100) { uint8_t l = pt->s[a].lvl, v = pt->s[a].vel; pt->s[a].lvl = pt->s[b].lvl; pt->s[a].vel = pt->s[b].vel; pt->s[b].lvl = l; pt->s[b].vel = v; break; }
        }
    } else if (op == 1) {                         /* a component dealt anew */
        int a = rng_int(&r, len); Step *s = &pt->s[a];
        s->ct = C_NONE; s->cu = 0; s->ca = 0;
        deal_component(s, part, &r, a == 0);
    } else {                                      /* two steps trade places */
        int a = rng_int(&r, len), b = rng_int(&r, len);
        if (part == P_KICK && (a == 0 || b == 0)) return;
        Step t = pt->s[a]; pt->s[a] = pt->s[b]; pt->s[b] = t;
    }
}
