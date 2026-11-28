/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: the engine. Four parts (KICK, SNARE, PERC, HATS), each a pattern on its own length driving a
 * drum engine; a clock of 96 ticks a beat they all step from (so polymeters stay locked for hours); the macros and
 * MOTION steering what plays; After Images heard (its notes, its clock); the bus's effects and the punch-ins. Plain C,
 * no hardware calls: the FM-1, the host simulator and tools/render. */
#ifndef ER_ENGINE_H
#define ER_ENGINE_H

#include "groove.h"
#include "fx.h"

#define ENGINE_SUB 8                     /* control steps an app block holds (fm1-base's 256 samples) */
#define BLKN (ENGINE_SUB * BLK)
#define NV 5                             /* voices a part: four sounding, one more fading out (never cut) */
#define EVQ 24                           /* a part's hits waiting for their moment */
#define UI_Q 64

typedef struct { int32_t tick; float sub; HitP hp; float pan; uint8_t alt; } Ev;
typedef struct { uint8_t part, vel, alt; uint8_t dur; } UiHit;   /* the screen's: a hit (dur: tenths of a second it rings) */

typedef struct {
    Pattern pat; uint32_t sig;           /* the pattern, and the groove's settings it was written from */
    Hit v[NV]; float vl[NV], vr[NV]; uint8_t valt[NV];   /* the voices: each its pan's gains (fixed for the hit) */
    Ev q[EVQ]; int nq;
    int16_t at[EVQ]; uint8_t nat;        /* this block's: the queue's first nat start at these samples */
    float bus[2][BLKN];                  /* this block's sound, after its TONE and level */
    uint32_t step;                       /* the next of its steps to deal */
    Svf lp[2], hp[2], floor[2]; float tone_now; int tone_cnt;
    float gain, present, present_t; int drift_bars;
    Drift fw, fe, fb;                    /* FLOW: its colours wandering */
    float fw_v, fe_v, fb_v;
    float meter;
    int remap; int remap_beat;           /* EVOLVE past 85: this beat played from another (reality) */
    float tone_rnd;                      /* a black key's: TONE's random offset, held till the next hit */
} Part;

/* the room's memory: the gated room's pre-delay, the reversed room's two chunks */
#define ROOM_BUF 8192
#define DLY_BUF 16384                    /* the delay: each side's line (0.37 s), counted in the sample rate's samples */
#define DL_DIV 4                         /* (the delay at a quarter of the rate, 11 kHz (v0.14; half before): its echoes
                                          * dulled below its top anyway; a quarter of the work and memory) */
#define DLY_H (DLY_BUF / DL_DIV)
#define PUNCH_BUF 16384                  /* the punch-ins' memory: the last 0.37 s, stereo (an eighth down to 82 BPM) */

enum { PU_NONE, PU_LPF, PU_FILL, PU_REP8, PU_REP16, PU_REP32, PU_GLITCH, PU_PDOWN, PU_PUP, PU_MASH, PU_REVERSE,
       PU_FREEZE, PU_SWELL, PU_CRUSH, PU_KILL, PU_REP12, PU_HPF, NUM_PU };
extern const char *const PU_NAME[NUM_PU];

typedef struct Engine Engine;
struct Engine {
    Groove *g;                           /* the settings it follows (the app's); EVOLVE's rewrites count up its muts */
    Part P[NPARTS];
    /* the clock: ticks (96 a beat) and the part of one */
    uint32_t tick; float frac, rate;
    float bpm, ext_bpm, beat;            /* beat: for the screen and the clock's lock (ticks / 96, within 4096 beats) */
    volatile float beat_adj;             /* following a clock: how far our beat is off the leader's, taken up gently */
    volatile int sync_req;               /* a Start: the top of the bar, every part from its first step */
    int playing;
    uint32_t clk_tick;                   /* MIDI clock out: the tick of the next (every 4) */
    Rng rng;
    /* After Images, heard: its notes (MIDI in, layers on four channels) */
    int ai_ground, ai_light; int ai_ground_new, ai_light_new; float ai_act; uint32_t ai_notes;
    /* the bus */
    float sc;                            /* the kick's level: the sidechain */
    Reverb rv; float rv_in[2][BLKN], rv_out[2][BLKN], rv_h[4][BLKN / 2], rv_last[2], rv_aa[2], rv_sm[2];   /* (rv_h: its half-rate in and out) */
    int rv_fed, rv_quiet, dl_quiet; float rv_cost, cmp_gt;   /* (the reverb: fed this block; blocks it has been silent; its time) */
    int16_t room_buf[ROOM_BUF]; int room_w; float gate, gate_hold, gate_s[BLKN], duck_s[BLKN]; int rchunk; float room_dip, room_t_now;
    int16_t dl[2][DLY_H]; int dw, dl_k; float dl_acc, dl_aa, dl_ho[2], dl_oc[2], dl_od[2], dl_sm[2]; float dt_a, dt_b, dxf, dfb_now, dly_dip; int dly_t_now; uint32_t dl_step;
    float dg_ph, dg_rate, dg_rate_t; OnePole dlp[2], plp1[2], frz_hp[2];
    float cmp_lvl, cmp_g, smash;         /* COMP: its level, its gain, CLEAN to TAPE SMASH (glided) */
    float pan_ph, pan_spin, pan_now[NPARTS];
    float lev_room, lev_dly, lev_comp, lev_pan;   /* the bus amounts, glided */
    /* the punch-ins */
    /* the balance (as After Images'): each part's level held to a pink-noise balance whatever MOOD (pg: their gains,
     * glided), the mix's tone tilted slowly towards a steady top-to-bottom, a leveller over seconds */
    float pg[NPARTS], tl_lo, tl_hi, tl_elo, tl_ehi, tl_db, tl_g, tl_s[2], lev_ss, lev_g;
    volatile int clean; float clean_g;      /* USB AUDIO's OUTPUT CLEAN: FX's effects (ROOM, DELAY, COMP, PAN, DUCK) out, glided */
    volatile int pu_want; volatile float pu_press;
    int pu_pend, pu_tap, pu_relq, pu_once; uint32_t pu_at, pu_rel_at;   /* quantised: the punch-in waiting for its beat, when; its end's sixteenth */   /* the key held (PU_*), its pressure 0..1 (from the UI) */
    int pu, pu_rel; float pu_mix; int16_t pb[2][PUNCH_BUF]; int pw, ps0, pu_frozen;
    float rpos, rlen, rnext; float lp_c, hp_c; Svf plp[2], php[2]; int pf_cnt;
    int pu_cap, pu_fwd, rec_since;       /* capturing forward: how much so far; the memory's clean run since it last froze */
    volatile int fill_want; uint32_t mfill_last;   /* the FILL key held (or held by OCT); the last beat it was looked at */
    Pattern fpat[NPARTS]; uint32_t fill_from, fill_to, fill_n, afill_bar;   /* a fill: the groove's variation, and when it plays */
    float m_d, m_x, m_tgt, m_g, m_gt, m_wow, m_flt, m_lp[2]; int m_cnt;   /* TAPE MASH: the tape's delay, a catch, a dropout, wow, flutter */
    Shifter shf[2]; float crush_h, crush_v[2]; float kill_g; float frz; int swell_left, swell_len;
    /* MIDI out */
    void (*midi)(uint32_t pkt);
    uint8_t midi_on, midi_ch, midi_clock;
    /* costs and the load guard */
    uint32_t (*clock)(void);
    float cost[NPARTS + 1];
    float gov; volatile uint32_t shed; uint32_t shed_seen;
    /* the screen's */
    UiHit uiq[UI_Q]; volatile uint32_t uiq_head;
    float meter[NPARTS]; int events[NPARTS];
    uint32_t mutated;                    /* EVOLVE's rewrites since the start (the app: the groove changed) */
    /* the keys' hits: a ring from the UI loop to the render (part, velocity, alt) */
    volatile uint32_t kq[16]; volatile uint32_t kq_w, kq_r;
    volatile float pu_lpw, pu_hpw;       /* the filter keys: 0 let go, else held, 1 + how hard (0..1) */
    float lp_d, hp_d;                    /* their depth (kept while they fade out) */
    uint32_t kick_ticks[8]; int kick_n; /* the kick's last hits (LINK: the hats keep out of their way) */
    uint32_t kick_bar, kick_last; int kick_in_bar;   /* the kick held firm: its hits this bar, its last step */
    uint32_t last_bar; int nblk;
    uint8_t off_note[NPARTS]; int off_left[NPARTS];   /* MIDI out: each part's note, blocks until its note-off */
    float frz_feed, sw_dip, frz_prev; int frz_len, frz_left, sw_off, frz_live, frz_hit, pu_quiet, pu_tries;
    float pn_w[NPARTS], pn_c0[NPARTS], pn_c1[NPARTS], pn_dw[NPARTS], pn_d0[NPARTS], pn_d1[NPARTS];   /* (PAN's gains, sliding) */
    float pk_run, frz_ref, frz_go; int fill_add;   /* (fill_add: this fill adds (1) or takes away (0)) */ uint32_t frz_bar;   /* (FREEZE held: caught anew each bar, the old wash let go meanwhile) */                                /* (the groove's recent loudest: what 'nothing in it' is measured against) */
    /* FX's PUNCH-INS page (the app's settings): TAPE MASH's amount, CRUSH's steps a unit and rate, REVERSE's part (-1 all;
     * rev_now: as it was when REVERSE started) */
    float mash_amt, crush_q, crush_hz; int rev_part, rev_now;
    /* REVERSE on one part: its own memory (that part alone, as it goes into the mix) and its eighths, in step with the
     * whole kit's (ch_*) */
    int16_t prb[2][PUNCH_BUF]; int prw, pr_on, pr_act, pr_pact, pr_s0, pr_ps0, pr_len, pr_plen, pr_xf; float pr_ph, pr_pph, pr_step, pr_mix;
    int ch_act, ch_pact, ch_s0, ch_ps0, ch_len, ch_plen, ch_xf; float ch_ph, ch_pph, ch_step;   /* (GLITCH, REVERSE: the
                                                  * live chopper, this step and the one it crossfades from) */ float frz_got, frz_lv;   /* (FREEZE's catch: the last half beat from the
                                                  * memory; SWELL's: how far back its hit is) */
    /* the black keys: randomisers, a new value each hit (sample and hold): which of TONE, WEIGHT, ENTROPY (bits 0-2)
     * each part has held, and how far (0..1) */
    volatile uint8_t rnd_bits[NPARTS]; volatile float rnd_depth[NPARTS];
};

void engine_init(Engine *e, Groove *g);
void engine_block_begin(Engine *e, int nsub);   /* the clock, the patterns, the hits (one core) */
void engine_block_part(Engine *e, int part);    /* a part's voices (either core) */
void engine_block_reverb(Engine *e);      /* the reverb on the last block's sends (the second core's, beside parts) */
void engine_block_mix(Engine *e, float *outL, float *outR);   /* the bus and the punch-ins (one core, after both) */
void engine_all_off(Engine *e);                 /* every voice faded out (a stop, a new groove) */
void engine_overload(Engine *e);                /* from the audio interrupt: that block nearly overran */
void engine_hear(Engine *e, int ch, int note, int vel);   /* a note from After Images (its layer's channel, 0..3) */
void engine_play(Engine *e, int part, float vel, int alt);   /* a key: a part's hit now */
float engine_part_tps(const Engine *e, int part);

#endif
