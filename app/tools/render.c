/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality on the desktop: the sound engine to a WAV, without the screen.
 *   render SEED SECONDS OUT.wav           a groove REC would deal from SEED (its key and tempo too)
 *   render default SECONDS OUT.wav        the groove the FM-1 starts with
 *   render sweep ENGINE OUT.wav           one engine, hit after hit: WEIGHT 0 -> 1, then ENTROPY 0 -> 1 (ENGINE: its name)
 *   render punch SEED OUT.wav             a groove with the punch-ins played over it
 *   render calibrate | bench              each engine's level / cost
 * Settings from the environment: MOOD ACT EVO DRIFT LINK WARP FLOW CHAOS (0..100), BPM, EVOSWEEP=1 (EVOLVE 0 -> 100
 * across the render), ACTSWEEP=1, ENGS=K,S,P,H (engine names), ROOM COMP DELAY PAN (0..100), RT CT DT PT (types),
 * DTIME (index), GLITCH, AI=1 (a pretend After Images: a GROUND note each beat, busy LIGHT). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <xmmintrin.h>
#include "../dsp/engine.h"

static Engine eng;
static Groove gr;

static void w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24 & 255, f); }
static void w16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
typedef struct { FILE *f; long frames; float peak; double ss; int jumps; float last[2]; } Wav;
static int wav_open(Wav *w, const char *path) { memset(w, 0, sizeof *w); w->f = fopen(path, "wb"); if (!w->f) return 0; for (int i = 0; i < 44; i++) fputc(0, w->f); return 1; }
static inline float soft(float x) { float a = fm_fabsf(x); if (a < 0.8f) return x; float y = 0.8f + 0.2f * fm_tanhf((a - 0.8f) / 0.2f); return x < 0 ? -y : y; }
static void wav_put(Wav *w, float l, float r)
{
    float in[2] = { l, r };
    for (int c = 0; c < 2; c++) {
        float a = fm_fabsf(in[c]); if (a > w->peak) w->peak = a;
        w->ss += (double)(in[c] * in[c]);
        if (fm_fabsf(in[c] - w->last[c]) > 0.9f) w->jumps++;
        w->last[c] = in[c];
        w16(w->f, (uint32_t)(int16_t)(soft(in[c]) * 32000.0f));
    }
    w->frames++;
}
static void wav_close(Wav *w, const char *name)
{
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f); w32(w->f, 36 + (uint32_t)w->frames * 4); fwrite("WAVEfmt ", 1, 8, w->f);
    w32(w->f, 16); w16(w->f, 1); w16(w->f, 2); w32(w->f, 44100); w32(w->f, 44100 * 4); w16(w->f, 4); w16(w->f, 16);
    fwrite("data", 1, 4, w->f); w32(w->f, (uint32_t)w->frames * 4);
    fclose(w->f);
    fprintf(stderr, "%s: %.1f s, peak %.2f, rms %.3f, jumps %d\n", name, (double)w->frames / 44100.0, (double)w->peak,
            w->frames ? __builtin_sqrt(w->ss / (double)(w->frames * 2)) : 0.0, w->jumps);
}

static int env_i(const char *k, int def) { const char *v = getenv(k); return v ? atoi(v) : def; }
static int engine_by_name(const char *s) { for (int e = 0; e < NUM_ENGINES; e++) if (!strcmp(ENGINE_NAME[e], s)) return e; return -1; }

static void apply_env(Groove *g)
{
    g->mood = (uint8_t)env_i("MOOD", g->mood); g->activity = (uint8_t)env_i("ACT", g->activity);
    g->evolve = (uint8_t)env_i("EVO", g->evolve); g->drift = (uint8_t)env_i("DRIFT", g->drift);
    g->link = (uint8_t)env_i("LINK", g->link); g->warp = (uint8_t)env_i("WARP", g->warp);
    g->flow = (uint8_t)env_i("FLOW", g->flow); g->chaos = (uint8_t)env_i("CHAOS", g->chaos);
    g->room = (uint8_t)env_i("ROOM", g->room); g->comp = (uint8_t)env_i("COMP", g->comp);
    g->pan = (uint8_t)env_i("PAN", g->pan);
    g->room_t = (uint8_t)env_i("RT", g->room_t); g->comp_t = (uint8_t)env_i("CT", g->comp_t);
    g->delay_t = (uint8_t)env_i("DT", g->delay_t); g->pan_t = (uint8_t)env_i("PT", g->pan_t);
    g->dtime = (uint8_t)env_i("DTIME", g->dtime); g->glitch = (uint8_t)env_i("GLITCH", g->glitch);
    if (getenv("BPM")) g->tempo = (uint8_t)(atoi(getenv("BPM")) - 40);
    if (getenv("ENGS")) {
        char buf[128]; strncpy(buf, getenv("ENGS"), sizeof buf - 1); buf[sizeof buf - 1] = 0;
        int p = 0; for (char *t = strtok(buf, ","); t && p < NPARTS; t = strtok(NULL, ","), p++) { int e = engine_by_name(t); if (e >= 0) { g->p[p].engine = (uint8_t)e; groove_part_engine(g, p); } }
    }
    { extern int engine_balance_raw; if (getenv("BALRAW")) engine_balance_raw = atoi(getenv("BALRAW")); }
    if (getenv("SEND")) {   /* (SEND=part,room,delay,pan: a part's sends, e.g. 0,40,30,50 the kick's) */
        int sp = 0, sr = 0, sd = 0, sn = 0;
        if (sscanf(getenv("SEND"), "%d,%d,%d,%d", &sp, &sr, &sd, &sn) == 4 && sp >= -1 && sp < NPARTS)   /* (-1: every part) */
            for (int q = sp < 0 ? 0 : sp; q < (sp < 0 ? NPARTS : sp + 1); q++) {
                g->p[q].s_room = (uint8_t)sr; g->p[q].s_delay = (uint8_t)sd; g->p[q].s_pan = (uint8_t)sn;
            }
    }
    if (getenv("SOLO")) { int k = atoi(getenv("SOLO")); for (int p = 0; p < NPARTS; p++) if (p != k) g->p[p].level = 0; }   /* (one part alone) */
    if (getenv("DRY")) { const char *d = getenv("DRY"); if (strchr(d, 'r')) g->room = 0; if (strchr(d, 'c')) g->comp = 0; if (strchr(d, 'p')) g->pan = 0; }   /* (r d c p: effects off) */
    if (getenv("SMOOTH")) {                       /* a smooth kit (sines only): any step in the output is a click */
        g->p[P_KICK].engine = D_MEMBRANE; g->p[P_SNARE].level = 0; g->p[P_HATS].level = 0; g->p[P_PERC].engine = D_MATERIAL;
        g->p[P_KICK].entropy = 10; g->p[P_PERC].entropy = 30; g->room = 0;
    }
    for (int p = 0; p < NPARTS; p++) {
        char k[16];
        snprintf(k, sizeof k, "W%d", p); if (getenv(k)) g->p[p].weight = (uint8_t)atoi(getenv(k));
        snprintf(k, sizeof k, "E%d", p); if (getenv(k)) g->p[p].entropy = (uint8_t)atoi(getenv(k));
        snprintf(k, sizeof k, "L%d", p); if (getenv(k)) g->p[p].level = (uint8_t)atoi(getenv(k));
    }
}

static void describe(const Groove *g)
{
    char nm[40]; groove_name(g->seed, nm);
    fprintf(stderr, "%s, %d BPM; ", nm, g->tempo + 40);
    for (int p = 0; p < NPARTS; p++) fprintf(stderr, "%s %s %s %d/%s w%d e%d%s", PART_NAME[p], ENGINE_NAME[g->p[p].engine], GEN_NAME[g->p[p].gen],
                                             g->p[p].len, RATE_NAME[g->p[p].rate], g->p[p].weight, g->p[p].entropy, p < 3 ? "; " : "\n");
    fprintf(stderr, "  room %d %s, comp %d %s, genre %s, delay %s %s, pan %d %s; macros mood %d act %d evolve %d drift %d\n", g->room, RT_NAME[g->room_t],
            g->comp, CT_NAME[g->comp_t], GENRE_NAME[g->genre % NUM_GENRES], DT_NAME[g->delay_t], DTIME_NAME[g->dtime], g->pan, PT_NAME[g->pan_t], g->mood, g->activity, g->evolve, g->drift);
}

static void dump(void)
{
    for (int p = 0; p < NPARTS; p++) {
        Pattern pt; pattern_make(&pt, &gr, p);
        fprintf(stderr, "  %-5s", PART_NAME[p]);
        for (int i = 0; i < pt.len; i++) fprintf(stderr, " %3d%c", pt.s[i].lvl, pt.s[i].ct ? "-rncpgaPA"[pt.s[i].ct] : ' ');
        fprintf(stderr, "\n");
    }
}

/* DUET=notes.txt (with AIWAV=its audio): After Images' notes heard at their moments, its tempo followed, its audio
 * mixed in, as over the cable */
static struct { long at; int ch, note, vel; } dnotes[4096]; static int dn_n, dn_at; static FILE *aiw;
static void duet_load(void)
{
    FILE *f = fopen(getenv("DUET"), "r"); if (!f) return;
    char line[64]; int bpm = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'T') { bpm = atoi(line + 2); gr.tempo = (uint8_t)(bpm - 40); }
        else if (line[0] == 'N' && dn_n < 4096) { sscanf(line + 2, "%ld %d %d %d", &dnotes[dn_n].at, &dnotes[dn_n].ch, &dnotes[dn_n].note, &dnotes[dn_n].vel); dn_n++; }
    }
    fclose(f);
    if (getenv("AISCALE")) for (int s = 0; s < NUM_SCALES; s++) if (!strcmp(SCALES[s].name, getenv("AISCALE"))) gr.scale = (uint8_t)s;
    if (getenv("AIWAV")) { aiw = fopen(getenv("AIWAV"), "rb"); if (aiw) fseek(aiw, 44, SEEK_SET); }
    fprintf(stderr, "  duet: %d notes of After Images' at %d BPM\n", dn_n, bpm);
}

/* puscan SEED: each punch-in pressed at every sixteenth of a bar (a 32nd after it, as a loose press), held two beats:
 * its level while it plays against the groove's own over the same stretch. Silent ones are what a press between the
 * hits gives */
static float block_rms(const float *L, const float *R, int n) { double s = 0; for (int i = 0; i < n; i++) s += (double)(L[i] * L[i] + R[i] * R[i]); return (float)(s / (2.0 * n)); }
static void puscan(uint32_t seed)
{
    static const int PUS[] = { PU_REP8, PU_REP12, PU_REP16, PU_REP32, PU_GLITCH, PU_REVERSE, PU_FREEZE, PU_SWELL, PU_MASH, PU_PDOWN };
    static float L[BLKN], R[BLKN];
    Groove base = gr;
    base.genre = (uint8_t)env_i("GENRE", base.genre);   /* (0 IDM, 1 TECHNO: what's dealt) */ groove_sends = env_i("SENDS", 0);
    if (seed) groove_roll(&base, seed, 1);
    apply_env(&base);
    for (unsigned u = 0; u < sizeof PUS / sizeof PUS[0]; u++) {
        int pu = PUS[u], silent = 0; float worst = 0, sum = 0;
        for (int k = 0; k < 16; k++) {
            float at = 8.0f + (float)k * 0.25f + 0.125f;
            static float keep[2][2048][BLKN];      /* (the groove alone, block by block: what FREEZE and SWELL add over
                                                    * it is the difference) */
            int over = pu == PU_FREEZE || pu == PU_SWELL;
            for (int pass = 0; pass < 2; pass++) {   /* (0: the groove alone; 1: the punch-in) */
                gr = base; engine_init(&eng, &gr); eng.playing = 1;
                double e = 0; int nb = 0, bi = 0;
                while (eng.beat < at + 2.0f && bi < 2048) {
                    if (pass && eng.beat >= at) eng.pu_want = pu;
                    engine_block_begin(&eng, ENGINE_SUB); engine_block_reverb(&eng);
                    for (int p = 0; p < NPARTS; p++) engine_block_part(&eng, p);
                    engine_block_mix(&eng, L, R);
                    if (!pass) { memcpy(keep[0][bi], L, sizeof L); memcpy(keep[1][bi], R, sizeof R); }
                    else if (over) for (int i = 0; i < BLKN; i++) { L[i] -= keep[0][bi][i]; R[i] -= keep[1][bi][i]; }
                    if (pass && getenv("PUSCANV") && k == 0 && pu == PU_FREEZE && (bi % 40) == 0) fprintf(stderr, "      b %.2f diff %.6f rvout %.5f frz %.2f pu %d\n", (double)eng.beat, (double)block_rms(L, R, BLKN), (double)eng.rv_out[0][5], (double)eng.frz, eng.pu);
                    if (pu == PU_SWELL ? eng.beat >= at : eng.beat >= floorf(at) + 1.1f) { e += (double)block_rms(L, R, BLKN); nb++; }   /* (from the beat
                                                    * it starts on; SWELL, from the press to the beat it lands on) */
                    bi++;
                }
                float db = 10.0f * log10f((float)(e / (nb ? nb : 1)) + 1e-12f);
                if (pass && getenv("PUSCANV")) fprintf(stderr, "    %s at %.3f: %.1f dB\n", PU_NAME[pu], (double)at, (double)db);
                static float dry; if (!pass) dry = db; else { float d = db - dry; sum += d; if (d < worst) worst = d; if (db < -50.0f || d < -18.0f) silent++; }
                eng.pu_want = 0;
            }
        }
        printf("  %-10s quiet presses %2d of 16   worst %6.1f dB   average %6.1f dB against the groove\n", PU_NAME[pu], silent, (double)worst, (double)(sum / 16.0f));
    }
}

static void run(const char *path, float secs, int punch)
{
    if (getenv("DUMP")) dump();
    engine_init(&eng, &gr);
    eng.playing = 1;
    Wav w; if (!wav_open(&w, path)) return;
    long frames = (long)(secs * SR);
    static float L[BLKN], R[BLKN];
    int evosweep = env_i("EVOSWEEP", 0), actsweep = env_i("ACTSWEEP", 0), ai = env_i("AI", 0);
    static const struct { float beat; int pu; } PLAN[] = {
        { 8.3f, PU_REP8 }, { 10, 0 }, { 12.15f, PU_REP16 }, { 13.4f, PU_REP32 }, { 14, 0 }, { 16, PU_PDOWN }, { 18, 0 }, { 20, PU_PUP }, { 22, 0 },
        { 24, PU_MASH }, { 27, 0 }, { 28.35f, PU_REVERSE }, { 30, 0 }, { 32.2f, PU_GLITCH }, { 35, 0 }, { 36, PU_FREEZE }, { 40, 0 },
        { 43.3f, PU_SWELL }, { 44.2f, 0 }, { 46, PU_CRUSH }, { 48, 0 }, { 50, PU_KILL }, { 52, 0 }, { 54.1f, PU_REP12 }, { 56, 0 },
        { 58, -1 }, { 61, -2 }, { 64, 0 }, { 64, 0 }, { 66.3f, -3 }, { 66.6f, -4 }, { 69.2f, -3 }, { 72.9f, -4 } };   /* (FILL: tapped once, then held three beats) */   /* (pressed off the beat: they keep to the grid) */
    static const struct { float beat; int pu; } DEMO[] = {   /* (PUDEMO: each pressed off the hits, a 32nd past an off
                                                              * sixteenth, held: REPEAT 1/16, GLITCH, REVERSE, FREEZE over
                                                              * two bar lines, SWELL) */
        { 8.875f, PU_REP16 }, { 11.1f, 0 }, { 16.875f, PU_GLITCH }, { 21.1f, 0 }, { 24.875f, PU_REVERSE }, { 29.1f, 0 },
        { 32.875f, PU_FREEZE }, { 41.1f, 0 }, { 44.875f, PU_SWELL }, { 49.1f, 0 } };
    int dnext = 0, pudemo = getenv("PUDEMO") != 0;
    int next = 0; float last_beat = -1;
    clock_t t0 = clock();
    for (long done = 0; done < frames; done += BLKN) {
        float k = (float)done / (float)frames;
        if (evosweep) gr.evolve = (uint8_t)(100.0f * k);
        if (actsweep) gr.activity = (uint8_t)(100.0f * k);
        if (getenv("FXTOUR")) {                    /* every 8 bars the next: the room's four, the compressor smashed, the
                                                    * delay granular then glitching, the pans */
            int sec = (int)(eng.beat / 32.0f) % 8;
            static const uint8_t RT[8] = { RT_ROOM, RT_GATED, RT_REVERSE, RT_FREEZE, RT_ROOM, RT_ROOM, RT_GATED, RT_ROOM };
            gr.room_t = RT[sec]; gr.room = sec < 4 ? 60 : 30;
            gr.comp_t = sec == 4 || sec == 7 ? CT_SMASH : CT_CLEAN; gr.comp = sec == 4 || sec == 7 ? 85 : 40;
            gr.delay_t = sec >= 5 ? DT_GRAIN : DT_CLEAN; gr.glitch = sec == 6 || sec == 7 ? 70 : 0;
            gr.dtime = sec == 6 ? 5 : 2; gr.dfb = sec >= 5 ? 70 : 40;
            gr.pan_t = (uint8_t)(sec % 3); gr.pan = 70;
            for (int p = 1; p < NPARTS; p++) { gr.p[p].s_room = 55; gr.p[p].s_delay = 50; gr.p[p].s_pan = 70; }
        }
        if (getenv("FILLDEMO")) { float b = eng.beat - 16.0f * (float)(int)(eng.beat / 16.0f); eng.fill_want = b >= 7.5f && b < 8.5f; }   /* (FILL tapped
                                                    * in every fourth bar's run-up: the third bar of four the fill) */
        if (getenv("TONESWEEP")) { float u = k * 4.0f; u -= (float)(int)u; u = u < 0.5f ? u * 2.0f : 2.0f - u * 2.0f; for (int p = 0; p < NPARTS; p++) gr.p[p].tone = (uint8_t)(25.0f + 50.0f * u); }   /* (back and forth across 12 o'clock) */
        if (ai) {                                 /* a pretend After Images: GROUND each beat, LIGHT busy */
            float b = eng.beat;
            if ((int)b != (int)last_beat) { engine_hear(&eng, 0, 36 + ((int)b / 4 % 3) * 5, 90); for (int i = 0; i < 3; i++) engine_hear(&eng, 2, 72 + i * 3, 70); }
            last_beat = b;
        }
        if (pudemo) while (dnext < (int)(sizeof DEMO / sizeof DEMO[0]) && eng.beat >= DEMO[dnext].beat) { eng.pu_want = DEMO[dnext].pu; dnext++; }
        if (punch) {
            while (next < (int)(sizeof PLAN / sizeof PLAN[0]) && eng.beat >= PLAN[next].beat) {
                int pu = PLAN[next].pu;
                if (pu == -1) { eng.pu_want = 0; eng.pu_hpw = 1.4f; } else if (pu == -2) { eng.pu_hpw = 0; eng.pu_lpw = 1.5f; }
                else if (pu == -3) eng.fill_want = 1; else if (pu == -4) eng.fill_want = 0;
                else { eng.pu_want = pu; eng.pu_lpw = eng.pu_hpw = 0; }
                next++;
            }
        }
        while (dn_at < dn_n && dnotes[dn_at].at < done + BLKN) { engine_hear(&eng, dnotes[dn_at].ch, dnotes[dn_at].note, dnotes[dn_at].vel); dn_at++; }
        if (getenv("CLEANAT")) eng.clean = (float)done / SR >= (float)atof(getenv("CLEANAT"));   /* (OUTPUT CLEAN from then on) */
        engine_block_begin(&eng, ENGINE_SUB);
        engine_block_reverb(&eng);
        for (int p = 0; p < NPARTS; p++) engine_block_part(&eng, p);
        engine_block_mix(&eng, L, R);
        if (getenv("PULOG")) {   /* (when each punch-in begins and ends, in beats) */
            static int was = 0; int now_pu = eng.pu && !eng.pu_rel ? eng.pu : 0;
            if (now_pu != was) { fprintf(stderr, "  beat %.3f: %s %s\n", (double)((float)eng.tick / (float)TPB), now_pu ? "on " : "off", PU_NAME[now_pu ? now_pu : was]); was = now_pu; }
        }
        float dg = aiw ? 0.62f : 0.9f;
        for (int i = 0; i < BLKN && done + i < frames; i++) {
            float al = 0, ar = 0;
            if (aiw) { int16_t s2[2]; if (fread(s2, 2, 2, aiw) == 2) { al = (float)s2[0] / 32768.0f * 0.85f; ar = (float)s2[1] / 32768.0f * 0.85f; } }
            wav_put(&w, L[i] * dg + al, R[i] * dg + ar);
        }
    }
    float cpu = (float)(clock() - t0) / CLOCKS_PER_SEC;
    wav_close(&w, path);
    fprintf(stderr, "  hits %d %d %d %d; %.2f s to render %.0f s (%.1f%% of real time); EVOLVE's rewrites %u; fills %u\n", eng.events[0], eng.events[1],
            eng.events[2], eng.events[3], (double)cpu, (double)secs, (double)(cpu / secs * 100.0f), eng.mutated, eng.fill_n);
}

static void sweep(int engine, const char *path)
{
    static Hit h; Wav w; if (!wav_open(&w, path)) return;
    float buf[BLK];
    int n = 24;
    for (int k = 0; k < 2 * n; k++) {
        HitP p; memset(&p, 0, sizeof p);
        p.engine = (uint8_t)engine; p.vel = 0.85f; p.bright = 0.5f; p.decay = getenv("DECAY") ? (float)atof(getenv("DECAY")) : 1.0f; p.seed = 77u + (uint32_t)k; p.gseed = 1234u;
        p.note = engine_part(engine) == P_PERC ? 40.0f + (float)((k * 5) % 12) : 60.0f;
        if (getenv("EONLY")) { if (k >= n) break; p.weight = 0.5f; p.entropy = (float)k / (float)(n - 1); }   /* (ENTROPY alone, WEIGHT in the middle) */
        else if (k < n) { p.weight = (float)k / (float)(n - 1); p.entropy = 0.2f; }
        else { p.weight = 0.45f; p.entropy = (float)(k - n) / (float)(n - 1); }
        if (h.on) { hit_choke(&h); while (h.on) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); for (int i = 0; i < BLK; i++) wav_put(&w, buf[i], buf[i]); } }
        hit_start(&h, &p);
        long len = (long)((getenv("SPACE") ? (float)atof(getenv("SPACE")) : 0.38f) * SR);
        for (long d = 0; d < len; d += BLK) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); for (int i = 0; i < BLK; i++) wav_put(&w, buf[i], buf[i]); }
        if (k == n - 1) for (int i = 0; i < (int)(0.6f * SR); i++) { memset(buf, 0, sizeof buf); if (h.on) hit_render(&h, buf, 1); wav_put(&w, buf[0], buf[0]); }
    }
    wav_close(&w, path);
}

static void calibrate(void)
{
    static Hit h; float buf[BLK];
    for (int e = 0; e < NUM_ENGINES; e++) {
        float pk = 0; double ss = 0; long cnt = 0;
        for (int k = 0; k < 9; k++) {
            HitP p; memset(&p, 0, sizeof p);
            p.engine = (uint8_t)e; p.vel = 1.0f; p.bright = 0.5f; p.decay = 1.0f; p.seed = 9u + (uint32_t)k; p.gseed = 5u;
            p.weight = 0.25f + 0.25f * (float)(k % 3); p.entropy = 0.15f + 0.3f * (float)(k / 3); p.note = 60.0f;
            hit_start(&h, &p);
            for (int b = 0; b < 400 && h.on; b++) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); for (int i = 0; i < BLK; i++) { float a = fm_fabsf(buf[i]); if (a > pk) pk = a; if (b < 20) { ss += (double)(buf[i] * buf[i]); cnt++; } } }
        }
        printf("  %-10s peak %.2f  rms(first 15 ms) %.3f\n", ENGINE_NAME[e], (double)pk, __builtin_sqrt(ss / (double)(cnt ? cnt : 1)));
    }
}

/* each engine's loudness across WEIGHT and ENTROPY (the first 60 ms: its punch), as a correction table for drums.c */
extern int drums_level_raw;
static void level_table(void)
{
    static Hit h; float buf[BLK];
    drums_level_raw = 1;
    printf("/* SPDX-License-Identifier: MPL-2.0 */\n/* each engine's level correction across WEIGHT (rows 0, 0.25 .. 1) and ENTROPY (columns 0, 0.0625 .. 1): measured by\n * tools/render levels (each hit's loudest 10 ms against the engine's middle), so a sweep keeps its level */\nstatic const float LEVEL[NUM_ENGINES][5][17] = {\n");
    for (int e = 0; e < NUM_ENGINES; e++) {
        double ref = 0, m[5][17];
        for (int wi = 0; wi < 5; wi++) for (int ei = 0; ei < 17; ei++) {
            double ss = 0; long cnt = 0;   /* (the loudest 10 ms of each hit: how loud a drum sounds) */
            for (int k = 0; k < 16; k++) {
                HitP p; memset(&p, 0, sizeof p);
                p.engine = (uint8_t)e; p.vel = 1.0f; p.bright = 0.5f; p.decay = 1.0f; p.seed = 31u + (uint32_t)k * 7u; p.gseed = 5u;
                p.weight = 0.25f * (float)wi; p.entropy = 0.0625f * (float)ei; p.note = engine_part(e) == P_PERC ? 40.0f + (float)(k * 2) : 60.0f;
                hit_start(&h, &p);
                static float win[4410]; int nw = 0;
                for (int b = 0; b < (int)(0.1f * SR / BLK) && h.on; b++) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); for (int i = 0; i < BLK && nw < 4410; i++) win[nw++] = buf[i]; }
                while (h.on) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); }
                while (nw < 441) win[nw++] = 0;    /* (a hit shorter than the window: the rest silence) */
                double best = 0;
                for (int s0 = 0; s0 + 441 <= nw; s0 += 110) { double q = 0; for (int i = 0; i < 441; i++) q += (double)(win[s0 + i] * win[s0 + i]); if (q > best) best = q; }
                ss += best; cnt += 441;
            }
            m[wi][ei] = __builtin_sqrt(ss / (double)(cnt ? cnt : 1));
        }
        ref = m[2][4] * 0.5 + m[2][8] * 0.5;
        printf("    {");
        for (int wi = 0; wi < 5; wi++) { printf(" {"); for (int ei = 0; ei < 17; ei++) { double c = m[wi][ei] > 1e-6 ? ref / m[wi][ei] : 1.0; if (c > 4.0) c = 4.0; if (c < 0.25) c = 0.25; printf(" %.2ff%s", c, ei < 16 ? "," : ""); } printf(" }%s", wi < 4 ? "," : ""); }
        printf(" },   /* %s */\n", ENGINE_NAME[e]);
    }
    printf("};\n");
}

static double loud10(HitP *p)              /* a hit's loudest 10 ms, in dB, averaged over a few seeds */
{
    static Hit h; static float win[8820]; float buf[BLK]; double sum = 0;
    for (int k = 0; k < 4; k++) {
        p->seed = 101u + (uint32_t)k * 13u; hit_start(&h, p); int nw = 0;
        while (h.on && nw + BLK <= 8820) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); for (int i = 0; i < BLK; i++) win[nw++] = buf[i]; }
        while (h.on) { memset(buf, 0, sizeof buf); hit_render(&h, buf, BLK); }
        while (nw < 441) win[nw++] = 0;
        double best = 0;
        for (int s0 = 0; s0 + 441 <= nw; s0 += 55) { double q = 0; for (int i = 0; i < 441; i++) q += (double)(win[s0 + i] * win[s0 + i]); if (q > best) best = q; }
        sum += best / 441.0;
    }
    return 10.0 * __builtin_log10(sum / 4.0 + 1e-12);
}
static void level_check(void)
{
    printf("each engine's level across WEIGHT (ENTROPY 0.3) and across ENTROPY (WEIGHT 0.5): the spread, loudest to quietest; its middle's level\n");
    for (int e = 0; e < NUM_ENGINES; e++) {
        double wmin = 1e9, wmax = -1e9, emin = 1e9, emax = -1e9;
        for (int i = 0; i <= 10; i++) {
            HitP p; memset(&p, 0, sizeof p); p.engine = (uint8_t)e; p.vel = 1.0f; p.bright = 0.5f; p.decay = 1.0f; p.gseed = 5u;
            p.note = engine_part(e) == P_PERC ? 50.0f : 60.0f;
            p.weight = (float)i / 10.0f; p.entropy = 0.3f; double a = loud10(&p); if (a < wmin) wmin = a; if (a > wmax) wmax = a;
            p.weight = 0.5f; p.entropy = (float)i / 10.0f; double b = loud10(&p); if (b < emin) emin = b; if (b > emax) emax = b;
        }
        HitP m; memset(&m, 0, sizeof m); m.engine = (uint8_t)e; m.vel = 1.0f; m.bright = 0.5f; m.decay = 1.0f; m.gseed = 5u; m.weight = 0.5f; m.entropy = 0.3f; m.note = engine_part(e) == P_PERC ? 50.0f : 60.0f;
        printf("  %-9s WEIGHT %4.1f dB   ENTROPY %4.1f dB   (%5.1f dB)\n", ENGINE_NAME[e], wmax - wmin, emax - emin, loud10(&m));
    }
}

static void bench(void)
{
    static Hit h; static float buf[BLK];
    printf("each engine, ns a sounding sample (this computer; the ratios carry to the FM-1)\n");
    for (int e = 0; e < NUM_ENGINES; e++) {
        clock_t c0 = clock(); long samples = 0;
        for (int rep = 0; rep < 300; rep++) {
            HitP p; memset(&p, 0, sizeof p);
            p.engine = (uint8_t)e; p.vel = 0.8f; p.bright = 0.5f; p.decay = 1.0f; p.seed = (uint32_t)rep + 1u; p.gseed = 3u;
            p.weight = (float)(rep % 5) * 0.25f; p.entropy = (float)(rep % 7) / 6.0f; p.note = 60.0f;
            hit_start(&h, &p);
            while (h.on) { hit_render(&h, buf, BLK); samples += BLK; }
        }
        double ns = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e9 / (double)(samples ? samples : 1);
        printf("  %-10s %6.1f ns  (%.2f s long on average)\n", ENGINE_NAME[e], ns, (double)samples / 300.0 / 44100.0);
    }
}

int main(int argc, char **argv)
{
    if (getenv("FTZ")) _mm_setcsr(_mm_getcsr() | 0x8040u);   /* (tiny numbers flushed to 0: timed against without, any
                                                               * difference is time spent on them, as the FM-1 would) */
    dsp_init(); drums_init();
    if (argc < 2) { fprintf(stderr, "render SEED|default SECONDS OUT.wav | sweep ENGINE OUT.wav | punch SEED OUT.wav | calibrate | bench\n"); return 2; }
    if (!strcmp(argv[1], "calibrate")) { calibrate(); return 0; }
    if (!strcmp(argv[1], "bench")) { bench(); return 0; }
    if (!strcmp(argv[1], "puscan") && argc > 2) { groove_default(&gr); puscan((uint32_t)strtoul(argv[2], 0, 0)); return 0; }
    if (!strcmp(argv[1], "levels")) { level_table(); return 0; }
    if (!strcmp(argv[1], "levelcheck")) { level_check(); return 0; }
    if (!strcmp(argv[1], "sweep") && argc > 3) { int e = engine_by_name(argv[2]); if (e < 0) return 2; sweep(e, argv[3]); return 0; }
    groove_default(&gr);
    if (!strcmp(argv[1], "punch") && argc > 3) {
        gr.genre = (uint8_t)env_i("GENRE", gr.genre); groove_sends = env_i("SENDS", 0); groove_sends = env_i("SENDS", 0);
        if (strcmp(argv[2], "default")) groove_roll(&gr, (uint32_t)strtoul(argv[2], 0, 0), 1);
        apply_env(&gr); describe(&gr);
        run(argv[3], 76.0f * 60.0f / (float)(gr.tempo + 40), 1);
        return 0;
    }
    if (argc < 4) return 2;
    gr.genre = (uint8_t)env_i("GENRE", gr.genre);
    if (strcmp(argv[1], "default")) groove_roll(&gr, (uint32_t)strtoul(argv[1], 0, 0), 1);
    apply_env(&gr);
    if (getenv("DUET")) duet_load();
    describe(&gr);
    run(argv[3], (float)atof(argv[2]), 0);
    return 0;
}
