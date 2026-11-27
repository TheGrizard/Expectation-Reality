/* SPDX-License-Identifier: MPL-2.0 */
/* Expectation Reality: generative percussion for the M-VAVE FM-1, a partner to After Images. Four parts (KICK, SNARE,
 * PERC, HATS), each a pattern on its own length driving a drum engine; REC deals a new groove. Part of fm1-base's
 * unity build (device/unity.c, host/plat_host.c): gfx.c and announce.c are in scope. After Images' app, its pages,
 * its saving and its clock, made over for drums.
 *
 * Controls
 *   K1-K4              at HOME the parts' levels; HOME held + K1-K4 the macros (MOOD, ACTIVITY, EVOLVE, DRIFT); on
 *                      the other pages, their four settings
 *   HOME EDIT SEQ ENV FX LFO GLO ARP   the pages: HOME, SOUND, GROOVE, MIX, SENDS, MOTION, MACROS, MIDI; pressed again
 *                      FX: EFFECTS, TYPES, DELAY; GLO: SCALE, SCREEN; ARP: CHANNELS, USB AUDIO
 *   HOME tapped at HOME   the processor's load, and whether saving works
 *   SELECT, SEL        the part the SOUND, GROOVE, MIX and SENDS pages show
 *   PRESET             the chosen part's engine;   ALGORITHM   your saved grooves
 *   REC                a new groove (held: in the same key and tempo); SEL held + REC: the chosen part anew
 *   SAVE               keep the groove; PLAY start / stop;  HOME held + OCT- / OCT+  back and forward through the grooves REC
 *                      replaced; OCT- / OCT+ held as a key is pressed: the key held (to the loop's end, for good); OCT+ alone:
 *                      every hold let go
 *   white keys         punch-ins: the bottom a high-pass, the top a low-pass, between them repeats, pitch, tape stop...
 *   black keys         randomisers: each part's colours thrown at every hit while held (the top one: every part's) */
#include "fastmath.h"
#include "dsp/engine.h"

static Engine eng PLAT_POOL;

/* ------------------------------------------------------------------ settings */
#define SET_MAGIC 0x45524731u            /* "ERG1" */
#define SET_VER 3                        /* the settings' version (2: MOOD turned round; 3: SCREEN's STYLE): each update that adds to them moves this on, and
                                          * settings_load brings an older one's forward (nothing saved is ever lost) */
#define NSAVED 16
typedef struct {
    uint32_t magic;
    uint8_t ver, slot, nsaved, playing;  /* slot: the saved groove playing (255: none) */
    uint8_t sync, midi_on, listen, cc_in; /* CLOCK INT / IN / OUT; NOTES OUT; LISTEN (After Images' notes heard); CC IN */
    uint8_t listen_ch, out_ch, palette, peg_n;   /* LISTEN CH (After Images' four layers from here); OUT CH; the screen */
    uint8_t fade, usb_full, usb_lvl, bright;     /* SCREEN's FADE; USB AUDIO's CONTROL (bit 0: FIXED) and OUTPUT (bit 1:
                                                  * CLEAN, the kit without FX's effects), and GAIN; SCREEN's BRIGHT */
    Groove cur;
    Groove saved[NSAVED];
    uint8_t style, rsv[3];               /* SCREEN's STYLE: 0 KNOCKOUT (a hit knocks its square out), 1 TONES (the squares in
                                          * greys, a hit flooding one with its colour, fading back to its grey) */
} settings_t;
#define SET_V2_SIZE ((int)__builtin_offsetof(settings_t, style))   /* (versions 1 and 2: all up to here) */
static settings_t set;
static uint8_t save_over;
static uint32_t xr_seen, xr_saves, save_end;
#define NPAL 11
#define NPEG_NS 6
static const uint8_t PEG_NS[NPEG_NS] = { 8, 10, 12, 15, 20, 24 };   /* COUNT: squares across */
static uint8_t set_dirty;
static int store_ok = 1;
static uint32_t autosave_at;
#define AUTOSAVE_MS 8000u
enum { SYNC_INT, SYNC_IN, SYNC_OUT, NSYNC };
static const char *const SYNC_NAME[NSYNC] = { "INT", "IN", "OUT" };
#define CLK_IN (set.sync == SYNC_IN)
#define CLK_OUT (set.sync == SYNC_OUT)

static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC; set.ver = SET_VER; set.slot = 255;
    set.midi_on = 1; set.listen = 1; set.cc_in = 1; set.sync = SYNC_INT;
    set.listen_ch = 0; set.out_ch = 9; set.palette = 9; set.peg_n = 0;   /* (ACID first) */
    set.fade = 50; set.usb_full = 0; set.usb_lvl = 80; set.bright = 80;
    groove_default(&set.cur);
}

static int settings_ok(void)
{
    if (set.magic != SET_MAGIC || set.ver < 1 || set.ver > SET_VER || set.nsaved > NSAVED || (set.slot != 255 && set.slot >= set.nsaved)) return 0;
    if (set.sync >= NSYNC || set.midi_on > 1 || set.listen > 1 || set.cc_in > 1 || set.listen_ch > 12 || set.out_ch > 15) return 0;
    if (set.palette >= NPAL || set.peg_n >= NPEG_NS || set.fade > 100 || set.usb_full > 3 || set.usb_lvl > 100 || set.bright > 100 || set.style > 1) return 0;
    if (!groove_ok(&set.cur)) return 0;
    for (int i = 0; i < set.nsaved; i++) if (!groove_ok(&set.saved[i])) return 0;
    return 1;
}

/* the settings kept: a version's own, or an older one brought forward (each later version adds its case here, never
 * dropping one), or nothing usable (another firmware's, a first start): the defaults */
static void settings_load(void)
{
    static uint8_t raw[sizeof(settings_t)];
    int n = plat_store_load(OBJ_SETTINGS, raw, sizeof raw);
    uint32_t magic = n >= 4 ? *(const uint32_t *)raw : 0;
    int ver = n >= 5 ? raw[4] : 0;
    if (magic == SET_MAGIC && ver == SET_VER && n == (int)sizeof set) memcpy(&set, raw, sizeof set);
    else if (magic == SET_MAGIC && (ver == 1 || ver == 2) && n == SET_V2_SIZE) {   /* 1 (v0.1), 2 (v0.2): all but STYLE;
                                                  * in 1, MOOD ran the other way */
        memset(&set, 0, sizeof set); memcpy(&set, raw, (unsigned)SET_V2_SIZE); set.ver = SET_VER; set.style = 0;
        if (ver == 1) {
            if (set.cur.mood <= 100) set.cur.mood = (uint8_t)(100 - set.cur.mood);
            for (int i = 0; i < set.nsaved && i < NSAVED; i++) if (set.saved[i].mood <= 100) set.saved[i].mood = (uint8_t)(100 - set.saved[i].mood);
        }
        if (settings_ok()) store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    }
    else n = -1;
    if (n < 0 || !settings_ok()) {
        settings_defaults();
        store_ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;
    }
    {   /* power-on: a part turned right down was a mute of the moment; it comes back */
        static const uint8_t LEVEL_DEF[NPARTS] = { 85, 72, 62, 58 };
        for (int p = 0; p < NPARTS; p++) if (set.cur.p[p].level < 10) set.cur.p[p].level = LEVEL_DEF[p];
    }
    set.playing = 1;
}

/* ------------------------------------------------------------------ history: the grooves REC replaced (OCT- / OCT+) */
#define NHIST 12
static Groove hist[NHIST];
static int hist_n, hist_at;
static void hist_push(void)
{
    if (hist_at < hist_n) hist_n = hist_at;
    if (hist_n == NHIST) { for (int i = 1; i < NHIST; i++) hist[i - 1] = hist[i]; hist_n--; }
    hist[hist_n++] = set.cur;
    hist_at = hist_n;
}

/* ------------------------------------------------------------------ sound */
#define OUT_GAIN 1.0f                    /* (unity: the volume knob, or USB GAIN at FIXED, at 100) */
#define LIM_CEIL 0.891f                  /* the output's ceiling: -1 dBFS */
static float lim_g = 1.0f;
static volatile int muted;
static float gain_now;
static volatile float out_lvl;
static volatile int save_hold;
static int two_cores;
static uint8_t on2_mask = 0x6u;

static inline float soft(float x)              /* (a last safety, past the limiter: only ever touched above -0.5 dBFS) */
{
    float a = fm_fabsf(x);
    if (a < 0.944f) return x;
    float y = 0.944f + 0.056f * fm_tanhf((a - 0.944f) / 0.056f);
    return x < 0 ? -y : y;
}

static void midi_send(uint32_t pkt) { plat_midi_send(pkt, TO_USB); }
static void cc_set(uint8_t *v, uint32_t x) { *v = (uint8_t)(x * 100u / 127u); }

/* MIDI in. After Images' notes, its four layers on LISTEN CH's four channels: heard (busier, the kick leaning into
 * GROUND, LIGHT answered). CC 20-23 the macros, 24-27 the bus's effects, 28-31 MOTION. Clock: its tempo followed, its
 * beat and bar locked to, start and stop obeyed (CLOCK IN). As After Images' own */
static volatile float ext_bpm; static volatile uint32_t ext_at, ext_last;
static uint32_t clk_n;
static int shown_bpm(void) { return eng.ext_bpm > 20.0f ? (int)(eng.ext_bpm + 0.5f) : 40 + set.cur.tempo; }

static void midi_in(void)
{
    uint32_t pkt; int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu, ch = st & 15u, ty = st & 0xF0u;
        if (st == 0xF8u) {
            static uint32_t count, bt[9]; static int bn, odd;
            uint32_t now = plat_ms();
            if (now - ext_last > 500u) { count = 0; bn = 0; }
            ext_last = now;
            if (count++ % 24u == 0) {
                if (bn == 9) { for (int i = 0; i < 8; i++) bt[i] = bt[i + 1]; bn = 8; }
                bt[bn++] = now;
                if (CLK_IN && bn >= 2) {
                    uint32_t one = bt[bn - 1] - bt[bn - 2], all = bt[bn - 1] - bt[0];
                    float b1 = one ? 60000.0f / (float)one : 0.0f, b = 60000.0f * (float)(bn - 1) / (float)(all ? all : 1u);
                    if (ext_bpm > 0 && b1 > 0 && fm_fabsf(b1 - ext_bpm) > 0.04f * ext_bpm && bn > 2) {
                        if (++odd >= 2) { bt[0] = bt[bn - 2]; bt[1] = bt[bn - 1]; bn = 2; b = b1; odd = 0; }
                    } else odd = 0;
                    if (b >= 20.0f && b <= 300.0f) ext_bpm = b;
                }
            }
            if (CLK_IN && set.playing && ++clk_n % 24u == 0) {   /* locked to the leader's beat and bar, not only its tempo */
                float err = (float)(clk_n / 24u) - eng.beat;
                err -= 4.0f * fm_floorf(err * 0.25f + 0.5f);
                eng.beat_adj = err;
            }
            if (CLK_IN) ext_at = now;
        } else if ((st == 0xFAu || st == 0xFBu) && CLK_IN) {
            eng.playing = set.playing = 1;
            if (st == 0xFAu) { eng.sync_req = 1; clk_n = 0; }
            else clk_n = (uint32_t)(eng.beat + 0.5f) * 24u;
        }
        else if (st == 0xFCu && CLK_IN) { eng.playing = set.playing = 0; engine_all_off(&eng); }
        else if (ty == 0x90u || ty == 0x80u) {
            if (set.listen && ch >= set.listen_ch && ch < set.listen_ch + 4u) engine_hear(&eng, (int)(ch - set.listen_ch), (int)d1, ty == 0x90u ? (int)d2 : 0);
        } else if (ty == 0xB0u && set.cc_in) {
            uint8_t *g[12] = { &set.cur.mood, &set.cur.activity, &set.cur.evolve, &set.cur.drift, &set.cur.room, &set.cur.dtime,
                               &set.cur.pan, &set.cur.comp,   /* (as the EFFECTS page's knobs) */ &set.cur.link, &set.cur.warp, &set.cur.flow, &set.cur.chaos };
            if (d1 == 25u) set.cur.dtime = (uint8_t)(d2 * (uint32_t)NUM_DTIMES / 128u);   /* (DELAY TIME: its steps) */
            else if (d1 >= 20u && d1 < 32u) cc_set(g[d1 - 20u], d2);
        }
    }
    if (ext_bpm > 0 && plat_ms() - ext_at > 1000u) ext_bpm = 0;
    eng.ext_bpm = CLK_IN ? ext_bpm : 0.0f;
}

static void parts_job(uint32_t mask) { for (int p = 0; p < NPARTS; p++) if (mask >> p & 1u) engine_block_part(&eng, p); }
static void core2_job(uint32_t mask) { parts_job(mask); engine_block_reverb(&eng); }   /* (the second core's: its parts, the reverb) */
static uint8_t balance(void)             /* the split that leaves the busier core least to do */
{
    uint8_t best = 0x6u; float best_t = 1e30f, total = 0;
    for (int p = 0; p < NPARTS; p++) total += eng.cost[p];
    total += eng.rv_cost;
    if (total <= 0) return best;
    for (uint8_t m = 0; m < 15; m++) {
        float t2 = eng.rv_cost;                  /* (the reverb: always the second core's) */
        for (int p = 0; p < NPARTS; p++) if (m >> p & 1u) t2 += eng.cost[p];
        float t1 = total - t2, t = t2 > t1 ? t2 : t1;
        if (t < best_t - 1.0f) { best_t = t; best = m; }
    }
    return best;
}
static volatile uint32_t c2_late;

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float L[BLKN], R[BLKN];
    /* the volume knob: its bottom few percent silent (the knob rarely reads a true 0 there, and the drums' hits came
     * through faintly), the rest as it was */
    float mq = (float)master_q12 < 6.0f ? 0.0f : ((float)master_q12 - 6.0f) / (4096.0f - 6.0f);
    float g = muted || save_hold ? 0.0f : mq * OUT_GAIN;
    static uint32_t since_boot;
    if (since_boot < (uint32_t)(1.1f * SR)) { since_boot += n; g *= clamp01(((float)since_boot - 0.8f * SR) / (0.3f * SR)); }
    if (set.usb_full & 1) { float u = (float)set.usb_lvl * 0.01f; g *= u * u; }
    int nsub = (int)(n / BLK);
    if (nsub > ENGINE_SUB) nsub = ENGINE_SUB;
    midi_in();
    eng.midi_on = set.midi_on; eng.midi_ch = set.out_ch; eng.midi_clock = CLK_OUT;
    {   static int was_out = -1; int out = CLK_OUT;
        if (was_out >= 0 && out != was_out && set.playing) { if (out) eng.sync_req = 1; plat_midi_send(0x0Fu | (out ? 0xFAu : 0xFCu) << 8, TO_USB); }
        was_out = out;
    }
    engine_block_begin(&eng, nsub);
    uint8_t mask = two_cores && plat_cpu2_on() ? on2_mask : 0;
    int sent = two_cores && plat_cpu2_on() && plat_cpu2_run(core2_job, mask);
    parts_job(sent ? (uint32_t)(~mask & 0xFu) : 0xFu);
    if (sent && plat_cpu2_wait() != 0) { c2_late++; core2_job(mask); }
    if (!sent) engine_block_reverb(&eng);
    engine_block_mix(&eng, L, R);
    if (two_cores) on2_mask = balance();
    uint32_t frames = n < (uint32_t)(nsub * BLK) ? n : (uint32_t)(nsub * BLK);
    for (uint32_t i = 0; i < frames; i++) {
        gain_now += (g - gain_now) * 0.002f;
        out_lvl += (fm_fabsf(L[i]) + fm_fabsf(R[i]) - out_lvl) * 0.0005f;
        /* the limiter: the two sides together, held under -1 dBFS (a peak over it turned down at once, let back up over
         * ~80 ms), so neither the speaker, the line out nor a recorder is ever driven past full scale */
        float l = L[i] * gain_now, r = R[i] * gain_now, pk = fm_maxf(fm_fabsf(l), fm_fabsf(r));
        if (pk * lim_g > LIM_CEIL) lim_g = LIM_CEIL / pk; else lim_g += (1.0f - lim_g) * 0.00028f;
        lr[2 * i] = (int32_t)(soft(l * lim_g) * 8388000.0f);
        lr[2 * i + 1] = (int32_t)(soft(r * lim_g) * 8388000.0f);
    }
}

void app_panic(void) { muted = 1; gain_now = 0; }

void app_init(void)
{
    dsp_init(); drums_init();
    settings_load();
    set.cur.tempo = 100 - 40;                    /* (100 BPM at every power-on; from then on only you change it) */
    save_end = plat_ms() | 1u;
    engine_init(&eng, &set.cur);
    eng.clock = plat_ticks24;
    eng.midi = midi_send;
    eng.playing = set.playing;
    two_cores = plat_cpu2_set(1) == 0;
}

/* ------------------------------------------------------------------ text */
static void text_cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
static int strlen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void int_text(int32_t v, char *b)
{
    char t[12]; int n = 0; uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do t[n++] = (char)('0' + u % 10u); while ((u /= 10u) && n < 11);
    if (v < 0) *b++ = '-';
    while (n) *b++ = t[--n];
    *b = 0;
}
static const char *const NOTE_NAME[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* ------------------------------------------------------------------ the pages */
enum { PG_HOME, PG_SOUND, PG_SEQ, PG_MIX, PG_SENDS, PG_FXBUS, PG_TYPES, PG_DELAY, PG_MOTION, PG_MACROS, PG_SCALE, PG_SCREEN,
       PG_MIDI, PG_CHAN, PG_USB, NPAGES };
static const char *const PAGE_NAME[NPAGES] = { "HOME", "SOUND", "GROOVE", "MIX", "SENDS", "EFFECTS", "TYPES", "DELAY", "MOTION",
                                                "MACROS", "SCALE", "SCREEN", "MIDI", "CHANNELS", "USB AUDIO" };
static const uint8_t PAGE_BTN[NPAGES] = { B_HOME, B_EDIT, B_SEQ, B_ENV, B_FX, B_FX, B_FX, B_FX, B_LFO, B_GLO, B_GLO, B_GLO, B_ARP, B_ARP, B_ARP };
enum { K_NONE, K_PCT, K_ENGINE, K_GEN, K_LEN, K_RATE, K_PITCH, K_PAN, K_TONE, K_SEND, K_RT, K_CT, K_DT, K_PT, K_DTIME,
       K_KEY, K_SCALE, K_TEMPO, K_ONOFF, K_SYNC, K_LCH, K_CH, K_PEGN, K_REC, K_ULVL, K_DECAY, K_ROUTE, K_STYLE };
typedef struct { const char *name; uint8_t kind, where; uint16_t off; } Knob;   /* where: 0 the groove, 1 the part, 2 the settings */
#define GO(f) (uint16_t)__builtin_offsetof(Groove, f)
#define PO(f) (uint16_t)__builtin_offsetof(PartP, f)
#define SO(f) (uint16_t)__builtin_offsetof(settings_t, f)
static const Knob PAGES[NPAGES][4] = {
    { { "MOOD", K_PCT, 0, GO(mood) }, { "ACTIVITY", K_PCT, 0, GO(activity) }, { "EVOLVE", K_PCT, 0, GO(evolve) }, { "DRIFT", K_PCT, 0, GO(drift) } },
    { { "ENGINE", K_ENGINE, 1, PO(engine) }, { "TONE", K_TONE, 1, PO(tone) }, { "WEIGHT", K_PCT, 1, PO(weight) }, { "ENTROPY", K_PCT, 1, PO(entropy) } },
    { { "TYPE", K_GEN, 1, PO(gen) }, { "LENGTH", K_LEN, 1, PO(len) }, { "DENSITY", K_PCT, 1, PO(density) }, { "RATE", K_RATE, 1, PO(rate) } },
    { { "LEVEL", K_PCT, 1, PO(level) }, { "DECAY", K_DECAY, 1, PO(decay) }, { "PITCH", K_PITCH, 1, PO(pitch) }, { "PAN", K_PAN, 1, PO(pan) } },
    /* the three effects pages in the same order: REVERB, DELAY, PAN, COMP */
    { { "REVERB", K_SEND, 1, PO(s_room) }, { "DELAY", K_SEND, 1, PO(s_delay) }, { "PAN", K_SEND, 1, PO(s_pan) }, { "COMP", K_SEND, 1, PO(s_comp) } },
    { { "REVERB SIZE", K_PCT, 0, GO(room) }, { "DELAY TIME", K_DTIME, 0, GO(dtime) }, { "PAN AMOUNT", K_PCT, 0, GO(pan) }, { "COMP AMOUNT", K_PCT, 0, GO(comp) } },
    { { "REVERB TYPE", K_RT, 0, GO(room_t) }, { "DELAY TYPE", K_DT, 0, GO(delay_t) }, { "PAN TYPE", K_PT, 0, GO(pan_t) }, { "COMP TYPE", K_CT, 0, GO(comp_t) } },
    { { "FEEDBACK", K_PCT, 0, GO(dfb) }, { "GLITCH", K_PCT, 0, GO(glitch) }, { "", K_NONE, 0, 0 }, { "", K_NONE, 0, 0 } },
    { { "LINK", K_PCT, 0, GO(link) }, { "WARP", K_PCT, 0, GO(warp) }, { "FLOW", K_PCT, 0, GO(flow) }, { "CHAOS", K_PCT, 0, GO(chaos) } },
    { { "MOOD", K_PCT, 0, GO(mood) }, { "ACTIVITY", K_PCT, 0, GO(activity) }, { "EVOLVE", K_PCT, 0, GO(evolve) }, { "DRIFT", K_PCT, 0, GO(drift) } },
    { { "KEY", K_KEY, 0, GO(key) }, { "SCALE", K_SCALE, 0, GO(scale) }, { "SWING", K_PCT, 0, GO(swing) }, { "TEMPO", K_TEMPO, 0, GO(tempo) } },
    { { "COUNT", K_PEGN, 2, SO(peg_n) }, { "FADE", K_PCT, 2, SO(fade) }, { "BRIGHT", K_PCT, 2, SO(bright) }, { "STYLE", K_STYLE, 2, SO(style) } },
    { { "CLOCK", K_SYNC, 2, SO(sync) }, { "NOTES OUT", K_ONOFF, 2, SO(midi_on) }, { "LISTEN", K_ONOFF, 2, SO(listen) }, { "CC IN", K_ONOFF, 2, SO(cc_in) } },
    { { "LISTEN CH", K_LCH, 2, SO(listen_ch) }, { "OUT CH", K_CH, 2, SO(out_ch) }, { "FOLLOW", K_ONOFF, 0, GO(follow) }, { "", K_NONE, 2, 0 } },
    { { "OUTPUT", K_ROUTE, 2, SO(usb_full) }, { "", K_NONE, 2, 0 }, { "CONTROL", K_REC, 2, SO(usb_full) }, { "GAIN", K_ULVL, 2, SO(usb_lvl) } },
};
static int page_has_part(int pg) { return pg == PG_SOUND || pg == PG_SEQ || pg == PG_MIX || pg == PG_SENDS; }

#define C_TXT RGB(232, 232, 232)
#define C_SUB RGB(150, 150, 160)
#define C_RULE RGB(40, 40, 48)
#define C_WELL RGB(30, 30, 36)
#define C_RED RGB(255, 90, 78)
/* each part its colour, everywhere: the palettes REC deals, KICK SNARE PERC HATS, four clearly different hues each
 * (After Images' nine, and ACID and ULTRAVIOLET) */
static const uint16_t LPAL[NPAL][NPARTS] = {
    { RGB(255, 72, 64), RGB(70, 140, 255), RGB(70, 230, 110), RGB(240, 240, 240) },    /* classic */
    { RGB(225, 45, 75), RGB(150, 85, 255), RGB(255, 150, 50), RGB(255, 235, 165) },    /* sunset */
    { RGB(45, 95, 235), RGB(255, 110, 90), RGB(60, 230, 210), RGB(255, 225, 240) },    /* ocean */
    { RGB(150, 70, 255), RGB(255, 60, 170), RGB(60, 215, 255), RGB(205, 255, 90) },    /* neon */
    { RGB(225, 45, 30), RGB(40, 190, 180), RGB(255, 180, 45), RGB(255, 240, 205) },    /* ember */
    { RGB(95, 180, 50), RGB(185, 70, 175), RGB(245, 205, 60), RGB(215, 255, 235) },    /* forest */
    { RGB(90, 135, 215), RGB(215, 100, 55), RGB(150, 205, 115), RGB(245, 245, 245) },  /* slate */
    { RGB(30, 175, 95), RGB(235, 60, 125), RGB(95, 180, 255), RGB(250, 250, 165) },    /* jade */
    { RGB(215, 120, 20), RGB(100, 85, 230), RGB(205, 230, 60), RGB(255, 222, 205) },   /* amber */
    { RGB(215, 255, 0), RGB(255, 0, 168), RGB(0, 229, 255), RGB(255, 255, 255) },      /* acid */
    { RGB(138, 43, 255), RGB(255, 0, 247), RGB(0, 240, 255), RGB(244, 232, 255) },     /* ultraviolet */
};
static uint16_t PART_C[NPARTS] = { RGB(215, 255, 0), RGB(255, 0, 168), RGB(0, 229, 255), RGB(255, 255, 255) };
static void palette_apply(void) { for (int p = 0; p < NPARTS; p++) PART_C[p] = LPAL[set.palette % NPAL][p]; }

static struct {
    uint32_t btn, keys;
    int page, sel;
    int last_knob; uint32_t knob_at;
    char pop_t[28], pop_s[28], pop_3[40]; uint16_t pop_c; uint32_t pop_until;
    uint32_t list_until;
    uint32_t rec_t; int rec_used, sel_used, home_used;
    uint32_t panel_until; int panel_kind;
    uint32_t last_view, last_head;
    float t;
    uint32_t uiq_tail;
    uint32_t splash_t0; int splash_on, touched;
    uint8_t pu_order[16], pu_n;           /* the punch-in keys playing (held or latched), in the order pressed */
    /* the holds: OCT- held as a key is pressed latches it to the loop's end, OCT+ for good; pressed again, it's let go;
     * OCT+ alone lets every hold go. w: the white keys, b: the black; until: the tick it ends (0 for good) */
    uint16_t lat_w, lat_b; uint32_t lat_w_until[16], lat_b_until[11]; float lat_w_dep[16], lat_b_dep[11];
    int oct_used[2], kept_ok;
    int pu_shown;
} ui;

static void settings_apply(void);
static const Knob *knob_of(int pg, int k) { return &PAGES[pg][k]; }
static uint8_t *knob_ptr(int pg, int k)
{
    const Knob *kn = knob_of(pg, k);
    if (kn->where == 1) return (uint8_t *)&set.cur.p[ui.sel] + kn->off;
    if (kn->where == 2) return (uint8_t *)&set + kn->off;
    return (uint8_t *)&set.cur + kn->off;
}

static void knob_text(int pg, int k, char *s)
{
    const Knob *kn = knob_of(pg, k); uint8_t v = *knob_ptr(pg, k);
    s[0] = 0;
    switch (kn->kind) {
    case K_NONE: break;
    case K_SEND: if (!v) text_cat(s, "DRY"); else int_text(v, s); break;
    case K_REC: text_cat(s, v & 1 ? "FIXED" : "MASTER"); break;
    case K_ROUTE: text_cat(s, v >> 1 & 1 ? "CLEAN" : "MIX"); break;
    case K_STYLE: text_cat(s, v ? "TONES" : "KNOCKOUT"); break;
    case K_ULVL: int_text(v, s); break;
    case K_TONE: if (v >= 48 && v <= 52) text_cat(s, "OPEN"); else { text_cat(s, v < 50 ? "LP " : "HP "); int_text(v < 50 ? (50 - v) * 2 : (v - 50) * 2, s + strlen_(s)); } break;
    case K_PEGN: int_text(PEG_NS[v % NPEG_NS], s); break;
    case K_SYNC: text_cat(s, SYNC_NAME[v % NSYNC]); break;
    case K_ENGINE: text_cat(s, ENGINE_NAME[v % NUM_ENGINES]); break;
    case K_GEN: text_cat(s, GEN_NAME[v % NUM_GENS]); break;
    case K_LEN: int_text(v, s); break;
    case K_RATE: text_cat(s, RATE_NAME[v % NUM_RATES]); break;
    case K_PITCH: { int o = (int8_t)v; if (o > 0) text_cat(s, "+"); int_text(o, s + (o > 0)); break; }
    case K_PAN: if (v >= 49 && v <= 51) text_cat(s, "C"); else { text_cat(s, v < 50 ? "L" : "R"); int_text(v < 50 ? 50 - v : v - 50, s + 1); } break;
    case K_DECAY: { int d = (int)v - 50; if (!d) text_cat(s, "AS DEALT"); else { if (d > 0) text_cat(s, "+"); int_text(d, s + strlen_(s)); } break; }
    case K_RT: text_cat(s, RT_NAME[v % NUM_RT]); break;
    case K_CT: text_cat(s, CT_NAME[v % NUM_CT]); break;
    case K_DT: text_cat(s, DT_NAME[v % NUM_DT]); break;
    case K_PT: text_cat(s, PT_NAME[v % NUM_PT]); break;
    case K_DTIME: text_cat(s, DTIME_NAME[v % NUM_DTIMES]); break;
    case K_KEY: text_cat(s, NOTE_NAME[v % 12]); break;
    case K_SCALE: text_cat(s, SCALES[v % NUM_SCALES].name); break;
    case K_TEMPO: if (eng.ext_bpm > 20.0f) { int_text(shown_bpm(), s); text_cat(s, " IN"); } else int_text(40 + v, s); break;
    case K_ONOFF: text_cat(s, v ? "ON" : "OFF"); break;
    case K_LCH: { int_text(v + 1, s); text_cat(s, "-"); char n[4]; int_text(v + 4, n); text_cat(s, n); break; }
    case K_CH: int_text(v + 1, s); break;
    default: int_text(v, s); break;
    }
}
static float knob_frac(int pg, int k)
{
    const Knob *kn = knob_of(pg, k); uint8_t v = *knob_ptr(pg, k);
    switch (kn->kind) {
    case K_NONE: return 0.0f;
    case K_REC: return (float)(v & 1);
    case K_ROUTE: return (float)(v >> 1 & 1);
    case K_STYLE: return (float)v;
    case K_PEGN: return (float)v / (NPEG_NS - 1);
    case K_SYNC: return (float)v / (NSYNC - 1);
    case K_ENGINE: return (float)v / (NUM_ENGINES - 1);
    case K_GEN: return (float)v / (NUM_GENS - 1);
    case K_LEN: return (float)(v - MINLEN) / (float)(MAXLEN - MINLEN);
    case K_RATE: return (float)v / (NUM_RATES - 1);
    case K_PITCH: return ((float)(int8_t)v + 24.0f) / 48.0f;
    case K_RT: return (float)v / (NUM_RT - 1);
    case K_CT: return (float)v;
    case K_DT: return (float)v;
    case K_PT: return (float)v / (NUM_PT - 1);
    case K_DTIME: return (float)v / (NUM_DTIMES - 1);
    case K_KEY: return (float)v / 11.0f;
    case K_SCALE: return (float)v / (NUM_SCALES - 1);
    case K_TEMPO: return (float)v / 160.0f;
    case K_ONOFF: return v ? 1.0f : 0.0f;
    case K_LCH: return (float)v / 12.0f;
    case K_CH: return (float)v / 15.0f;
    default: return (float)v / 100.0f;
    }
}

/* ------------------------------------------------------------------ messages */
static void say_text(const char *a, const char *b)
{
    if (!ann_on()) return;
    char t[72] = "";
    text_cat(t, a); text_cat(t, " "); text_cat(t, b);
    ann_say(t, 0, ANN_NOW);
}
void app_say(const char *a, const char *b)
{
    int i;
    for (i = 0; i < 27 && a && a[i]; i++) ui.pop_t[i] = a[i];
    ui.pop_t[i] = 0;
    for (i = 0; i < 27 && b && b[i]; i++) ui.pop_s[i] = b[i];
    ui.pop_s[i] = 0;
    ui.pop_3[0] = 0; ui.pop_c = C_TXT;
    ui.pop_until = plat_ms() + 1500u;
    ui.list_until = 0;
    say_text(ui.pop_t, ui.pop_s);
}
static void pop(const char *a, const char *b, uint16_t c) { app_say(a, b); ui.pop_c = c; }

/* ------------------------------------------------------------------ actions */
static void changed(void) { set_dirty = 1; autosave_at = plat_ms() + AUTOSAVE_MS; }
static void groove_label(char *t)
{
    groove_name(set.cur.seed, t);
    if (set.slot != 255 && memcmp(&set.cur, &set.saved[set.slot], sizeof(Groove))) text_cat(t, "*");
}

static uint8_t deck[NPAL]; static int deck_n;
static void palette_next(void)           /* REC's palettes: a shuffled deck, none twice till all have come */
{
    if (deck_n == 0) {
        for (int i = 0; i < NPAL; i++) deck[i] = (uint8_t)i;
        for (int i = NPAL - 1; i > 0; i--) { int j = (int)((plat_ticks24() ^ (uint32_t)i * 2654435761u) % (uint32_t)(i + 1)); uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        deck_n = NPAL;
        if (deck[deck_n - 1] == set.palette) { uint8_t t = deck[0]; deck[0] = deck[deck_n - 1]; deck[deck_n - 1] = t; }
    }
    set.palette = deck[--deck_n];
    palette_apply();
}

static void vis_deal(void);
static void new_groove(int keep)          /* REC: a new groove (a new key too); held: in the same key. The tempo never: it's yours */
{
    int key = set.cur.key, scale = set.cur.scale, tempo = set.cur.tempo;
    PartP was[NPARTS]; for (int p = 0; p < NPARTS; p++) was[p] = set.cur.p[p];
    palette_next();
    hist_push();
    uint32_t seed = (set.cur.seed * 1664525u + 1013904223u) ^ plat_ms() * 2654435761u ^ plat_ticks24();
    groove_roll(&set.cur, seed, 1);
    if (keep || CLK_IN) { set.cur.key = (uint8_t)key; set.cur.scale = (uint8_t)scale; }
    set.cur.tempo = (uint8_t)tempo;   /* (the tempo only ever changed by you: never by REC, live) */
    /* one part's sound carried over from the last groove, a part at random: its EDIT settings as they were (SOUND:
     * ENGINE, TONE, WEIGHT, ENTROPY; MIX: LEVEL, DECAY, PITCH, PAN), only its pattern new. Grooves dealt one after
     * another share a thread, for a set that flows */
    int kp = (int)((seed >> 11) % NPARTS);
    {   PartP *q = &set.cur.p[kp]; const PartP *o = &was[kp];
        q->engine = o->engine; q->tone = o->tone; q->weight = o->weight; q->entropy = o->entropy;
        q->level = o->level; q->decay = o->decay; q->pitch = o->pitch; q->pan = o->pan;
    }
    ui.kept_ok = 0;                              /* (the parts sounding as before: one, at least; for the tests) */
    for (int p = 0; p < NPARTS; p++) {
        const PartP *q = &set.cur.p[p], *o = &was[p];
        if (q->engine == o->engine && q->tone == o->tone && q->weight == o->weight && q->entropy == o->entropy && q->level == o->level
            && q->decay == o->decay && q->pitch == o->pitch && q->pan == o->pan) ui.kept_ok++;
    }
    set.slot = 255;
    changed();
    eng.sync_req = set.playing && !CLK_IN;    /* (from the top: its one on the one; following a clock, the leader's) */
    vis_deal();
    char nm[32]; groove_name(seed, nm);
    {
        pop(keep ? "SAME KEY" : "NEW GROOVE", nm, PART_C[(seed >> 3) % NPARTS]);
        text_cat(ui.pop_3, NOTE_NAME[set.cur.key]); text_cat(ui.pop_3, " "); text_cat(ui.pop_3, SCALES[set.cur.scale].name);
        text_cat(ui.pop_3, "  KEPT: "); text_cat(ui.pop_3, PART_NAME[kp]);
    }
}

static void new_part(void)
{
    groove_roll_part(&set.cur, ui.sel, plat_ms() * 2654435761u ^ plat_ticks24() ^ (uint32_t)ui.sel);
    changed();
    pop(PART_NAME[ui.sel], "ANEW", PART_C[ui.sel]);
    text_cat(ui.pop_3, GEN_NAME[set.cur.p[ui.sel].gen]); text_cat(ui.pop_3, " + "); text_cat(ui.pop_3, ENGINE_NAME[set.cur.p[ui.sel].engine]);
}

static void hist_step(int d)
{
    char nm[32]; uint8_t tempo = set.cur.tempo;   /* (the tempo yours: kept) */
    if (d < 0) {
        if (hist_at == 0) { pop("HISTORY", "THE FIRST", C_SUB); return; }
        if (hist_at == hist_n) { hist_push(); hist_at = hist_n - 1; }
        set.cur = hist[--hist_at];
    } else {
        if (hist_at + 1 >= hist_n) { pop("HISTORY", "THE LATEST", C_SUB); return; }
        set.cur = hist[++hist_at];
    }
    set.cur.tempo = tempo;
    set.slot = 255;
    changed();
    groove_name(set.cur.seed, nm);
    pop(d < 0 ? "BACK TO" : "FORWARD TO", nm, C_TXT);
}

/* SAVE while it plays: the flash's erase stops the sound for tens of ms, so the sound fades out first, the save, and
 * back in: a soft dip, not a crunch (After Images' way) */
static int save_pending; static uint32_t save_t;
static int store_save(void) { int ok = plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0; save_end = plat_ms() | 1u; return ok; }
static uint32_t drops(void) { return plat_xruns() - xr_saves; }
static void store_now(int slot)
{
    store_ok = store_save();
    char s[12] = "SLOT ", n[4]; int_text(slot + 1, n); text_cat(s, n);
    if (store_ok && save_over) { save_over = 0; text_cat(s, ": OVER IT"); pop("ALL 16 FULL", s, C_RED); }
    else if (store_ok) pop("SAVED", s, C_TXT);
    else pop("NOT SAVED", "UNTIL POWER OFF", C_RED);
}
static void save_groove(void)
{
    save_over = set.slot == 255 && set.nsaved >= NSAVED;
    int slot = set.slot != 255 ? set.slot : set.nsaved < NSAVED ? set.nsaved++ : NSAVED - 1;
    set.saved[slot] = set.cur; set.slot = (uint8_t)slot;
    set_dirty = 0; autosave_at = 0;
    if (set.playing) { save_pending = slot + 1; save_hold = 1; save_t = plat_ms(); return; }
    store_now(slot);
}
static void load_step(int d)
{
    if (!set.nsaved) { pop("NO GROOVES", "SAVE ONE FIRST", C_SUB); return; }
    int at = set.slot == 255 ? (d > 0 ? 0 : set.nsaved - 1) : (set.slot + d % set.nsaved + set.nsaved) % set.nsaved;
    if (set.slot == 255) hist_push();
    { uint8_t tempo = set.cur.tempo; set.slot = (uint8_t)at; set.cur = set.saved[at]; set.cur.tempo = tempo; }   /* (the
                                                  * tempo yours: a saved groove plays at the one you're at) */
    changed(); set_dirty = 0;
    ui.list_until = plat_ms() + 2000u; ui.pop_until = 0;
    char nm[32]; groove_name(set.cur.seed, nm); say_text("GROOVE", nm);
}

static uint32_t stopped_at;
static void play_toggle(void)
{
    set.playing = (uint8_t)!set.playing;
    if (!set.playing) stopped_at = plat_ms();
    if (set.playing) eng.sync_req = 1;           /* (from the top of the bar) */
    eng.playing = set.playing;
    if (CLK_OUT) midi_send(0x0Fu | (set.playing ? 0xFAu : 0xFCu) << 8);
    if (!set.playing) engine_all_off(&eng);
    pop(set.playing ? "PLAY" : "STOP", "", C_TXT);
}

static void info(void)
{
    char s[28] = "CPU ", n[8];
    int_text((int32_t)plat_cpu_pct(), n); text_cat(s, n); text_cat(s, "%");
    pop("EXPECTATION", s, store_ok ? C_TXT : C_RED);
    text_cat(ui.pop_3, two_cores && plat_cpu2_on() ? "2 CORES" : "1 CORE");
    {   char m[12]; text_cat(ui.pop_3, "  "); int_text((int32_t)drops(), m); text_cat(ui.pop_3, m); text_cat(ui.pop_3, " DROPS"); }
    if (c2_late) { char m[12]; text_cat(ui.pop_3, "  "); int_text((int32_t)c2_late, m); text_cat(ui.pop_3, m); text_cat(ui.pop_3, " LATE"); }
    text_cat(ui.pop_3, store_ok ? "  SAVING OK" : "  NO SAVING");
    ui.pop_until = plat_ms() + 3500u;
}

static void select_part(int p)
{
    ui.sel = (p % NPARTS + NPARTS) % NPARTS;
    pop("PART", PART_NAME[ui.sel], PART_C[ui.sel]);
    text_cat(ui.pop_3, GEN_NAME[set.cur.p[ui.sel].gen]); text_cat(ui.pop_3, " + "); text_cat(ui.pop_3, ENGINE_NAME[set.cur.p[ui.sel].engine]);
}

static void knob_turn(int k, int d)
{
    const Knob *kn = knob_of(ui.page, k); uint8_t *p = knob_ptr(ui.page, k);
    int v = kn->kind == K_PITCH ? (int8_t)*p : kn->kind == K_REC ? *p & 1 : kn->kind == K_ROUTE ? *p >> 1 & 1 : *p, lo = 0, hi = 100, step = 2, wrap = 0;
    if (kn->kind == K_NONE) return;
    switch (kn->kind) {
    case K_REC: case K_ROUTE: case K_STYLE: hi = 1; step = 1; break;
    case K_ULVL: step = 5; break;
    case K_PEGN: hi = NPEG_NS - 1; step = 1; break;
    case K_SYNC: hi = NSYNC - 1; step = 1; break;
    case K_ENGINE: hi = NUM_ENGINES - 1; step = 1; wrap = 1; break;
    case K_GEN: hi = NUM_GENS - 1; step = 1; wrap = 1; break;
    case K_LEN: lo = MINLEN; hi = MAXLEN; step = 1; break;
    case K_RATE: hi = NUM_RATES - 1; step = 1; break;
    case K_PITCH: lo = -24; hi = 24; step = 1; break;
    case K_PAN: case K_DECAY: step = 1; break;
    case K_RT: hi = NUM_RT - 1; step = 1; break;
    case K_CT: hi = NUM_CT - 1; step = 1; break;
    case K_DT: hi = NUM_DT - 1; step = 1; break;
    case K_PT: hi = NUM_PT - 1; step = 1; break;
    case K_DTIME: hi = NUM_DTIMES - 1; step = 1; break;
    case K_KEY: hi = 11; step = 1; wrap = 1; break;
    case K_SCALE: hi = NUM_SCALES - 1; step = 1; wrap = 1; break;
    case K_TEMPO: hi = 160; step = 1; break;
    case K_ONOFF: hi = 1; step = 1; break;
    case K_CH: hi = 15; step = 1; break;
    case K_LCH: hi = 12; step = 1; break;
    default: break;
    }
    v += d * step;
    if (wrap) v = ((v - lo) % (hi - lo + 1) + (hi - lo + 1)) % (hi - lo + 1) + lo;
    else v = v < lo ? lo : v > hi ? hi : v;
    if (kn->kind == K_PITCH) *(int8_t *)p = (int8_t)v;
    else if (kn->kind == K_REC) *p = (uint8_t)((*p & 2u) | (unsigned)v);          /* (CONTROL and OUTPUT share a byte) */
    else if (kn->kind == K_ROUTE) *p = (uint8_t)((*p & 1u) | (unsigned)v << 1);
    else *p = (uint8_t)v;
    if (kn->kind == K_ENGINE) groove_part_engine(&set.cur, ui.sel);   /* (a new engine: its colours dealt to suit) */
    if (kn->where == 2) { autosave_at = plat_ms() + AUTOSAVE_MS; settings_apply(); }
    else changed();
    if (kn->kind == K_ONOFF && kn->off == SO(midi_on) && kn->where == 2 && !v) engine_all_off(&eng);
    ui.last_knob = k; ui.knob_at = plat_ms(); ui.pop_until = 0;
    if (ann_on()) { char s[24]; knob_text(ui.page, k, s); say_text(kn->name, s); }
}

static void settings_apply(void)
{
    plat_usb_audio_full(set.usb_full & 1);
    eng.clean = set.usb_full >> 1 & 1;   /* OUTPUT CLEAN: the kit without FX's effects, to the computer and the speaker */
    palette_apply();
}

static void level_turn(int p, int d)
{
    PartP *q = &set.cur.p[p];
    q->level = (uint8_t)clamp(q->level + 2 * d, 0, 100);
    changed();
    ui.panel_kind = 0; ui.panel_until = plat_ms() + 2000u; ui.last_knob = p; ui.knob_at = plat_ms(); ui.pop_until = 0;
    if (ann_on()) { char n[8]; int_text(q->level, n); say_text(PART_NAME[p], n); }
}

static void page_to(int pg)
{
    ui.page = pg; ui.knob_at = 0; ui.pop_until = 0;
    if (ann_on()) say_text(PAGE_NAME[pg], page_has_part(pg) ? PART_NAME[ui.sel] : "");
}

/* ------------------------------------------------------------------ the keys: punch-ins and randomisers
 * White keys, low to high: HIGH-PASS, FILL, REPEAT 1/8, 1/12, 1/16, 1/32, GLITCH, PITCH DOWN, PITCH UP, TAPE MASH,
 * REVERSE, FREEZE, SWELL, CRUSH, KILL KICK, LOW-PASS. The latest held plays (the filters alongside, their own).
 * Black keys, in their groups: KICK's TONE, WEIGHT, ENTROPY; SNARE's WEIGHT, ENTROPY; PERC's TONE, WEIGHT, ENTROPY;
 * HATS' WEIGHT, ENTROPY; the top one every part's three. Held, each hit of the part takes new values. */
static const uint8_t PU_KEY[16] = { PU_HPF, PU_FILL, PU_REP8, PU_REP12, PU_REP16, PU_REP32, PU_GLITCH, PU_PDOWN, PU_PUP, PU_MASH,
                                    PU_REVERSE, PU_FREEZE, PU_SWELL, PU_CRUSH, PU_KILL, PU_LPF };
static const struct { int8_t part; uint8_t bits; } RND_KEY[11] = {
    { P_KICK, 1 }, { P_KICK, 2 }, { P_KICK, 4 }, { P_SNARE, 2 }, { P_SNARE, 4 }, { P_PERC, 1 }, { P_PERC, 2 }, { P_PERC, 4 },
    { P_HATS, 2 }, { P_HATS, 4 }, { -1, 7 } };
static float key_depth(uint32_t kk, int pressure_ok)
{
    float v = (float)touch_vel(kk); if (v <= 0) v = 90.0f;
    float d = clamp01((v - 60.0f) / 67.0f);
    if (pressure_ok && touch_sensed(kk)) d = fm_maxf(d * 0.5f, (float)touch_pressure(kk) / 127.0f);
    return d;
}

static uint32_t loop_end(void)          /* where the groove's loop ends next (the kick's pattern), at least a beat away */
{
    const Pattern *pt = &eng.P[P_KICK].pat;
    uint32_t loop = (uint32_t)(pt->len ? pt->len : 16) * RATE_TICKS[pt->rate % NUM_RATES], t = eng.tick;
    if (loop < (uint32_t)TPB) loop = TPB * 4u;
    uint32_t end = (t / loop + 1u) * loop;
    if (end - t < (uint32_t)TPB) end += loop;
    return end ? end : 1u;
}

static void pu_drop(int i)               /* a white key no longer playing (let go and not held, or its hold ended) */
{
    int w = 0; for (int j = 0; j < ui.pu_n; j++) if (ui.pu_order[j] != i) ui.pu_order[w++] = ui.pu_order[j];
    ui.pu_n = (uint8_t)w;
}

static void holds_clear(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    for (int i = 0; i < 16; i++) if ((ui.lat_w >> i & 1u) && !(ui.keys >> WK[i] & 1u)) pu_drop(i);
    ui.lat_w = 0; ui.lat_b = 0;
}

static void keys_input(uint32_t keys, uint32_t btn)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    int hold = btn >> B_OCTUP & 1u ? 2 : btn >> B_OCTDN & 1u ? 1 : 0;   /* (OCT+: for good; OCT-: to the loop's end) */
    uint32_t now_t = eng.tick;
    /* the holds that have reached the loop's end */
    for (int i = 0; i < 16; i++)
        if ((ui.lat_w >> i & 1u) && ui.lat_w_until[i] && (int32_t)(now_t - ui.lat_w_until[i]) >= 0 && set.playing) {
            ui.lat_w &= (uint16_t)~(1u << i); if (!(keys >> WK[i] & 1u)) pu_drop(i);
        }
    for (int j = 0; j < 11; j++)
        if ((ui.lat_b >> j & 1u) && ui.lat_b_until[j] && (int32_t)(now_t - ui.lat_b_until[j]) >= 0 && set.playing) ui.lat_b &= (uint16_t)~(1u << j);
    for (int i = 0; i < 16; i++) {
        int kk = WK[i]; uint32_t b = 1u << kk;
        if (kk >= NKEYS) continue;
        int pu = PU_KEY[i], filt = pu == PU_HPF || pu == PU_LPF || pu == PU_FILL;   /* (the filters and FILL: alongside the punch-in) */
        if ((keys & b) && !(ui.keys & b)) {      /* pressed */
            if (hold) {                          /* a hold: one punch-in held at a time (they never play together: pitch up and
                                                  * down would cancel, two loops fight), so it takes the place of the one
                                                  * before; the two filters are held alongside it */
                if (!filt) for (int k = 2; k < 15; k++) if (k != i && (ui.lat_w >> k & 1u)) { ui.lat_w &= (uint16_t)~(1u << k); if (!(keys >> WK[k] & 1u)) pu_drop(k); }
                ui.lat_w |= (uint16_t)(1u << i); ui.lat_w_until[i] = hold == 2 ? 0 : loop_end();
                ui.lat_w_dep[i] = key_depth(kk, i == 0); ui.oct_used[hold - 1] = 1;
                pop(hold == 2 ? "HELD" : "HELD TO THE LOOP'S END", PU_NAME[pu], C_TXT);
            } else if (ui.lat_w >> i & 1u) {     /* pressed again: its hold let go (it plays while pressed) */
                ui.lat_w &= (uint16_t)~(1u << i);
                pop("LET GO", PU_NAME[pu], C_TXT);
            } else pop("PUNCH-IN", PU_NAME[pu], C_TXT);
            if (!filt) { pu_drop(i); if (ui.pu_n < 16) ui.pu_order[ui.pu_n++] = (uint8_t)i; }
            ui.pop_until = plat_ms() + 700u;
        }
        if (!(keys & b) && (ui.keys & b) && !(ui.lat_w >> i & 1u)) pu_drop(i);   /* let go, not held */
    }
    int top = ui.pu_n ? ui.pu_order[ui.pu_n - 1] : -1;
    eng.pu_want = top >= 0 ? PU_KEY[top] : 0;
    eng.pu_press = top < 0 ? 0.0f : keys >> WK[top] & 1u ? key_depth(WK[top], 1) : ui.lat_w_dep[top];   /* (how hard: TAPE MASH's depth) */
    eng.fill_want = (keys >> WK[1] & 1u) || (ui.lat_w >> 1 & 1u);   /* FILL: a fill each beat while held */
    eng.pu_hpw = keys >> WK[0] & 1u ? 1.0f + key_depth(WK[0], 1) : ui.lat_w & 1u ? 1.0f + ui.lat_w_dep[0] : 0.0f;          /* (the bottom key: pressure) */
    eng.pu_lpw = keys >> WK[15] & 1u ? 1.0f + key_depth(WK[15], 0) : ui.lat_w >> 15 & 1u ? 1.0f + ui.lat_w_dep[15] : 0.0f;   /* (the top: how hard it was struck) */
    uint8_t bits[NPARTS] = { 0, 0, 0, 0 }; float dep[NPARTS] = { 0, 0, 0, 0 };
    for (int j = 0; j < 11; j++) {
        int kk = BK[j];
        if (kk >= NKEYS) continue;
        int held = keys >> kk & 1u, pressed = held && !(ui.keys >> kk & 1u);
        if (pressed) {
            const char *what = RND_KEY[j].part < 0 ? "EVERY PART" : PART_NAME[RND_KEY[j].part];
            uint16_t c = RND_KEY[j].part < 0 ? C_TXT : PART_C[RND_KEY[j].part];
            if (hold) {
                ui.lat_b |= (uint16_t)(1u << j); ui.lat_b_until[j] = hold == 2 ? 0 : loop_end();
                ui.lat_b_dep[j] = 0.4f + 0.6f * key_depth((uint32_t)kk, 1); ui.oct_used[hold - 1] = 1;
                pop(hold == 2 ? "RANDOM, HELD" : "RANDOM, TO THE LOOP'S END", what, c);
            } else if (ui.lat_b >> j & 1u) { ui.lat_b &= (uint16_t)~(1u << j); pop("RANDOM, LET GO", what, c); }
            else pop("RANDOM", what, c);
            text_cat(ui.pop_3, RND_KEY[j].bits == 7 ? "TONE, WEIGHT, ENTROPY" : RND_KEY[j].bits == 1 ? "TONE" : RND_KEY[j].bits == 2 ? "WEIGHT" : "ENTROPY");
            ui.pop_until = plat_ms() + 900u;
        }
        float d = held ? 0.4f + 0.6f * key_depth((uint32_t)kk, 1) : ui.lat_b >> j & 1u ? ui.lat_b_dep[j] : -1.0f;
        if (d < 0) continue;
        for (int p = 0; p < NPARTS; p++) if (RND_KEY[j].part < 0 || RND_KEY[j].part == p) { bits[p] |= RND_KEY[j].bits; if (d > dep[p]) dep[p] = d; }
    }
    for (int p = 0; p < NPARTS; p++) { eng.rnd_depth[p] = dep[p]; eng.rnd_bits[p] = bits[p]; }
}

static uint32_t holds_lit(void)          /* the held keys lit */
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) if ((ui.lat_w >> i & 1u) && WK[i] < NKEYS) m |= 1u << WK[i];
    for (int j = 0; j < 11; j++) if ((ui.lat_b >> j & 1u) && BK[j] < NKEYS) m |= 1u << BK[j];
    return m;
}

/* ------------------------------------------------------------------ input */
static int32_t enc(int e) { int32_t d = plat_enc(e); if (d) ui.touched = 1; return d; }

static void input(void)
{
    uint32_t btn = plat_buttons(), keys;
    touch_poll();
    keys = touch_keys();
    uint32_t down = btn & ~ui.btn, up = ui.btn & ~btn;
    int32_t d;
    if (down || (keys & ~ui.keys)) ui.touched = 1;
    {   /* a page's button: its page; pressed again, the next of its pages */
        static const uint8_t BTNS[7] = { B_EDIT, B_SEQ, B_ENV, B_FX, B_LFO, B_GLO, B_ARP };
        for (int b = 0; b < 7; b++) {
            if (!(down >> BTNS[b] & 1u)) continue;
            int first = -1, next = -1, here = PAGE_BTN[ui.page] == BTNS[b];
            for (int pg = 1; pg < NPAGES; pg++) if (PAGE_BTN[pg] == BTNS[b]) {
                if (first < 0) first = pg;
                if (here && pg > ui.page && next < 0) next = pg;
            }
            page_to(here ? (next >= 0 ? next : first) : first);
        }
    }
    if (down >> B_HOME & 1u) { ui.home_used = ui.page != PG_HOME; if (ui.page != PG_HOME) page_to(PG_HOME); }
    if ((up >> B_HOME & 1u) && !ui.home_used) info();
    if (down >> B_SEL & 1u) ui.sel_used = 0;
    if ((up >> B_SEL & 1u) && !ui.sel_used) select_part(ui.sel + 1);
    if (down >> B_REC & 1u) {
        if (btn >> B_SEL & 1u) { new_part(); ui.sel_used = 1; ui.rec_t = 0; }
        else { ui.rec_t = plat_ms(); ui.rec_used = 0; }
    }
    if (ui.rec_t && (btn >> B_REC & 1u) && !ui.rec_used && plat_ms() - ui.rec_t >= 900u) { new_groove(1); ui.rec_used = 1; }
    if (ui.rec_t && (up >> B_REC & 1u)) { if (!ui.rec_used) new_groove(0); ui.rec_t = 0; }
    if (down >> B_SAVE & 1u) save_groove();
    if (down >> B_PLAY & 1u) play_toggle();
    /* OCT- and OCT+: the holds (held as a key is pressed; OCT+ alone lets them all go). With HOME held: back and forward
     * through the grooves REC replaced */
    for (int k = 0; k < 2; k++) {
        int bt = k ? B_OCTUP : B_OCTDN;
        if (down >> bt & 1u) {
            ui.oct_used[k] = 0;
            if (btn >> B_HOME & 1u) { hist_step(k ? 1 : -1); ui.oct_used[k] = 1; ui.home_used = 1; }
        }
        if ((up >> bt & 1u) && !ui.oct_used[k] && k == 1 && (ui.lat_w || ui.lat_b)) { holds_clear(); pop("HOLDS", "ALL LET GO", C_TXT); ui.pop_until = plat_ms() + 700u; }
    }
    keys_input(keys, btn & ~(btn >> B_HOME & 1u ? (1u << B_OCTDN | 1u << B_OCTUP) : 0u));
    for (int k = 0; k < 4; k++) {
        if ((d = enc(EN_K1 + k)) == 0) continue;
        if (ui.page != PG_HOME) knob_turn(k, (int)d);
        else if (btn >> B_HOME & 1u) { knob_turn(k, (int)d); ui.home_used = 1; ui.panel_kind = 1; ui.panel_until = plat_ms() + 1500u; }
        else level_turn(k, (int)d);
    }
    if (btn >> B_HOME & 1u) { ui.panel_kind = 1; ui.panel_until = plat_ms() + 300u; }
    if ((d = enc(EN_SELECT)) != 0) select_part(ui.sel + (int)d);
    if ((d = enc(EN_ALGO)) != 0) load_step((int)d);
    if ((d = enc(EN_PRESET)) != 0) {
        int pg = ui.page, lk = ui.last_knob; uint32_t ka = ui.knob_at;
        ui.page = PG_SOUND; knob_turn(0, (int)d); ui.page = pg;
        if (pg != PG_SOUND) { ui.last_knob = lk; ui.knob_at = ka; char s[24]; knob_text(PG_SOUND, 0, s); pop(PART_NAME[ui.sel], s, PART_C[ui.sel]); }
    }
    ui.btn = btn; ui.keys = keys;
    {   /* the lights: the page's button lit, the others that do something glowing; SAVE lit while unsaved; PLAY green */
        uint32_t used = 1u << B_SEL | 1u << B_REC | 1u << B_SAVE | 1u << B_OCTDN | 1u << B_OCTUP | 1u << B_PLAY;
        for (int pg = 0; pg < NPAGES; pg++) used |= 1u << PAGE_BTN[pg];
        uint32_t lit = (btn & used) | 1u << PAGE_BTN[ui.page] | (set_dirty ? 1u << B_SAVE : 0u) | (set.playing ? LED_PLAY_GREEN : 0u);
        plat_lights(lit, keys | holds_lit(), used | LED_PLAY_GREEN, 0x7FFFFFFu, 1);
    }
}

void app_overload(void) { engine_overload(&eng); }
void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ the view: After Images' MATRIX turned inside out
 * A grid of lit squares, laid out anew by each REC in the palette's four colours. Each hit knocks a square out, somewhere
 * on the grid, and it fades back in in its part's colour, as long as the sound takes to die away; the loud kicks and
 * snares knock out a block of four. So the hits slowly repaint the layout: a hat-heavy groove drifts towards the hats'
 * colour, a sparse one keeps more of what REC laid out. A whole square fills its cell, meeting its neighbours edge to
 * edge; one knocked out grows back to it. SCREEN: COUNT (squares across), FADE, BRIGHT. */
#define HEAD_H 30
static int full_now(void) { return ui.page == PG_HOME; }
#define VIEW_Y (full_now() ? 0 : HEAD_H)
#define VIEW_H (240 - VIEW_Y)
#define PANEL_H 110
#define PANEL_TOP (VIEW_H - PANEL_H)
#define VG_MAX 24
typedef struct { uint8_t part; uint8_t base; uint16_t f; uint32_t t; } VCell;   /* its colour (a part's), its brightness, its fade (ms), its last hit */
static VCell vg[VG_MAX][VG_MAX];
static int vg_n_now = -1;
static uint32_t jrng = 0x2545F491u;
static uint32_t jr(void) { jrng ^= jrng << 13; jrng ^= jrng >> 17; jrng ^= jrng << 5; return jrng; }
static int splash_grid;
static int vg_cols(void) { return splash_grid ? 12 : PEG_NS[set.peg_n % NPEG_NS]; }
static int vg_cell(void) { return 240 / vg_cols(); }
static int vg_rows_all(void) { int r = 240 / vg_cell(); return r > VG_MAX ? VG_MAX : r; }

static uint16_t grey(float v)
{
    v = clamp01(v);
    uint32_t r = (uint32_t)(v * 31.0f + 0.5f), g = (uint32_t)(v * 63.0f + 0.5f);
    return (uint16_t)(r << 11 | g << 5 | r);
}
static uint16_t mix_c(uint16_t a, uint16_t b, float t)   /* a to b, t 0..1 */
{
    float ra = (float)(a >> 11 & 31u), ga = (float)(a >> 5 & 63u), ba = (float)(a & 31u);
    float rb = (float)(b >> 11 & 31u), gb = (float)(b >> 5 & 63u), bb = (float)(b & 31u);
    return (uint16_t)((uint32_t)(ra + (rb - ra) * t + 0.5f) << 11 | (uint32_t)(ga + (gb - ga) * t + 0.5f) << 5 | (uint32_t)(ba + (bb - ba) * t + 0.5f));
}
static uint16_t dim(uint16_t c, float k)
{
    if (k >= 1) return c;
    if (k <= 0) return C_BLACK;
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((uint32_t)((float)r * k) << 11) | ((uint32_t)((float)g * k) << 5) | (uint32_t)((float)b * k));
}

static void vis_deal(void)               /* REC: the grid laid out anew, patches of the four colours and loose squares */
{
    int cols = vg_cols(), rows = vg_rows_all(), ns = 2 + (int)(jr() % 3u);
    struct { float x, y; uint8_t p; } sd[4];
    for (int i = 0; i < ns; i++) { sd[i].x = (float)(jr() % 1000u) * 0.001f * (float)cols; sd[i].y = (float)(jr() % 1000u) * 0.001f * (float)rows; sd[i].p = (uint8_t)(jr() % NPARTS); }
    uint32_t loose = jr() % 50u;
    for (int y = 0; y < VG_MAX; y++)
        for (int x = 0; x < VG_MAX; x++) {
            int best = 0; float bd = 1e9f;
            for (int i = 0; i < ns; i++) { float dx = sd[i].x - (float)x, dy = sd[i].y - (float)y, dd = dx * dx + dy * dy; if (dd < bd) { bd = dd; best = i; } }
            VCell *c = &vg[y][x];
            c->part = jr() % 100u < loose ? (uint8_t)(jr() % NPARTS) : sd[best].p;
            c->base = (uint8_t)(90 + jr() % 166u); c->t = 0; c->f = 1;
        }
    vg_n_now = splash_grid ? -2 : set.peg_n;
}

static void vis_hit(const UiHit *h, uint32_t now)
{
    static const uint16_t FADE[NPARTS] = { 650, 420, 900, 160 };
    int cols = vg_cols(), rows = (VIEW_H) / vg_cell(); if (rows > VG_MAX) rows = VG_MAX; if (rows < 1) rows = 1;
    int big = h->vel > 95 && h->part <= P_SNARE && cols >= 8 ? 2 : 1;
    int x0 = (int)(jr() % (uint32_t)(cols - big + 1)), y0 = (int)(jr() % (uint32_t)(rows - big + 1));
    float fk = 0.4f + (float)set.fade * 0.021f;                /* FADE: 0.4 .. 2.5 */
    float f = (float)FADE[h->part % NPARTS] * fk * (h->part == P_HATS && h->alt ? 3.0f : 1.0f);
    for (int a = 0; a < big; a++)
        for (int b = 0; b < big; b++) { VCell *c = &vg[y0 + a][x0 + b]; c->part = h->part; c->t = now | 1u; c->f = (uint16_t)clamp((int)f, 60, 6000); c->base = 255; }
}

static void vis_step(void)
{
    if (vg_n_now != (splash_grid ? -2 : set.peg_n)) vis_deal();
    uint32_t head = eng.uiq_head, now = plat_ms();
    if (head - ui.uiq_tail > UI_Q) ui.uiq_tail = head - UI_Q;
    while (ui.uiq_tail != head) { UiHit h = eng.uiq[ui.uiq_tail % UI_Q]; ui.uiq_tail++; if (set.playing) vis_hit(&h, now); }
}

static void draw_grid(void)
{
    int cols = vg_cols(), cell = vg_cell(), rows = VIEW_H / cell;
    if (rows > VG_MAX) rows = VG_MAX;
    int y0 = (VIEW_H - rows * cell) / 2;
    uint32_t now = plat_ms();
    float br = 0.35f + 0.65f * (float)set.bright * 0.01f;
    for (int y = 0; y < rows; y++) {
        int py = y0 + y * cell;
        if (py + cell + cv_oy < cv_y0 || py + cv_oy >= cv_y1) continue;
        for (int x = 0; x < cols; x++) {
            VCell *c = &vg[y][x];
            float k = 1.0f;
            if (c->t) { float e = (float)(now - c->t) / (float)c->f; if (e < 1.0f) { k = e < 0 ? 0 : e; k = k * k * (3.0f - 2.0f * k); } else c->t = 0; }
            if (set.style) {                     /* TONES: each square a grey (its part's colour's own lightness, times its
                                                  * shade), a hit flooding it with the colour, fading back to its grey */
                uint16_t pc = PART_C[c->part % NPARTS];
                float lum = (0.30f * (float)(pc >> 11 & 31u) / 31.0f + 0.59f * (float)(pc >> 5 & 63u) / 63.0f + 0.11f * (float)(pc & 31u) / 31.0f);
                float gv = (0.12f + 0.78f * lum) * (float)c->base / 255.0f * br;
                uint16_t gc = grey(gv), hc = dim(pc, br);
                cv_rect(x * cell, py, cell, cell, k >= 1.0f ? gc : mix_c(hc, gc, k));
                continue;
            }
            float lv = k * (float)c->base / 255.0f * br;
            if (lv < 0.03f) continue;
            /* a square knocked out grows back as it fades in; whole, it fills its cell: no line, it meets its neighbours */
            int sz = k >= 1.0f ? cell : (int)((float)cell * (0.3f + 0.7f * k) + 0.5f), off = (cell - sz) / 2;
            cv_rect(x * cell + off, py + off, sz, sz, dim(PART_C[c->part % NPARTS], lv));
        }
    }
}

/* ------------------------------------------------------------------ the panel, the header, the popups (After Images') */
static int panel_on_(void) { return ui.page != PG_HOME || plat_ms() < ui.panel_until; }
static void frame_box(int x, int y, int w, int h, uint16_t c)
{
    cv_rect(x, y, w, h, C_BLACK);
    cv_rect(x, y, w, 2, c); cv_rect(x, y + h - 2, w, 2, c); cv_rect(x, y, 2, h, c); cv_rect(x + w - 2, y, 2, h, c);
}
static void draw_popup(void)
{
    int wt = text_w(&FONT_S, ui.pop_t), ws = text_w(&FONT_M, ui.pop_s), w3 = ui.pop_3[0] ? text_w(&FONT_S, ui.pop_3) : 0;
    int w = wt > ws ? wt : ws; if (w3 > w) w = w3;
    w = clamp(w + 28, 120, 236);
    int h = ui.pop_3[0] ? 78 : 58, y0 = ((panel_on_() ? PANEL_TOP : VIEW_H) - h) / 2 + 4;
    frame_box(120 - w / 2, y0, w, h, ui.pop_c);
    cv_text(120 - wt / 2, y0 + 7, &FONT_S, ui.pop_t, ui.pop_c);
    cv_text(120 - ws / 2, y0 + 25, &FONT_M, ui.pop_s, C_TXT);
    if (ui.pop_3[0]) cv_text(120 - w3 / 2, y0 + 54, &FONT_S, ui.pop_3, C_SUB);
}
static void draw_list(void)
{
    int n = set.nsaved, cur = set.slot == 255 ? 0 : set.slot, rows = 4;
    int first = clamp(cur - rows / 2, 0, n > rows ? n - rows : 0);
    frame_box(6, 4, 228, 94, C_TXT);
    cv_text(14, 6, &FONT_S, "GROOVES", C_TXT);
    for (int i = 0; i < rows && first + i < n; i++) {
        char t[40], nm[32]; int y = 26 + i * 18;
        int_text(first + i + 1, t); text_cat(t, "  "); groove_name(set.saved[first + i].seed, nm); text_cat(t, nm);
        if (first + i == cur) { cv_rect(10, y - 1, 220, 18, C_TXT); cv_text(16, y, &FONT_S, t, C_BLACK); }
        else cv_text(16, y, &FONT_S, t, C_SUB);
    }
}
static void draw_panel_rows(int y0);

static void draw_view(void)
{
    uint32_t now = plat_ms();
    int panel = panel_on_();
    for (int top = 0; top < VIEW_H; top += 60) {
        int h = VIEW_H - top < 60 ? VIEW_H - top : 60;
        cv_begin(240, (uint32_t)h, C_BLACK);
        cv_oy = -top; cv_y0 = 0; cv_y1 = h;
        draw_grid();
        if (!set.playing) { int w = text_w(&FONT_M, "STOPPED"); int yy = (panel ? PANEL_TOP : VIEW_H) / 2 - 12; cv_rect(120 - w / 2 - 6, yy - 3, w + 12, 28, C_BLACK); cv_text(120 - w / 2, yy, &FONT_M, "STOPPED", C_SUB); }
        if (panel && top + h > PANEL_TOP) { cv_rect(0, PANEL_TOP, 240, PANEL_H, C_BLACK); draw_panel_rows(PANEL_TOP); }
        if (now < ui.list_until) draw_list();
        else if (now < ui.pop_until) draw_popup();
        cv_blit(0, (uint32_t)(VIEW_Y + top));
    }
}

static void draw_header(void)
{
    char name[40], right[32] = "";
    groove_label(name);
    text_cat(right, NOTE_NAME[set.cur.key]); text_cat(right, " "); text_cat(right, SCALES[set.cur.scale].name);
    cv_begin(240, HEAD_H, C_BLACK);
    cv_text(4, 2, &FONT_B, name, C_TXT);
    {
        char t[16]; int_text(shown_bpm(), t); text_cat(t, eng.ext_bpm > 20.0f ? " BPM IN" : " BPM");
        cv_text(4, 17, &FONT_XS, t, C_SUB);
        int x = 4 + text_w(&FONT_XS, t) + 10;
        for (int p = 0; p < NPARTS; p++) { cv_rect(x, 19, 6, 6, dim(PART_C[p], 0.3f + 0.7f * eng.meter[p])); x += 10; }
    }
    cv_text(236 - text_w(&FONT_XS, right), 4, &FONT_XS, right, C_SUB);
    cv_rect(0, HEAD_H - 1, 240, 1, C_RULE);
    cv_blit(0, 0);
}

static void draw_levels(int y0)
{
    cv_rect(0, y0, 240, 1, C_RULE);
    for (int p = 0; p < NPARTS; p++) {
        int cx = 30 + 60 * p, top = y0 + 26, h = 54, hot = ui.knob_at && plat_ms() - ui.knob_at < 2000u && ui.last_knob == p;
        const char *nm = PART_NAME[p];
        if (hot) cv_rect(60 * p + 1, y0 + 1, 58, PANEL_H - 1, RGB(20, 20, 26));
        cv_text(cx - text_w(&FONT_B, nm) / 2, y0 + 6, &FONT_B, nm, PART_C[p]);
        cv_rect(cx - 9, top, 18, h, C_WELL);
        int m = (int)(eng.meter[p] * (float)h);
        cv_rect(cx - 7, top + h - m, 14, m, dim(PART_C[p], 0.65f));
        int lv = set.cur.p[p].level, my = top + h - lv * h / 100;
        cv_rect(cx - 13, my - 1, 26, 3, C_WHITE);
        char v[8]; int_text(lv, v);
        cv_text(cx - text_w(&FONT_B, v) / 2, top + h + 5, &FONT_B, v, hot ? C_WHITE : C_TXT);
    }
}

static void draw_panel_rows(int y0)
{
    int pg = ui.page, lay = page_has_part(pg);
    uint16_t pc = lay ? PART_C[ui.sel] : C_TXT;
    if (pg == PG_HOME && ui.panel_kind == 0) { draw_levels(y0); return; }
    cv_rect(0, y0, 240, lay ? 2 : 1, lay ? pc : C_RULE);
    int tx = cv_text(6, y0 + 4, &FONT_B, pg == PG_HOME ? "MACROS" : PAGE_NAME[pg], lay ? pc : C_TXT);
    if (pg != PG_HOME) {
        int n = 0, at = 0;
        for (int q = 1; q < NPAGES; q++) if (PAGE_BTN[q] == PAGE_BTN[pg]) { if (q < pg) at++; n++; }
        for (int d = 0; n > 1 && d < n; d++) cv_rect(tx + 8 + d * 8, y0 + 10, 5, 5, d == at ? (lay ? pc : C_TXT) : C_RULE);
    }
    if (lay) { const char *n = PART_NAME[ui.sel]; cv_text(234 - text_w(&FONT_B, n), y0 + 4, &FONT_B, n, pc); }
    int focus = ui.knob_at && plat_ms() - ui.knob_at < 2500u;
    if (focus) {
        char v[24]; knob_text(pg, ui.last_knob, v);
        int x = cv_text(6, y0 + 24, &FONT_S, knob_of(pg, ui.last_knob)->name, C_SUB);
        cv_text(x + 10, y0 + 20, &FONT_M, v, pc);
    } else {
        static const char *const HINT[NPAGES] = {
            "WHERE THE GROOVE GOES", "SELECT CHOOSES THE PART", "ITS PATTERN: ITS OWN LENGTH", "SELECT CHOOSES THE PART",
            "THIS PART INTO EACH EFFECT", "THE WHOLE KIT'S EFFECTS", "EACH EFFECT'S KIND", "THE DELAY, IN TIME",
            "HOW THE PARTS MOVE", "WHERE THE GROOVE GOES", "THE PERC'S NOTES, THE TEMPO", "THE SQUARES",
            "SYNC, AND AFTER IMAGES HEARD", "AFTER IMAGES' LAYERS, THE DRUMS OUT", "THE RECORDING'S LEVEL" };
        cv_text(6, y0 + 26, &FONT_XS, HINT[pg], dim(C_SUB, 0.8f));
    }
    for (int k = 0; k < 4; k++) {
        int cx = 60 * k + 30, y = y0 + 50, hot = focus && k == ui.last_knob;
        const char *nm = knob_of(pg, k)->name;
        cv_text(cx - text_w(&FONT_XS, nm) / 2, y, &FONT_XS, nm, hot ? pc : C_SUB);
        char v[24]; knob_text(pg, k, v);
        int room = k < 3 && knob_of(pg, k + 1)->kind == K_NONE ? 112 : 56;
        const felucca_font_t *f = text_w(&FONT_M, v) <= room ? &FONT_M : text_w(&FONT_B, v) <= room + 2 ? &FONT_B : &FONT_XS;
        if (text_w(f, v) > room + 2) v[9] = 0;
        int vx = cx - text_w(f, v) / 2; if (vx < 4) vx = 4;
        cv_text(vx, y + (f == &FONT_M ? 14 : 18), f, v, hot ? C_WHITE : C_TXT);
        if (knob_of(pg, k)->kind == K_NONE) continue;
        int bw = 48, fill = (int)(knob_frac(pg, k) * (float)bw + 0.5f);
        cv_rect(cx - bw / 2, y + 44, bw, 4, C_WELL);
        cv_rect(cx - bw / 2, y + 44, fill, 4, dim(pc, hot ? 1.0f : 0.7f));
    }
}

static void speak(void)
{
    if (ann_on() && ann_where()) {
        char t[160] = "", n[40];
        groove_label(n); text_cat(t, n); text_cat(t, ". ");
        text_cat(t, PAGE_NAME[ui.page]); text_cat(t, " page");
        if (page_has_part(ui.page)) { text_cat(t, ", "); text_cat(t, PART_NAME[ui.sel]); }
        for (int k = 0; k < 4; k++) {
            char v[24];
            if (ui.page == PG_HOME) { int_text(set.cur.p[k].level, v); text_cat(t, ". "); text_cat(t, PART_NAME[k]); }
            else { knob_text(ui.page, k, v); text_cat(t, ". "); text_cat(t, knob_of(ui.page, k)->name); }
            text_cat(t, " "); text_cat(t, v);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* ------------------------------------------------------------------ the opening: the grid, the name knocked out of it */
#define SPLASH_MS 2700u
static const char SPLASH_TXT[2][12] = { "EXPECTATION", "REALITY" };
static struct { uint8_t gx, gy; char ch; uint16_t at; } sp_let[18];
void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    splash_grid = 1; vis_deal();
    int n = 0;
    for (int w = 0; w < 2; w++) for (int i = 0; SPLASH_TXT[w][i]; i++) { sp_let[n].gx = (uint8_t)(i + (w ? 2 : 0)); sp_let[n].gy = (uint8_t)(4 + w * 2); sp_let[n].ch = SPLASH_TXT[w][i]; sp_let[n].at = 0; n++; }
    for (int k = 0; k < 18; k++) { int i; do i = (int)(jr() % 18u); while (sp_let[i].at); sp_let[i].at = (uint16_t)(250 + k * 85 + (int)(jr() % 40u)); }
}
static void splash_frame(uint32_t ms)
{
    float fade = 1.0f - clampf(((float)ms - 2050.0f) / 550.0f, 0.0f, 1.0f);
    for (int top = 0; top < 240; top += 60) {
        cv_begin(240, 60, C_BLACK);
        cv_oy = -top; cv_y0 = 0; cv_y1 = 60;
        for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) {
            int py = y * 20; if (py + 20 + cv_oy < 0 || py + cv_oy >= 60) continue;
            VCell *c = &vg[y][x];
            int out = 0;
            for (int i = 0; i < 18; i++) if (sp_let[i].gx == x && sp_let[i].gy == y && (int)ms >= sp_let[i].at) out = 1;
            if (!out) cv_rect(x * 20 + 1, py + 1, 18, 18, dim(PART_C[c->part % NPARTS], (float)c->base / 255.0f * 0.55f * fade));
        }
        for (int i = 0; i < 18; i++) {
            if ((int)ms < sp_let[i].at) continue;
            int cx = sp_let[i].gx * 20 + 10, cy = sp_let[i].gy * 20 + 10;
            if (cy + 12 < top || cy - 12 >= top + 60) continue;
            char t[2] = { sp_let[i].ch, 0 };
            cv_text(cx - text_w(&FONT_B, t) / 2, cy - 7, &FONT_B, t, dim(PART_C[i % NPARTS], fade));
        }
        cv_blit(0, (uint32_t)top);
    }
}
static void splash_end(void)
{
    ui.splash_on = 0; splash_grid = 0;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    vis_deal();
    ui.last_view = plat_ms();
}

void app_ui_init(void)
{
    touch_on(1);
    ui.sel = P_HATS;
    settings_apply();
    ui.page = PG_HOME;
    ui.uiq_tail = eng.uiq_head;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    app_splash();
    ui.splash_on = 1; ui.splash_t0 = plat_ms(); ui.touched = 0;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    {   uint32_t xr = plat_xruns();
        if (xr != xr_seen) { if (save_end && now - save_end < 500u) xr_saves += xr - xr_seen; xr_seen = xr; }
    }
    input();
    speak();
    if (ui.splash_on) {
        static uint32_t last;
        if (ui.touched || now - ui.splash_t0 >= SPLASH_MS) splash_end();
        else { if (now - last >= 30u) { last = now; splash_frame(now - ui.splash_t0); } return; }
    }
    {   static uint32_t mut_seen;               /* EVOLVE rewrote a pattern: the groove changed (kept at the next save) */
        if (eng.mutated != mut_seen) { mut_seen = eng.mutated; changed(); }
    }
    if (now - ui.last_view >= 40u) {
        float dt = (float)(now - ui.last_view) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        ui.last_view = now; ui.t += dt;
        vis_step();
        draw_view();
    }
    {   /* the load guard: past 66 % of the render's time, fewer voices; back slowly as it eases */
        static uint32_t last_g;
        if (now - last_g >= 20u) {
            last_g = now;
            uint32_t cpu = plat_cpu_pct();
            float g = eng.gov;
            if (cpu > 66u) g -= 0.04f; else if (cpu < 55u) g += 0.004f;
            eng.gov = g < 0.3f ? 0.3f : g > 1.0f ? 1.0f : g;
        }
    }
    if (save_pending && (gain_now < 0.004f || now - save_t > 250u)) { store_now(save_pending - 1); save_pending = 0; save_hold = 0; }
    /* the settings kept, never while it plays (the flash's erase would drop the audio): once stopped and faded */
    static int auto_fading;
    if (auto_fading && set.playing) { auto_fading = 0; save_hold = 0; }
    if (autosave_at && (int32_t)(now - autosave_at) >= 0 && !set.playing && now - stopped_at >= 2500u && !save_pending
        && (out_lvl < 0.002f || now - stopped_at >= 6000u)
        && (auto_fading || (save_hold = 1, auto_fading = 1, save_t = now, 0)) && (gain_now <= 0.004f || now - save_t >= 250u)) {
        autosave_at = 0; auto_fading = 0;
        store_ok = store_save();
        save_hold = 0;
    }
    if (now - ui.last_head >= 100u && !full_now()) { ui.last_head = now; draw_header(); }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "sel")) *got = ui.sel;
    else if (!strcmp(what, "nsaved")) *got = set.nsaved;
    else if (!strcmp(what, "slot")) *got = set.slot == 255 ? -1 : set.slot;
    else if (!strcmp(what, "playing")) *got = set.playing;
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "seed")) *got = (int)(set.cur.seed & 0x7FFFFFFF);
    else if (!strcmp(what, "tempo")) *got = 40 + set.cur.tempo;
    else if (!strcmp(what, "mood")) *got = set.cur.mood;
    else if (!strcmp(what, "clean")) *got = eng.clean;
    else if (!strcmp(what, "kept")) *got = ui.kept_ok >= 1;
    else if (!strcmp(what, "style")) *got = set.style;
    else if (!strcmp(what, "usb_full")) *got = set.usb_full;
    else if (!strcmp(what, "activity")) *got = set.cur.activity;
    else if (!strcmp(what, "evolve")) *got = set.cur.evolve;
    else if (!strcmp(what, "drift")) *got = set.cur.drift;
    else if (!strcmp(what, "warp")) *got = set.cur.warp;
    else if (!strcmp(what, "room")) *got = set.cur.room;
    else if (!strcmp(what, "comp_t")) *got = set.cur.comp_t;
    else if (!strcmp(what, "sync")) *got = set.sync;
    else if (!strcmp(what, "palette")) *got = set.palette;
    else if (!strcmp(what, "peg_n")) *got = set.peg_n;
    else if (!strcmp(what, "hist")) *got = hist_n;
    else if (!strcmp(what, "pu")) *got = eng.pu;
    else if (!strcmp(what, "pu_want")) *got = eng.pu_want;
    else if (!strcmp(what, "fill_want")) *got = eng.fill_want;
    else if (!strcmp(what, "hpf")) *got = eng.pu_hpw > 0;
    else if (!strcmp(what, "lpf")) *got = eng.pu_lpw > 0;
    else if (!strcmp(what, "heard")) *got = (int)eng.ai_notes;
    else if (!strcmp(what, "clockbpm")) *got = (int)(eng.bpm + 0.5f);
    else if (!strcmp(what, "beatlag")) {
        float err = ((float)clk_n / 24.0f) - eng.beat; err -= 4.0f * fm_floorf(err * 0.25f + 0.5f);
        *got = (int)(err * 100.0f + (err < 0 ? -0.5f : 0.5f));
    }
    else if (!strcmp(what, "engine")) *got = set.cur.p[ui.sel].engine;
    else if (!strcmp(what, "tone")) *got = set.cur.p[ui.sel].tone;
    else if (!strcmp(what, "weight")) *got = set.cur.p[ui.sel].weight;
    else if (!strcmp(what, "entropy")) *got = set.cur.p[ui.sel].entropy;
    else if (!strcmp(what, "len")) *got = set.cur.p[ui.sel].len;
    else if (!strcmp(what, "s_room")) *got = set.cur.p[ui.sel].s_room;
    else if (!strncmp(what, "rnd", 3)) *got = eng.rnd_bits[atoi(what + 3) & 3];
    else if (!strncmp(what, "level", 5)) *got = set.cur.p[atoi(what + 5) & 3].level;
    else if (!strncmp(what, "events", 6)) *got = eng.events[atoi(what + 6) & 3];
    else if (!strcmp(what, "muts")) { int m = 0; for (int p = 0; p < NPARTS; p++) m += set.cur.p[p].muts; *got = m; }
    else return 0;
    return 1;
}
#endif
