# Sound

RoastSim generates the roaster's sound procedurally from the live simulation: no sample files, nothing scripted. One portable synthesizer (`service/src/audio_synth.cpp`, class `AudioSynth`) is shared by every front end:

| Front end | How it plays |
|---|---|
| Win32 app | **Simulation ▸ Sound** and **Simulation ▸ Volume** menus (local playback via `waveOut`) |
| `roastsimd --sound` | local playback on Windows |
| Web UI | **Sound** checkbox: the host renders per browser and streams 16-bit PCM over the WebSocket, so phones and Linux/Haiku machines hear exactly the same thing |
| Future GTK / Haiku shells | call `rs_audio_render()` from their own audio callback (or add a backend in `platform_audio.cpp`) |
| Offline | `roastsim_sound --roaster … --bean … --from fc --out file.wav` renders a WAV, for auditioning and tuning |

## What you hear

**The machine** follows the controls and the machine state.

| Roaster | Layers |
|---|---|
| Popper | Universal-motor whine (its pitch follows the fan) and loud air rush; beans rattling in the vortex |
| Air roaster | Softer brushless blower and air rush; beans rattling against the chamber |
| Drum | Drum rumble pulsing with each vane (4 per turn); gear motor; exhaust-fan whoosh; **gas burner roar** with flame flutter (gas) or element hum at twice mains frequency (electric/IR/induction); beans cascading in waves as each vane tips |
| Stir pan | Gear-motor whir, the arm scraping round the pan, the dome fan; beans shuffling |
| Cooling tray | Tray fan and stirrer rattle once the beans drop (with the cooling switch on) |
| Any | Bimetal thermostat clicks as it cycles; the click of a thermal fuse blowing |

The beans' own sound changes through the roast. Green, wet beans thud dully; dry, roasted beans are light and rattle brightly. A stalled fluid bed goes quiet and dull.

**Cracks:** every first and second crack the physics produces is voiced individually, from the bean that cracked:

| Bean property | Effect on the crack |
|---|---|
| size (`bean_mass_mg`, plus expansion) | bigger beans pop lower and a little louder |
| density / hardness | harder beans crack sharper, higher and louder |
| moisture left at the crack | wetter beans burst fuller and longer, with a low steam "thump" |
| `crack_loudness` (bean preset) | overall audibility; decaf is 0.45, matching its famously quiet first crack |
| first vs second crack | FC: woody snap with a body. SC: fine, fast, high crackle (like crisped rice), often in tiny clusters |
| where it happens | inside a drum cracks are muffled (×0.8, duller), under a stir-pan dome ×0.65; in a popper and in the cooling tray they're heard in the open |

Crack timing comes from the simulation, so the density of the crackle (a few pops at the start of first crack, a rolling chorus at its peak, a roar from a 5 kg batch) is real roast behaviour, not a script. At high simulation speeds the crack stream is thinned and slightly boosted, so it stays listenable. Cracks skipped over by a fast-forward (preheat, MCP `advance_time`, a `step` command) are not played afterwards; only those from about the last second of simulated time (more at higher speeds) are voiced.

## Levels

Measured with `roastsim_sound --stats` at first crack with default controls:

| Roaster | Machine RMS | First-crack peaks |
|---|---|---|
| Popper | 0.076 | ~0.5 |
| Air roaster | 0.051 | ~0.47 |
| Home drum | 0.034 | ~0.21 (muffled) |
| Shop drum (gas) | 0.063 | ~0.3 |
| Stir pan | 0.029 | ~0.5 |

The bean comparison on the popper (crack peaks): Kenya 0.50 > Colombia 0.46 > Ethiopia 0.41 (small beans) ≫ decaf 0.19.

## APIs

* C++: `AudioSynth synth(host, 44100); synth.render(buf, n); synth.setMix({master, machine, cracks});`
* C: `rs_audio_create(host, rate)`, `rs_audio_render`, `rs_audio_set_mix`, `rs_audio_play` / `rs_audio_pause` (local output where supported), `rs_audio_destroy`.
* WebSocket: `{"cmd":"audio","value":true,"rate":24000}` starts a stream of **binary** frames, each a chunk of little-endian int16 mono PCM at that rate, kept about 150 ms ahead of real time. `{"cmd":"audio_mix","master":0.8,"machine":1,"cracks":1}` sets levels; `{"cmd":"audio","value":false}` stops the stream.
* Core: `Simulation::cracksSince(seq)` gives the raw crack events (`CrackEvent`: time, first/second, size, hardness, moisture, loudness, where). The future 3D view can use the same events for crack particles.

## Tuning

Use `roastsim_sound` (machine only: `--cracks 0`; cracks only: `--machine 0`; `--stats` prints per-second RMS, peak and crack counts). Machine recipes are in `AudioSynth::Impl::refresh()`, crack voicing in `scheduleCracks()`. Bean audibility belongs in the bean preset (`crack_loudness`), not in the synthesizer.
