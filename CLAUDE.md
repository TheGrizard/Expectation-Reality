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
  never metallic), an almost-vocal hat and impulse-click hats. The owner may send reference samples to tune towards.
- REC generates a fixed groove that loops exactly; the macros add evolution into it: probability and step components
  like Teenage Engineering's and Elektron's sequencers (conditions, ratchets, nudges and so on).
- HOME held + K1-K4 (and GLO's MACROS page) keep After Images' names: MOOD (how the hits are played: hard, staccato
  and bright at 0; soft, long, full and darker at 100; in the middle a variety, each part dealt its own place),
  ACTIVITY (density), EVOLVE (from the fixed loop, expectation, towards variation and evolving, reality), DRIFT (the
  mixer: drum parts fading in and out over time; 0 keeps everything as set). Timing lives in MOTION's WARP.
- The LFO page keeps After Images' MOTION settings by name (LINK, WARP, FLOW, CHAOS), read for drums: LINK the
  parts (and After Images) answering each other, WARP swing and push/drag, FLOW slow drift of the sounds, CHAOS
  ratchets, skips and displacement. WARP is a humaniser (timing and velocity); SWING is its own setting (GLO's
  SCALE page), dealt by REC.
- Velocity is shaped for groove, experimental but groovy (Autechre, Boards of Canada): the bar's weight (downbeats
  strongest), a drifting accent cycle of 3, 5 or 7 sixteenths, a slow wave; harder hits brighter and heavier.
- First iteration: IDM in feel and machine-like.
- Generators: Turing machines for the high percussion (hats, shakers); Euclidean rhythms, a Grids-style groove map,
  polymeters, bouncing-ball rolls, probability and ghost notes.
- Effects, in three places:
  - The white keys are always punch-ins, OP-XY-style, for performance: a high-pass (about 500 Hz, to be confirmed)
    on the bottom white key and a low-pass on the top one, each faded in and out so it never clicks; pitch up and
    pitch down (the owner especially likes these), a ratchet (eighths to 64ths), repeats, glitch, tape stop, reverse, a
    room freeze, a reverse swell, crush, kill kick. Repeats, ratchet, glitch and reverse are held to the grid.
  - The black keys are randomisers on each part's SOUND settings, sample and hold at every hit while held: the
    first group of three KICK's, the next two SNARE's, the next three PERC's, the next two HATS', the lone top one
    every part's (in the two-key groups WEIGHT and ENTROPY; TONE left out: to confirm with the owner).
  - FX: SENDS for the chosen part (SEL), as in After Images: ROOM, DELAY, PAN and DUCK (how far it ducks under the
    kick), REC dealing sensible starting amounts. The kick is never sent to ROOM, DELAY or PAN.
  - FX again: the bus amounts, ROOM, COMP, DELAY, PAN. FX a third time: each one's type (a knob each, its name on
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
  COUNT setting on the SCREEN page for other sizes. Only loud kicks and snares knock out a 2x2 block.
  Colours: four clearly contrasting hues a palette, one per part, as After Images' rule has it; REC deals one from a
  shuffled deck: After Images' nine (classic, sunset, ocean, neon, ember, forest, slate, jade, amber) and the owner's
  favourites ACID (#d7ff00, #ff00a8, #00e5ff, #ffffff) and ULTRAVIOLET (#8a2bff, #ff00f7, #00f0ff, #f4e8ff), in the
  order KICK, SNARE, PERC, HATS.
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
