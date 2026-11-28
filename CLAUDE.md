# Expectation Reality

A generative percussion firmware for the M-VAVE FM-1, built on
[fm1-base](https://github.com/charlesvestal/fm1-base) (a git submodule in `base/`, as in After Images). Press REC and
it deals a new groove; macros steer it rather than programming steps by hand. It runs on its own, but its main job
is to play under [After Images](https://github.com/TheGrizard/After-Images): following its MIDI clock and listening
to the notes it sends (one channel a layer) so the percussion reacts to what's playing.

Sound world: synthetic, interesting percussion. Autechre, Burial, Boards of Canada, Objekt; ambient and electronica.
Generative at its core, with macros across the main features, as After Images has.

## Direction from the owner (November 2026)

- A partner firmware to After Images, inheriting the look and feel of its menu system. Keep a lot of After Images:
  its pages and setting names wherever they mean the same thing (EDIT's SOUND page: ENGINE, TONE, WEIGHT, ENTROPY,
  with TONE as After Images' DJ filter: 12 o'clock open, left a low-pass, right a high-pass); rename only where the
  drum meaning differs. ENTROPY focuses on the hit's attack (its first moments: a soft start, click, noise snap, zap,
  ring-modulated edge, crushed stutter), its character carried through the rest of the hit and stretched as the hit
  gets longer; WEIGHT is the hit's fullness and spreads the exciter (longer, lower, wider).
- Always a kit: four roles (KICK, SNARE, PERC, HATS) that hold the groove, with really interesting synthesised
  engines and really interesting patterns: lots of polymeters. Sounds short and staccato, not cliché or predictable;
  perc low (just above the kick); pitch falls fast, never slow sweeps. Engines include a soft 808, a synthesised clap,
  a circuit-bent BENT (wild, every WEIGHT/ENTROPY setting its own), FOLEY (box and wood bangs, footsteps, drags: low,
  never metallic), an almost-vocal hat and impulse-click hats; and (v0.13, the owner's ask: five more a part, expressive across WEIGHT
  and ENTROPY, their bodies taking ENTROPY's whole range) PING, FOLD, TAPE, THUD, RUMBLE; RIM, SNAP, BRUSH, SYNTH,
  SPRING; BLOCK, TABLA, HAND, TINE, BOTTLE; SHAKER, FM HAT, AIR, CHIP, FOIL. The ENGINE knob steps through them
  grouped by part. The owner may send reference samples to tune towards.
- The kick is the anchor: ACTIVITY adds only a few kicks a bar (never neighbouring sixteenths unless written strong),
  CHAOS never touches it, EVOLVE gives it only accents, ghosts and conditions (no nudges, rolls, drops, swapped beats
  or rewrites), WARP loosens it least. ACTIVITY at 0 is a simple, sparse beat: each part's core hits.
- Any engine can play on any part, in that part's register (a snare's or hat's engine on the kick dropped down).
- REC generates a fixed groove that loops exactly; the macros add evolution into it: probability and step components
  like Teenage Engineering's and Elektron's sequencers (conditions, ratchets, nudges and so on).
- HOME held + K1-K4 (and GLO's MACROS page) keep After Images' names: MOOD (how the hits are played: heavy, long,
  full and soft (darker) at 0; bright, short, hard and staccato at 100; in the middle a variety, each part dealt its own place),
  ACTIVITY (density), EVOLVE (from the fixed loop, expectation, towards variation and evolving, reality), DRIFT (the
  mixer: drum parts fading in and out over time; 0 keeps everything as set). Timing lives in MOTION's WARP.
- The LFO page keeps After Images' MOTION settings by name (LINK, WARP, FLOW, CHAOS), read for drums: LINK the
  parts (and After Images) answering each other, WARP swing and push/drag, FLOW slow drift of the sounds, CHAOS
  ratchets, skips and displacement. WARP is a humaniser (timing and velocity); SWING is its own setting (GLO's
  SCALE page), dealt by REC.
- Velocity is shaped for groove, experimental but groovy (Autechre, Boards of Canada): the bar's weight (downbeats
  strongest), a drifting accent cycle of 3, 5 or 7 sixteenths, a slow wave; harder hits brighter and heavier.
- First iteration: IDM in feel and machine-like. GLO's PERSONALITY (its own page, GLO twice; GLO is MACROS,
  PERSONALITY, VISUALS, SCALE; in the code, a groove's genre) chooses what REC deals: BRAINDANCE (IDM, the first),
  FLOORS (techno: four on the floor always, the snare or clap on two and four, perc Euclid's on odd lengths and
  Turing hats round an off-beat: polymetric, hypnotic; next to no swing), CLOUDS (ambient: soft and sparse,
  micro-sounds: a soft kick about once a bar, the snare a rare texture, tiny clicks, dust and grains in 1/32s and
  triplets on odd lengths) or IMPLODE (lo-fi hip hop: boom-bap, written kicks, a crack on two and four with ghosts,
  dusty hats, dark tones, heavy swing). Choosing one sets its tempo (BRAINDANCE 140, FLOORS 130, CLOUDS 65, IMPLODE
  85); a groove keeps its personality.
- Generators: Turing machines for the high percussion (hats, shakers); Euclidean rhythms, a Grids-style groove map,
  polymeters, fills as simple groove changes (no rolls, nothing accelerating), probability and ghost notes.
- Effects, in three places:
  - The white keys are always punch-ins, OP-XY-style, for performance: a high-pass (from about 350 Hz)
    on the bottom white key and a low-pass on the top one, each faded in and out so it never clicks; pitch up and
    pitch down (the owner especially likes these), FILL on the second white key (a simple change of the groove, every
    part a slight variation of its pattern, for a bar or while held; never rolls or frenetic runs, never accelerating), repeats, glitch, tape mash (a held, chewed-tape wobble), reverse, a
    room freeze, a reverse swell, crush, kill kick. Repeats, glitch and reverse are held to the grid. FREEZE takes the kick in too.
    Quantised (the owner's timing is loose): a punch-in starts on the next beat (up to a sixteenth late: at once), ends
    the moment it's let go (FREEZE's wash gone within a quarter second); SWELL its own timing, the filters instant.
    FILL is a contrast: ACTIVITY low it adds (perc and hats busier), high it takes away (their core hits), in between
    either at random.
    Holds: OCT- held as a key (white or black) is pressed holds it to the loop's end, OCT+ for good; pressed again it
    is let go; OCT+ alone lets every hold go. One white key at a time, the filters and FILL included, and one black key
    at a time (v0.16, the owner's rule: stacked effects overloaded the FM-1): the newest plays, letting it go hands back
    to the one before if still held; a new hold replaces the old. Groove history (REC's replaced grooves) is on HOME held + OCT- / OCT+.
  - The black keys are randomisers on each part's SOUND settings, sample and hold at every hit while held: the
    first group of three KICK's, the next two SNARE's, the next three PERC's, the next two HATS', the lone top one
    every part's (in the two-key groups WEIGHT and ENTROPY; TONE left out: to confirm with the owner).
  - The three FX pages in one order, REVERB, DELAY, PAN, COMP. FX: SENDS for the chosen part (SEL): REVERB, DELAY,
    PAN, COMP (how much goes through the compressor, which always hears the kick: it pumps; this replaced DUCK).
    PERSONALITY's SENDS: DRY (REC deals every part dry; the owner adds them; the kick can be sent too) or DEALT (each
    personality's own sends for the snare, perc and hats, all under 50 for now; the kick never to the reverb, delay or
    pan, only some COMP).
  - FX again: REVERB SIZE, DELAY TIME, PAN AMOUNT, COMP AMOUNT (CCs 24-27 in that order; the reverb's and delay's
    levels come from the sends). FX a fourth time: DELAY's FEEDBACK and GLITCH. FX a third time: each one's type,
    REVERB TYPE, DELAY TYPE, PAN TYPE, COMP TYPE (a knob each, its name on
    screen, a change crossfaded), as After Images' second effects page offers DIRT's varieties and CHORUS's
    ensemble. ROOM: room, gated (pre-delay and gate synced to the tempo), reverse, freeze. COMP: clean, tape smash.
    DELAY: clean, granular (dirty). PAN: sweep, spin, scatter.
  - COMP is tape-style, simple and musical, extreme when cranked. It takes the whole kit, kick included (glue, and
    dirt on the kick), and the kick is hard-wired as its sidechain. DELAY is tempo-synced and can go very fast, into
    Autechre glitch. Room for anything else weird and wonderful.
- Visuals: After Images' MATRIX turned inside out, squares instead of circles, brighter and more extreme. REC lays
  the grid out anew in a random arrangement of colours; each hit knocks a square out somewhere on the grid
  (scattered, not in rows), and it fades back in in that part's colour, so the hits slowly repaint the layout
  (After Images adds a dot on a hit; this removes one). Whole squares meet edge to edge (no black line); a knocked-out
  one grows back. 8x8 by default (the owner loves it), with After Images'
  COUNT setting on the SCREEN page for other sizes. Only loud kicks and snares knock out a 2x2 block. The top of
  the loop (the kick and snare patterns' cycle) clears the middle of the grid (4x4 at 8x8, half its width at other
  sizes), growing back; in TONES, black fading back to grey.
  Colours: four clearly contrasting hues a palette, one per part, as After Images' rule has it; REC deals one from a
  shuffled deck: After Images' nine (classic, sunset, ocean, neon, ember, forest, slate, jade, amber) and the owner's
  favourites ACID (#d7ff00, #ff00a8, #00e5ff, #ffffff) and ULTRAVIOLET (#8a2bff, #ff00f7, #00f0ff, #f4e8ff), in the
  order KICK, SNARE, PERC, HATS; and, the owner asking for more neon, LASER, TOXIC, MIAMI, PLASMA and CANDY.
- The balance, as After Images': each part held at its MOOD 50 loudness at every MOOD (a table measured in the
  simulator, BS.1770-weighted), the mix tilted slowly to a steady top-to-bottom, a leveller (BAL_RMS; the owner finds
  it hot: never nudge it up), a limiter at -1 dBFS last. The volume knob at full and USB GAIN at 100 are unity.
- CPU: the device ran 94% with 1136 drops at v0.2; v0.7 (reverb and delay at half rate, idle filters and pans skipped)
  ran 52%, no drops, with every send and the delay's feedback up (some power-ons fell back to one core: 93%). Measure instruction counts (callgrind on the render tool: about
  240 M a second matched 94%) before and after every DSP change; per block, `CGBLOCK=1` under callgrind (a count after
  each stage: the worst block on one core and on two). Calibrated on v0.7: device % ≈ 4 + 2.3 × the model's (one block's
  instructions against 255 M a second). v0.13 with FX SENDS DEALT and punch-ins: two cores ~66% (worst ~85%); one core
  can't carry it (even dry, over 100% at worst): the second core stood down (MISREAD on some units) means drops; v0.13 still dropped on two cores at ACTIVITY 100,
  EVOLVE 100 (worst blocks ~100%): v0.14 sets fm1.toml audio_ahead (a block rendered ahead, a lone late block rides
  through; clock-following allows for the 5.8 ms). The device's own readings: with sends DRY ~50% whatever is played; DEALT
  far heavier than the model's instruction counts show (memory and the shared instruction cache, likely), so v0.14 also
  runs the reverb and delay at a quarter of the rate (RV_DIV, DL_DIV), the reverb's smear on one channel and its work
  from RAM (.c1_text, fx.h ER_RAM, reached through a pointer), the delay in a loop of its own; the reverb runs on the second core and rests when
  silent, the delay rests, the compressor works only on what's sent, each part's filters rest at TONE's middle.
- SCREEN's STYLE: KNOCKOUT, or TONES (greys from each part's colour's lightness; a hit floods the square with the
  colour, fading back to its grey).
- REC carries one part (at random) into the new groove: its EDIT settings (SOUND: engine, TONE, WEIGHT, ENTROPY;
  MIX: LEVEL, DECAY, PITCH, PAN) as they were, its pattern new: flow between grooves when building a set.
- TEMPO the owner's: 100 BPM at power-on; REC never changes it (live). A saved groove keeps its tempo: recalled (or
  history stepped to), its tempo comes back, and REC follows on from it.
- ACTIVITY by rank within each pattern: 0 stripped back (each part's core), 100 what 40 was in v0.2.
- USB AUDIO as After Images: OUTPUT (MIX, or CLEAN: the kit without FX's effects, the speaker too), CONTROL (MASTER
  or FIXED), GAIN.
- Linking two FM-1s: this one as USB host (fm1-base `usb_host`) through a mains-powered hub, After Images' FM-1 in a
  hub port; a small host-mode test firmware first. Fallbacks: a computer, AUM on an iPad or an Ableton Move as host.

## Working rules

- Build and test in the host simulator before every push. The device build only runs on GitHub when the owner
  publishes a release.
- To release, give the owner the link https://github.com/TheGrizard/Expectation-Reality/releases/new?tag=vX.Y with
  the next version number and remind them to leave pre-release unticked. After they say "published", check the
  firmware build and the install site runs with `gh` and tell them they passed.
- Never post, comment on or close GitHub issues without asking first.
- Performance must be 100% perfect: no clicks, crackles, dropouts or CPU spikes. Measure in the simulator rather
  than guess, and say honestly if something is only proposed and not yet in the code.
- Saved patches and settings must survive every update (settings carry a version number and are migrated).
- When a change affects how it sounds, render audio examples the owner can listen to before committing to it.
- Keep the manual, README and site up to date with every feature.
- Explain things in plain words; the owner is a musician, not a programmer.

## Reference: After Images

Reuse After Images' approaches where they fit rather than reinventing them: the page and knob layout style, the host
simulator scripts and test runner (tests fail on any compiler warning in the app's code), versioned settings with
migration, the release workflow (build and attest firmware on a tag, then the install site), the installer site with
manual and browser emulator, `fmhub.json` for the FM-1 Firmware Hub, the visuals, and the click-free audio work
(per-sample gain glides, a short declick on note starts, nothing stepping once per block). Don't copy its
ambient-specific design unless asked.
