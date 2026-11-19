---
name: add-roaster
description: Add a new roaster preset to the RoastSim simulator (a JSON file in data/roasters/) that models a specific coffee roasting machine, whether a popcorn popper, a fluid-bed air roaster, a home or shop drum roaster (gas, electric, induction, infrared) or a stir-arm pan roaster. Then calibrate it until it roasts like the real machine. Use this whenever someone wants RoastSim to simulate a particular roaster or a variant of one, including "add my roaster", "make a preset for a 2 kg gas drum", "simulate a heat-gun dog-bowl setup", tuning an existing roaster preset to match real roast logs, or changing a roaster's controls or probe positions for Artisan.
---

# Add a roaster preset to RoastSim

A roaster preset is one JSON file in `data/roasters/`. It picks one of three machine families already in the C++ core, declares the controls and probes the real machine has, and sets the physical parameters (power, airflow, thermal masses, heat-transfer coefficients). No C++ is needed unless the machine fits none of the families (see "When no family fits").

Success means two things. The preset passes validation, and a roaster who knows the real machine would find the simulated roast believable: turning point, first-crack time, how it responds to heat and fan, and what goes wrong when it's misused. Calibrating is iterative. Expect a few rounds of changing parameters and re-running the reference roast.

## Workflow

### 1. Pick the machine family

| Family (`kind`) | Real machines | Closest existing preset |
|---|---|---|
| `fluid_bed` | hot-air roasters, popcorn poppers, fluid-bed/"air" roasters | `air_roaster`, `popper_stock` (variac only), `popper_modded` (SSR + separate fan) |
| `drum` | any rotating drum: home electric, induction, IR/quartz, shop/production gas, also rotating-chamber hot-air roasters (approximate) | `drum_home_1kg` (induction), `drum_tabletop_ir`, `drum_shop_5kg_gas`, `drum_shop_15kg_gas` |
| `stir_pan` | heated plate or pan plus a stirring arm under a dome or lid; stove-top stir poppers | `stir_pan` |

Approximations that work: a rotating glass chamber blown with hot air is a `drum` with `heat_source: "electric"`, `frac_air` around 0.8, and a light, low-conductivity drum. A heat-gun-and-bowl setup is a `stir_pan` (bowl as "pan", hand stirring as `stir`) with the heat gun mostly heating air. Say in `description` what was approximated.

### 2. Gather the machine's facts

Rated heater power and voltage, or burner kW. Batch range. Drum diameter, length and approximate drum mass. Which controls the real machine exposes (and their ranges). Where its temperature probes sit. Typical roast times users report. Anything the user tells you about its personality ("takes forever to preheat", "fan has to come down as beans lighten", "the thermostat cuts out").

Estimate what is missing using `references/roaster-fields.md`. It has scaling rules (e.g. drum mass per kg of capacity, burner kW per kg). Ask the user only when a choice would change the machine's character and you cannot infer it, such as whether their popper is stock or modded.

### 3. Write the file

Copy the closest preset, then:
- **Identity:** `id` (snake_case, equal to the file name), `name` and `description`. The description is shown in the UIs: write one or two sentences on what the machine is and how to drive it.
- **`batch`:** `default_g`, `min_g`, `max_g` from the real machine.
- **`controls`:** only what the real machine has (`heat`, `fan`, `drum`, `stir`, `voltage`). Labels as printed on the machine. Gas valves get `slew_per_s` (15–25). A cooling-tray toggle is added automatically. A machine with a fixed-speed drum or stir arm has no `drum`/`stir` control and runs at 75 % / 60 % of `rpm_max` / `stir_rpm_max`. Set those maxima so that this equals the real speed (e.g. a 48 rpm drum → `rpm_max` 64).
- **`probes`:** exactly one `BT` and one `ET`, plus optional extras. Locations: `bean_mass`, `inlet_air`, `exhaust_air`, `chamber_air`, `wall`, `element`, `ambient`. Place ET where the real machine (or common Artisan mods) measures it. Extras appear in Artisan as additional channels, in definition order (Modbus registers 2 and 3).
- **Family block** (`fluid_bed`, `drum` or `stir_pan`): scale the template's numbers with the rules in the reference. Change one group at a time rather than every number.

### 4. Validate, then calibrate

Build and run the checker. See "Running the tools" for the build command.

```
build/roastsim_check --data data --roaster <id>
build/roastsim_check --data data --roaster <id> --batch <min_g>
build/roastsim_check --data data --roaster <id> --batch <max_g>
```

The reference roast holds controls at their defaults with fixed heat (100 % for fluid beds and stir pans, 85 % for drums), preheats drums to 200 °C ET, and drops at 20 % DTR. Its envelope: TP within 2:30, FC between 3:00 and 16:00, weight loss 9–18 %, final moisture ≤ 4.5 %, no fuse, stall, overheat, centrifuging, scorching or ejection warnings. `--heat` and `--preheat` let you try other operating points.

Then compare with what the real machine does. If the user gave real numbers ("FC around 9:30 at 70 % gas, 1.5 kg batch"), reproduce that roast as a scenario script in `scripts/<id>_<what>.json`, following the existing ones. Run it with `build/roastsim_script scripts/<file>.json --data data --every 60` and tune until it is close, say FC within about ±45 s and drop temperature within about ±5 °C. The script stays in the repo as documentation of what the preset was calibrated against.

Calibration knobs, in the order to reach for them, are in `references/calibration.md`. Read it before changing heat-transfer coefficients: several knobs interact, and the table says which symptom each one fixes.

Also exercise the machine's personality on purpose. Hazards and alarms emerge from the physics, so a preset whose failure modes don't show up is usually mis-scaled. What to expect:
- fluid bed, fan too low → `bed_stalled` (and on a popper, eventually `fuse_blown`); fan too high or batch too small → `beans_ejecting`
- stir pan, arm stopped → `scorching` and a large `agtron_sd`
- drum turned above its critical speed → `centrifuging`; very low drum speed → uneven roast
- an overfilled drum has **no** alarm. It just roasts slowly and the BT probe stays buried, which is realistic, so mention it rather than chasing a warning.

### 5. Optionally lock it in with a test

If the user wants it, add a case to `tests/test_main.cpp` following `drum_shop_roast` (a scenario plus `checkSensible(result, fcMin, fcMax)`). Then build and run `build/roastsim_tests data`. Mention this option even if you don't do it.

### 6. Report back

Tell the user:
- the file you created, the family chosen, and what was approximated
- key parameters, marking each *given* or *estimated*
- the reference-roast numbers (TP, DRY, FCs, drop, loss, Agtron) and how they compare with the real machine
- any warnings left, and why they're acceptable

The preset appears in the simulator after a restart. Rebuilding copies `data/` next to the executables, and `roastsimd --data data` reads the source folder directly.

## Running the tools

On Windows the toolchain is MSYS2 UCRT64 and is not on PATH by default. In the Bash tool:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja      # only if build/ doesn't exist yet
cmake --build build --target roastsim_check --target roastsim_script
./build/roastsim_check.exe --data data --roaster <id>
```

On Linux or Haiku, drop the PATH line and the `.exe`. `--json` gives machine-readable output, and `--all` runs every preset (useful to confirm you didn't break others).

## When no family fits

If the machine's heat transfer really differs from all three families, a preset would be misleading. For example, a solar roaster, or a microwave heating beans from the inside. In that case:
1. Say so plainly and give the closest approximation with its limits.
2. If the user wants a real model, that is a new `MachineModel` subclass (one `.cpp` in `core/src/`, a `RoasterKind` value and a JSON block). See `docs/ARCHITECTURE.md` "Adding things". That is a code change, not this skill's workflow.

## Scope

Only add or edit files in `data/roasters/` and, when calibrating against real data, `scripts/` and optionally `tests/`. Don't change other presets or core constants to make a new preset fit. Other presets were calibrated against those constants, so changing them silently shifts every other machine.
