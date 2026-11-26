# Expectation Reality

A generative percussion firmware for the M-VAVE FM-1, built on
[fm1-base](https://github.com/charlesvestal/fm1-base) (a git submodule in `base/`, as in After Images). Press REC and
it deals a new groove; macros steer it rather than programming steps by hand. It runs on its own, but its main job
is to play under [After Images](https://github.com/TheGrizard/After-Images): following its MIDI clock and listening
to the notes it sends (one channel a layer) so the percussion reacts to what's playing.

Sound world: synthetic, interesting percussion. Autechre, Burial, Boards of Canada, Objekt; ambient and electronica.
Generative at its core, with macros across the main features, as After Images has.

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
